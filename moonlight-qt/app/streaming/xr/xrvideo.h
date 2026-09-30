// VipleStream 2.0 §VR M3a X2 — XrVideo：把解碼幀畫進 XR 影像 quad（libplacebo 共用 XrContext 的 VkDevice）。
// 只在 HAVE_OPENXR 且 HAVE_LIBPLACEBO_VULKAN 時編譯。設計：vr_architecture.md §2.4（XrRenderer 與
// 共用碼、C20 測試實例）。
//
// 執行緒模型：
//   - init()／destroy()：XrContext 的 bring-up／destroyAll（frame thread 未跑或已 join）。
//   - submit()：decoder／render thread（XrRenderer::renderFrame）——只把 av_frame_clone 放進
//     mailbox（latest wins），不碰 GPU。
//   - testMap()：decoder thread（XrRenderer::testRenderFrame，C20）——用獨立的 pl_tex 組在同一個
//     pl_gpu 上真的 map 一次再 unmap；pl_gpu 本身執行緒安全，pl_tex 各執行緒不共用。
//   - update()：XR frame thread——唯一使用 pl_renderer 與影像 swapchain 的執行緒。有新幀就
//     acquire→pl_vulkan_release_ex→pl_map_avframe_ex→pl_render_image→pl_vulkan_hold_ex→
//     pl_gpu_finish（GPU 確定做完）→pl_unmap_avframe→xrReleaseSwapchainImage；沒有新幀就回傳上次
//     release 的影像給 layer 用。
//
// swapchain 格式：libplacebo 的 pl_fmt 沒有 sRGB 格式，所以
//   1. runtime 有 *_SRGB 且接受 MUTABLE_FORMAT：以對應的 UNORM 包裝，libplacebo 直接寫 sRGB 編碼值
//      （target transfer = sRGB），runtime 依 sRGB 解讀——8-bit 精度在感知空間；
//   2. 否則 RGBA16F：輸出線性光；
//   3. 否則 UNORM8：輸出線性光（OpenXR 把非 sRGB 格式當線性）。

#pragma once

#include "xrcontext.h"

#include <QString>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>

struct AVFrame;

class XrVideo
{
public:
    struct Config {
        QString dumpPath;          // dev：第 dumpAfterFrames 幀讀回存 PNG（最長邊 ≤ 1280）
        int dumpAfterFrames = 300;
    };

    XrVideo(XrContext* ctx, const Config& config);
    ~XrVideo();

    XrVideo(const XrVideo&) = delete;
    XrVideo& operator=(const XrVideo&) = delete;

    bool init(QString* error);
    void destroy();  // 可重入；呼叫時 frame thread 必須已停、device 已 idle

    // 影像路徑是否可用（XrRenderer 的 SW 格式測試 map 失敗時標成不可用，之後的 decoder 嘗試改走平面）
    bool usable() const { return m_Usable.load(std::memory_order_acquire); }
    void markUnusable(const char* why);

    pl_gpu gpu() const;

    // decoder／render thread
    void submit(const AVFrame* frame);
    bool testMap(const AVFrame* frame);

    // XR frame thread：有可顯示的影像就回 true 並填 subImage 與寬高比
    bool update(XrSession session, XrSwapchainSubImage* subImage, float* aspect);

    // 10 s 統計（XrContext::maybeLogStats 呼叫；回傳後歸零視窗）
    QString takeStatsLine();

private:
    enum class TargetMode { None, SrgbMutable, Float16Linear, Unorm8Linear };

    bool ensureSwapchain(XrSession session, int width, int height);
    void destroySwapchain();
    bool mapFrame(const AVFrame* frame, pl_frame* out, pl_tex* texSet);
    bool renderNewFrame(AVFrame* frame);
    void dumpTexture(pl_tex tex);

    XrContext* m_Ctx;
    Config m_Config;
    std::atomic<bool> m_Usable{false};

    pl_log m_Log = nullptr;
    pl_vulkan m_Vulkan = nullptr;
    pl_renderer m_Renderer = nullptr;
    pl_tex m_Tex[4] = {};       // XR thread 的 map 材質
    pl_tex m_TestTex[4] = {};   // testMap 用（m_TestMutex）
    std::mutex m_TestMutex;
    VkSemaphore m_Sem = VK_NULL_HANDLE;
    uint64_t m_SemValue = 0;

    // 影像 swapchain（XR thread）
    XrSwapchain m_Swapchain = XR_NULL_HANDLE;
    int m_SwWidth = 0;
    int m_SwHeight = 0;
    int64_t m_SwFormat = 0;       // swapchain 的 VkFormat
    VkFormat m_ViewFormat = VK_FORMAT_UNDEFINED;  // libplacebo 包裝用的格式
    TargetMode m_Mode = TargetMode::None;
    std::vector<VkImage> m_Images;
    std::vector<pl_tex> m_Wrapped;
    bool m_HaveReleased = false;  // 至少 release 過一次才能拿去當 layer
    bool m_LoggedFormat = false;

    // mailbox（latest wins）
    std::mutex m_MailboxMutex;
    AVFrame* m_Pending = nullptr;

    // 統計
    std::mutex m_StatsMutex;
    uint64_t m_Received = 0;
    uint64_t m_Overwritten = 0;
    uint64_t m_Drawn = 0;
    uint64_t m_RenderErrors = 0;
    std::vector<uint32_t> m_RenderUs;  // 本視窗每次 render 的微秒
    uint64_t m_TotalDrawn = 0;         // dump 用（XR thread）
    bool m_Dumped = false;
};
