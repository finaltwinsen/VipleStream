// VipleStream 2.0 §VR M3a X2 — PlVkRenderer 與 XrRenderer（XR 虛擬螢幕）共用的 libplacebo 純函式。
// 設計：vr_architecture.md §2.4「XrRenderer 與共用碼」——兩者都用 pl_map_avframe_ex 把解碼幀
// 對映成 pl_frame，但各自擁有 pl_gpu 與 render target，所以這裡只放不帶狀態的函式；XrRenderer
// 不繼承 PlVkRenderer。

#pragma once

extern "C" {
#include <libavutil/frame.h>
}

#include <libplacebo/log.h>
#include <libplacebo/renderer.h>

namespace PlvkCommon {

// libplacebo → SDL log（過濾 "Masking `…" 的噪音警告）
void logCallback(void* priv, enum pl_log_level level, const char* msg);

#ifdef HAVE_DRM
// §SF-DRMSPLIT：單一 layer、兩平面的 DRM_PRIME（Qualcomm iris 的 NV12／P010）改寫成每平面一個
// layer（R8＋GR88／R16＋GR1616），libplacebo 的 pl_map_avframe_drm 才認得。回傳新的 AVFrame
// （呼叫端 av_frame_free）；不需要或無法改寫時回 nullptr。*outcome 給 log 用。
AVFrame* splitDrmPrimeLayers(const AVFrame* frame, const char** outcome);
#endif

// pl_map_avframe_ex 成功後的共用修正：HDR 最低亮度 0 視為無限對比、強制 full range
// （host 的 AMF AV1 在 bitstream 不設 full range 的繞法）。
// forceFullRange=true：平面 PlVk 的既有行為（它向 host 要求 full range，並無條件標 full，
// 繞過 AMF AV1 不寫 range 旗標）。false：依幀的 color_range 決定——XR 的串流 range 由探測時
// 選到的平面 decoder 決定，可能是 limited（§VR M3a S1：Linux 協商到 MPEG，強標 full 使黑位抬高）。
void fixupMappedFrame(const AVFrame* frame, pl_frame* mapped, bool forceFullRange = true);

}  // namespace PlvkCommon
