package com.limelight.computers;

import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.util.Locale;
import java.util.Random;

import com.limelight.LimeLog;

import android.content.Context;

public class IdentityManager {
    private static final String UNIQUE_ID_FILE_NAME = "uniqueid";
    private static final int UID_SIZE_IN_BYTES = 8;

    private String uniqueId;

    public IdentityManager(Context c) {
        uniqueId = loadUniqueId(c);
        if (uniqueId == null) {
            uniqueId = generateNewUniqueId(c);
        }

        // §REVIEW-1127 android #5：logcat 會經 LogDrop 上傳，完整 uniqueid 能跟 host 端的 client
        // 識別碼對起來；只印前 4 碼供辨識
        LimeLog.info("UID is now: " + (uniqueId.length() > 4 ? uniqueId.substring(0, 4) + "..." : uniqueId));
    }

    public String getUniqueId() {
        return uniqueId;
    }

    private static String loadUniqueId(Context c) {
        // 2 Hex digits per byte
        char[] uid = new char[UID_SIZE_IN_BYTES * 2];
        LimeLog.info("Reading UID from disk");
        try (final InputStreamReader reader =
                     new InputStreamReader(c.openFileInput(UNIQUE_ID_FILE_NAME))
        ) {
            if (reader.read(uid) != UID_SIZE_IN_BYTES * 2) {
                LimeLog.severe("UID file data is truncated");
                return null;
            }
            return new String(uid);
        } catch (FileNotFoundException e) {
            LimeLog.info("No UID file found");
            return null;
        } catch (IOException e) {
            LimeLog.severe("Error while reading UID file");
            e.printStackTrace();
            return null;
        }
    }

    private static String generateNewUniqueId(Context c) {
        // Generate a new UID hex string
        LimeLog.info("Generating new UID");
        String uidStr = String.format((Locale)null, "%016x", new Random().nextLong());

        try (final OutputStreamWriter writer =
                     new OutputStreamWriter(c.openFileOutput(UNIQUE_ID_FILE_NAME, 0))
        ) {
            writer.write(uidStr);
            LimeLog.info("UID written to disk");
        } catch (IOException e) {
            LimeLog.severe("Error while writing UID file");
            e.printStackTrace();
        }

        // We can return a UID even if I/O fails
        return uidStr;
    }
}
