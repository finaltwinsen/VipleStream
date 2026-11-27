/**
 * @file streaming/vr/vrgamemv.h
 * @brief VipleStream §VR-GAME-MV（M-MV2）：頭盔端接收遊戲內動態向量與深度。
 *
 * 資料來源：Sunshine/src/vr/vr_mv_codec.cpp 的 **v2** 壓縮流（12 B 標頭 ＋ LZ4(Planar SoA 水平差分、ZigZag)）。
 * 這裡只做解碼；編碼端（量化、前景池化、死區）一個位元組都不動。
 *
 * 流程（docs/specs/vr_game_motion_vectors.md §8.2、§8.3）：
 *   common-c 在影像 socket 上收齊同一 videoFrameIndex 的所有 T_MV 分片後，呼叫 sink
 *   （LiVrGameMvSink，影像接收執行緒）→ sink **只把壓縮流複製進環**（每幀 16～22 KB，數微秒；
 *   Limelight.h 要求 sink「必須很快」，不要在收包執行緒解壓）→ XrVideo 的影像 render 執行緒在
 *   renderNewFrame／合成之前 lookup，當場解碼（1/16 網格、每格 5 B 的平面，LZ4 解壓約 0.1 ms）並上傳成貼圖。
 *
 * 環以 videoFrameIndex 為 key 保留最近 8 幀（存的是壓縮流，解碼結果由 lookup 產生、交出去之後由呼叫端持有）。
 * 執行緒安全：pushRaw／lookup／統計各自加鎖；lookup 先整份複製再解碼，不持鎖做慢工作。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vrgamemv {

// ── v2 格式常數（對齊 Sunshine/src/vr/vr_mv_codec.h；wire 上的值必須完全相同）──
constexpr uint8_t kVersion = 2;
constexpr uint8_t kUvScaleLog2 = 12;  ///< vx, vy 單位＝SBS UV / 2^12
constexpr uint8_t kDepthBits = 6;     ///< 深度＝round(sqrt(逆深度) × 63)
constexpr uint8_t kDs = 2;            ///< 伺服器端的 1/8 → 1/16 已是各取一格，解碼端只還原平面
constexpr uint16_t kMaxGridW = 640;   ///< 雙眼並排 1/16 網格總寬上限
constexpr uint16_t kMaxGridH = 360;
constexpr uint8_t kFlagHasDepth = 0x01;
constexpr uint8_t kFlagReset = 0x02;  ///< 遊戲切鏡頭／重建超解析度：這一幀禁止外插
constexpr uint8_t kFlagHeadRotSubtracted = 0x04;  ///< M-MV1 一律為 0（頭盔端自己扣）

constexpr int kRingFrames = 8;  ///< 以 videoFrameIndex 為 key 保留最近幾幀

/// 解碼後的一幀（只保留接進補幀需要的欄位）
struct Frame {
    uint32_t videoFrameIndex = 0;
    uint8_t frameStep = 1;   ///< K_step：兩張編碼幀之間跨越的遊戲幀數（伺服器夾在 1..4）
    uint8_t flags = 0;       ///< 壓縮流標頭的 kFlag*
    uint16_t gridW = 0;      ///< 雙眼並排網格總寬（偶數；左眼 [0, gridW/2)）
    uint16_t gridH = 0;
    bool hasDepth = false;
    std::vector<int16_t> vx;     ///< 長度 gridW*gridH，SBS UV × 2^12，方向＝uv_prev − uv_cur
    std::vector<int16_t> vy;     ///< 同上（y 由上往下為正）
    std::vector<uint8_t> depth;  ///< hasDepth 時長度 gridW*gridH，0..63（sqrt 量化的正規化逆深度）

    bool reset() const { return (flags & kFlagReset) != 0; }
};

/**
 * @brief 解碼一份 v2 壓縮流（12 B 標頭 ＋ LZ4 payload）成 Frame。
 * @param why 失敗原因（可為 nullptr）
 */
bool decode(const uint8_t* data, size_t len, Frame& out, std::string* why);

/**
 * @brief common-c 的 LiVrGameMvSink：收包執行緒呼叫，data 只在呼叫期間有效。
 *        ctx 不使用（Ring 是行程全域單例）。
 */
void sinkEntry(uint32_t videoFrameIndex, uint8_t frameStep, const uint8_t* data, uint32_t len, void* ctx);

/// 退回原本 synthFlow 的原因（統計用）
enum class Fallback : uint8_t {
    NoData = 0,   ///< F1：沒協商／這一幀的 T_MV 沒到（掉包或遊戲沒開超解析度）
    Reset,        ///< F2：遊戲 RESET
    Warmup,       ///< §8.3 穩定期未滿 30 張
    Cadence,      ///< F3：兩張編碼幀不連續或沒有算圖姿態
    HeadTurn,     ///< F4：兩張之間轉頭超過 10°
    Upload,       ///< 貼圖上傳失敗
    Shader,       ///< kExtFlowBody 失敗
    kCount
};

const char* fallbackName(Fallback f);

/**
 * @brief 行程全域的環（一個行程只有一場串流）。sink 執行緒 pushRaw、影像 render 執行緒 lookup。
 *        環裡存的是壓縮流；解碼在 lookup 的呼叫端執行緒做（sink 只要複製，不會拖慢收包）。
 */
class Ring {
public:
    static Ring& instance();

    // ── sink 執行緒 ──
    void pushRaw(uint32_t videoFrameIndex, uint8_t frameStep, const uint8_t* data, size_t len);
    // ── 消費端（XrVideo 影像 render 執行緒）──
    // 查不到（沒送、太舊被覆蓋）或解碼失敗回 nullptr
    std::shared_ptr<const Frame> lookup(uint32_t videoFrameIndex);
    // 換場：清掉所有資料與統計（在 LiStartConnection 之前呼叫）
    void reset();

    // ── 統計 ──
    void noteLookup(bool hit);
    void noteAdopted();
    void noteFallback(Fallback r);
    void noteUpload(bool ok);
    void noteShader(bool ok);
    // 每 10 秒印一行 [VIPLE-VR-GAMEMV]（render 執行緒每幀呼叫；沒開 gameMv 時呼叫端不會走到）
    void logIfDue(uint64_t nowUs);

private:
    Ring() = default;

    struct Raw {
        uint32_t videoFrameIndex = 0;
        uint8_t frameStep = 1;
        std::vector<uint8_t> data;
    };

    void noteReceived();
    void noteDecode(uint64_t us, bool ok);

    std::mutex m_Lock;
    Raw m_Slots[kRingFrames];
    unsigned m_Write = 0;

    std::mutex m_StatsLock;
    uint64_t m_Rx = 0;
    uint64_t m_DecOk = 0;
    uint64_t m_DecFail = 0;
    uint64_t m_Hit = 0;
    uint64_t m_Miss = 0;
    uint64_t m_Use = 0;
    uint64_t m_Fb[static_cast<int>(Fallback::kCount)] = {};
    uint64_t m_UploadOk = 0;
    uint64_t m_UploadFail = 0;
    uint64_t m_ShaderOk = 0;
    uint64_t m_ShaderFail = 0;
    std::vector<uint32_t> m_DecUs;
    uint64_t m_LastLogUs = 0;
    // 累計（不歸零；log 行尾帶總數）
    uint64_t m_TotalRx = 0, m_TotalUse = 0, m_TotalDecFail = 0;
};

/// IEEE 754 binary16 轉換（與 Sunshine/src/vr/vr_mv_codec.cpp 的 float_to_half 同一套捨入）
uint16_t floatToHalf(float f) noexcept;

}  // namespace vrgamemv
