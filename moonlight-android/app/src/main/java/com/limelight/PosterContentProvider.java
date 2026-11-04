package com.limelight;

import android.content.ContentProvider;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.UriMatcher;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import com.limelight.grid.assets.DiskAssetLoader;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.List;

public class PosterContentProvider extends ContentProvider {


    public static final String AUTHORITY = "poster." + BuildConfig.APPLICATION_ID;
    public static final String PNG_MIME_TYPE = "image/png";
    public static final int APP_ID_PATH_INDEX = 2;
    public static final int COMPUTER_UUID_PATH_INDEX = 1;
    private DiskAssetLoader mDiskAssetLoader;

    private static final UriMatcher sUriMatcher;
    private static final String BOXART_PATH = "boxart";
    private static final int BOXART_URI_ID = 1;

    static {
        sUriMatcher = new UriMatcher(UriMatcher.NO_MATCH);
        sUriMatcher.addURI(AUTHORITY, BOXART_PATH, BOXART_URI_ID);
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        int match = sUriMatcher.match(uri);
        if (match == BOXART_URI_ID) {
            return openBoxArtFile(uri, mode);
        }
        return openBoxArtFile(uri, mode);

    }

    public ParcelFileDescriptor openBoxArtFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) {
            throw new UnsupportedOperationException("This provider is only for read mode");
        }

        List<String> segments = uri.getPathSegments();
        if (segments.size() != 3) {
            throw new FileNotFoundException();
        }
        String appId = segments.get(APP_ID_PATH_INDEX);
        String uuid = segments.get(COMPUTER_UUID_PATH_INDEX);
        // §REVIEW-1104：防範路徑遍歷攻擊，拒絕非 boxart 路徑或含斜線、.. 的區段，並用 canonical path 限制在 boxart 目錄內
        if (!BOXART_PATH.equals(segments.get(0))) {
            throw new FileNotFoundException();
        }
        for (String segment : segments) {
            if (segment.contains("..") || segment.contains("/") || segment.contains("\\")) {
                throw new FileNotFoundException();
            }
        }

        int parsedAppId;
        try {
            parsedAppId = Integer.parseInt(appId);
        } catch (NumberFormatException e) {
            throw new FileNotFoundException();
        }

        File file = mDiskAssetLoader.getFile(uuid, parsedAppId);
        try {
            File baseDir = new File(getContext().getCacheDir(), BOXART_PATH).getCanonicalFile();
            File canonicalFile = file.getCanonicalFile();
            if (!canonicalFile.getPath().startsWith(baseDir.getPath() + File.separator)) {
                throw new FileNotFoundException();
            }
        } catch (IOException e) {
            throw new FileNotFoundException();
        }
        if (file.exists()) {
            return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
        }
        throw new FileNotFoundException();
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("This provider is only for read mode");
    }

    @Override
    public String getType(Uri uri) {
        return PNG_MIME_TYPE;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException("This provider is only for read mode");
    }

    @Override
    public boolean onCreate() {
        mDiskAssetLoader = new DiskAssetLoader(getContext());
        return true;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection,
                        String[] selectionArgs, String sortOrder) {
        throw new UnsupportedOperationException("This provider doesn't support query");
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection,
                      String[] selectionArgs) {
        throw new UnsupportedOperationException("This provider is support read only");
    }


    public static Uri createBoxArtUri(String uuid, String appId) {
        return new Uri.Builder()
                .scheme(ContentResolver.SCHEME_CONTENT)
                .authority(AUTHORITY)
                .appendPath(BOXART_PATH)
                .appendPath(uuid)
                .appendPath(appId)
                .build();
    }

}
