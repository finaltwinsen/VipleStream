#pragma once

#include "renderer.h"
#include "SDL_compat.h"

class SdlAudioRenderer : public IAudioRenderer
{
public:
    SdlAudioRenderer();

    virtual ~SdlAudioRenderer();

    virtual bool prepareForPlayback(const OPUS_MULTISTREAM_CONFIGURATION* opusConfig);

    virtual void* getAudioBuffer(int* size);

    virtual bool submitAudio(int bytesWritten);

    virtual AudioFormat getAudioBufferFormat();

private:
    SDL_AudioDeviceID m_AudioDevice;
    void* m_AudioBuffer;
    int m_FrameSize;

    // §PERF-1113：SDL 佇列只會變深不會自己變淺（網路突發、兩端時鐘差），這裡記「佇列谷底」來判斷
    // 有沒有常駐的多餘延遲；谷底長時間高於門檻才丟一個 Opus 幀，把深度慢慢拉回來。
    Uint32 m_BytesPerMs = 0;        // 0 = 還沒準備好，不做任何修剪
    Uint32 m_TrimThresholdMs = 0;   // 谷底高於這個值才算「多餘」
    Uint32 m_TrimWinStart = 0;      // 目前觀察窗的起點（SDL_GetTicks）
    Uint32 m_TrimWinFloorMs = 0xFFFFFFFFu; // 觀察窗內送出前看到的最小佇列深度
    bool m_TrimArmed = false;       // 已判定要丟一幀，正在等一個安靜的幀
    Uint32 m_TrimArmedAt = 0;
    Uint32 m_LastLowFloorAt = 0;           // 最近一次觀察窗谷底貼到底（不到一個 Opus 幀）的時刻
    // 診斷用（每 30 秒一行 [VIPLE-AUDIO-Q]）
    Uint32 m_StatStart = 0;
    Uint32 m_StatMinMs = 0xFFFFFFFFu;
    Uint32 m_StatMaxMs = 0;
    Uint32 m_StatPendingMaxMs = 0;
    Uint32 m_TrimTotal = 0;
};
