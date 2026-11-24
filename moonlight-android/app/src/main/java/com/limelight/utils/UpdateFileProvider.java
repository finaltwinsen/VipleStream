package com.limelight.utils;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;

/**
 * 自訂更新檔案提供者（ContentProvider）
 *
 * 用於在不引入第三方庫（例如 AndroidX）的情況下，向系統套件安裝程式
 * 提供安全的 content:// URI 以安裝下載的 APK 檔案。
 */
public class UpdateFileProvider extends ContentProvider {

    // §VITURE：診斷報告不能跟 APK 放同一個 updates 目錄。URI 只有檔名，所以用檔名前綴決定要去 glasses 還是 updates。
    public static final String GLASSES_CACHE_DIR = "glasses";
    private static final String GLASSES_NAME_PREFIX = "viplestream-glasses-";
    private static final String GLASSES_NAME_SUFFIX = ".txt";

    private static final String[] DEFAULT_PROJECTION = {
            OpenableColumns.DISPLAY_NAME,
            OpenableColumns.SIZE
    };

    /**
     * 為指定的更新檔案產生 content:// URI
     */
    public static Uri getUriForFile(Context context, String authority, File file) {
        return new Uri.Builder()
                .scheme("content")
                .authority(authority)
                .encodedPath(file.getName())
                .build();
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        // 僅允許唯讀模式
        if (!"r".equals(mode)) {
            throw new SecurityException("僅支援唯讀模式");
        }

        Context context = getContext();
        if (context == null) {
            throw new FileNotFoundException("Context 為 null");
        }

        String fileName = uri.getLastPathSegment();
        if (fileName == null || fileName.contains("..") || fileName.contains("/") || fileName.contains("\\")) {
            throw new SecurityException("不合法的檔案路徑");
        }

        File requestedFile = resolveReadable(context, fileName);
        return ParcelFileDescriptor.open(requestedFile, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public String getType(Uri uri) {
        String fileName = uri == null ? null : uri.getLastPathSegment();
        if (fileName != null && isGlassesReportName(fileName)) {
            return "text/plain";
        }
        return "application/vnd.android.package-archive";
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        if (projection == null) {
            projection = DEFAULT_PROJECTION;
        }
        Context context = getContext();
        if (context == null) {
            return null;
        }

        String fileName = uri.getLastPathSegment();
        if (fileName == null || fileName.contains("..") || fileName.contains("/") || fileName.contains("\\")) {
            return null;
        }

        File requestedFile;
        try {
            requestedFile = resolveReadable(context, fileName);
        } catch (FileNotFoundException e) {
            return null;
        }

        MatrixCursor cursor = new MatrixCursor(projection, 1);
        MatrixCursor.RowBuilder row = cursor.newRow();
        for (String col : projection) {
            if (OpenableColumns.DISPLAY_NAME.equals(col)) {
                row.add(requestedFile.getName());
            } else if (OpenableColumns.SIZE.equals(col)) {
                row.add(requestedFile.length());
            } else {
                row.add(null);
            }
        }
        return cursor;
    }

    private static File resolveReadable(Context context, String fileName) throws FileNotFoundException {
        try {
            // 限制僅能存取 cacheDir/updates/ 目錄下的檔案，並進行標準路徑檢查防範路徑遍歷攻擊
            File updatesDir = new File(context.getCacheDir(), "updates").getCanonicalFile();
            File requestedFile = new File(updatesDir, fileName).getCanonicalFile();
            if (!requestedFile.getPath().startsWith(updatesDir.getPath() + File.separator)) {
                throw new SecurityException("拒絕存取：偵測到路徑遍歷攻擊");
            }
            if (requestedFile.exists()) {
                return requestedFile;
            }
            if (!isGlassesReportName(fileName)) {
                throw new FileNotFoundException("找不到更新檔案：" + requestedFile.getAbsolutePath());
            }
            File glassesDir = new File(context.getCacheDir(), GLASSES_CACHE_DIR).getCanonicalFile();
            File glassesFile = new File(glassesDir, fileName).getCanonicalFile();
            if (!glassesFile.getPath().startsWith(glassesDir.getPath() + File.separator)) {
                throw new SecurityException("拒絕存取：偵測到路徑遍歷攻擊");
            }
            if (!glassesFile.exists()) {
                throw new FileNotFoundException("找不到診斷報告：" + glassesFile.getAbsolutePath());
            }
            return glassesFile;
        } catch (FileNotFoundException e) {
            throw e;
        } catch (IOException e) {
            throw new FileNotFoundException("無法解析檔案路徑：" + e.getMessage());
        }
    }

    private static boolean isGlassesReportName(String fileName) {
        return fileName.startsWith(GLASSES_NAME_PREFIX)
                && fileName.endsWith(GLASSES_NAME_SUFFIX)
                && fileName.length() > GLASSES_NAME_PREFIX.length() + GLASSES_NAME_SUFFIX.length();
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException("不支援插入操作");
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("不支援刪除操作");
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("不支援更新操作");
    }
}
