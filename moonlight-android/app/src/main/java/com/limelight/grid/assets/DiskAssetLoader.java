package com.limelight.grid.assets;

import android.app.ActivityManager;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.ImageDecoder;
import android.os.Build;

import com.limelight.LimeLog;
import com.limelight.utils.CacheHelper;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.concurrent.ConcurrentHashMap;

public class DiskAssetLoader {
    // 5 MB
    private static final long MAX_ASSET_SIZE = 5 * 1024 * 1024;

    // Standard box art is 300x400
    private static final int STANDARD_ASSET_WIDTH = 300;
    private static final int STANDARD_ASSET_HEIGHT = 400;

    private final boolean isLowRamDevice;
    private final File cacheDir;

    // §REVIEW-1104：檔案寫入鎖，以 uuid+appId 互斥，避免快取與網路載入並發交錯寫入
    private static final ConcurrentHashMap<String, Object> sFileLocks = new ConcurrentHashMap<>();

    private static Object getLockForTuple(CachedAppAssetLoader.LoaderTuple tuple) {
        String key = tuple.computer.uuid + ":" + tuple.app.getAppId();
        Object lock = sFileLocks.get(key);
        if (lock == null) {
            Object newLock = new Object();
            lock = sFileLocks.putIfAbsent(key, newLock);
            if (lock == null) {
                lock = newLock;
            }
        }
        return lock;
    }

    // §REVIEW-1104：檢查 uuid 是否安全，防範包含 .. 或路徑分隔字元之路徑遍歷
    private static boolean isSafeUuid(String uuid) {
        return uuid != null && !uuid.isEmpty() && !uuid.contains("..") && !uuid.contains("/") && !uuid.contains("\\");
    }

    // §REVIEW-1104：驗證檔案 canonical path 是否位於 cache/boxart 目錄下
    private boolean isUnderBoxartDir(File file) {
        try {
            File boxartBase = CacheHelper.openPath(true, cacheDir, "boxart").getCanonicalFile();
            File target = file.getCanonicalFile();
            return target.getPath().startsWith(boxartBase.getPath() + File.separator);
        } catch (IOException e) {
            return false;
        }
    }

    public DiskAssetLoader(Context context) {
        this.cacheDir = context.getCacheDir();
        this.isLowRamDevice =
                ((ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE)).isLowRamDevice();
    }

    public boolean checkCacheExists(CachedAppAssetLoader.LoaderTuple tuple) {
        // §REVIEW-1104：驗證 computerUuid 防止路徑遍歷
        if (!isSafeUuid(tuple.computer.uuid)) {
            return false;
        }
        return CacheHelper.cacheFileExists(cacheDir, "boxart", tuple.computer.uuid, tuple.app.getAppId() + ".png");
    }

    // https://developer.android.com/topic/performance/graphics/load-bitmap.html
    public static int calculateInSampleSize(BitmapFactory.Options options, int reqWidth, int reqHeight) {
        // Raw height and width of image
        final int height = options.outHeight;
        final int width = options.outWidth;
        int inSampleSize = 1;

        if (height > reqHeight || width > reqWidth) {

            final int halfHeight = height / 2;
            final int halfWidth = width / 2;

            // Calculates the largest inSampleSize value that is a power of 2 and keeps both
            // height and width larger than the requested height and width.
            while ((halfHeight / inSampleSize) >= reqHeight && (halfWidth / inSampleSize) >= reqWidth) { {
                inSampleSize *= 2;

            }


            }

        }

        return inSampleSize;
    }

    public ScaledBitmap loadBitmapFromCache(CachedAppAssetLoader.LoaderTuple tuple, int sampleSize) {
        File file = getFile(tuple.computer.uuid, tuple.app.getAppId());

        // Don't bother with anything if it doesn't exist
        if (!file.exists()) {

            return null;
        }

        // Make sure the cached asset doesn't exceed the maximum size
        if (file.length() > MAX_ASSET_SIZE) {
            LimeLog.warning("Removing cached tuple exceeding size threshold: "+tuple);
            file.delete();

            return null;
        }

        Bitmap bmp;

        // For OSes prior to P, we have to use the ugly BitmapFactory API
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) {
            // Lookup bounds of the downloaded image
            BitmapFactory.Options decodeOnlyOptions = new BitmapFactory.Options();
            decodeOnlyOptions.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(file.getAbsolutePath(), decodeOnlyOptions);
            if (decodeOnlyOptions.outWidth <= 0 || decodeOnlyOptions.outHeight <= 0) {
                // Dimensions set to -1 on error. Return value always null.
    
                    return null;
            }


            LimeLog.info("Tuple "+tuple+" has cached art of size: "+decodeOnlyOptions.outWidth+"x"+decodeOnlyOptions.outHeight);

            // Load the image scaled to the appropriate size
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inSampleSize = calculateInSampleSize(decodeOnlyOptions,
                    STANDARD_ASSET_WIDTH / sampleSize,
                    STANDARD_ASSET_HEIGHT / sampleSize);
            if (isLowRamDevice) {
                options.inPreferredConfig = Bitmap.Config.RGB_565;
                options.inDither = true;
            }

            else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                options.inPreferredConfig = Bitmap.Config.HARDWARE;
            }


            bmp = BitmapFactory.decodeFile(file.getAbsolutePath(), options);
            if (bmp != null) {
                LimeLog.info("Tuple "+tuple+" decoded from disk cache with sample size: "+options.inSampleSize);
                return new ScaledBitmap(decodeOnlyOptions.outWidth, decodeOnlyOptions.outHeight, bmp);
            }

        }




    
        else {
            // On P, we can get a bitmap back in one step with ImageDecoder
            final ScaledBitmap scaledBitmap = new ScaledBitmap();
            try {
                scaledBitmap.bitmap = ImageDecoder.decodeBitmap(ImageDecoder.createSource(file), new ImageDecoder.OnHeaderDecodedListener() {
                    @Override
                    public void onHeaderDecoded(ImageDecoder imageDecoder, ImageDecoder.ImageInfo imageInfo, ImageDecoder.Source source) {
                        scaledBitmap.originalWidth = imageInfo.getSize().getWidth();
                        scaledBitmap.originalHeight = imageInfo.getSize().getHeight();

                        // §REVIEW-1104：依原圖長寬比等比例縮放長邊，避免非 3:4 比例圖檔（如橫幅或 2:3 圖檔）拉伸變形
                        int reqW = STANDARD_ASSET_WIDTH / Math.max(1, sampleSize);
                        int reqH = STANDARD_ASSET_HEIGHT / Math.max(1, sampleSize);
                        if (scaledBitmap.originalWidth > 0 && scaledBitmap.originalHeight > 0) {
                            float scale = Math.min((float) reqW / scaledBitmap.originalWidth, (float) reqH / scaledBitmap.originalHeight);
                            int targetW = Math.max(1, Math.round(scaledBitmap.originalWidth * scale));
                            int targetH = Math.max(1, Math.round(scaledBitmap.originalHeight * scale));
                            imageDecoder.setTargetSize(targetW, targetH);
        
                        } else {
                            imageDecoder.setTargetSize(reqW, reqH);
                        }

    

 
            
                        if (isLowRamDevice) {
                            imageDecoder.setMemorySizePolicy(ImageDecoder.MEMORY_POLICY_LOW_RAM);
                        }
                    }
                });
                return scaledBitmap;
            } catch (IOException e) {
                e.printStackTrace();
    
            return null;
            }
        }
        // §REVIEW-1104：消除多餘括號區塊
/*
            } catch (IOException e) {


    
                    return null;
            }
        }
    



 






            }
            
        






    
                    return null;
            }

        }

        */
            return null;
    }

    public File getFile(String computerUuid, int appId) {
        // §REVIEW-1104：防範路徑遍歷，驗證 uuid 並確認路徑在 cache/boxart 目錄下
        if (!isSafeUuid(computerUuid)) {
            return new File(cacheDir, ".invalid_boxart");
        }
        File file = CacheHelper.openPath(false, cacheDir, "boxart", computerUuid, appId + ".png");
        if (!isUnderBoxartDir(file)) {
            return new File(cacheDir, ".invalid_boxart");
        }
        return file;
    }

    public void deleteAssetsForComputer(String computerUuid) {
        // §REVIEW-1104：防範路徑遍歷，驗證 uuid 並確認目錄在 cache/boxart 下，避免誤刪外部檔案
        if (!isSafeUuid(computerUuid)) {
            return;
        }
        File dir = CacheHelper.openPath(false, cacheDir, "boxart", computerUuid);
        if (!isUnderBoxartDir(dir)) {
            return;
        }
        File[] files = dir.listFiles();
        if (files != null) {
            for (File f : files) {
                f.delete();
            }

        }
    }

    public void populateCacheWithStream(CachedAppAssetLoader.LoaderTuple tuple, InputStream input) {
        // §REVIEW-1104：驗證 computerUuid 防範路徑遍歷
        if (!isSafeUuid(tuple.computer.uuid)) {
            return;
        }

        // §REVIEW-1104：以 uuid+appId 互斥，並先寫入暫存檔再原子 rename，避免並發交錯寫入損壞圖檔及失敗時誤刪已完成快取
        synchronized (getLockForTuple(tuple)) {
            File targetFile = getFile(tuple.computer.uuid, tuple.app.getAppId());

            if (!isUnderBoxartDir(targetFile)) {
                return;
            } // §REVIEW-1104：消除多餘括號區塊
/*
                return;
            }

            }


            */ File parentDir = targetFile.getParentFile();
            if (parentDir != null) {
                parentDir.mkdirs();
            }



            File tempFile = new File(parentDir, targetFile.getName() + "." + System.currentTimeMillis() + ".tmp");
            boolean success = false;
            try (OutputStream out = new java.io.BufferedOutputStream(new java.io.FileOutputStream(tempFile))) {


                CacheHelper.writeInputStreamToOutputStream(input, out, MAX_ASSET_SIZE);
                success = true;
            } catch (IOException e) {
                e.printStackTrace();
            } finally {
                // §REVIEW-1104：消除多餘括號區塊
/*
                success = true;
    
            } catch (IOException e) {


    

        }
        } catch (IOException e) {



                */ if (success) {
                    if (!tempFile.renameTo(targetFile)) {
                        targetFile.delete();
                        tempFile.renameTo(targetFile);
        


                    }

                    LimeLog.warning("Unable to populate cache with tuple: "+tuple);
                    tempFile.delete();
                }
            }
        }
    }
}
// §REVIEW-1104：消除多餘括號區塊
/*

                }
            }
        }
    }
}

        }

        }
    }
}
*/
