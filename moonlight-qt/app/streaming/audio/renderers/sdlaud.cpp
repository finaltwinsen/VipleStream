#include "sdl.h"

#include <Limelight.h>

SdlAudioRenderer::SdlAudioRenderer()
    : m_AudioDevice(0),
      m_AudioBuffer(nullptr)
{
    // VipleStream: DO NOT assert(!SDL_WasInit(AUDIO)). We intentionally
    // leave SDL_INIT_AUDIO initialized across sessions — see the
    // destructor for why. SDL_InitSubSystem is refcounted so calling
    // it again on an already-inited subsystem is cheap (just increments).
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s",
                     SDL_GetError());
        // §REVIEW-1103：這裡原本 SDL_assert(SDL_WasInit(AUDIO))——在失敗分支裡恆假，debug 建置必中斷；
        // 失敗後 prepareForPlayback 的 SDL_OpenAudioDevice 也會失敗並走無音訊路徑，不需要斷言。
    }
}

bool SdlAudioRenderer::prepareForPlayback(const OPUS_MULTISTREAM_CONFIGURATION* opusConfig)
{
    SDL_AudioSpec want, have;

    SDL_zero(want);
    want.freq = opusConfig->sampleRate;
    want.format = AUDIO_F32SYS;
    want.channels = opusConfig->channelCount;

    // On PulseAudio systems, setting a value too small can cause underruns for other
    // applications sharing this output device. We impose a floor of 480 samples (10 ms)
    // to mitigate this issue. Otherwise, we will buffer up to 3 frames of audio which
    // is 15 ms at regular 5 ms frames and 30 ms at 10 ms frames for slow connections.
    // The buffering helps avoid audio underruns due to network jitter.
    want.samples = SDL_max(480, opusConfig->samplesPerFrame * 3);

    m_FrameSize = opusConfig->samplesPerFrame *
                  opusConfig->channelCount *
                  getAudioBufferSampleSize();

    m_AudioDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (m_AudioDevice == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to open audio device: %s",
                     SDL_GetError());
        return false;
    }

    m_AudioBuffer = SDL_malloc(m_FrameSize);
    if (m_AudioBuffer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to allocate audio buffer");
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Desired audio buffer: %u samples (%u bytes)",
                want.samples,
                want.samples * want.channels * getAudioBufferSampleSize());

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Obtained audio buffer: %u samples (%u bytes)",
                have.samples,
                have.size);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SDL audio driver: %s",
                SDL_GetCurrentAudioDriver());

    // §PERF-1113：佇列深度改用毫秒看。門檻刻意放寬（寧可少丟）：至少 30 ms、至少 4 個 Opus 幀、
    // 至少兩個裝置週期——這是「谷底」的門檻，正常抖動只會讓谷底更低，不會碰到它。
    {
        Uint32 bytesPerSec = (Uint32)opusConfig->sampleRate *
                             (Uint32)opusConfig->channelCount *
                             (Uint32)getAudioBufferSampleSize();
        m_BytesPerMs = bytesPerSec / 1000;

        Uint32 frameMs = opusConfig->sampleRate > 0 ?
                    (Uint32)(opusConfig->samplesPerFrame * 1000 / opusConfig->sampleRate) : 5;
        Uint32 devPeriodMs = have.freq > 0 ? (Uint32)((Uint32)have.samples * 1000 / (Uint32)have.freq) : 10;

        m_TrimThresholdMs = 30;
        if (m_TrimThresholdMs < frameMs * 4) {
            m_TrimThresholdMs = frameMs * 4;
        }
        if (m_TrimThresholdMs < devPeriodMs * 2) {
            m_TrimThresholdMs = devPeriodMs * 2;
        }

        Uint32 now = SDL_GetTicks();
        m_TrimWinStart = now;
        m_TrimWinFloorMs = 0xFFFFFFFFu;
        m_TrimArmed = false;
        m_LastLowFloorAt = now;  // 開場一分鐘內不修
        m_StatStart = now;
        m_StatMinMs = 0xFFFFFFFFu;
        m_StatMaxMs = 0;
        m_StatPendingMaxMs = 0;
        m_TrimTotal = 0;

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-AUDIO-Q] trim threshold: floor > %u ms (frame %u ms, device period %u ms)",
                    m_TrimThresholdMs, frameMs, devPeriodMs);
    }

    // Start playback
    SDL_PauseAudioDevice(m_AudioDevice, 0);

    return true;
}

SdlAudioRenderer::~SdlAudioRenderer()
{
    if (m_AudioDevice != 0) {
        // Stop playback
        SDL_PauseAudioDevice(m_AudioDevice, 1);
        SDL_CloseAudioDevice(m_AudioDevice);
    }

    if (m_AudioBuffer != nullptr) {
        SDL_free(m_AudioBuffer);
    }

    // VipleStream: DO NOT call SDL_QuitSubSystem(SDL_INIT_AUDIO) here.
    //
    // Symbolicated crash dump (SDL3.dll+0x176B85, RBX/RCX = ASCII bytes
    // "a:b0,b:b...", RAX = current TID) shows SDL3 holds a dangling
    // reference to a closed audio device after SDL_CloseAudioDevice +
    // SDL_QuitSubSystem. A SDL-internal worker thread then runs the
    // recursive-lock-acquire helper (which calls GetCurrentThreadId and
    // compares it against lock->owner_tid at offset 0xC) on a freed
    // device — the memory has already been reused by an ASCII string
    // allocation, so the dereference faults.
    //
    // Leaving SDL_INIT_AUDIO initialized across session boundaries
    // keeps the subsystem's thread alive and its internal device tables
    // coherent between close-device / open-device cycles, side-stepping
    // the race. SDL_Quit() at process exit cleans up properly.
}

void* SdlAudioRenderer::getAudioBuffer(int*)
{
    return m_AudioBuffer;
}

bool SdlAudioRenderer::submitAudio(int bytesWritten)
{
    if (bytesWritten == 0) {
        // Nothing to do
        return true;
    }

    // Don't queue if there's already more than 30 ms of audio data waiting
    // in Moonlight's audio queue.
    if (LiGetPendingAudioDuration() > 30) {
        return true;
    }

    // Provide backpressure on the queue to ensure too many frames don't build up
    // in SDL's audio queue, but don't wait forever to avoid a deadlock if the
    // audio device fails.
    for (int i = 0; i < 100; i++) {
        // Our device may enter a permanent error status upon removal, so we need
        // to recreate the audio device to pick up the new default audio device.
        if (SDL_GetAudioDeviceStatus(m_AudioDevice) == SDL_AUDIO_STOPPED) {
            return false;
        }

        // Only queue more samples where there are 10 frames or less in SDL's queue
        if (SDL_GetQueuedAudioSize(m_AudioDevice) / m_FrameSize <= 10) {
            break;
        }

        SDL_Delay(1);
    }

    // §PERF-1113：主動把常駐的多餘佇列深度拉回來。
    // 判斷用「觀察窗內送出前的最小深度（谷底）」：抖動會讓谷底變低，所以谷底還高於門檻就表示
    // 那一段純粹是延遲、不是抖動緩衝。每個觀察窗最多丟一個 Opus 幀（不做重取樣調速），
    // 丟之前先等一個接近無聲的幀（最多等 1 秒），把爆音的機會壓低。
    if (m_BytesPerMs != 0) {
        const Uint32 kTrimWindowMs = 3000;      // 觀察窗長度，同時也是兩次丟幀的最短間隔
        const Uint32 kTrimQuietWaitMs = 1000;   // 等安靜幀的上限
        const float kTrimQuietPeak = 0.003f;    // 約 -50 dBFS
        const Uint32 kStatIntervalMs = 30000;

        Uint32 now = SDL_GetTicks();
        Uint32 queuedMs = SDL_GetQueuedAudioSize(m_AudioDevice) / m_BytesPerMs;

        if (queuedMs < m_StatMinMs) m_StatMinMs = queuedMs;
        if (queuedMs > m_StatMaxMs) m_StatMaxMs = queuedMs;
        Uint32 pendingMs = (Uint32)LiGetPendingAudioDuration();
        if (pendingMs > m_StatPendingMaxMs) m_StatPendingMaxMs = pendingMs;

        bool dropThisFrame = false;

        if (!m_TrimArmed) {
            if (queuedMs < m_TrimWinFloorMs) m_TrimWinFloorMs = queuedMs;
            if (now - m_TrimWinStart >= kTrimWindowMs) {
                // 複核（11-13）：佇列的常駐深度同時也是抖動緩衝。最近一分鐘內谷底曾經貼到底（不到一個 Opus 幀＝
                // 差一點或已經斷音），表示這段深度是用得到的，不修；遠端或擁擠 Wi-Fi 上抖動 30～50 ms 時才不會
                // 修掉之後下一次抖動又斷音
                const Uint32 kTrimQuietAfterLowMs = 60000;
                const Uint32 frameMs = m_BytesPerMs != 0 ? (Uint32)m_FrameSize / m_BytesPerMs : 5;
                if (m_TrimWinFloorMs != 0xFFFFFFFFu && m_TrimWinFloorMs <= frameMs) {
                    m_LastLowFloorAt = now;
                }
                if (m_TrimWinFloorMs != 0xFFFFFFFFu && m_TrimWinFloorMs > m_TrimThresholdMs &&
                    now - m_LastLowFloorAt > kTrimQuietAfterLowMs) {
                    m_TrimArmed = true;
                    m_TrimArmedAt = now;
                }
                m_TrimWinStart = now;
                m_TrimWinFloorMs = 0xFFFFFFFFu;
            }
        }

        if (m_TrimArmed) {
            if (queuedMs <= m_TrimThresholdMs) {
                // 等待期間深度自己降下來了，不必丟
                m_TrimArmed = false;
                m_TrimWinStart = now;
                m_TrimWinFloorMs = 0xFFFFFFFFu;
            }
            else {
                bool quiet = true;
                const float* pcm = (const float*)m_AudioBuffer;
                int sampleCount = bytesWritten / (int)sizeof(float);
                for (int s = 0; s < sampleCount; s++) {
                    if (pcm[s] > kTrimQuietPeak || pcm[s] < -kTrimQuietPeak) {
                        quiet = false;
                        break;
                    }
                }
                if (quiet || now - m_TrimArmedAt >= kTrimQuietWaitMs) {
                    dropThisFrame = true;
                    m_TrimArmed = false;
                    m_TrimWinStart = now;
                    m_TrimWinFloorMs = 0xFFFFFFFFu;
                    m_TrimTotal++;
                }
            }
        }

        // 節流：30 秒一行，含這段時間的佇列深度範圍與累計丟幀數
        if (now - m_StatStart >= kStatIntervalMs) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-AUDIO-Q] sdl queue min=%u max=%u ms, pending max=%u ms, trimmed=%u frames (thr %u ms)",
                        m_StatMinMs == 0xFFFFFFFFu ? 0 : m_StatMinMs, m_StatMaxMs,
                        m_StatPendingMaxMs, m_TrimTotal, m_TrimThresholdMs);
            m_StatStart = now;
            m_StatMinMs = 0xFFFFFFFFu;
            m_StatMaxMs = 0;
            m_StatPendingMaxMs = 0;
        }

        if (dropThisFrame) {
            return true;
        }
    }

    if (SDL_QueueAudio(m_AudioDevice, m_AudioBuffer, bytesWritten) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to queue audio sample: %s",
                     SDL_GetError());
    }

    return true;
}

IAudioRenderer::AudioFormat SdlAudioRenderer::getAudioBufferFormat()
{
    return AudioFormat::Float32NE;
}
