package com.limelight.computers;

import android.content.Context;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteException;

import com.limelight.LimeLog;
import com.limelight.nvstream.http.ComputerDetails;
import com.limelight.nvstream.http.NvHTTP;

import java.net.InetAddress;
import java.net.UnknownHostException;
import java.util.LinkedList;
import java.util.List;

public class LegacyDatabaseReader {
    private static final String COMPUTER_DB_NAME = "computers.db";
    private static final String COMPUTER_TABLE_NAME = "Computers";

    private static final String ADDRESS_PREFIX = "ADDRESS_PREFIX__";

    private static ComputerDetails getComputerFromCursor(Cursor c) {
        ComputerDetails details = new ComputerDetails();

        details.name = c.getString(0);
        details.uuid = c.getString(1);

        // An earlier schema defined addresses as byte blobs. We'll
        // gracefully migrate those to strings so we can store DNS names
        // too. To disambiguate, we'll need to prefix them with a string
        // greater than the allowable IP address length.
        try {
            byte[] blob = c.getBlob(2);
            if (blob != null) {
                details.localAddress = new ComputerDetails.AddressTuple(InetAddress.getByAddress(blob).getHostAddress(), NvHTTP.DEFAULT_HTTP_PORT);
            }
            LimeLog.warning("DB: Legacy local address for " + details.name);
        } catch (UnknownHostException e) {
            // This is probably a hostname/address with the prefix string
            // §REVIEW-1104：欄位為 NULL 時 stringData 為 null，避免 startsWith 發生 NPE
            String stringData = c.getString(2);
            if (stringData != null && stringData.startsWith(ADDRESS_PREFIX)) {
                details.localAddress = new ComputerDetails.AddressTuple(c.getString(2).substring(ADDRESS_PREFIX.length()), NvHTTP.DEFAULT_HTTP_PORT);
            } else if (stringData != null) {
                LimeLog.severe("DB: Corrupted local address for " + details.name);
            }
        }

        try {
            byte[] blob = c.getBlob(3);
            if (blob != null) {
                details.remoteAddress = new ComputerDetails.AddressTuple(InetAddress.getByAddress(blob).getHostAddress(), NvHTTP.DEFAULT_HTTP_PORT);
            }
            LimeLog.warning("DB: Legacy remote address for " + details.name);
        } catch (UnknownHostException e) {
            // This is probably a hostname/address with the prefix string
            // §REVIEW-1104：欄位為 NULL 時 stringData 為 null，避免 startsWith 發生 NPE
            String stringData = c.getString(3);
            if (stringData != null && stringData.startsWith(ADDRESS_PREFIX)) {
                details.remoteAddress = new ComputerDetails.AddressTuple(c.getString(3).substring(ADDRESS_PREFIX.length()), NvHTTP.DEFAULT_HTTP_PORT);
            } else if (stringData != null) {
                LimeLog.severe("DB: Corrupted remote address for " + details.name);
            }
        }

        // On older versions of Moonlight, this is typically where manual addresses got stored,
        // so let's initialize it just to be safe.
        details.manualAddress = details.remoteAddress;

        details.macAddress = c.getString(4);

        // This signifies we don't have dynamic state (like pair state)
        details.state = ComputerDetails.State.UNKNOWN;

        return details;
    }

    private static List<ComputerDetails> getAllComputers(SQLiteDatabase db) {
        try (final Cursor c = db.rawQuery("SELECT * FROM " + COMPUTER_TABLE_NAME, null)) {
            LinkedList<ComputerDetails> computerList = new LinkedList<>();
            while (c.moveToNext()) {
                // §REVIEW-1104：單筆解析失敗只跳過該列，不中斷整表遷移
                try {
                    ComputerDetails details = getComputerFromCursor(c);

                // If a critical field is corrupt or missing, skip the database entry
                if (details.uuid == null) {
                    continue;
                }

                    computerList.add(details);
                } catch (Exception e) {
                    e.printStackTrace();
                }
            }

            return computerList;
        }
    }

    // §REVIEW-1104：提供獨立刪除舊庫方法，延後至寫入新庫成功後才刪除舊庫
    public static void deleteDatabase(Context c) { c.deleteDatabase(COMPUTER_DB_NAME); }

    public static List<ComputerDetails> migrateAllComputers(Context c) {
        try (final SQLiteDatabase computerDb = SQLiteDatabase.openDatabase(
                c.getDatabasePath(COMPUTER_DB_NAME).getPath(),
                null, SQLiteDatabase.OPEN_READONLY)
        ) {
            // Open the existing database
            return getAllComputers(computerDb);
        } catch (Exception e) {
            // §REVIEW-1104：刪舊庫改到新庫寫入成功之後
            return new LinkedList<ComputerDetails>();
        }
    }
}