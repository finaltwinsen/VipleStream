/**
 * @file streaming/vr/vrframemeta.h
 * @brief VipleStream 2.0 §VR：解出的幀 ↔ 0x81 VR metadata 配對（取代 FIFO），加上 echo 統計。
 *
 * decoder 可能丟幀（v4l2m2m、硬體 decoder 錯誤），用 FIFO 對應 DU 與解出的幀，
 * 丟一幀之後 renderPose 就全部錯位。VR session 改成：
 *   submit 時 m_Pkt->pts = frameNumber，並以 frameNumber 為 key 登記 metadata；
 *   receive 後用 frame->pts 查表（docs/vr_architecture.md §2.3「幀與 metadata 的配對」）。
 *
 * 只在 decoder 執行緒使用（submit 與 receive 同一條執行緒），不加鎖。
 */
#pragma once

#include <Limelight.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

// M4a R1：decoder 執行緒查到的 0x81 meta 交給 XR 影像 render 執行緒（以 pts＝frameNumber 為 key）。
// 不掛在 AVFrame::opaque_ref：那是串接鏈（活幀計數標記、VAAPI 的 DRM 描述子都會把它包進新 buffer），
// 最外層不會是 meta；pts 則會被 av_frame_clone／av_frame_copy_props／backend 的轉換保留。
class VrRenderMetaRing {
public:
    static void reset();
    static void publish(int64_t pts, const VIPLE_VR_FRAME_META& meta);
    // 查不到（太舊被覆蓋，或從沒登記）回 false
    static bool lookup(int64_t pts, VIPLE_VR_FRAME_META* meta);

private:
    static constexpr int kSize = 512;  // M4a R4：128 格時在途幀雜湊碰撞會蓋掉 meta（見 xrvideo.cpp）
    // pts 是 90 kHz 的 rtpTimestamp（60 fps 每幀 +1500），直接取餘數只會落在少數格子，改用雜湊
    static int slot(int64_t pts) { return (int)(((uint64_t)pts * 0x9E3779B97F4A7C15ull) >> 55); }  // 64−log2(kSize)
    struct Entry {
        int64_t pts;
        bool used;
        VIPLE_VR_FRAME_META meta;
    };
    static std::mutex s_Lock;
    static Entry s_Entries[kSize];
};

class VrFrameMetaTracker {
public:
    // injectDropEvery：每 N 個解出的幀丟一幀（模擬 decoder 吞幀），0＝關
    // injectLossSec：每 N 秒注入一次 LOSS（驗證 LOSS→REFRESH_START 往返），0＝關
    VrFrameMetaTracker(int injectDropEvery, int injectLossSec);
    ~VrFrameMetaTracker();

    // submitDecodeUnit：登記這一幀的 metadata
    void onSubmit(const DECODE_UNIT* du);

    // 解出一幀後以 pts（= frameNumber）查表。fifoFrameNumber 是舊的 FIFO 配對會
    // 給出的幀號（-1＝佇列空），只用來統計「如果還用 FIFO 會錯幾幀」。
    // 回傳 false 表示查不到（meta-miss）。
    bool onDecoded(int64_t pts, int fifoFrameNumber, VIPLE_VR_FRAME_META* meta);

    // 這一幀要不要被注入的「decoder 吞幀」丟掉
    bool shouldInjectDrop();

    // decoder 吞幀後 FIFO 跳過的項目數
    void noteFifoSkipped(int count);

    // decoder 沒有帶回 pts（AV_NOPTS_VALUE）時退回 FIFO
    void notePtsMissing();

    uint32_t lastSubmittedFrame() const { return m_LastSubmittedFrame; }

private:
    struct Slot {
        uint32_t frameNumber;
        bool used;
        VIPLE_VR_FRAME_META meta;
    };

    struct Stats {
        uint32_t submitted = 0;
        uint32_t decoded = 0;
        uint32_t metaHit = 0;
        uint32_t metaMiss = 0;
        uint32_t headerMissing = 0;   // 查到了但這一幀沒有 0x81 VR header
        uint32_t echoMatched = 0;     // server 標 ECHO_MATCHED
        uint32_t echoKnown = 0;       // echoSampleId 在 VrSampleHistory 查得到
        uint32_t poseFallback = 0;
        uint32_t refreshDone = 0;
        uint32_t fifoMismatch = 0;    // 舊 FIFO 配對會給出不同幀號的次數
        uint32_t fifoSkipped = 0;
        uint32_t injectedDrops = 0;
        uint32_t injectedLoss = 0;
        uint32_t ptsMissing = 0;
    };

    void maybeInjectLoss(uint32_t frameNumber);
    void maybeLog(bool final);
    static void logStats(const char* prefix, const Stats& s, std::vector<float>& ages);

    std::array<Slot, 64> m_Slots {};
    int m_InjectDropEvery;
    int m_InjectLossSec;
    uint32_t m_DecodedSinceDrop = 0;
    uint64_t m_LastLossInjectMs = 0;
    uint64_t m_LastLogMs = 0;
    uint32_t m_LastSubmittedFrame = 0;
    Stats m_Interval;
    Stats m_Total;
    std::vector<float> m_IntervalAgesMs;
    std::vector<float> m_TotalAgesMs;
};
