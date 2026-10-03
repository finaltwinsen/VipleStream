// VipleStream 2.0 §VR M3a X2 — PlVkRenderer 與 XrRenderer（XR 虛擬螢幕）共用的 libplacebo 純函式。
// 設計：vr_architecture.md §2.4「XrRenderer 與共用碼」——兩者都用 pl_map_avframe_ex 把解碼幀
// 對映成 pl_frame，但各自擁有 pl_gpu 與 render target，所以這裡只放不帶狀態的函式；XrRenderer
// 不繼承 PlVkRenderer。例外是 DrmTexCache：狀態由使用端各自持有一份（各自的 pl_gpu）。

#pragma once

extern "C" {
#include <libavutil/frame.h>
}

#include <libplacebo/log.h>
#include <libplacebo/renderer.h>

#include <cstdint>
#include <vector>

namespace PlvkCommon {

// libplacebo → SDL log（過濾 "Masking `…" 的噪音警告）
void logCallback(void* priv, enum pl_log_level level, const char* msg);

#ifdef HAVE_DRM
// §SF-DRMSPLIT：單一 layer、兩平面的 DRM_PRIME（Qualcomm iris 的 NV12／P010）改寫成每平面一個
// layer（R8＋GR88／R16＋GR1616），libplacebo 的 pl_map_avframe_drm 才認得。回傳新的 AVFrame
// （呼叫端 av_frame_free）；不需要或無法改寫時回 nullptr。*outcome 給 log 用。
AVFrame* splitDrmPrimeLayers(const AVFrame* frame, const char** outcome);

// §SF-DMABUF-CACHE：DRM_PRIME 幀的 dmabuf 匯入快取，取代 libplacebo pl_map_avframe_drm 的每幀匯入。
// libplacebo 每幀每平面都新建 pl_tex（DRM_IOCTL_PRIME_FD_TO_HANDLE）、unmap 時銷毀；解碼器的 capture
// pool 只有十幾個 buffer，改成依 dmabuf 身分（fstat 的 dev/ino，加上 offset/pitch/fourcc/modifier/
// 尺寸）快取，只在第一次遇到時匯入。dmabuf 的 inode 每個 buffer 唯一，fd 號碼被重用（解碼器重建）也
// 不會誤認；hw_frames_ctx 換了就整批清掉，避免抓著舊 pool 的記憶體。上限 64 筆（最久沒用的先放）。
// 起因：Steam Frame（SteamOS 6.18 msm）在匯入失敗的錯誤路徑 drm_gem_put_pages NULL deref（kernel
// Oops），執行緒永遠卡在核心、行程成殭屍、頭盔整個卡死，只能重開機。G-α（平面 1440p60）與 2026-10-03
// PCVR 第三輪（4320x2160@120，XR 路徑當時還是每幀匯入）各踩到一次。
// 只能由單一執行緒使用（各自的 render thread）；frame 要先經過 splitDrmPrimeLayers。
class DrmTexCache
{
public:
    explicit DrmTexCache(const char* logTag) : m_Tag(logTag) {}

    // 成功時 out 的平面材質來自快取，out->user_data 為 nullptr：pl_unmap_avframe 只會清空結構、不會
    // 銷毀快取的材質。任何一步不成立就回 false，由呼叫端退回 pl_map_avframe_ex。
    bool map(pl_gpu gpu, const AVFrame* frame, pl_frame* out);
    // pl_tex_destroy 會等 GPU 用完才真的釋放
    void clear(pl_gpu gpu, const char* reason);
    // GPU 卡住（等不到結果）時：不呼叫 libplacebo，直接丟掉記錄（材質刻意洩漏）
    void abandon();

private:
    struct Entry {
        uint64_t dev = 0, ino = 0, modifier = 0;
        uint32_t offset = 0, pitch = 0, fourcc = 0;
        int w = 0, h = 0;
        pl_tex tex = nullptr;
        uint64_t lastUse = 0;
    };
    const char* m_Tag;
    std::vector<Entry> m_Entries;
    const void* m_Ctx = nullptr;   // hw_frames_ctx 換了（解碼器重建）就整批清掉
    uint64_t m_Tick = 0;
    uint64_t m_Imports = 0;
    uint64_t m_Hits = 0;
    bool m_LoggedPitchClamp = false;
};
#endif

// pl_map_avframe_ex 成功後的共用修正：HDR 最低亮度 0 視為無限對比、強制 full range
// （host 的 AMF AV1 在 bitstream 不設 full range 的繞法）。
// forceFullRange=true：平面 PlVk 的既有行為（它向 host 要求 full range，並無條件標 full，
// 繞過 AMF AV1 不寫 range 旗標）。false：依幀的 color_range 決定——XR 的串流 range 由探測時
// 選到的平面 decoder 決定，可能是 limited（§VR M3a S1：Linux 協商到 MPEG，強標 full 使黑位抬高）。
void fixupMappedFrame(const AVFrame* frame, pl_frame* mapped, bool forceFullRange = true);

}  // namespace PlvkCommon
