package com.limelight.utils;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.Looper;
import android.os.SystemClock;
import android.preference.PreferenceManager;
import android.util.Base64;
import android.util.Log;

import com.limelight.BuildConfig;
import com.limelight.computers.ComputerDatabaseManager;
import com.limelight.nvstream.http.ComputerDetails;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.TimeZone;
import java.util.concurrent.TimeUnit;
import java.util.zip.GZIPOutputStream;

import okhttp3.MediaType;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.RequestBody;
import okhttp3.Response;
import okhttp3.ResponseBody;

/**
 * §LOGDROP：測試者不會用 adb。ENDPOINT 空著代表這次建置還沒部署收件端，功能整個關掉。
 */
public final class LogDrop {
    public static final String ENDPOINT = "https://script.google.com/macros/s/AKfycbzaTG_jii7H8485RNs9YRigKwT29F4RnA8wsLFQDGRPdiRVkt2dVMHiea56GNcJehu75w/exec";

    private static final String TAG = "VipleLogDrop";
    private static final String LOG = "[VIPLE-LOGDROP] ";
    private static final String PREF_CONSENT = "logdrop_consent";
    private static final String PREF_INSTALL_ID = "logdrop_install_id";
    private static final String PREF_LAST_CODE = "logdrop_last_code";
    private static final String PREF_LAST_AT = "logdrop_last_success_ms";
    private static final String PREF_DAY = "logdrop_day";
    private static final String PREF_DAY_COUNT = "logdrop_day_count";
    private static final String PREF_TRIGGER_PREFIX = "logdrop_trigger_ms_";
    private static final String ALPHABET = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
    private static final int INSTALL_ID_LENGTH = 8;
    private static final long TRIGGER_GAP_MS = 10L * 60L * 1000L;
    private static final int DAILY_LIMIT = 5;
    private static final int LOGCAT_LIMIT = 4 * 1024 * 1024;
    private static final long LOGCAT_TIMEOUT_MS = 10_000L;
    private static final int NOTE_LIMIT = 2000;

    private static OkHttpClient client;

    private LogDrop() {
    }

    public static final class Result {
        public final boolean ok;
        public final String code;
        public final String error;

        public Result(boolean ok, String code, String error) {
            this.ok = ok;
            this.code = code == null ? "" : code;
            this.error = error == null ? "" : error;
        }

        public static Result failure(String error) {
            return new Result(false, "", error);
        }
    }

    public static boolean isConfigured() {
        return ENDPOINT.length() > 0;
    }

    public static String consent(Context ctx) {
        String value = prefs(ctx).getString(PREF_CONSENT, "");
        if ("yes".equals(value) || "no".equals(value)) {
            return value;
        }
        return "";
    }

    public static void setConsent(Context ctx, boolean allow) {
        // §LOGDROP：apply() 若在程序被殺掉前沒寫完，下次又會再問一次。
        prefs(ctx).edit().putString(PREF_CONSENT, allow ? "yes" : "no").commit();
    }

    public static boolean isAutoEnabled(Context ctx) {
        return isConfigured() && "yes".equals(consent(ctx));
    }

    public static String installId(Context ctx) {
        synchronized (LogDrop.class) {
            SharedPreferences preferences = prefs(ctx);
            String existing = preferences.getString(PREF_INSTALL_ID, "");
            if (isInstallId(existing)) {
                return existing;
            }
            String created = newInstallId();
            preferences.edit().putString(PREF_INSTALL_ID, created).commit();
            return created;
        }
    }

    public static String lastSuccessCode(Context ctx) {
        String code = prefs(ctx).getString(PREF_LAST_CODE, "");
        return code == null ? "" : code;
    }

    public static long lastSuccessAt(Context ctx) {
        return prefs(ctx).getLong(PREF_LAST_AT, 0L);
    }

    public static Result upload(Context ctx, String source, String report, String note) {
        // §LOGDROP：診斷畫面是手動送出。trigger 用協定裡的 glasses-diag，才不會和自動上傳混成同一種。
        return upload(ctx, source, report, note, "glasses-diag");
    }

    public static Result upload(Context ctx, String source, String report, String note, String trigger) {
        if (Looper.getMainLooper() != null && Looper.getMainLooper().isCurrentThread()) {
            // §LOGDROP：讀 logcat 和上傳都不能卡住畫面。
            throw new IllegalStateException("upload must not run on the main thread");
        }
        if (!isConfigured()) {
            Log.i(TAG, LOG + "upload skipped endpoint empty");
            return failure("disabled");
        }
        try {
            String body = combine(report, readOwnLogcat());
            String redacted = LogRedactor.redact(body, computerNames(ctx));
            JSONObject json = new JSONObject();
            json.put("v", 1);
            json.put("source", source == null ? "" : source);
            json.put("app_version", clip(BuildConfig.VERSION_NAME, 64));
            // §LOGDROP：Build.SERIAL 要特權權限，而且和匿名 ID 對得起來，所以不讀。
            json.put("device", deviceLabel());
            json.put("install_id", installId(ctx));
            json.put("trigger", trigger == null ? "" : trigger);
            json.put("note", clip(note, NOTE_LIMIT));
            json.put("encoding", "gzip-base64");
            json.put("payload", gzipBase64(redacted));
            return post(ctx, json);
        } catch (JSONException error) {
            Log.w(TAG, LOG + "json build failed");
            return failure("bad_response");
        } catch (IOException error) {
            Log.w(TAG, LOG + "network " + error.getClass().getSimpleName());
            return failure("network");
        } catch (IllegalArgumentException error) {
            // §LOGDROP：網址不合法時 OkHttp 在送出前就丟這個，測試者看到的是連不上。
            Log.w(TAG, LOG + "network " + error.getClass().getSimpleName());
            return failure("network");
        }
    }

    public static void autoUploadAsync(Context appCtx, String source, String trigger, String report) {
        if (appCtx == null || !isAutoEnabled(appCtx)) {
            return;
        }
        final Context app = appCtx.getApplicationContext();
        final String sourceText = source == null ? "" : source;
        final String triggerText = trigger == null ? "" : trigger;
        final String reportText = report == null ? "" : report;
        String block = reserve(app, triggerText);
        if (block != null) {
            // §LOGDROP：同一個狀況短時間結束兩次、或當天已經送滿，都不要再打收件端。
            Log.i(TAG, LOG + "auto trigger=" + safeToken(triggerText) + " skipped " + block);
            return;
        }
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    Result result = upload(app, sourceText, reportText, "", triggerText);
                    if (result.ok) {
                        Log.i(TAG, LOG + "auto trigger=" + safeToken(triggerText) + " ok code=" + result.code);
                    } else {
                        Log.w(TAG, LOG + "auto trigger=" + safeToken(triggerText) + " error=" + result.error);
                    }
                } catch (RuntimeException error) {
                    Log.w(TAG, LOG + "auto trigger=" + safeToken(triggerText) + " error=exception", error);
                }
            }
        }, "viple-logdrop").start();
    }

    private static Result post(Context ctx, JSONObject json) throws IOException {
        RequestBody body = RequestBody.create(
                json.toString(),
                MediaType.parse("application/json; charset=utf-8"));
        Request request = new Request.Builder()
                .url(ENDPOINT)
                .post(body)
                .build();
        Response response = client().newCall(request).execute();
        try {
            ResponseBody responseBody = response.body();
            String raw = responseBody == null ? "" : responseBody.string();
            Result result = interpret(response.code(), raw);
            if (result.ok) {
                // §LOGDROP：設定頁的「最近一次」包含手動上傳，不然按了按鈕卻沒有代碼可對。
                noteSuccess(ctx, result.code);
            }
            return result;
        } finally {
            response.close();
        }
    }

    private static Result interpret(int status, String raw) {
        String body = raw == null ? "" : raw.trim();
        if (body.length() == 0) {
            if (status < 200 || status >= 300) {
                Log.w(TAG, LOG + "http_" + status);
                return failure("http_" + status);
            }
            logBad(status, raw);
            return failure("bad_response");
        }
        JSONObject json;
        try {
            json = new JSONObject(body);
        } catch (JSONException error) {
            logBad(status, raw);
            return failure("bad_response");
        }
        Object okValue = json.opt("ok");
        if (!(okValue instanceof Boolean)) {
            return incomplete(status, raw);
        }
        if ((Boolean) okValue) {
            if (!json.has("code") || json.isNull("code")) {
                return incomplete(status, raw);
            }
            String code = clip(json.optString("code", ""), 64);
            if (code.length() == 0) {
                return incomplete(status, raw);
            }
            return new Result(true, code, "");
        }
        if (!json.has("error") || json.isNull("error")) {
            return incomplete(status, raw);
        }
        String error = clip(json.optString("error", ""), 200);
        if (error.length() == 0) {
            return incomplete(status, raw);
        }
        return failure(error);
    }

    private static Result incomplete(int status, String raw) {
        if (status < 200 || status >= 300) {
            Log.w(TAG, LOG + "http_" + status);
            return failure("http_" + status);
        }
        logBad(status, raw);
        return failure("bad_response");
    }

    private static void logBad(int status, String raw) {
        String body = raw == null ? "" : raw;
        if (body.length() > 200) {
            body = body.substring(0, 200);
        }
        body = body.replace('\n', ' ').replace('\r', ' ');
        Log.w(TAG, LOG + "bad_response http=" + status + " body=" + body);
    }

    private static Result failure(String error) {
        return new Result(false, "", error == null ? "" : error);
    }

    private static void noteSuccess(Context ctx, String code) {
        synchronized (LogDrop.class) {
            prefs(ctx).edit()
                    .putString(PREF_LAST_CODE, code)
                    .putLong(PREF_LAST_AT, System.currentTimeMillis())
                    .commit();
        }
    }

    private static String reserve(Context ctx, String trigger) {
        synchronized (LogDrop.class) {
            SharedPreferences preferences = prefs(ctx);
            long now = System.currentTimeMillis();
            String key = PREF_TRIGGER_PREFIX + safeToken(trigger);
            long last = preferences.getLong(key, 0L);
            if (last > 0L && now >= last && now - last < TRIGGER_GAP_MS) {
                return "interval";
            }
            String day = localDay(now);
            int count = day.equals(preferences.getString(PREF_DAY, ""))
                    ? preferences.getInt(PREF_DAY_COUNT, 0)
                    : 0;
            if (count >= DAILY_LIMIT) {
                return "daily";
            }
            // §LOGDROP：網路失敗若把次數退回，背景會一直重送。決定送出時就扣。
            preferences.edit()
                    .putLong(key, now)
                    .putString(PREF_DAY, day)
                    .putInt(PREF_DAY_COUNT, count + 1)
                    .commit();
            return null;
        }
    }

    private static String localDay(long nowMs) {
        SimpleDateFormat format = new SimpleDateFormat("yyyy-MM-dd", Locale.US);
        format.setTimeZone(TimeZone.getDefault());
        return format.format(new Date(nowMs));
    }

    private static String safeToken(String value) {
        if (value == null || value.length() == 0) {
            return "";
        }
        StringBuilder out = new StringBuilder(Math.min(value.length(), 32));
        for (int i = 0; i < value.length() && out.length() < 32; i++) {
            char c = value.charAt(i);
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
                out.append(c);
            }
        }
        return out.toString();
    }

    private static boolean isInstallId(String value) {
        if (value == null || value.length() != INSTALL_ID_LENGTH) {
            return false;
        }
        for (int i = 0; i < value.length(); i++) {
            if (ALPHABET.indexOf(value.charAt(i)) < 0) {
                return false;
            }
        }
        return true;
    }

    private static String newInstallId() {
        SecureRandom random = new SecureRandom();
        char[] out = new char[INSTALL_ID_LENGTH];
        for (int i = 0; i < out.length; i++) {
            out[i] = ALPHABET.charAt(random.nextInt(ALPHABET.length()));
        }
        return new String(out);
    }

    private static List<String> computerNames(Context ctx) {
        ArrayList<String> names = new ArrayList<String>();
        ComputerDatabaseManager database = null;
        try {
            // §LOGDROP：已存主機的名稱會出現在連線 log。讀不到資料庫就只靠 IP／MAC 規則。
            database = new ComputerDatabaseManager(ctx.getApplicationContext());
            List<ComputerDetails> computers = database.getAllComputers();
            for (int i = 0; i < computers.size(); i++) {
                ComputerDetails details = computers.get(i);
                if (details != null && details.name != null) {
                    names.add(details.name);
                }
            }
        } catch (RuntimeException error) {
            Log.w(TAG, LOG + "computer names unavailable");
        } finally {
            if (database != null) {
                try {
                    database.close();
                } catch (RuntimeException error) {
                    Log.w(TAG, LOG + "computer database close failed");
                }
            }
        }
        return names;
    }

    private static String deviceLabel() {
        String manufacturer = Build.MANUFACTURER == null ? "" : Build.MANUFACTURER;
        String model = Build.MODEL == null ? "" : Build.MODEL;
        String release = Build.VERSION.RELEASE == null ? "" : Build.VERSION.RELEASE;
        return clip(manufacturer + " " + model + " / Android " + release, 200);
    }

    private static String combine(String report, String logcat) {
        String head = report == null ? "" : report;
        String tail = logcat == null ? "" : logcat;
        StringBuilder out = new StringBuilder(head.length() + tail.length() + 24);
        out.append(head);
        if (head.length() > 0 && !head.endsWith("\n")) {
            out.append('\n');
        }
        out.append("----- logcat -----\n");
        out.append(tail);
        return out.toString();
    }

    private static String gzipBase64(String text) throws IOException {
        ByteArrayOutputStream raw = new ByteArrayOutputStream();
        GZIPOutputStream gzip = new GZIPOutputStream(raw);
        try {
            gzip.write(text.getBytes(StandardCharsets.UTF_8));
        } finally {
            gzip.close();
        }
        return Base64.encodeToString(raw.toByteArray(), Base64.NO_WRAP);
    }

    private static String readOwnLogcat() {
        java.lang.Process process = null;
        try {
            process = Runtime.getRuntime().exec(new String[] {
                    "logcat", "-d", "-v", "threadtime", "--pid=" + android.os.Process.myPid()
            });
            ByteTail tail = readTail(process.getInputStream(), process, LOGCAT_TIMEOUT_MS);
            String text = new String(tail.toArray(), StandardCharsets.UTF_8);
            if (tail.truncated()) {
                // §LOGDROP：切在 4 MB 邊界可能切到半個字或半行，從下一個換行才是完整的一行。
                int newline = text.indexOf('\n');
                if (newline >= 0 && newline + 1 < text.length()) {
                    text = text.substring(newline + 1);
                }
            }
            return text;
        } catch (Exception error) {
            Log.w(TAG, LOG + "logcat failed " + error.getClass().getSimpleName());
            return "[VIPLE-LOGDROP] logcat unavailable\n";
        } finally {
            if (process != null) {
                process.destroy();
            }
        }
    }

    // §LOGDROP：Android 上行程輸出的 available() 常常是 0，用它判斷會空等到逾時。
    private static ByteTail readTail(final InputStream in, final java.lang.Process process, long timeoutMs) {
        final ByteTail tail = new ByteTail(LOGCAT_LIMIT);
        final InputStream err = process.getErrorStream();
        Thread reader = new Thread(new Runnable() {
            @Override
            public void run() {
                byte[] chunk = new byte[8192];
                try {
                    int n;
                    while ((n = in.read(chunk)) >= 0) {
                        if (n > 0) {
                            tail.add(chunk, n);
                        }
                    }
                } catch (IOException ignored) {
                    // §LOGDROP：逾時關掉行程時，阻塞的 read 會在這裡結束。
                }
            }
        }, "viple-logdrop-logcat");
        reader.setDaemon(true);
        Thread errReader = new Thread(new Runnable() {
            @Override
            public void run() {
                byte[] chunk = new byte[1024];
                try {
                    // §LOGDROP：不把 stderr 讀掉的話，緩衝寫滿時 logcat 會停住。
                    while (err.read(chunk) >= 0) {
                    }
                } catch (IOException ignored) {
                }
            }
        }, "viple-logdrop-logcat-err");
        errReader.setDaemon(true);
        reader.start();
        errReader.start();
        long deadline = SystemClock.elapsedRealtime() + timeoutMs;
        try {
            while (reader.isAlive()) {
                long left = deadline - SystemClock.elapsedRealtime();
                if (left <= 0L) {
                    Log.w(TAG, LOG + "logcat read timed out");
                    process.destroy();
                    break;
                }
                reader.join(Math.min(left, 200L));
            }
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            process.destroy();
        }
        try {
            reader.join(500L);
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
        }
        return tail;
    }

    private static OkHttpClient client() {
        OkHttpClient current = client;
        if (current != null) {
            return current;
        }
        synchronized (LogDrop.class) {
            if (client == null) {
                // §LOGDROP：Apps Script 的 doPost 先回 302。OkHttp 跟隨轉址時會把 POST 改成 GET，才能拿到 JSON。
                // §LOGDROP：預設寫入逾時 10 秒，logcat 壓完仍可能超過，慢速網路會被切掉。
                // §LOGDROP：收件端一收到就計次。連線失敗又自動重送，同一份報告會變成兩筆。
                client = new OkHttpClient.Builder()
                        .connectTimeout(20, TimeUnit.SECONDS)
                        .readTimeout(120, TimeUnit.SECONDS)
                        .writeTimeout(120, TimeUnit.SECONDS)
                        .followRedirects(true)
                        .followSslRedirects(true)
                        .retryOnConnectionFailure(false)
                        .build();
            }
            return client;
        }
    }

    private static SharedPreferences prefs(Context ctx) {
        return PreferenceManager.getDefaultSharedPreferences(ctx);
    }

    private static String clip(String value, int max) {
        if (value == null || value.length() == 0) {
            return "";
        }
        if (value.length() <= max) {
            return value;
        }
        int end = max;
        // §LOGDROP：裁在代理對中間，收件端的 JSON 會壞掉。
        if (end > 0 && Character.isHighSurrogate(value.charAt(end - 1))) {
            end--;
        }
        return value.substring(0, end);
    }

    private static final class ByteTail {
        private final byte[] ring;
        private int size;
        private int write;
        private boolean truncated;

        ByteTail(int limit) {
            ring = new byte[limit];
        }

        synchronized void add(byte[] chunk, int n) {
            if (n <= 0) {
                return;
            }
            if (n >= ring.length) {
                System.arraycopy(chunk, n - ring.length, ring, 0, ring.length);
                write = 0;
                size = ring.length;
                truncated = true;
                return;
            }
            int first = Math.min(n, ring.length - write);
            System.arraycopy(chunk, 0, ring, write, first);
            int rest = n - first;
            if (rest > 0) {
                System.arraycopy(chunk, first, ring, 0, rest);
            }
            write = (write + n) % ring.length;
            if (size + n > ring.length) {
                truncated = true;
                size = ring.length;
            } else {
                size += n;
            }
        }

        synchronized byte[] toArray() {
            byte[] out = new byte[size];
            if (size == 0) {
                return out;
            }
            int start = size < ring.length ? 0 : write;
            int first = Math.min(size, ring.length - start);
            System.arraycopy(ring, start, out, 0, first);
            if (first < size) {
                System.arraycopy(ring, 0, out, first, size - first);
            }
            return out;
        }

        synchronized boolean truncated() {
            return truncated;
        }
    }
}
