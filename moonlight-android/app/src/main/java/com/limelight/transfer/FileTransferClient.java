/**
 * VipleStream §N — in-stream 雙向檔案傳輸 client（Android）。
 *
 * 對應 server `/transfer/*` HTTPS sidechannel。Lifecycle = streaming session
 * 同生命週期：Game.onStart 啟動 polling，Game.onStop 中止。
 *
 * 不動 moonlight-common-c。複用 NvHTTP 的 OkHttpClient + paired cert。
 *
 * Android scoped-storage 規範下檔案進 `MediaStore.Downloads.EXTERNAL_CONTENT_URI`。
 */
package com.limelight.transfer;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.MediaStore;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.UUID;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

import okhttp3.HttpUrl;
import okhttp3.MediaType;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.RequestBody;
import okhttp3.Response;
import okhttp3.ResponseBody;
import okio.BufferedSink;
import okio.BufferedSource;
import okio.Okio;
import okio.Source;

public class FileTransferClient {
    private static final String TAG = "VipleXfer";
    private static final long POLL_INTERVAL_MS = 2000;
    private static final long CHUNK_BYTES = 64 * 1024;

    /** UI 回呼介面：給 Game 顯示 toast / 更新 overlay。 */
    public interface Listener {
        void onStatus(String message);
    }

    private final Context appContext;
    private final OkHttpClient httpClient;
    private final HttpUrl baseHttpsUrl;
    private final Listener listener;
    private final ScheduledExecutorService scheduler =
            Executors.newSingleThreadScheduledExecutor(r -> {
                Thread t = new Thread(r, "VipleXfer-Poll");
                t.setDaemon(true);
                return t;
            });
    private final AtomicReference<String> activeToken = new AtomicReference<>(null);
    private volatile boolean started = false;

    public FileTransferClient(Context appContext, OkHttpClient client, HttpUrl httpsBase, Listener listener) {
        this.appContext = appContext.getApplicationContext();
        this.httpClient = client;
        this.baseHttpsUrl = httpsBase;
        this.listener = listener;
    }

    /** 啟動 polling，可重入。 */
    public synchronized void start() {
        if (started) return;
        started = true;
        Log.i(TAG, "[VIPLE-XFER] FileTransferClient start base=" + baseHttpsUrl);
        scheduler.scheduleWithFixedDelay(this::pollOnce, 500, POLL_INTERVAL_MS, TimeUnit.MILLISECONDS);
    }

    /** 停止 polling，中止 in-flight transfer。 */
    public synchronized void stop() {
        if (!started) return;
        started = false;
        Log.i(TAG, "[VIPLE-XFER] FileTransferClient stop");
        scheduler.shutdownNow();
        activeToken.set(null);
    }

    /** Game.java 觸發 cancel（v1：不暴露 hotkey，但保留 API）。 */
    public void cancelCurrent() {
        String token = activeToken.get();
        if (token == null) return;
        try {
            JSONObject obj = new JSONObject();
            obj.put("kind", "canceled");
            obj.put("token", token);
            postResult(obj);
        } catch (JSONException ignored) {}
        activeToken.set(null);
        emit("File transfer cancelled");
    }

    private void pollOnce() {
        if (!started) return;
        if (activeToken.get() != null) return;  // 有 transfer 進行中，先別 poll
        try {
            Request req = new Request.Builder()
                    .url(baseHttpsUrl.newBuilder().addPathSegments("transfer/poll").build())
                    .get()
                    .build();
            try (Response r = httpClient.newCall(req).execute()) {
                int code = r.code();
                if (code == 204) return;
                if (code != 200) {
                    Log.d(TAG, "[VIPLE-XFER] poll non-200 " + code);
                    return;
                }
                ResponseBody body = r.body();
                if (body == null) return;
                JSONObject cmd = new JSONObject(body.string());
                dispatch(cmd);
            }
        } catch (Exception e) {
            // Silent — server 短暫不在、stream 收尾、暫時 SSL 重連都會這裡產生例外
            Log.d(TAG, "[VIPLE-XFER] poll error: " + e.getMessage());
        }
    }

    private void dispatch(JSONObject cmd) throws JSONException {
        String type = cmd.optString("type", "");
        String token = cmd.optString("token", "");
        String filename = cmd.optString("filename", "");
        long size = cmd.optLong("size", 0);
        Log.i(TAG, "[VIPLE-XFER] cmd type=" + type + " token=" + token + " filename=" + filename + " size=" + size);
        switch (type) {
            case "list_dir":
                handleListDir(token);
                break;
            case "download_to_client":
                handleDownload(token, filename, size);
                break;
            case "upload_from_client":
                handleUpload(token, filename);
                break;
            case "cancel":
                if (token.equals(activeToken.get())) {
                    cancelCurrent();
                }
                break;
            default:
                Log.w(TAG, "[VIPLE-XFER] unknown cmd type " + type);
        }
    }

    // ── Handler: LIST_DIR ────────────────────────────────────────────────
    private void handleListDir(String queryId) throws JSONException {
        JSONArray entries = new JSONArray();
        ContentResolver cr = appContext.getContentResolver();
        Uri collection;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            collection = MediaStore.Downloads.EXTERNAL_CONTENT_URI;
        } else {
            collection = MediaStore.Files.getContentUri("external");
        }
        String[] projection = {
                MediaStore.Downloads.DISPLAY_NAME,
                MediaStore.Downloads.SIZE,
                MediaStore.Downloads.DATE_MODIFIED
        };
        String selection = null;
        String[] selArgs = null;
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            selection = MediaStore.Files.FileColumns.RELATIVE_PATH + " LIKE ?";
            selArgs = new String[]{Environment.DIRECTORY_DOWNLOADS + "/%"};
        }
        try (Cursor cur = cr.query(collection, projection, selection, selArgs,
                MediaStore.Downloads.DISPLAY_NAME + " ASC")) {
            if (cur != null) {
                int idxName = cur.getColumnIndexOrThrow(MediaStore.Downloads.DISPLAY_NAME);
                int idxSize = cur.getColumnIndexOrThrow(MediaStore.Downloads.SIZE);
                int idxMtime = cur.getColumnIndex(MediaStore.Downloads.DATE_MODIFIED);
                while (cur.moveToNext()) {
                    JSONObject e = new JSONObject();
                    e.put("name", cur.getString(idxName));
                    e.put("size", cur.getLong(idxSize));
                    e.put("mtime", idxMtime >= 0 ? cur.getLong(idxMtime) * 1000L : 0L);
                    entries.put(e);
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "[VIPLE-XFER] list_dir query failed: " + e.getMessage());
        }
        JSONObject out = new JSONObject();
        out.put("kind", "listing");
        out.put("query_id", queryId);
        out.put("entries", entries);
        postResult(out);
        Log.i(TAG, "[VIPLE-XFER] sent listing entries=" + entries.length());
    }

    // ── Handler: DOWNLOAD_TO_CLIENT ───────────────────────────────────────
    private void handleDownload(String token, String filename, long expectedSize) throws JSONException {
        if (activeToken.get() != null) {
            Log.w(TAG, "[VIPLE-XFER] download requested but transfer in progress");
            return;
        }
        String safeName = sanitizeFilename(filename);
        if (safeName == null) {
            Log.w(TAG, "[VIPLE-XFER] download: bad filename " + filename);
            return;
        }
        activeToken.set(token);
        emit("Receiving file: " + safeName + " (0%)");

        ContentValues values = new ContentValues();
        values.put(MediaStore.Downloads.DISPLAY_NAME, safeName);
        values.put(MediaStore.Downloads.MIME_TYPE, "application/octet-stream");
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            values.put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS);
            values.put(MediaStore.Downloads.IS_PENDING, 1);
        }
        Uri collection = (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
                ? MediaStore.Downloads.EXTERNAL_CONTENT_URI
                : MediaStore.Files.getContentUri("external");

        ContentResolver cr = appContext.getContentResolver();
        Uri uri = cr.insert(collection, values);
        if (uri == null) {
            Log.w(TAG, "[VIPLE-XFER] download: failed to allocate MediaStore entry");
            postFailed(token, "mediastore_insert_failed");
            activeToken.set(null);
            return;
        }

        Request req = new Request.Builder()
                .url(baseHttpsUrl.newBuilder().addPathSegments("transfer/blob")
                        .addQueryParameter("token", token).build())
                .get()
                .build();
        boolean ok = false;
        long total = 0;
        int lastBucket = -1;
        try (Response resp = httpClient.newCall(req).execute();
             OutputStream out = cr.openOutputStream(uri)) {
            if (resp.code() != 200 || resp.body() == null || out == null) {
                throw new IOException("HTTP " + resp.code());
            }
            try (InputStream in = resp.body().byteStream()) {
                byte[] buf = new byte[(int) CHUNK_BYTES];
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                    total += n;
                    if (expectedSize > 0) {
                        int bucket = (int) (total * 20L / expectedSize);
                        if (bucket != lastBucket && bucket <= 20) {
                            lastBucket = bucket;
                            int pct = bucket * 5;
                            emit("Receiving file: " + safeName + " (" + pct + "%)");
                            reportProgress(token, "in", total, expectedSize);
                        }
                    }
                }
                ok = true;
            }
        } catch (Exception e) {
            Log.w(TAG, "[VIPLE-XFER] download failed: " + e.getMessage());
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            ContentValues done = new ContentValues();
            done.put(MediaStore.Downloads.IS_PENDING, 0);
            cr.update(uri, done, null, null);
        }
        if (ok) {
            emit("Received: " + safeName + " → Downloads");
            postDone(token);
        } else {
            cr.delete(uri, null, null);
            postFailed(token, "io_error");
        }
        activeToken.set(null);
        Log.i(TAG, "[VIPLE-XFER] download finished token=" + token + " ok=" + ok + " bytes=" + total);
    }

    // ── Handler: UPLOAD_FROM_CLIENT ───────────────────────────────────────
    private void handleUpload(String token, String filename) throws JSONException {
        if (activeToken.get() != null) {
            Log.w(TAG, "[VIPLE-XFER] upload requested but transfer in progress");
            return;
        }
        String safeName = sanitizeFilename(filename);
        if (safeName == null) {
            postFailed(token, "bad_filename");
            return;
        }
        activeToken.set(token);

        // 查 MediaStore 找到對應 Uri
        ContentResolver cr = appContext.getContentResolver();
        Uri found = findDownloadByName(cr, safeName);
        if (found == null) {
            Log.w(TAG, "[VIPLE-XFER] upload: file not found in Downloads: " + safeName);
            postFailed(token, "file_missing");
            activeToken.set(null);
            return;
        }
        long size = 0;
        try (Cursor c = cr.query(found, new String[]{MediaStore.Downloads.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst()) size = c.getLong(0);
        }

        emit("Sending file: " + safeName + " (0%)");

        // Stream upload — OkHttp RequestBody 用 writeTo() 逐塊讀寫，不 buffer 整檔
        final long fileSize = size;
        final String fname = safeName;
        final Uri srcUri = found;
        RequestBody body = new RequestBody() {
            @Override public MediaType contentType() {
                return MediaType.parse("application/octet-stream");
            }
            @Override public long contentLength() {
                return fileSize > 0 ? fileSize : -1;
            }
            @Override public void writeTo(BufferedSink sink) throws IOException {
                long written = 0;
                int lastBucket = -1;
                try (InputStream in = cr.openInputStream(srcUri)) {
                    if (in == null) throw new IOException("openInputStream null");
                    try (Source src = Okio.source(in)) {
                        BufferedSource bs = Okio.buffer(src);
                        byte[] buf = new byte[(int) CHUNK_BYTES];
                        int n;
                        while ((n = bs.read(buf)) > 0) {
                            sink.write(buf, 0, n);
                            written += n;
                            if (fileSize > 0) {
                                int bucket = (int) (written * 20L / fileSize);
                                if (bucket != lastBucket && bucket <= 20) {
                                    lastBucket = bucket;
                                    int pct = bucket * 5;
                                    emit("Sending file: " + fname + " (" + pct + "%)");
                                    reportProgress(token, "out", written, fileSize);
                                }
                            }
                        }
                    }
                }
            }
        };

        Request req = new Request.Builder()
                .url(baseHttpsUrl.newBuilder().addPathSegments("transfer/blob")
                        .addQueryParameter("token", token).build())
                .post(body)
                .build();
        boolean ok = false;
        try (Response resp = httpClient.newCall(req).execute()) {
            ok = resp.isSuccessful();
        } catch (Exception e) {
            Log.w(TAG, "[VIPLE-XFER] upload failed: " + e.getMessage());
        }
        if (ok) {
            emit("Sent: " + safeName);
        } else {
            emit("Send failed: " + safeName);
            postFailed(token, "upload_failed");
        }
        activeToken.set(null);
        Log.i(TAG, "[VIPLE-XFER] upload finished token=" + token + " ok=" + ok);
    }

    private Uri findDownloadByName(ContentResolver cr, String name) {
        Uri collection = (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
                ? MediaStore.Downloads.EXTERNAL_CONTENT_URI
                : MediaStore.Files.getContentUri("external");
        String sel = MediaStore.Downloads.DISPLAY_NAME + "=?";
        try (Cursor c = cr.query(collection, new String[]{MediaStore.Downloads._ID},
                sel, new String[]{name}, null)) {
            if (c != null && c.moveToFirst()) {
                long id = c.getLong(0);
                return Uri.withAppendedPath(collection, String.valueOf(id));
            }
        }
        return null;
    }

    // ── Utilities ─────────────────────────────────────────────────────────
    private void postResult(JSONObject obj) {
        try {
            RequestBody body = RequestBody.create(obj.toString(), MediaType.parse("application/json"));
            Request req = new Request.Builder()
                    .url(baseHttpsUrl.newBuilder().addPathSegments("transfer/result").build())
                    .post(body)
                    .build();
            try (Response r = httpClient.newCall(req).execute()) {
                // ignore body
            }
        } catch (Exception e) {
            Log.d(TAG, "[VIPLE-XFER] postResult err: " + e.getMessage());
        }
    }

    private void postDone(String token) {
        try {
            JSONObject obj = new JSONObject();
            obj.put("kind", "done");
            obj.put("token", token);
            postResult(obj);
        } catch (JSONException ignored) {}
    }

    private void postFailed(String token, String err) {
        try {
            JSONObject obj = new JSONObject();
            obj.put("kind", "failed");
            obj.put("token", token);
            obj.put("error", err);
            postResult(obj);
        } catch (JSONException ignored) {}
    }

    private void reportProgress(String token, String dir, long done, long total) {
        try {
            JSONObject obj = new JSONObject();
            obj.put("kind", "progress");
            obj.put("token", token);
            obj.put("direction", dir);
            obj.put("bytes_done", done);
            obj.put("total_bytes", total);
            postResult(obj);
        } catch (JSONException ignored) {}
    }

    private void emit(String msg) {
        if (listener != null) listener.onStatus(msg);
    }

    private static String sanitizeFilename(String raw) {
        if (raw == null || raw.isEmpty() || raw.length() > 255) return null;
        if (raw.equals(".") || raw.equals("..")) return null;
        String forbidden = "/\\:*?\"<>|";
        for (int i = 0; i < raw.length(); ++i) {
            char ch = raw.charAt(i);
            if (ch < 0x20) return null;
            if (forbidden.indexOf(ch) >= 0) return null;
        }
        return raw;
    }
}
