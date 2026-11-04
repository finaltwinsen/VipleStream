package com.limelight.computers;

import android.content.Context;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteException;

import com.limelight.nvstream.http.ComputerDetails;
import com.limelight.nvstream.http.NvHTTP;

import java.io.ByteArrayInputStream;
import java.security.cert.CertificateException;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.util.LinkedList;
import java.util.List;

public class LegacyDatabaseReader2 {
    private static final String COMPUTER_DB_NAME = "computers2.db";
    private static final String COMPUTER_TABLE_NAME = "Computers";

    private static ComputerDetails getComputerFromCursor(Cursor c) {
        ComputerDetails details = new ComputerDetails();

        details.uuid = c.getString(0);
        details.name = c.getString(1);
        // §REVIEW-1104：檢查 getString 是否為 null，避免 AddressTuple 拋出 IllegalArgumentException
        String localAddr = c.getString(2);
        if (localAddr != null) {
            details.localAddress = new ComputerDetails.AddressTuple(localAddr, NvHTTP.DEFAULT_HTTP_PORT);
        }
        String remoteAddr = c.getString(3);
        if (remoteAddr != null) {
            details.remoteAddress = new ComputerDetails.AddressTuple(remoteAddr, NvHTTP.DEFAULT_HTTP_PORT);
        }
        String manualAddr = c.getString(4);
        if (manualAddr != null) {
            details.manualAddress = new ComputerDetails.AddressTuple(manualAddr, NvHTTP.DEFAULT_HTTP_PORT);
        }
        details.macAddress = c.getString(5);

        // This column wasn't always present in the old schema
        if (c.getColumnCount() >= 7) {
            try {
                byte[] derCertData = c.getBlob(6);

                if (derCertData != null) {
                    details.serverCert = (X509Certificate) CertificateFactory.getInstance("X.509")
                            .generateCertificate(new ByteArrayInputStream(derCertData));
                }
            } catch (CertificateException e) {
                e.printStackTrace();
            }
        }

        // This signifies we don't have dynamic state (like pair state)
        details.state = ComputerDetails.State.UNKNOWN;

        return details;
    }

    public static List<ComputerDetails> getAllComputers(SQLiteDatabase computerDb) {
        try (final Cursor c = computerDb.rawQuery("SELECT * FROM "+COMPUTER_TABLE_NAME, null)) {
            LinkedList<ComputerDetails> computerList = new LinkedList<>();
            while (c.moveToNext()) {
                // §REVIEW-1104：單列解析失敗只跳過該列，不中斷整表遷移
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
    public static void deleteDatabase(Context c) {
        c.deleteDatabase(COMPUTER_DB_NAME);
    }

    public static List<ComputerDetails> migrateAllComputers(Context c) {
        try (final SQLiteDatabase computerDb = SQLiteDatabase.openDatabase(
                c.getDatabasePath(COMPUTER_DB_NAME).getPath(),
                null, SQLiteDatabase.OPEN_READONLY)
        ) {
            // Open the existing database
            return getAllComputers(computerDb);
        } catch (Exception e) {
            return new LinkedList<ComputerDetails>();
        } finally {
            // Close and delete the old DB
            // §REVIEW-1104：舊庫刪除改到新庫寫入成功之後
        }
    }
}