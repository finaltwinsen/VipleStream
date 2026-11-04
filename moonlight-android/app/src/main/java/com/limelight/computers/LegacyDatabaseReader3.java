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

public class LegacyDatabaseReader3 {
    private static final String COMPUTER_DB_NAME = "computers3.db";
    private static final String COMPUTER_TABLE_NAME = "Computers";

    private static final char ADDRESS_DELIMITER = ';';
    private static final char PORT_DELIMITER = '_';

    private static String readNonEmptyString(String input) {
        if (input.isEmpty()) {
            return null;
        }

        return input;
    }

    private static ComputerDetails.AddressTuple splitAddressToTuple(String input) {
        if (input == null) {
            return null;
        }

        // §REVIEW-1104：由最後一個底線切分埠號以支援含底線主機名，解析失敗則回傳 null
        int lastIndex = input.lastIndexOf(PORT_DELIMITER);
        if (lastIndex == -1) {
            return new ComputerDetails.AddressTuple(input, NvHTTP.DEFAULT_HTTP_PORT);
        }
        else {
            String addr = input.substring(0, lastIndex);
            if (addr.isEmpty()) {
                return null;
            }
            try {
                return new ComputerDetails.AddressTuple(addr, Integer.parseInt(input.substring(lastIndex + 1)));
            } catch (NumberFormatException e) {
                return null;
            }
        }
    }

    private static String splitTupleToAddress(ComputerDetails.AddressTuple tuple) {
        return tuple.address+PORT_DELIMITER+tuple.port;
    }

    private static ComputerDetails getComputerFromCursor(Cursor c) {
        ComputerDetails details = new ComputerDetails();

        details.uuid = c.getString(0);
        details.name = c.getString(1);

        // §REVIEW-1104：檢查欄位是否為 null 與段數是否足夠，避免 NPE 與 ArrayIndexOutOfBoundsException
        String rawAddresses = c.getString(2);
        String[] addresses = rawAddresses != null ? rawAddresses.split(""+ADDRESS_DELIMITER, -1) : new String[0];

        details.localAddress = addresses.length > 0 ? splitAddressToTuple(readNonEmptyString(addresses[0])) : null;
        details.remoteAddress = addresses.length > 1 ? splitAddressToTuple(readNonEmptyString(addresses[1])) : null;
        details.manualAddress = addresses.length > 2 ? splitAddressToTuple(readNonEmptyString(addresses[2])) : null;
        details.ipv6Address = addresses.length > 3 ? splitAddressToTuple(readNonEmptyString(addresses[3])) : null;

        // External port is persisted in the remote address field
        if (details.remoteAddress != null) {
            details.externalPort = details.remoteAddress.port;
        }
        else {
            details.externalPort = NvHTTP.DEFAULT_HTTP_PORT;
        }

        details.macAddress = c.getString(3);

        try {
            byte[] derCertData = c.getBlob(4);

            if (derCertData != null) {
                details.serverCert = (X509Certificate) CertificateFactory.getInstance("X.509")
                        .generateCertificate(new ByteArrayInputStream(derCertData));
            }
        } catch (CertificateException e) {
            e.printStackTrace();
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
