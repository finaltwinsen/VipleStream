package com.limelight.binding.audio;

import android.content.Context;
import android.content.Intent;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.media.audiofx.AudioEffect;
import android.os.Build;

import com.limelight.LimeLog;
import com.limelight.nvstream.av.audio.AudioRenderer;
import com.limelight.nvstream.jni.MoonBridge;

public class AndroidAudioRenderer implements AudioRenderer {

    private final Context context;
    private final boolean enableAudioFx;

    // §REVIEW-1104：保存音訊配置供 AudioTrack 重建，並透過 volatile 變數追蹤停止狀態與 track 參照
    private volatile AudioTrack track;
    private int channelConfig;
    private int sampleRate;
    private int bufferSize;
    private boolean lowLatency;
    private volatile boolean stopping = false;
    // §PERF-1113：音訊解碼／播放執行緒是 common-c 用 pthread 開的（nice 0），第一次進 playDecodedAudio
    // 時提到 THREAD_PRIORITY_AUDIO（−16，Android 給音訊執行緒的標準級距），CPU 有競爭時不被背景工作擠掉
    private boolean playThreadPriorityRaised = false;

    public AndroidAudioRenderer(Context context, boolean enableAudioFx) {
        this.context = context;
        this.enableAudioFx = enableAudioFx;
    }

    private AudioTrack createAudioTrack(int channelConfig, int sampleRate, int bufferSize, boolean lowLatency) {
        AudioAttributes.Builder attributesBuilder = new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_GAME);
        AudioFormat format = new AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(sampleRate)
                .setChannelMask(channelConfig)
                .build();

        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            // Use FLAG_LOW_LATENCY on L through N
            if (lowLatency) {
                attributesBuilder.setFlags(AudioAttributes.FLAG_LOW_LATENCY);
            }
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            AudioTrack.Builder trackBuilder = new AudioTrack.Builder()
                    .setAudioFormat(format)
                    .setAudioAttributes(attributesBuilder.build())
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .setBufferSizeInBytes(bufferSize);

            // Use PERFORMANCE_MODE_LOW_LATENCY on O and later
            if (lowLatency) {
                trackBuilder.setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY);
            }

            return trackBuilder.build();
        }
        else {
            return new AudioTrack(attributesBuilder.build(),
                    format,
                    bufferSize,
                    AudioTrack.MODE_STREAM,
                    AudioManager.AUDIO_SESSION_ID_GENERATE);
        }
    }

    @Override
    public int setup(MoonBridge.AudioConfiguration audioConfiguration, int sampleRate, int samplesPerFrame) {
        int channelConfig;
        int bytesPerFrame;

        switch (audioConfiguration.channelCount)
        {
            case 2:
                channelConfig = AudioFormat.CHANNEL_OUT_STEREO;
                break;
            case 4:
                channelConfig = AudioFormat.CHANNEL_OUT_QUAD;
                break;
            case 6:
                channelConfig = AudioFormat.CHANNEL_OUT_5POINT1;
                break;
            case 8:
                // AudioFormat.CHANNEL_OUT_7POINT1_SURROUND isn't available until Android 6.0,
                // yet the CHANNEL_OUT_SIDE_LEFT and CHANNEL_OUT_SIDE_RIGHT constants were added
                // in 5.0, so just hardcode the constant so we can work on Lollipop.
                channelConfig = 0x000018fc; // AudioFormat.CHANNEL_OUT_7POINT1_SURROUND
                break;
            default:
                LimeLog.severe("Decoder returned unhandled channel count");
                return -1;
        }

        LimeLog.info("Audio channel config: "+String.format("0x%X", channelConfig));

        bytesPerFrame = audioConfiguration.channelCount * samplesPerFrame * 2;

        // We're not supposed to request less than the minimum
        // buffer size for our buffer, but it appears that we can
        // do this on many devices and it lowers audio latency.
        // We'll try the small buffer size first and if it fails,
        // use the recommended larger buffer size.

        for (int i = 0; i < 4; i++) {
            boolean lowLatency;
            int bufferSize;

            // We will try:
            // 1) Small buffer, low latency mode
            // 2) Large buffer, low latency mode
            // 3) Small buffer, standard mode
            // 4) Large buffer, standard mode

            switch (i) {
                case 0:
                case 1:
                    lowLatency = true;
                    break;
                case 2:
                case 3:
                    lowLatency = false;
                    break;
                default:
                    // Unreachable
                    throw new IllegalStateException();
            }

            switch (i) {
                case 0:
                case 2:
                    bufferSize = bytesPerFrame * 2;
                    break;

                case 1:
                case 3:
                    // Try the larger buffer size
                    bufferSize = Math.max(AudioTrack.getMinBufferSize(sampleRate,
                            channelConfig,
                            AudioFormat.ENCODING_PCM_16BIT),
                            bytesPerFrame * 2);

                    // Round to next frame
                    bufferSize = (((bufferSize + (bytesPerFrame - 1)) / bytesPerFrame) * bytesPerFrame);
                    break;
                default:
                    // Unreachable
                    throw new IllegalStateException();
            }

            // Skip low latency options if hardware sample rate doesn't match the content
            if (AudioTrack.getNativeOutputSampleRate(AudioManager.STREAM_MUSIC) != sampleRate && lowLatency) {
                continue;
            }

            // Skip low latency options when using audio effects, since low latency mode
            // precludes the use of the audio effect pipeline (as of Android 13).
            if (enableAudioFx && lowLatency) {
                continue;
            }

            try {
                track = createAudioTrack(channelConfig, sampleRate, bufferSize, lowLatency);
                track.play();

                // §REVIEW-1104：記錄建立成功之音訊參數以備後續 dead object 時重建使用
                this.channelConfig = channelConfig;
                this.sampleRate = sampleRate;
                this.bufferSize = bufferSize;
                this.lowLatency = lowLatency;
                this.stopping = false;
                this.playThreadPriorityRaised = false;

                // Successfully created working AudioTrack. We're done here.
                LimeLog.info("Audio track configuration: "+bufferSize+" "+lowLatency);
                break;
            } catch (Exception e) {
                // Try to release the AudioTrack if we got far enough
                e.printStackTrace();
                try {
                    if (track != null) {
                        track.release();
                        track = null;
                    }
                } catch (Exception ignored) {}
            }
        }

        if (track == null) {
            // Couldn't create any audio track for playback
            return -2;
        }

        return 0;
    }

    @Override
    public void playDecodedAudio(short[] audioData) {
        // §REVIEW-1104：若已進入停止流程則直接丟棄音訊資料，避免阻塞解碼執行緒
        if (stopping) {
            return;
        }
        if (!playThreadPriorityRaised) {
            playThreadPriorityRaised = true;
            try {
                android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_AUDIO);
                LimeLog.info("[VIPLE-THREAD-PRIO] audio play thread tid=" + android.os.Process.myTid() + " -> THREAD_PRIORITY_AUDIO");
            } catch (Throwable t) {
                LimeLog.warning("[VIPLE-THREAD-PRIO] audio play thread priority not changed: " + t);
            }
        }
        // Only queue up to 40 ms of pending audio data in addition to what AudioTrack is buffering for us.
        if (MoonBridge.getPendingAudioDuration() < 40) {
            // This will block until the write is completed. That can cause a backlog
            // of pending audio data, so we do the above check to be able to bound
            // latency at 40 ms in that situation.
            // §REVIEW-1104：檢查 write() 回傳值處理短寫，遇 dead object 等錯誤則重建 track 再寫
            int offset = 0;
            int retryCount = 0;
            while (offset < audioData.length && !stopping) {
                AudioTrack currentTrack = track;
                if (currentTrack == null) {
                    break;
                }
                int ret;
                try {
                    ret = currentTrack.write(audioData, offset, audioData.length - offset);
                } catch (Exception e) {
                    LimeLog.warning("AudioTrack write exception: " + e.getMessage());
                    break;
                }
                if (ret > 0) {
                    offset += ret;
                } else if (!stopping && (ret == AudioTrack.ERROR_DEAD_OBJECT || ret == AudioTrack.ERROR_INVALID_OPERATION) && retryCount == 0) {
                    retryCount++;
                    LimeLog.warning("AudioTrack write error " + ret + ", recreating track");
                    if (!recreateTrack()) {
                        break;
                    }
                } else {
                    LimeLog.warning("AudioTrack write failed with code: " + ret);
                    break;
                }
            }
        }
        else {
            LimeLog.info("Too much pending audio data: " + MoonBridge.getPendingAudioDuration() +" ms");
        }
    }

    @Override
    public void start() {
        if (enableAudioFx) {
            // Open an audio effect control session to allow equalizers to apply audio effects
            Intent i = new Intent(AudioEffect.ACTION_OPEN_AUDIO_EFFECT_CONTROL_SESSION);
            i.putExtra(AudioEffect.EXTRA_AUDIO_SESSION, track.getAudioSessionId());
            i.putExtra(AudioEffect.EXTRA_PACKAGE_NAME, context.getPackageName());
            i.putExtra(AudioEffect.EXTRA_CONTENT_TYPE, AudioEffect.CONTENT_TYPE_GAME);
            context.sendBroadcast(i);
        }
    }

    @Override
    public void stop() {
        // §REVIEW-1104：停流時先設旗標並 pause 打斷阻塞中的 write，避免 join 時卡住
        stopping = true;
        AudioTrack t = track;
        if (t != null) {
            try {
                t.pause();
            } catch (Exception ignored) {}
        }
        if (enableAudioFx) {
            // Close our audio effect control session when we're stopping
            Intent i = new Intent(AudioEffect.ACTION_CLOSE_AUDIO_EFFECT_CONTROL_SESSION);
            i.putExtra(AudioEffect.EXTRA_AUDIO_SESSION, track.getAudioSessionId());
            i.putExtra(AudioEffect.EXTRA_PACKAGE_NAME, context.getPackageName());
            context.sendBroadcast(i);
        }
    }

    @Override
    public synchronized void cleanup() {
        // Immediately drop all pending data
        // §REVIEW-1104：停流時已呼叫 pause，此處確保 flush 與 release 安全執行並防範空指標
        stopping = true;
        if (track != null) {
            try {
                track.pause();
            } catch (Exception ignored) {}
            try {
                track.flush();
            } catch (Exception ignored) {}
            try {
                track.release();
            } catch (Exception ignored) {}
            track = null;
        }


    }

    // §REVIEW-1104：當 AudioTrack 遭遇 dead object 或 invalid operation 時重新建立實例
    private synchronized boolean recreateTrack() {
        if (stopping) {
            return false;
        }
        try {
            if (track != null) {
                try {
                    track.release();
                } catch (Exception ignored) {}
                track = null;
            }
            AudioTrack newTrack = createAudioTrack(channelConfig, sampleRate, bufferSize, lowLatency);
            newTrack.play();
            track = newTrack;
            if (enableAudioFx) {
                start();
            }
            return true;
        } catch (Exception e) {
            LimeLog.warning("Failed to recreate AudioTrack: " + e.getMessage());
            track = null;
            return false;
        }
    }
}
