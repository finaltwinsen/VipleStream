/**
 * @file streaming/vr/vrgamemv.cpp
 * @brief VipleStream §VR-GAME-MV（M-MV2）：頭盔端 v2 壓縮流解碼、環與統計。
 *
 * 壓縮流（Sunshine/src/vr/vr_mv_codec.cpp 的 encode_planes）：
 *   12 B 標頭：version(1) flags(1) grid_w(2) grid_h(2) uv_scale_log2(1) depth_bits(1) uncompressed_len(4)
 *   之後是 LZ4 block：Planar SoA（vx lo、vx hi、vy lo、vy hi、[depth]），每列水平差分後 ZigZag 16 bit。
 *
 * 這一支只做解碼與暫存：sink（影像接收執行緒）只把壓縮流複製進環，解壓在環的 lookup（影像 render 執行緒）做。
 * Limelight.h 對 sink 的要求是「必須很快、microseconds 級」，所以收包執行緒不碰 LZ4。
 *
 * LZ4：用官方 LZ4 v1.10.0（streaming/vr/lz4/，BSD-2，與 Sunshine/third-party/lz4 同一份）的 LZ4_decompress_safe。
 * 不從 Sunshine 樹引用：Steam Frame 的 flatpak 快照只帶 moonlight-qt（＋picoquic），相對路徑會讓 flatpak 建置失敗。
 */
#include "vrgamemv.h"

#include <SDL.h>

#include "lz4/lz4.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>

namespace vrgamemv {

namespace {

inline uint16_t rd16(const uint8_t* p) noexcept
{
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t rd32(const uint8_t* p) noexcept
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline int16_t zigzagDecode16(uint16_t zz) noexcept
{
    // 與 Sunshine/src/vr/vr_mv_codec.h 的 zigzag_decode16 完全相同
    return static_cast<int16_t>((zz >> 1) ^ static_cast<uint16_t>(-static_cast<int16_t>(zz & 1u)));
}


inline uint64_t steadyUs() noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

const char* fallbackName(Fallback f)
{
    switch (f) {
    case Fallback::NoData:   return "nodata";
    case Fallback::Reset:    return "reset";
    case Fallback::Warmup:   return "warmup";
    case Fallback::Cadence:  return "cadence";
    case Fallback::HeadTurn: return "headturn";
    case Fallback::Upload:   return "upload";
    case Fallback::Shader:   return "shader";
    case Fallback::kCount:   break;
    }
    return "?";
}

uint16_t floatToHalf(float f) noexcept
{
    // 與 Sunshine/src/vr/vr_mv_codec.cpp 的 float_to_half 相同（round-to-nearest-even、次正規數、Inf/NaN）
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = bits & 0x007FFFFFu;

    if (((bits >> 23) & 0xFFu) == 0xFFu) {
        if (mant != 0) {
            return static_cast<uint16_t>(sign | 0x7E00u | (mant >> 13));
        }
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    if (exp <= 0) {
        if (exp < -10) {
            return static_cast<uint16_t>(sign);
        }
        mant |= 0x00800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - exp);
        const uint32_t round_bit = 1u << (shift - 1);
        const uint32_t sticky = (mant & (round_bit - 1)) != 0 ? 1u : 0u;
        uint32_t half_mant = mant >> shift;
        if ((mant & round_bit) != 0 && (sticky != 0 || (half_mant & 1u) != 0)) {
            ++half_mant;
        }
        return static_cast<uint16_t>(sign | half_mant);
    }
    if (exp >= 31) {
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    uint32_t half_exp = static_cast<uint32_t>(exp) << 10;
    uint32_t half_mant = mant >> 13;
    const uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half_mant & 1u) != 0)) {
        ++half_mant;
        if (half_mant == 0x0400u) {
            half_mant = 0;
            half_exp += 0x0400u;
            if (half_exp >= 0x7C00u) {
                return static_cast<uint16_t>(sign | 0x7C00u);
            }
        }
    }
    return static_cast<uint16_t>(sign | half_exp | half_mant);
}

bool decode(const uint8_t* data, size_t len, Frame& out, std::string* why)
{
    const auto fail = [why](const char* w) {
        if (why != nullptr) {
            *why = w;
        }
        return false;
    };
    if (data == nullptr || len <= 12) {
        return fail("short");
    }
    const uint8_t version = data[0];
    const uint8_t flags = data[1];
    const uint16_t gw = rd16(data + 2);
    const uint16_t gh = rd16(data + 4);
    const uint8_t uvScale = data[6];
    const uint8_t depthBits = data[7];
    const uint32_t uncompressedLen = rd32(data + 8);
    if (version != kVersion) {
        return fail("version");
    }
    if (uvScale != kUvScaleLog2 || depthBits != kDepthBits) {
        return fail("scale");
    }
    if (gw < 2 || gw > kMaxGridW || (gw & 1u) != 0 || gh < 1 || gh > kMaxGridH) {
        return fail("grid");
    }
    const bool hasDepth = (flags & kFlagHasDepth) != 0;
    const size_t cells = static_cast<size_t>(gw) * static_cast<size_t>(gh);
    const size_t expected = cells * (hasDepth ? 5u : 4u);
    if (uncompressedLen != expected) {
        return fail("len");
    }

    // 解壓工作緩衝區：sink 只有一條執行緒（多連線由 common-c 去重後才呼叫），thread_local 重複使用
    thread_local std::vector<uint8_t> planar;
    planar.resize(expected);
    const int got = LZ4_decompress_safe(reinterpret_cast<const char*>(data + 12), reinterpret_cast<char*>(planar.data()),
                                        static_cast<int>(len - 12), static_cast<int>(expected));
    if (got < 0 || static_cast<size_t>(got) != expected) {
        return fail("lz4");
    }

    out.vx.resize(cells);
    out.vy.resize(cells);
    if (hasDepth) {
        out.depth.resize(cells);
    }
    else {
        out.depth.clear();
    }
    out.flags = flags;
    out.gridW = gw;
    out.gridH = gh;
    out.hasDepth = hasDepth;

    const uint8_t* pVxLo = planar.data();
    const uint8_t* pVxHi = pVxLo + cells;
    const uint8_t* pVyLo = pVxHi + cells;
    const uint8_t* pVyHi = pVyLo + cells;
    const uint8_t* pDepth = hasDepth ? (pVyHi + cells) : nullptr;

    // 逐列還原水平差分（累加用 uint16_t，和伺服器端完全相同；16 bit 繞回語意一致）
    for (uint16_t y = 0; y < gh; y++) {
        const size_t row = static_cast<size_t>(y) * static_cast<size_t>(gw);
        uint16_t accX = 0;
        uint16_t accY = 0;
        uint8_t accD = 0;
        for (uint16_t x = 0; x < gw; x++) {
            const size_t i = row + x;
            const uint16_t zzX = static_cast<uint16_t>(pVxLo[i]) | (static_cast<uint16_t>(pVxHi[i]) << 8);
            const uint16_t zzY = static_cast<uint16_t>(pVyLo[i]) | (static_cast<uint16_t>(pVyHi[i]) << 8);
            accX = static_cast<uint16_t>(accX + static_cast<uint16_t>(zigzagDecode16(zzX)));
            accY = static_cast<uint16_t>(accY + static_cast<uint16_t>(zigzagDecode16(zzY)));
            out.vx[i] = static_cast<int16_t>(accX);
            out.vy[i] = static_cast<int16_t>(accY);
            if (hasDepth) {
                accD = static_cast<uint8_t>(accD + pDepth[i]);
                // §REVIEW-1127 qt_xr #11：合法 server 的量化上限是 (1 << kDepthBits) - 1 = 63；uint8 累加繞回會超過，
                // 整幀拒收（out 是呼叫端的新 Frame，失敗時會被丟掉）
                if (accD > ((1u << kDepthBits) - 1u)) {
                    return fail("depth");
                }
                out.depth[i] = accD;
            }
        }
    }
    return true;
}

void sinkEntry(uint32_t videoFrameIndex, uint8_t frameStep, const uint8_t* data, uint32_t len, void* ctx)
{
    (void) ctx;
    // Limelight.h 對 sink 的要求：只做複製、必須很快。解壓留給 lookup 的呼叫端
    // （影像 render 執行緒）——那裡的 0.1 ms 有 decUs 統計盯著，收包執行緒則完全不碰解壓。
    Ring::instance().pushRaw(videoFrameIndex, frameStep, data, len);
}

Ring& Ring::instance()
{
    static Ring s_Ring;
    return s_Ring;
}

void Ring::pushRaw(uint32_t videoFrameIndex, uint8_t frameStep, const uint8_t* data, size_t len)
{
    noteReceived();
    if (data == nullptr || len <= 12) {
        noteDecode(0, false);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(m_Lock);
        Raw& slot = m_Slots[m_Write++ % kRingFrames];
        slot.videoFrameIndex = videoFrameIndex;
        slot.frameStep = frameStep == 0 ? 1 : std::min<uint8_t>(frameStep, 4);  // 伺服器端 K_step 夾在 1..4
        slot.data.assign(data, data + len);
    }
}

std::shared_ptr<const Frame> Ring::lookup(uint32_t videoFrameIndex)
{
    // 先在鎖內整份複製（不持鎖解壓），找不到就回 nullptr
    thread_local std::vector<uint8_t> raw;
    uint8_t step = 1;
    {
        std::lock_guard<std::mutex> lk(m_Lock);
        raw.clear();
        for (int i = 1; i <= kRingFrames; i++) {
            const Raw& e = m_Slots[(m_Write + kRingFrames - static_cast<unsigned>(i)) % kRingFrames];
            if (!e.data.empty() && e.videoFrameIndex == videoFrameIndex) {
                raw = e.data;
                step = e.frameStep;
                break;
            }
        }
    }
    if (raw.empty()) {
        return nullptr;
    }

    auto f = std::make_shared<Frame>();
    const uint64_t t0 = steadyUs();
    std::string why;
    const bool ok = decode(raw.data(), raw.size(), *f, &why);
    noteDecode(steadyUs() - t0, ok);
    if (!ok) {
        // 只印前幾次，避免壞掉的來源灌滿 log
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 3) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-GAMEMV] T_MV decode failed (%s), len=%u", why.c_str(),
                        static_cast<unsigned>(raw.size()));
        }
        return nullptr;
    }
    f->videoFrameIndex = videoFrameIndex;
    f->frameStep = step;
    return f;
}

void Ring::reset()
{
    {
        std::lock_guard<std::mutex> lk(m_Lock);
        for (auto& e : m_Slots) {
            e = Raw {};
        }
        m_Write = 0;
    }
    std::lock_guard<std::mutex> lk(m_StatsLock);
    m_Rx = m_DecOk = m_DecFail = 0;
    m_Hit = m_Miss = m_Use = 0;
    for (uint64_t& v : m_Fb) {
        v = 0;
    }
    m_UploadOk = m_UploadFail = m_ShaderOk = m_ShaderFail = 0;
    m_DecUs.clear();
    m_LastLogUs = 0;
    m_TotalRx = m_TotalUse = m_TotalDecFail = 0;
}

void Ring::noteReceived()
{
    std::lock_guard<std::mutex> lk(m_StatsLock);
    m_Rx++;
    m_TotalRx++;
}

void Ring::noteDecode(uint64_t us, bool ok)
{
    std::lock_guard<std::mutex> lk(m_StatsLock);
    if (ok) {
        m_DecOk++;
        if (m_DecUs.size() < 4096) {
            m_DecUs.push_back(static_cast<uint32_t>(std::min<uint64_t>(us, 0xffffffffu)));
        }
    }
    else {
        m_DecFail++;
        m_TotalDecFail++;
    }
}

void Ring::noteLookup(bool hit)
{
    std::lock_guard<std::mutex> lk(m_StatsLock);
    if (hit) {
        m_Hit++;
    }
    else {
        m_Miss++;
    }
}

void Ring::noteAdopted()
{
    std::lock_guard<std::mutex> lk(m_StatsLock);
    m_Use++;
    m_TotalUse++;
}

void Ring::noteFallback(Fallback r)
{
    const int i = static_cast<int>(r);
    if (i < 0 || i >= static_cast<int>(Fallback::kCount)) {
        return;
    }
    std::lock_guard<std::mutex> lk(m_StatsLock);
    m_Fb[i]++;
}

void Ring::noteUpload(bool ok)
{
    std::lock_guard<std::mutex> lk(m_StatsLock);
    (ok ? m_UploadOk : m_UploadFail)++;
}

void Ring::noteShader(bool ok)
{
    std::lock_guard<std::mutex> lk(m_StatsLock);
    (ok ? m_ShaderOk : m_ShaderFail)++;
}

void Ring::logIfDue(uint64_t nowUsNow)
{
    std::lock_guard<std::mutex> lk(m_StatsLock);
    if (m_LastLogUs != 0 && nowUsNow - m_LastLogUs < 10000000ull) {
        return;
    }
    m_LastLogUs = nowUsNow;

    // 解碼耗時：這一視窗的 p50／p95（樣本不多，直接排序）
    uint32_t p50 = 0;
    uint32_t p95 = 0;
    if (!m_DecUs.empty()) {
        std::vector<uint32_t> s = m_DecUs;
        std::sort(s.begin(), s.end());
        p50 = s[s.size() / 2];
        p95 = s[std::min(s.size() - 1, (s.size() * 95) / 100)];
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VR-GAMEMV] 10s: rx=%llu dec=%llu fail=%llu hit=%llu miss=%llu use=%llu "
                "fb(nodata=%llu reset=%llu warmup=%llu cadence=%llu headturn=%llu upload=%llu shader=%llu) "
                "upl=%llu/%llu sh=%llu/%llu decUs p50=%u p95=%u | total rx=%llu use=%llu fail=%llu",
                static_cast<unsigned long long>(m_Rx), static_cast<unsigned long long>(m_DecOk),
                static_cast<unsigned long long>(m_DecFail), static_cast<unsigned long long>(m_Hit),
                static_cast<unsigned long long>(m_Miss), static_cast<unsigned long long>(m_Use),
                static_cast<unsigned long long>(m_Fb[static_cast<int>(Fallback::NoData)]),
                static_cast<unsigned long long>(m_Fb[static_cast<int>(Fallback::Reset)]),
                static_cast<unsigned long long>(m_Fb[static_cast<int>(Fallback::Warmup)]),
                static_cast<unsigned long long>(m_Fb[static_cast<int>(Fallback::Cadence)]),
                static_cast<unsigned long long>(m_Fb[static_cast<int>(Fallback::HeadTurn)]),
                static_cast<unsigned long long>(m_Fb[static_cast<int>(Fallback::Upload)]),
                static_cast<unsigned long long>(m_Fb[static_cast<int>(Fallback::Shader)]),
                static_cast<unsigned long long>(m_UploadOk), static_cast<unsigned long long>(m_UploadFail),
                static_cast<unsigned long long>(m_ShaderOk), static_cast<unsigned long long>(m_ShaderFail), p50, p95,
                static_cast<unsigned long long>(m_TotalRx), static_cast<unsigned long long>(m_TotalUse),
                static_cast<unsigned long long>(m_TotalDecFail));

    // 只歸零視窗（總數留在行尾）
    m_Rx = m_DecOk = m_DecFail = 0;
    m_Hit = m_Miss = m_Use = 0;
    for (uint64_t& v : m_Fb) {
        v = 0;
    }
    m_UploadOk = m_UploadFail = m_ShaderOk = m_ShaderFail = 0;
    m_DecUs.clear();
}

}  // namespace vrgamemv
