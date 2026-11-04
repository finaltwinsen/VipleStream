package com.limelight.binding.crypto;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.io.StringWriter;
import java.math.BigInteger;
import java.security.KeyFactory;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.NoSuchAlgorithmException;
import java.security.PrivateKey;
import java.security.Provider;
import java.security.SecureRandom;
import java.security.cert.CertificateException;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.security.spec.InvalidKeySpecException;
import java.security.spec.PKCS8EncodedKeySpec;
import java.nio.charset.StandardCharsets;
import java.util.Calendar;
import java.util.Date;
import java.util.Locale;

import org.bouncycastle.asn1.x500.X500Name;
import org.bouncycastle.asn1.x500.X500NameBuilder;
import org.bouncycastle.asn1.x500.style.BCStyle;
import org.bouncycastle.asn1.x509.SubjectPublicKeyInfo;
import org.bouncycastle.cert.X509v3CertificateBuilder;
import org.bouncycastle.cert.jcajce.JcaX509CertificateConverter;
import org.bouncycastle.jce.provider.BouncyCastleProvider;
import org.bouncycastle.openssl.jcajce.JcaPEMWriter;
import org.bouncycastle.operator.ContentSigner;
import org.bouncycastle.operator.jcajce.JcaContentSignerBuilder;

import android.annotation.SuppressLint;
import android.content.Context;
import android.util.Base64;

import com.limelight.LimeLog;
import com.limelight.nvstream.http.LimelightCryptoProvider;

public class AndroidCryptoProvider implements LimelightCryptoProvider {

    private final File certFile;
    private final File keyFile;

    private X509Certificate cert;
    private PrivateKey key;
    private byte[] pemCertBytes;

    private static final Object globalCryptoLock = new Object();

    private static final Provider bcProvider = new BouncyCastleProvider();

    public AndroidCryptoProvider(Context c) {
        String dataPath = c.getFilesDir().getAbsolutePath();

        certFile = new File(dataPath + File.separator + "client.crt");
        keyFile = new File(dataPath + File.separator + "client.key");
    }

    private byte[] loadFileToBytes(File f) {
        if (!f.exists()) {
            return null;
        }

        try (final FileInputStream fin = new FileInputStream(f)) {
            // §REVIEW-1104：以迴圈讀滿檔案避免短讀誤判為讀取失敗，防止有效憑證被重新覆寫
            byte[] fileData = new byte[(int) f.length()];
            int offset = 0;
            while (offset < fileData.length) {
                int bytesRead = fin.read(fileData, offset, fileData.length - offset);
                if (bytesRead == -1) {
                    break;
                }
                offset += bytesRead;
            }
            if (offset != fileData.length) {
                // Failed to read
                fileData = null;
            }
            return fileData;
        } catch (IOException e) {
            return null;
        }
    }

    private boolean loadCertKeyPair() {
        byte[] certBytes = loadFileToBytes(certFile);
        byte[] keyBytes = loadFileToBytes(keyFile);

        // If either file was missing, we definitely can't succeed
        if (certBytes == null || keyBytes == null) {
            LimeLog.info("Missing cert or key; need to generate a new one");
            // §REVIEW-1104：檔案缺失時清空三個欄位還原，避免殘留不一致狀態
            cert = null;
            key = null;
            pemCertBytes = null;
            return false;
        }

        try {
            CertificateFactory certFactory = CertificateFactory.getInstance("X.509", bcProvider);
            X509Certificate loadedCert = (X509Certificate) certFactory.generateCertificate(new ByteArrayInputStream(certBytes));
            // §REVIEW-1104：暫存解析結果，待公私鑰皆成功解析再賦值欄位
            KeyFactory keyFactory = KeyFactory.getInstance("RSA", bcProvider);
            PrivateKey loadedKey = keyFactory.generatePrivate(new PKCS8EncodedKeySpec(keyBytes));
            cert = loadedCert;
            key = loadedKey;
            pemCertBytes = certBytes;

        } catch (CertificateException e) {
            // May happen if the cert is corrupt
            LimeLog.warning("Corrupted certificate");
            // §REVIEW-1104：憑證損壞時清空欄位還原，避免狀態不一致
            cert = null;
            key = null;
            pemCertBytes = null;
            return false;
        } catch (NoSuchAlgorithmException e) {
            throw new RuntimeException(e);
        } catch (InvalidKeySpecException e) {
            // May happen if the key is corrupt
            LimeLog.warning("Corrupted key");
            // §REVIEW-1104：私鑰損壞時清空欄位還原，避免公私鑰不配對
            cert = null;
            key = null;
            pemCertBytes = null;
            return false;
        }

        return true;
    }

    @SuppressLint("TrulyRandom")
    private boolean generateCertKeyPair() {
        byte[] snBytes = new byte[8];
        new SecureRandom().nextBytes(snBytes);

        KeyPair keyPair;
        try {
            KeyPairGenerator keyPairGenerator = KeyPairGenerator.getInstance("RSA", bcProvider);
            keyPairGenerator.initialize(2048);
            keyPair = keyPairGenerator.generateKeyPair();
        } catch (NoSuchAlgorithmException e) {
            throw new RuntimeException(e);
        }

        Date now = new Date();

        // Expires in 20 years
        Calendar calendar = Calendar.getInstance();
        calendar.setTime(now);
        calendar.add(Calendar.YEAR, 20);
        Date expirationDate = calendar.getTime();

        BigInteger serial = new BigInteger(snBytes).abs();

        X500NameBuilder nameBuilder = new X500NameBuilder(BCStyle.INSTANCE);
        nameBuilder.addRDN(BCStyle.CN, "NVIDIA GameStream Client");
        X500Name name = nameBuilder.build();

        X509v3CertificateBuilder certBuilder = new X509v3CertificateBuilder(name, serial, now, expirationDate, Locale.ENGLISH, name,
            SubjectPublicKeyInfo.getInstance(keyPair.getPublic().getEncoded()));

        try {
            ContentSigner sigGen = new JcaContentSignerBuilder("SHA256withRSA").setProvider(bcProvider).build(keyPair.getPrivate());
            cert = new JcaX509CertificateConverter().setProvider(bcProvider).getCertificate(certBuilder.build(sigGen));
            key = keyPair.getPrivate();
        } catch (Exception e) {
            throw new RuntimeException(e);
        }

        LimeLog.info("Generated a new key pair");

        // Save the resulting pair
        // §REVIEW-1104：檢查存檔結果，若存檔失敗則回傳 false
        if (!saveCertKeyPair()) {
            return false;
        }


        return true;
    }

    private boolean saveCertKeyPair() {
        File certTempFile = new File(certFile.getAbsolutePath() + ".tmp");
        File keyTempFile = new File(keyFile.getAbsolutePath() + ".tmp");

        // §REVIEW-1104：先寫入暫存檔並 fsync 後再 rename 覆蓋正式檔，存檔當下填入 pemCertBytes，失敗時清空欄位還原
        try {


            // Write the certificate in OpenSSL PEM format (important for the server)
            StringWriter strWriter = new StringWriter();
            try (final JcaPEMWriter pemWriter = new JcaPEMWriter(strWriter)) {
                pemWriter.writeObject(cert);
            }

            // Line endings MUST be UNIX for the PC to accept the cert properly
            String pemStr = strWriter.getBuffer().toString();
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < pemStr.length(); i++) {
                char c = pemStr.charAt(i);
                if (c != '\r') {
                    sb.append(c);
                }
            }
            byte[] certBytes = sb.toString().getBytes(StandardCharsets.UTF_8);

            try (final FileOutputStream certOut = new FileOutputStream(certTempFile);
                 final FileOutputStream keyOut = new FileOutputStream(keyTempFile)) {
                certOut.write(certBytes);
                certOut.flush();
                certOut.getFD().sync();

    
    
                keyOut.write(key.getEncoded());
                keyOut.flush();
                keyOut.getFD().sync();
            }

            if (!certTempFile.renameTo(certFile)) {
                certFile.delete();
                if (!certTempFile.renameTo(certFile)) {
                    throw new IOException("Failed to rename temp certificate file");
                }
            }
            if (!keyTempFile.renameTo(keyFile)) {
                keyFile.delete();
                if (!keyTempFile.renameTo(keyFile)) {
                    throw new IOException("Failed to rename temp key file");
                }
            }

            pemCertBytes = certBytes;
            // §REVIEW-1104：確認記憶體中已產生有效的 PEM 與私鑰，保持金鑰狀態完整
            if (pemCertBytes != null) {
                if (key != null) {
                    // Verified certificate and key in memory





                }
            }




            LimeLog.info("Saved generated key pair to disk");
            return true;
        } catch (IOException e) {
            // This isn't good because it means we'll have
            // to re-pair next time
            e.printStackTrace();
            certTempFile.delete();
            keyTempFile.delete();
            cert = null;
            key = null;
            pemCertBytes = null;
            return false;
        }
    }

    public X509Certificate getClientCertificate() {
        // Use a lock here to ensure only one guy will be generating or loading
        // the certificate and key at a time
        synchronized (globalCryptoLock) {
            // Return a loaded cert if we have one
            if (cert != null) {
                return cert;
            }

            // No loaded cert yet, let's see if we have one on disk
            if (loadCertKeyPair()) {
                // Got one
                return cert;
            }

            // Try to generate a new key pair
            if (!generateCertKeyPair()) {
                // Failed
                return null;
            }

            // Load the generated pair
            // §REVIEW-1104：檢查重讀結果，若重讀失敗回傳 null，避免返回不完整狀態
            if (!loadCertKeyPair()) {
                return null;
            }
            return cert;
        }
    }

    public PrivateKey getClientPrivateKey() {
        // Use a lock here to ensure only one guy will be generating or loading
        // the certificate and key at a time
        synchronized (globalCryptoLock) {
            // Return a loaded key if we have one
            if (key != null) {
                return key;
            }

            // No loaded key yet, let's see if we have one on disk
            if (loadCertKeyPair()) {
                // Got one
                return key;
            }

            // Try to generate a new key pair
            if (!generateCertKeyPair()) {
                // Failed
                return null;
            }

            // Load the generated pair
            // §REVIEW-1104：檢查重讀結果，若重讀失敗回傳 null，避免返回不完整狀態
            if (!loadCertKeyPair()) {
                return null;
            }
            return key;
        }
    }

    public byte[] getPemEncodedClientCertificate() {
        synchronized (globalCryptoLock) {
            // Call our helper function to do the cert loading/generation for us
            getClientCertificate();

            // Return a cached value if we have it
            return pemCertBytes;
        }
    }

    @Override
    public String encodeBase64String(byte[] data) {
        return Base64.encodeToString(data, Base64.NO_WRAP);
    }
}
