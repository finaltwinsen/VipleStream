#include "../session.h"
#include "renderers/renderer.h"

#ifdef HAVE_SLAUDIO
#include "renderers/slaud.h"
#endif

#include "renderers/sdl.h"

#include <Limelight.h>

#include <cmath>

#define TRY_INIT_RENDERER(renderer, opusConfig)        \
{                                                      \
    IAudioRenderer* __renderer = new renderer();       \
    if (__renderer->prepareForPlayback(opusConfig))    \
        return __renderer;                             \
    delete __renderer;                                 \
}

IAudioRenderer* Session::createAudioRenderer(const POPUS_MULTISTREAM_CONFIGURATION opusConfig)
{
    // Handle explicit ML_AUDIO setting and fail if the requested backend fails
    QString mlAudio = qgetenv("ML_AUDIO").toLower();
    if (mlAudio == "sdl") {
        TRY_INIT_RENDERER(SdlAudioRenderer, opusConfig)
        return nullptr;
    }
#if defined(HAVE_SLAUDIO)
    else if (mlAudio == "slaudio") {
        TRY_INIT_RENDERER(SLAudioRenderer, opusConfig)
        return nullptr;
    }
#endif
    else if (!mlAudio.isEmpty()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unknown audio backend: %s",
                     SDL_getenv("ML_AUDIO"));
        return nullptr;
    }

    // -------------- Automatic backend selection below this line ---------------

#if defined(HAVE_SLAUDIO)
    // Steam Link should always have SLAudio
    TRY_INIT_RENDERER(SLAudioRenderer, opusConfig)
#endif

    // Default to SDL
    TRY_INIT_RENDERER(SdlAudioRenderer, opusConfig)

    return nullptr;
}

bool Session::initializeAudioRenderer()
{
    int error;

    SDL_assert(m_OriginalAudioConfig.channelCount > 0);
    SDL_assert(m_AudioRenderer == nullptr);
    SDL_assert(m_OpusDecoder == nullptr);

    m_AudioRenderer = createAudioRenderer(&m_OriginalAudioConfig);

    // We may be unable to create an audio renderer right now
    if (m_AudioRenderer == nullptr) {
        return false;
    }

    // Allow the chosen renderer to remap Opus channels as needed to ensure proper output
    m_ActiveAudioConfig = m_OriginalAudioConfig;
    m_AudioRenderer->remapChannels(&m_ActiveAudioConfig);

    // Create the Opus decoder with the renderer's preferred channel mapping
    m_OpusDecoder =
        opus_multistream_decoder_create(m_ActiveAudioConfig.sampleRate,
                                        m_ActiveAudioConfig.channelCount,
                                        m_ActiveAudioConfig.streams,
                                        m_ActiveAudioConfig.coupledStreams,
                                        m_ActiveAudioConfig.mapping,
                                        &error);
    if (m_OpusDecoder == nullptr) {
        delete m_AudioRenderer;
        m_AudioRenderer = nullptr;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to create decoder: %d",
                     error);
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Audio stream has %d channels",
                m_ActiveAudioConfig.channelCount);
    return true;
}

int Session::getAudioRendererCapabilities(int audioConfiguration)
{
    int caps = 0;

    Q_UNUSED(audioConfiguration);

    // All audio renderers support arbitrary audio duration
    caps |= CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION;

#ifdef STEAM_LINK
    // Steam Link devices have slow Opus decoders
    caps |= CAPABILITY_SLOW_OPUS_DECODER;
#endif

    return caps;
}

bool Session::testAudio(int audioConfiguration)
{
    // Build a fake OPUS_MULTISTREAM_CONFIGURATION to give
    // the renderer the channel count and sample rate.
    OPUS_MULTISTREAM_CONFIGURATION opusConfig = {};
    opusConfig.sampleRate = 48000;
    opusConfig.samplesPerFrame = 240;
    opusConfig.channelCount = CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(audioConfiguration);

    IAudioRenderer* audioRenderer = createAudioRenderer(&opusConfig);
    if (audioRenderer == nullptr) {
        return false;
    }

    delete audioRenderer;

    return true;
}

int Session::arInit(int /* audioConfiguration */,
                    const POPUS_MULTISTREAM_CONFIGURATION opusConfig,
                    void* /* arContext */, int /* arFlags */)
{
    SDL_memcpy(&s_ActiveSession->m_OriginalAudioConfig, opusConfig, sizeof(*opusConfig));
    s_ActiveSession->initializeAudioRenderer();
    return 0;
}

void Session::arCleanup()
{
    delete s_ActiveSession->m_AudioRenderer;
    s_ActiveSession->m_AudioRenderer = nullptr;

    opus_multistream_decoder_destroy(s_ActiveSession->m_OpusDecoder);
    s_ActiveSession->m_OpusDecoder = nullptr;
}

void Session::arDecodeAndPlaySample(char* sampleData, int sampleLength)
{
    int samplesDecoded;

#ifndef STEAM_LINK
    // Set this thread to high priority to reduce the chance of missing
    // our sample delivery time. On Steam Link, this causes starvation
    // of other threads due to severely restricted CPU time available,
    // so we will skip it on that platform.
    if (s_ActiveSession->m_AudioSampleCount == 0) {
        if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH) < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to set audio thread to high priority: %s",
                        SDL_GetError());
        }
    }
#endif

    // See if we need to drop this sample
    if (s_ActiveSession->m_DropAudioEndTime != 0) {
        if (SDL_TICKS_PASSED(SDL_GetTicks(), s_ActiveSession->m_DropAudioEndTime)) {
            // Avoid calling SDL_GetTicks() now
            s_ActiveSession->m_DropAudioEndTime = 0;

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Audio drop window has ended");
        }
        else {
            // We're still in the drop window
            return;
        }
    }

    s_ActiveSession->m_AudioSampleCount++;

    // If audio is muted, don't decode or play the audio
    if (s_ActiveSession->m_AudioMuted) {
        return;
    }

    if (s_ActiveSession->m_AudioRenderer != nullptr) {
        int sampleSize = s_ActiveSession->m_AudioRenderer->getAudioBufferSampleSize();
        int frameSize = sampleSize * s_ActiveSession->m_ActiveAudioConfig.channelCount;
        int desiredBufferSize = frameSize * s_ActiveSession->m_ActiveAudioConfig.samplesPerFrame;
        void* buffer = s_ActiveSession->m_AudioRenderer->getAudioBuffer(&desiredBufferSize);
        if (buffer == nullptr) {
            return;
        }

        if (s_ActiveSession->m_AudioRenderer->getAudioBufferFormat() == IAudioRenderer::AudioFormat::Float32NE) {
            samplesDecoded = opus_multistream_decode_float(s_ActiveSession->m_OpusDecoder,
                                                           (unsigned char*)sampleData,
                                                           sampleLength,
                                                           (float*)buffer,
                                                           desiredBufferSize / frameSize,
                                                           0);
        }
        else {
            samplesDecoded = opus_multistream_decode(s_ActiveSession->m_OpusDecoder,
                                                     (unsigned char*)sampleData,
                                                     sampleLength,
                                                     (short*)buffer,
                                                     desiredBufferSize / frameSize,
                                                     0);
        }

        // Update desiredSize with the number of bytes actually populated by the decoding operation
        if (samplesDecoded > 0) {
            SDL_assert(desiredBufferSize >= frameSize * samplesDecoded);
            desiredBufferSize = frameSize * samplesDecoded;

            // §VR-LOWBATT：嘟一聲（150 ms），880 Hz、音量約 -12 dBFS，頭尾 5 ms 淡入淡出免得爆音（11-04 使用者：兩聲改一聲）。
            // 混在串流聲音上：host 沒聲音時也有靜音封包，所以照樣聽得到
            Session* s = s_ActiveSession;
            if (s->m_BeepRequest.exchange(false)) {
                s->m_BeepPos = 0;
            }
            if (s->m_BeepPos >= 0) {
                const int rate = s->m_ActiveAudioConfig.sampleRate > 0 ? s->m_ActiveAudioConfig.sampleRate : 48000;
                const int ch = s->m_ActiveAudioConfig.channelCount;
                const int on = rate * 150 / 1000, gap = 0, ramp = rate * 5 / 1000;
                const int total = on;
                const bool isFloat = s->m_AudioRenderer->getAudioBufferFormat() == IAudioRenderer::AudioFormat::Float32NE;
                for (int i = 0; i < samplesDecoded && s->m_BeepPos < total; ++i, ++s->m_BeepPos) {
                    const int p = s->m_BeepPos;
                    const int k = p < on ? p : (p >= on + gap ? p - on - gap : -1);
                    if (k < 0) {
                        continue;
                    }
                    float env = 1.0f;
                    if (k < ramp) {
                        env = static_cast<float>(k) / ramp;
                    }
                    else if (k > on - ramp) {
                        env = static_cast<float>(on - k) / ramp;
                    }
                    const float v = 0.25f * env * std::sin(2.0f * 3.14159265f * 880.0f * static_cast<float>(k) / rate);
                    for (int c = 0; c < ch; ++c) {
                        if (isFloat) {
                            float& x = static_cast<float*>(buffer)[i * ch + c];
                            x = std::fmax(-1.0f, std::fmin(1.0f, x + v));
                        }
                        else {
                            short& x = static_cast<short*>(buffer)[i * ch + c];
                            const int y = x + static_cast<int>(v * 32767.0f);
                            x = static_cast<short>(y > 32767 ? 32767 : (y < -32768 ? -32768 : y));
                        }
                    }
                }
                if (s->m_BeepPos >= total) {
                    s->m_BeepPos = -1;
                }
            }
        }
        else {
            desiredBufferSize = 0;
        }

        if (!s_ActiveSession->m_AudioRenderer->submitAudio(desiredBufferSize)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Reinitializing audio renderer after failure");

            opus_multistream_decoder_destroy(s_ActiveSession->m_OpusDecoder);
            s_ActiveSession->m_OpusDecoder = nullptr;

            delete s_ActiveSession->m_AudioRenderer;
            s_ActiveSession->m_AudioRenderer = nullptr;
        }
    }

    // Only try to recreate the audio renderer every 200 samples (1 second)
    // to avoid thrashing if the audio device is unavailable. It is
    // safe to reinitialize here because we can't be torn down while
    // the audio decoder/playback thread is still alive.
    if (s_ActiveSession->m_AudioRenderer == nullptr && (s_ActiveSession->m_AudioSampleCount % 200) == 0) {
        // Since we're doing this inline and audio initialization takes time, we need
        // to drop samples to account for the time we've spent blocking audio rendering
        // so we return to real-time playback and don't accumulate latency.
        Uint32 audioReinitStartTime = SDL_GetTicks();
        if (s_ActiveSession->initializeAudioRenderer()) {
            Uint32 audioReinitStopTime = SDL_GetTicks();

            s_ActiveSession->m_DropAudioEndTime = audioReinitStopTime + (audioReinitStopTime - audioReinitStartTime);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Audio reinitialization took %d ms - starting drop window",
                        audioReinitStopTime - audioReinitStartTime);
        }
    }
}
