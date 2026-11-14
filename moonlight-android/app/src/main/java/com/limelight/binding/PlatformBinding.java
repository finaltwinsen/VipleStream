package com.limelight.binding;

import android.content.Context;

import com.limelight.binding.audio.AndroidAudioRenderer;
import com.limelight.binding.crypto.AndroidCryptoProvider;
import com.limelight.nvstream.av.audio.AudioRenderer;
import com.limelight.nvstream.http.LimelightCryptoProvider;

public class PlatformBinding {
    // 整個行程共用一個 provider。每個 NvHTTP 建構時都會向它要憑證、私鑰與 PEM，
    // 以前每次 new 一個，等於主機清單每 1.5 秒每個位址重讀 client.crt／client.key
    // 並用 BouncyCastle 重新解析一次；共用之後只有第一次會讀檔。
    // AndroidCryptoProvider 只從 Context 取 filesDir 的路徑、不留 Context（不會抓住
    // Activity），三個 getter 都在它自己的 static 鎖裡，跨執行緒共用沒有問題。
    private static volatile LimelightCryptoProvider cryptoProvider;

    public static LimelightCryptoProvider getCryptoProvider(Context c) {
        LimelightCryptoProvider provider = cryptoProvider;
        if (provider == null) {
            synchronized (PlatformBinding.class) {
                provider = cryptoProvider;
                if (provider == null) {
                    provider = new AndroidCryptoProvider(c);
                    cryptoProvider = provider;
                }
            }
        }
        return provider;
    }
}
