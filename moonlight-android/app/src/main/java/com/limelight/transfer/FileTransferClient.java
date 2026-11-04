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
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

import okhttp3.Call;
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
    /**
     * §N.5.bug fix (2026-05-14): Supplier 替代 OkHttpClient field. 原 cached
     * OkHttpClient instance 用整個 FileTransferClient lifetime — Conscrypt/
     * OkHttp 內部 SSL state 累積到第 K 個 request 後 TLS handshake corrupt,
     * 報 TLSV1_ALERT_INTERNAL_ERROR (server-side verify_callback 沒對應 fire).
     * 改成每次 newCall 之前 supplier.get() 拿 fresh wrapped client
     * (NvHTTP.getLongConnectClient now returns fresh per call) — emulate
     * NvHTTP short-lived call pattern (openHttpConnection 每次 fresh wrap)，
     * 該 pattern 一直工作正常.
     */
    private final java.util.function.Supplier<OkHttpClient> clientSupplier;
    private final HttpUrl baseHttpsUrl;
    private final Listener listener;
    private final ScheduledExecutorService scheduler =
            Executors.newSingleThreadScheduledExecutor(r -> {
                Thread t = new Thread(r, "VipleXfer-Poll");
                t.setDaemon(true);
                return t;
            });
    /**
     * §REVIEW-1103：傳輸本體改到獨立執行緒。原本在 poll 執行緒上同步跑完整個傳輸、傳輸期間也不 poll，
     * Web UI 按取消排進來的 cancel 命令要等傳完才收得到（取消無效）。現在 poll 照常跑，收到 cancel 就中止。
     */
    private final ExecutorService transferExecutor =
            Executors.newSingleThreadExecutor(r -> {
                Thread t = new Thread(r, "VipleXfer-Transfer");
                t.setDaemon(true);
                return t;
            });
    /**
     * §REVIEW-1103：/transfer/result 一律非同步送。原本在資料迴圈裡同步送進度（每次都是新的 TLS 連線，拖慢傳輸），
     * cancelCurrent 還會在 UI 執行緒上同步送（NetworkOnMainThreadException 被吞掉，server 收不到）。
     * 單一執行緒，同一筆傳輸的 progress／done／failed 依序送出。
     */
    private final ExecutorService resultExecutor =
            Executors.newSingleThreadExecutor(r -> {
                Thread t = new Thread(r, "VipleXfer-Result");
                t.setDaemon(true);
                return t;
            });
    private final AtomicReference<String> activeToken = new AtomicReference<>(null);
    /** §REVIEW-1103：進行中的 HTTP 呼叫，取消時 cancel() 讓阻塞中的讀寫立刻丟例外。 */
    private final AtomicReference<Call> activeCall = new AtomicReference<>(null);
    /** §REVIEW-1103：被取消的 token（client 自己取消或 server 送 cancel）。 */
    private volatile String canceledToken = null;
    /** §REVIEW-1103：進度回報合併，佇列裡最多一筆，只送最新的。 */
    private final AtomicReference<JSONObject> pendingProgress = new AtomicReference<>(null);
    private volatile boolean started = false;

    public FileTransferClient(Context appContext, java.util.function.Supplier<OkHttpClient> clientSupplier,
                              HttpUrl httpsBase, Listener listener) {
        this.appContext = appContext.getApplicationContext();
        this.clientSupplier = clientSupplier;
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
        // §REVIEW-1103：原本只清 token，進行中的傳輸照跑到結束；現在直接中止 HTTP 呼叫
        String token = activeToken.get();
        if (token != null) {
            canceledToken = token;
        }
        Call call = activeCall.get();
        if (call != null) {
            call.cancel();
        }
        transferExecutor.shutdownNow();
        // 已排隊的 done／failed／canceled 讓它送完（shutdown 不丟棄已排入的工作）
        resultExecutor.shutdown();
    }

    /** Game.java 觸發 cancel（點檔案傳輸浮層）。可在任何執行緒呼叫，不做網路 I/O。 */
    public void cancelCurrent() {
        String token = activeToken.get();
        if (token == null) return;
        cancelToken(token);
    }

    private void cancelToken(String token) {
        if (token.equals(canceledToken)) return;  // 已經取消過
        canceledToken = token;
        Call call = activeCall.get();
        if (call != null) {
            call.cancel();
        }
        try {
            JSONObject obj = new JSONObject();
            obj.put("kind", "canceled");
            obj.put("token", token);
            postResult(obj);
        } catch (JSONException ignored) {}
        emit("File transfer cancelled");
        Log.i(TAG, "[VIPLE-XFER] cancel token=" + token);
    }

    private boolean isCanceled(String token) {
        return !started || token.equals(canceledToken) || !token.equals(activeToken.get());
    }

    private void pollOnce() {
        if (!started) return;
        // §REVIEW-1103：傳輸中也照常 poll（才收得到 cancel）；新的傳輸命令在 dispatch 回 busy
        try {
            Request req = new Request.Builder()
                    .url(baseHttpsUrl.newBuilder().addPathSegments("transfer/poll").build())
                    .get()
                    .build();
            try (Response r = clientSupplier.get().newCall(req).execute()) {
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
                startTransfer(token, () -> handleDownload(token, filename, size));
                break;
            case "upload_from_client":
                startTransfer(token, () -> handleUpload(token, filename));
                break;
            case "cancel":
                if (token.equals(activeToken.get())) {
                    cancelToken(token);
                }
                break;
            default:
                Log.w(TAG, "[VIPLE-XFER] unknown cmd type " + type);
        }
    }

    private interface TransferBody {
        void run() throws Exception;
    }

    /**
     * §REVIEW-1103：取得 token、在傳輸執行緒上跑，結束（含任何例外）一律釋放 token。
     * 原本 activeToken.set 之後 MediaStore 丟例外（SecurityException 等）就不會釋放，
     * pollOnce 看到 token 非 null 就不再 poll，整場串流的檔案傳輸停擺。
     */
    private void startTransfer(String token, TransferBody body) {
        if (token.isEmpty()) {
            return;
        }
        if (!activeToken.compareAndSet(null, token)) {
            Log.w(TAG, "[VIPLE-XFER] transfer requested but another transfer in progress");
            postFailed(token, "busy");
            return;
        }
        try {
            transferExecutor.execute(() -> {
                try {
                    body.run();
                } catch (Exception e) {
                    Log.w(TAG, "[VIPLE-XFER] transfer error token=" + token, e);
                    if (!isCanceled(token)) {
                        postFailed(token, "client_error");
                    }
                } finally {
                    activeCall.set(null);
                    activeToken.compareAndSet(token, null);
                }
            });
        } catch (RejectedExecutionException e) {
            // stop() 之後才進來
            activeToken.compareAndSet(token, null);
        }
    }

    // ── Handler: LIST_DIR ────────────────────────────────────────────────
    private void handleListDir(String queryId) throws JSONException {
        JSONArray entries = new JSONArray();
        ContentResolver cr = appContext.getContentResolver();
        Uri collection = downloadsCollection();
        String[] projection = {
                MediaStore.Downloads.DISPLAY_NAME,
                MediaStore.Downloads.SIZE,
                MediaStore.Downloads.DATE_MODIFIED
        };
        String selection = null;
        String[] selArgs = null;
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            // §REVIEW-1103：RELATIVE_PATH 欄位 API 29 才有，舊版改用 DATA 路徑篩 Download 資料夾
            selection = MediaStore.MediaColumns.DATA + " LIKE ?";
            selArgs = new String[]{"%/" + Environment.DIRECTORY_DOWNLOADS + "/%"};
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

    // ── Handler: DOWNLOAD_TO_CLIENT（在傳輸執行緒上跑）────────────────────
    private void handleDownload(String token, String filename, long expectedSize) throws Exception {
        String safeName = sanitizeFilename(filename);
        if (safeName == null) {
            Log.w(TAG, "[VIPLE-XFER] download: bad filename " + filename);
            postFailed(token, "bad_filename");  // §REVIEW-1103：原本不回報，server 端一直等
            return;
        }
        emit("Receiving file: " + safeName + " (0%)");

        ContentValues values = new ContentValues();
        values.put(MediaStore.Downloads.DISPLAY_NAME, safeName);
        values.put(MediaStore.Downloads.MIME_TYPE, "application/octet-stream");
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            values.put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS);
            values.put(MediaStore.Downloads.IS_PENDING, 1);
        } else {
            // §REVIEW-1103：API 29 以前沒有 RELATIVE_PATH，用 DATA 指定 Download 資料夾裡的實際路徑
            @SuppressWarnings("deprecation")
            java.io.File dir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
            //noinspection ResultOfMethodCallIgnored
            dir.mkdirs();
            values.put(MediaStore.MediaColumns.DATA, new java.io.File(dir, safeName).getAbsolutePath());
        }

        ContentResolver cr = appContext.getContentResolver();
        Uri uri = null;
        boolean ok = false;
        boolean canceled = false;
        long total = 0;
        try {
            uri = cr.insert(downloadsCollection(), values);
            if (uri == null) {
                Log.w(TAG, "[VIPLE-XFER] download: failed to allocate MediaStore entry");
                // §REVIEW-1104：前面已顯示 0% 進度，配置失敗提早返回也要通知失敗，否則畫面停在 0%
                emit("Receive failed: " + safeName);
                postFailed(token, "mediastore_insert_failed");
                return;
            }

            Request req = new Request.Builder()
                    .url(baseHttpsUrl.newBuilder().addPathSegments("transfer/blob")
                            .addQueryParameter("token", token).build())
                    .get()
                    .build();
            // §N.5.bug fix (2026-05-14): clientSupplier.get() returns fresh wrapped
            // OkHttpClient per call (NvHTTP.getLongConnectClient cache removed).
            Call call = clientSupplier.get().newCall(req);
            activeCall.set(call);
            if (isCanceled(token)) {
                call.cancel();
            }
            int lastBucket = -1;
            boolean completed = false;
            try (Response resp = call.execute();
                 OutputStream out = cr.openOutputStream(uri)) {
                if (resp.code() != 200 || resp.body() == null || out == null) {
                    throw new IOException("HTTP " + resp.code());
                }
                try (InputStream in = resp.body().byteStream()) {
                    byte[] buf = new byte[(int) CHUNK_BYTES];
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        if (isCanceled(token)) {
                            canceled = true;
                            break;
                        }
                        out.write(buf, 0, n);
                        total += n;
                        if (expectedSize > 0) {
                            int bucket = (int) (total * 20L / expectedSize);
                            if (bucket != lastBucket && bucket <= 20) {
                                lastBucket = bucket;
                                int pct = bucket * 5;
                                emit("Receiving file: " + safeName + " (" + pct + "%)");
                                reportProgress(token, total, expectedSize);
                            }
                        }
                    }
                }
                if (!canceled) {
                    // §REVIEW-1103：連線中途被切但沒報錯時大小會對不上，不能當成功
                    if (expectedSize > 0 && total != expectedSize) {
                        throw new IOException("size mismatch got=" + total + " expected=" + expectedSize);
                    }
                    completed = true;
                }
            }
            // §REVIEW-1103：放在 try-with-resources 之後，輸出串流 close()（寫回儲存空間）失敗也算失敗
            ok = completed;
        } catch (Exception e) {
            if (isCanceled(token)) {
                canceled = true;
            } else {
                // §N.5.bug diagnostic (2026-05-14) — full stack trace
                Log.w(TAG, "[VIPLE-XFER] download failed", e);
            }
        } finally {
            if (uri != null) {
                try {
                    if (ok) {
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                            ContentValues done = new ContentValues();
                            done.put(MediaStore.Downloads.IS_PENDING, 0);
                            cr.update(uri, done, null, null);
                        }
                    } else {
                        cr.delete(uri, null, null);
                    }
                } catch (Exception e) {
                    Log.w(TAG, "[VIPLE-XFER] download: MediaStore finalize failed: " + e.getMessage());
                }
            }
        }

        if (canceled) {
            // cancelToken 已回報 canceled 並顯示訊息
        } else if (ok) {
            emit("Received: " + safeName + " → Downloads");
            postDone(token);
        } else {
            // §REVIEW-1104：下載失敗也要通知 UI 進入終態，3 秒後才會收起進度浮層
            emit("Receive failed: " + safeName);
            postFailed(token, "io_error");
        }
        Log.i(TAG, "[VIPLE-XFER] download finished token=" + token + " ok=" + ok
                + " canceled=" + canceled + " bytes=" + total);
    }

    // ── Handler: UPLOAD_FROM_CLIENT（在傳輸執行緒上跑）────────────────────
    private void handleUpload(String token, String filename) throws Exception {
        String safeName = sanitizeFilename(filename);
        if (safeName == null) {
            postFailed(token, "bad_filename");
            return;
        }

        // 查 MediaStore 找到對應 Uri
        ContentResolver cr = appContext.getContentResolver();
        Uri found = findDownloadByName(cr, safeName);
        if (found == null) {
            Log.w(TAG, "[VIPLE-XFER] upload: file not found in Downloads: " + safeName);
            postFailed(token, "file_missing");
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
                            if (isCanceled(token)) {
                                throw new IOException("canceled");  // §REVIEW-1103
                            }
                            sink.write(buf, 0, n);
                            written += n;
                            if (fileSize > 0) {
                                int bucket = (int) (written * 20L / fileSize);
                                if (bucket != lastBucket && bucket <= 20) {
                                    lastBucket = bucket;
                                    int pct = bucket * 5;
                                    emit("Sending file: " + fname + " (" + pct + "%)");
                                    // §REVIEW-1103：上傳方向 server 自己數 blob 位元組，不必再送進度
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
        Call call = clientSupplier.get().newCall(req);
        activeCall.set(call);
        if (isCanceled(token)) {
            call.cancel();
        }
        boolean ok = false;
        try (Response resp = call.execute()) {
            ok = resp.isSuccessful();
        } catch (Exception e) {
            if (!isCanceled(token)) {
                Log.w(TAG, "[VIPLE-XFER] upload failed", e);
            }
        }
        boolean canceled = isCanceled(token);
        if (canceled) {
            // cancelToken 已回報 canceled 並顯示訊息
        } else if (ok) {
            emit("Sent: " + safeName);
        } else {
            emit("Send failed: " + safeName);
            postFailed(token, "upload_failed");
        }
        Log.i(TAG, "[VIPLE-XFER] upload finished token=" + token + " ok=" + ok + " canceled=" + canceled);
    }

    private static Uri downloadsCollection() {
        return (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
                ? MediaStore.Downloads.EXTERNAL_CONTENT_URI
                : MediaStore.Files.getContentUri("external");
    }

    private Uri findDownloadByName(ContentResolver cr, String name) {
        Uri collection = downloadsCollection();
        String sel = MediaStore.Downloads.DISPLAY_NAME + "=?";
        String[] args = new String[]{name};
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            // §REVIEW-1103：舊版 Files collection 涵蓋整個外部儲存，限定在 Download 資料夾（跟 listing 一致）
            sel += " AND " + MediaStore.MediaColumns.DATA + " LIKE ?";
            args = new String[]{name, "%/" + Environment.DIRECTORY_DOWNLOADS + "/%"};
        }
        try (Cursor c = cr.query(collection, new String[]{MediaStore.Downloads._ID},
                sel, args, null)) {
            if (c != null && c.moveToFirst()) {
                long id = c.getLong(0);
                return Uri.withAppendedPath(collection, String.valueOf(id));
            }
        }
        return null;
    }

    // ── Utilities ─────────────────────────────────────────────────────────
    /** §REVIEW-1103：非同步送出（見 resultExecutor 註解）。 */
    private void postResult(JSONObject obj) {
        try {
            resultExecutor.execute(() -> postResultSync(obj));
        } catch (RejectedExecutionException e) {
            // stop() 之後
        }
    }

    private void postResultSync(JSONObject obj) {
        try {
            RequestBody body = RequestBody.create(obj.toString(), MediaType.parse("application/json"));
            Request req = new Request.Builder()
                    .url(baseHttpsUrl.newBuilder().addPathSegments("transfer/result").build())
                    .post(body)
                    .build();
            // §N.5.bug fix — fresh client per call (see field comment).
            try (Response r = clientSupplier.get().newCall(req).execute()) {
                // ignore body
            }
        } catch (Exception e) {
            // §N.5.bug diagnostic — full stack trace, same root cause as
            // handleDownload SSL fail.
            Log.w(TAG, "[VIPLE-XFER] postResult err", e);
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

    /**
     * §REVIEW-1103：下載（server→client）方向的進度回報，server 用它更新 Web UI 進度（server 看不到 client 收了多少）。
     * 非同步、合併：佇列裡已經有一筆還沒送就只換成最新值，不會在資料迴圈裡同步建 TLS 連線。
     */
    private void reportProgress(String token, long done, long total) {
        try {
            JSONObject obj = new JSONObject();
            obj.put("kind", "progress");
            obj.put("token", token);
            obj.put("direction", "in");
            obj.put("bytes_done", done);
            obj.put("total_bytes", total);
            if (pendingProgress.getAndSet(obj) == null) {
                try {
                    resultExecutor.execute(() -> {
                        JSONObject p = pendingProgress.getAndSet(null);
                        if (p != null) {
                            postResultSync(p);
                        }
                    });
                } catch (RejectedExecutionException e) {
                    pendingProgress.set(null);
                }
            }
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
