package com.limelight.nvstream.http;

import org.bouncycastle.crypto.BlockCipher;
import org.bouncycastle.crypto.engines.AESLightEngine;
import org.bouncycastle.crypto.params.KeyParameter;

import org.xmlpull.v1.XmlPullParserException;

import com.limelight.LimeLog;

import java.security.cert.Certificate;
import java.io.*;
import java.security.*;
import java.security.cert.*;
import java.util.Arrays;
import java.util.Locale;

public class PairingManager {

    private NvHTTP http;
    
    private PrivateKey pk;
    private X509Certificate cert;
    private byte[] pemCertBytes;

    private X509Certificate serverCert;
    
    public enum PairState {
        NOT_PAIRED,
        PAIRED,
        PIN_WRONG,
        FAILED,
        ALREADY_IN_PROGRESS
    }
    
    public PairingManager(NvHTTP http, LimelightCryptoProvider cryptoProvider) {
        this.http = http;
        this.cert = cryptoProvider.getClientCertificate();
        this.pemCertBytes = cryptoProvider.getPemEncodedClientCertificate();
        this.pk = cryptoProvider.getClientPrivateKey();
    }
    
    final private static char[] hexArray = "0123456789ABCDEF".toCharArray();
    private static String bytesToHex(byte[] bytes) {
        char[] hexChars = new char[bytes.length * 2];
        for ( int j = 0; j < bytes.length; j++ ) {
            int v = bytes[j] & 0xFF;
            hexChars[j * 2] = hexArray[v >>> 4];
            hexChars[j * 2 + 1] = hexArray[v & 0x0F];
        }
        return new String(hexChars);
    }
    
    // §REVIEW-1104：檢查十六進位字串是否為偶數長度且僅含合法十六進位字元，不合法時回傳 null 避免拋出 IllegalArgumentException
    private static byte[] hexToBytes(String s) {
        if (s == null) {
            return null;
        }
        int len = s.length();
        if (len % 2 != 0) {
            return null;
        }

        byte[] data = new byte[len / 2];
        for (int i = 0; i < len; i += 2) {
            int d1 = Character.digit(s.charAt(i), 16);
            int d2 = Character.digit(s.charAt(i+1), 16);
            if (d1 == -1 || d2 == -1) {
                return null;
            }
            data[i / 2] = (byte) ((d1 << 4) + d2);

        }
        return data;
    }
    
    private X509Certificate extractPlainCert(String text) throws XmlPullParserException, IOException
    {
        // Plaincert may be null if another client is already trying to pair
        String certText = NvHTTP.getXmlString(text, "plaincert", false);
        if (certText != null) {
            byte[] certBytes = hexToBytes(certText);
            // §REVIEW-1104：plaincert 不是合法 hex 時拋出 IOException，避免未檢查例外導致背景執行緒崩潰
            if (certBytes == null) {
                throw new IOException("Invalid hex in plaincert");
            }

            try {
                CertificateFactory cf = CertificateFactory.getInstance("X.509");
                return (X509Certificate)cf.generateCertificate(new ByteArrayInputStream(certBytes));
            } catch (CertificateException e) {
                e.printStackTrace();
                // §REVIEW-1104：憑證解析失敗改拋 IOException，讓呼叫端正常捕捉而非因 RuntimeException 崩潰
                throw new IOException(e);
            } catch (Exception e) {
                e.printStackTrace();
                throw new RuntimeException(e);
            }
        }
        else {
            return null;
        }
    }
    
    private byte[] generateRandomBytes(int length)
    {
        byte[] rand = new byte[length];
        new SecureRandom().nextBytes(rand);
        return rand;
    }
    
    private static byte[] saltPin(byte[] salt, String pin) throws UnsupportedEncodingException {
        byte[] saltedPin = new byte[salt.length + pin.length()];
        System.arraycopy(salt, 0, saltedPin, 0, salt.length);
        System.arraycopy(pin.getBytes("UTF-8"), 0, saltedPin, salt.length, pin.length());
        return saltedPin;
    }

    private static Signature getSha256SignatureInstanceForKey(Key key) throws NoSuchAlgorithmException {
        switch (key.getAlgorithm()) {
            case "RSA":
                return Signature.getInstance("SHA256withRSA");
            case "EC":
                return Signature.getInstance("SHA256withECDSA");
            default:
                throw new NoSuchAlgorithmException("Unhandled key algorithm: " + key.getAlgorithm());
        }
    }
    
    private static boolean verifySignature(byte[] data, byte[] signature, Certificate cert) {
        try {
            Signature sig = PairingManager.getSha256SignatureInstanceForKey(cert.getPublicKey());
            sig.initVerify(cert.getPublicKey());
            sig.update(data);
            return sig.verify(signature);
        } catch (NoSuchAlgorithmException | SignatureException | InvalidKeyException e) {
            e.printStackTrace();
            throw new RuntimeException(e);
        }
    }
    
    private static byte[] signData(byte[] data, PrivateKey key) {
        try {
            Signature sig = PairingManager.getSha256SignatureInstanceForKey(key);
            sig.initSign(key);
            sig.update(data);
            return sig.sign();
        } catch (NoSuchAlgorithmException | SignatureException | InvalidKeyException e) {
            e.printStackTrace();
            throw new RuntimeException(e);
        }
    }

    private static byte[] performBlockCipher(BlockCipher blockCipher, byte[] input) {
        int blockSize = blockCipher.getBlockSize();
        int blockRoundedSize = (input.length + (blockSize - 1)) & ~(blockSize - 1);

        byte[] blockRoundedInputData = Arrays.copyOf(input, blockRoundedSize);
        byte[] blockRoundedOutputData = new byte[blockRoundedSize];

        for (int offset = 0; offset < blockRoundedSize; offset += blockSize) {
            blockCipher.processBlock(blockRoundedInputData, offset, blockRoundedOutputData, offset);
        }

        return blockRoundedOutputData;
    }
    
    private static byte[] decryptAes(byte[] encryptedData, byte[] aesKey) {
        BlockCipher aesEngine = new AESLightEngine();
        aesEngine.init(false, new KeyParameter(aesKey));
        return performBlockCipher(aesEngine, encryptedData);
    }
    
    private static byte[] encryptAes(byte[] plaintextData, byte[] aesKey) {
        BlockCipher aesEngine = new AESLightEngine();
        aesEngine.init(true, new KeyParameter(aesKey));
        return performBlockCipher(aesEngine, plaintextData);
    }
    
    private static byte[] generateAesKey(PairingHashAlgorithm hashAlgo, byte[] keyData) {
        return Arrays.copyOf(hashAlgo.hashData(keyData), 16);
    }
    
    private static byte[] concatBytes(byte[] a, byte[] b) {
        byte[] c = new byte[a.length + b.length];
        System.arraycopy(a, 0, c, 0, a.length);
        System.arraycopy(b, 0, c, a.length, b.length);
        return c;
    }
    
    public static String generatePinString() {
        SecureRandom r = new SecureRandom();
        return String.format((Locale)null, "%d%d%d%d",
                r.nextInt(10), r.nextInt(10),
                r.nextInt(10), r.nextInt(10));
    }

    public X509Certificate getPairedCert() {
        return serverCert;
    }
    
    public PairState pair(String serverInfo, String pin) throws IOException, XmlPullParserException {
        // §REVIEW-1104：若用戶端憑證或私鑰未成功載入（pemCertBytes 為 null），提前回傳 FAILED 避免 bytesToHex 拋出 NullPointerException
        if (pemCertBytes == null || cert == null || pk == null) {
            return PairState.FAILED;
        }
        PairingHashAlgorithm hashAlgo;

        int serverMajorVersion = http.getServerMajorVersion(serverInfo);
        LimeLog.info("Pairing with server generation: "+serverMajorVersion);
        if (serverMajorVersion >= 7) {
            // Gen 7+ uses SHA-256 hashing
            hashAlgo = new Sha256PairingHash();
        }
        else {
            // Prior to Gen 7, SHA-1 is used
            hashAlgo = new Sha1PairingHash();
        }
        
        // Generate a salt for hashing the PIN
        byte[] salt = generateRandomBytes(16);

        // Combine the salt and pin, then create an AES key from them
        byte[] aesKey = generateAesKey(hashAlgo, saltPin(salt, pin));
        
        // Send the salt and get the server cert. This doesn't have a read timeout
        // because the user must enter the PIN before the server responds
        String getCert = http.executePairingCommand("phrase=getservercert&salt="+
                bytesToHex(salt)+"&clientcert="+bytesToHex(pemCertBytes),
                false);
        if (!NvHTTP.getXmlString(getCert, "paired", true).equals("1")) {
            return PairState.FAILED;
        }

        // Save this cert for retrieval later
        serverCert = extractPlainCert(getCert);
        if (serverCert == null) {
            // Attempting to pair while another device is pairing will cause GFE
            // to give an empty cert in the response.
            http.unpair();
            return PairState.ALREADY_IN_PROGRESS;
        }

        // Require this cert for TLS to this host
        http.setServerCert(serverCert);
        
        // Generate a random challenge and encrypt it with our AES key
        byte[] randomChallenge = generateRandomBytes(16);
        byte[] encryptedChallenge = encryptAes(randomChallenge, aesKey);
        
        // Send the encrypted challenge to the server
        String challengeResp = http.executePairingCommand("clientchallenge="+bytesToHex(encryptedChallenge), true);
        if (!NvHTTP.getXmlString(challengeResp, "paired", true).equals("1")) {
            http.unpair();
            return PairState.FAILED;
        }
        
        // Decode the server's response and subsequent challenge
        byte[] encServerChallengeResponse = hexToBytes(NvHTTP.getXmlString(challengeResp, "challengeresponse", true));
        // §REVIEW-1104：challengeresponse 非合法十六進位時取消配對並回傳 FAILED
        if (encServerChallengeResponse == null) {
            http.unpair();
            return PairState.FAILED;
        }
        byte[] decServerChallengeResponse = decryptAes(encServerChallengeResponse, aesKey);
        // §REVIEW-1104：確認解密後回應長度足夠（≥ hashLength + 16），避免 copyOfRange 拋出 ArrayIndexOutOfBoundsException
        if (decServerChallengeResponse.length < hashAlgo.getHashLength() + 16) {
            http.unpair();
            return PairState.FAILED;
        }
        
        byte[] serverResponse = Arrays.copyOfRange(decServerChallengeResponse, 0, hashAlgo.getHashLength());
        byte[] serverChallenge = Arrays.copyOfRange(decServerChallengeResponse, hashAlgo.getHashLength(), hashAlgo.getHashLength() + 16);
        
        // Using another 16 bytes secret, compute a challenge response hash using the secret, our cert sig, and the challenge
        byte[] clientSecret = generateRandomBytes(16);
        byte[] challengeRespHash = hashAlgo.hashData(concatBytes(concatBytes(serverChallenge, cert.getSignature()), clientSecret));
        byte[] challengeRespEncrypted = encryptAes(challengeRespHash, aesKey);
        String secretResp = http.executePairingCommand("serverchallengeresp="+bytesToHex(challengeRespEncrypted), true);
        if (!NvHTTP.getXmlString(secretResp, "paired", true).equals("1")) {
            http.unpair();
            return PairState.FAILED;
        }
        
        // Get the server's signed secret
        byte[] serverSecretResp = hexToBytes(NvHTTP.getXmlString(secretResp, "pairingsecret", true));
        // §REVIEW-1104：確認 pairingsecret 為合法十六進位且長度 ≥ 16，避免 copyOfRange 拋出 IllegalArgumentException
        if (serverSecretResp == null || serverSecretResp.length < 16) {
            http.unpair();
            return PairState.FAILED;
        }
        byte[] serverSecret = Arrays.copyOfRange(serverSecretResp, 0, 16);
        byte[] serverSignature = Arrays.copyOfRange(serverSecretResp, 16, serverSecretResp.length);

        // Ensure the authenticity of the data
        if (!verifySignature(serverSecret, serverSignature, serverCert)) {
            // Cancel the pairing process
            http.unpair();
            
            // Looks like a MITM
            return PairState.FAILED;
        }
        
        // Ensure the server challenge matched what we expected (aka the PIN was correct)
        byte[] serverChallengeRespHash = hashAlgo.hashData(concatBytes(concatBytes(randomChallenge, serverCert.getSignature()), serverSecret));
        if (!Arrays.equals(serverChallengeRespHash, serverResponse)) {
            // Cancel the pairing process
            http.unpair();
            
            // Probably got the wrong PIN
            return PairState.PIN_WRONG;
        }
        
        // Send the server our signed secret
        byte[] clientPairingSecret = concatBytes(clientSecret, signData(clientSecret, pk));
        String clientSecretResp = http.executePairingCommand("clientpairingsecret="+bytesToHex(clientPairingSecret), true);
        if (!NvHTTP.getXmlString(clientSecretResp, "paired", true).equals("1")) {
            http.unpair();
            return PairState.FAILED;
        }
        
        // Do the initial challenge (seems necessary for us to show as paired)
        String pairChallenge = http.executePairingChallenge();
        if (!NvHTTP.getXmlString(pairChallenge, "paired", true).equals("1")) {
            http.unpair();
            return PairState.FAILED;
        }

        return PairState.PAIRED;
    }
    
    private interface PairingHashAlgorithm {
        int getHashLength();
        byte[] hashData(byte[] data);
    }
    
    private static class Sha1PairingHash implements PairingHashAlgorithm {
        public int getHashLength() {
            return 20;
        }
        
        public byte[] hashData(byte[] data) {
            try {
                MessageDigest md = MessageDigest.getInstance("SHA-1");
                return md.digest(data);
            }
            catch (NoSuchAlgorithmException e) {
                e.printStackTrace();
                throw new RuntimeException(e);
            }
        }
    }
    
    private static class Sha256PairingHash implements PairingHashAlgorithm {
        public int getHashLength() {
            return 32;
        }
        
        public byte[] hashData(byte[] data) {
            try {
                MessageDigest md = MessageDigest.getInstance("SHA-256");
                return md.digest(data);
            }
            catch (NoSuchAlgorithmException e) {
                e.printStackTrace();
                throw new RuntimeException(e);
            }
        }
    }
}
