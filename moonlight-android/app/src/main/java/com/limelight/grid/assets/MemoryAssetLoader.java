package com.limelight.grid.assets;

import android.util.LruCache;

import com.limelight.LimeLog;

import java.lang.ref.SoftReference;
import java.util.HashMap;

public class MemoryAssetLoader {
    private static final int maxMemory = (int) (Runtime.getRuntime().maxMemory() / 1024);
    private static final LruCache<String, ScaledBitmap> memoryCache = new LruCache<String, ScaledBitmap>(maxMemory / 16) {
        @Override
        protected int sizeOf(String key, ScaledBitmap bitmap) {
            // Sizeof returns kilobytes
            return bitmap.bitmap.getByteCount() / 1024;
        }

        @Override
        protected void entryRemoved(boolean evicted, String key, ScaledBitmap oldValue, ScaledBitmap newValue) {
            super.entryRemoved(evicted, key, oldValue, newValue);

            if (evicted) {
                // Keep a soft reference around to the bitmap as long as we can
                // §REVIEW-1104：evictionCache 與 memoryCache 的 put/evictAll 共用 evictionCache 鎖，避免並發存取 HashMap 造成死迴圈或崩潰
                synchronized (evictionCache) {
                    evictionCache.put(key, new SoftReference<>(oldValue));
                }
            }
        }
    };
    private static final HashMap<String, SoftReference<ScaledBitmap>> evictionCache = new HashMap<>();

    private static String constructKey(CachedAppAssetLoader.LoaderTuple tuple) {
        return tuple.computer.uuid+"-"+tuple.app.getAppId();
    }

    public ScaledBitmap loadBitmapFromCache(CachedAppAssetLoader.LoaderTuple tuple) {
        final String key = constructKey(tuple);

        ScaledBitmap bmp = memoryCache.get(key);
        if (bmp != null) {
            LimeLog.info("LRU cache hit for tuple: "+tuple);
            return bmp;
        }

        synchronized (evictionCache) {
            SoftReference<ScaledBitmap> bmpRef = evictionCache.get(key);
        if (bmpRef != null) {
            bmp = bmpRef.get();
            if (bmp != null) {
                LimeLog.info("Eviction cache hit for tuple: "+tuple);

                // Put this entry back into the LRU cache
                evictionCache.remove(key);
                memoryCache.put(key, bmp);

                return bmp;
            }
            else {
                // The data is gone, so remove the dangling SoftReference now
                evictionCache.remove(key);
            }
        }

        }

        return null;
    }

    public void populateCache(CachedAppAssetLoader.LoaderTuple tuple, ScaledBitmap bitmap) {
        // §REVIEW-1104：以 evictionCache 鎖保護 memoryCache.put，確保鎖順序一致避免死鎖
        synchronized (evictionCache) {
            memoryCache.put(constructKey(tuple), bitmap);
        }
    }

    public void clearCache() {
        // §REVIEW-1104：同步保護 clear 與 evictAll，避免背景載入與清理快取交錯
        synchronized (evictionCache) {
        // We must evict first because that will push all items into the eviction cache

    
            memoryCache.evictAll();
            evictionCache.clear();
        }
    }
}
