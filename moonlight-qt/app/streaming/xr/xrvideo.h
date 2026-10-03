// VipleStream 2.0 §VR M3a X2 — XrVideo：把解碼幀畫進 XR 影像 quad（libplacebo 共用 XrContext 的 VkDevice）。
// 只在 HAVE_OPENXR 且 HAVE_LIBPLACEBO_VULKAN 時編譯。設計：vr_architecture.md §2.4（XrRenderer 與
// 共用碼、C20 測試實例）。
//
// 執行緒模型（X3 起）：
//   - init()／destroy()：XrContext 的 bring-up／destroyAll（XR frame thread 未跑或已 join）。
//     init 起一條「影像 render thread」，destroy 先停它再放資源。
//   - submit()：decoder／render thread（XrRenderer::renderFrame）——只把 av_frame_clone 放進
//     mailbox（latest wins）並喚醒影像 render thread，不碰 GPU。
//   - testMap()：decoder thread（XrRenderer::testRenderFrame，C20）——用獨立的 pl_tex 組在同一個
//     pl_gpu 上真的 map 一次再 unmap；pl_gpu 本身執行緒安全，pl_tex 各執行緒不共用。
//   - 影像 render thread：唯一使用 pl_renderer 與影像 swapchain 的執行緒。有新幀就
//     acquire→pl_vulkan_release_ex→pl_map_avframe_ex→pl_render_image→pl_vulkan_hold_ex→
//     pl_gpu_finish（GPU 確定做完才放掉解碼幀）→pl_unmap_avframe→xrReleaseSwapchainImage。
//     X2 把這串放在 XR frame thread 上，每新幀阻塞約 8 ms、開場 shader 編譯造成漏幀；X3 移出。
//   - current()／frameEnded()：XR frame thread——只讀「最後一次 release 的影像」（swapchain 保留
//     最後一幀，就是 XrContext 自有的最後一幀複本：decoder 因 RENDER_DEVICE_RESET 重建時 XrVideo
//     不動，XR thread 繼續重送）。解析度改變時舊 swapchain 先退休，等 XR thread 用新一代送出
//     xrEndFrame（frameEnded）後才銷毀，避免 layer 引用已銷毀的 swapchain。
//
// swapchain 格式：libplacebo 的 pl_fmt 沒有 sRGB 格式，所以
//   1. runtime 有 *_SRGB 且接受 MUTABLE_FORMAT：以對應的 UNORM 包裝，libplacebo 直接寫 sRGB 編碼值
//      （target transfer = sRGB），runtime 依 sRGB 解讀——8-bit 精度在感知空間；
//   2. 否則 RGBA16F：輸出線性光；
//   3. 否則 UNORM8：輸出線性光（OpenXR 把非 sRGB 格式當線性）。

#pragma once

#include "xrcontext.h"
#include "streaming/video/ffmpeg-renderers/plvk_common.h"  // §SF-DMABUF-CACHE：DrmTexCache

#include <QString>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <Limelight.h>

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
        // dev：第一幀進來 20 s 後丟掉接下來 testStallMs 毫秒的幀（驗 stale 1 s／5 s 轉換），0＝不用
        int testStallMs = 0;
        // M4a R4（PCVR）：沒有 0x81 render pose 的幀在 acquire 之前就丟掉。OpenXR layer 參照整個
        // swapchain、runtime 顯示最後 release 的影像，畫進去就回不去上一張有 pose 的影像；R1 讓
        // XR thread 看到「目前影像無 meta」→ 當成 no-video 送 loading quad，頭盔裡閃一幀（S1 真 driver
        // 每 10 s 約 2 次）。
        bool requireMeta = false;
    };

    // XR frame thread 讀取的「目前可顯示影像」
    struct Current {
        XrSwapchainSubImage subImage;
        float aspect = 0.0f;
        uint64_t generation = 0;
        uint64_t lastDrawnUs = 0;  // steady clock；最近一次畫出新幀的時間
        uint64_t seq = 0;          // M4a R3：發布序號（每畫出一張新影像 +1）
        uint32_t renderUs = 0;     // M4a R3：這張影像的 map＋render＋等 GPU 時間
        // M4a R1（PCVR）：這張影像的 0x81 render pose（FFmpegVideoDecoder 以 pts 查到後放進
        // VrRenderMetaRing，render 時以 frame->pts 取回）。hasMeta=false＝沒有 meta（β 或非 VR session）。
        bool hasMeta = false;
        VIPLE_VR_FRAME_META meta = {};
    };

    XrVideo(XrContext* ctx, const Config& config);
    ~XrVideo();

    XrVideo(const XrVideo&) = delete;
    XrVideo& operator=(const XrVideo&) = delete;

    bool init(QString* error);
    // 可重入；呼叫時 frame thread 必須已停。gpuWedged＝XrContext 判定 GPU 工作等不到結果（runtime 串流中
    // 死掉）：跳過所有會等 GPU 的呼叫（pl_gpu_finish、pl_tex_destroy、pl_vulkan_destroy），刻意洩漏
    void destroy(bool gpuWedged = false);

    // 影像路徑是否可用（XrRenderer 的 SW 格式測試 map 失敗時標成不可用，之後的 decoder 嘗試改走平面）
    bool usable() const { return m_Usable.load(std::memory_order_acquire); }
    void markUnusable(const char* why);

    pl_gpu gpu() const;

    // decoder／render thread
    void submit(const AVFrame* frame);
    bool testMap(const AVFrame* frame);

    // XR frame thread：有可顯示的影像（至少 release 過一次）就回 true
    bool current(Current* out);
    // XR frame thread：xrEndFrame 已送出引用 generation 這一代 swapchain 的 layer
    void frameEnded(uint64_t generation);
    // 暖機：XrContext bring-up 後、第一幀之前可呼叫（目前由 render thread 在第一幀自行處理）
    void setSession(XrSession session);

    // 10 s 統計（XrContext::maybeLogStats 呼叫；回傳後歸零視窗）
    QString takeStatsLine();

private:
    enum class TargetMode { None, SrgbMutable, Float16Linear, Unorm8Linear };

    struct SwapchainSet {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        int width = 0;
        int height = 0;
        std::vector<pl_tex> wrapped;
        uint64_t generation = 0;
    };

    bool ensureSwapchain(XrSession session, int width, int height);
    void destroySwapchainSet(SwapchainSet& s);
    void destroyAllSwapchains();
    // useCache：DRM_PRIME 走 dmabuf 匯入快取（只有影像 render thread 可以用）
    bool mapFrame(const AVFrame* frame, pl_frame* out, pl_tex* texSet, bool useCache);
    bool renderNewFrame(AVFrame* frame);
    void dumpTexture(pl_tex tex);
    void renderThreadMain();
    void reapRetired();

    XrContext* m_Ctx;
    Config m_Config;
    std::atomic<bool> m_Usable{false};

    pl_log m_Log = nullptr;
    pl_vulkan m_Vulkan = nullptr;
    pl_renderer m_Renderer = nullptr;
    pl_tex m_Tex[4] = {};       // XR thread 的 map 材質
    pl_tex m_TestTex[4] = {};   // testMap 用（m_TestMutex）
#ifdef HAVE_DRM
    // §SF-DMABUF-CACHE（2026-10-03）：影像 render thread 的 DRM_PRIME 匯入快取。原本每幀 pl_map_avframe_ex
    // 都重新匯入 dmabuf（4320x2160@120、兩平面＝每秒 240 次），PCVR 第三輪第 6 分鐘踩到 SteamOS msm 匯入
    // 失敗路徑的 kernel Oops，頭盔卡死、只能重開機（G-α 平面路徑同一個問題）。testMap 不用（另一條執行緒、一次性）。
    PlvkCommon::DrmTexCache m_DrmCache{"[VIPLE-XR]"};
    bool m_LoggedCacheFallback = false;
#endif
    std::mutex m_TestMutex;
    VkSemaphore m_Sem = VK_NULL_HANDLE;
    uint64_t m_SemValue = 0;
    // M4a 收尾：render 後等 GPU 逾時時卡住的解碼幀（仍被 GPU 參照，不可 unmap／free——也讓 VAAPI surface
    // 不被回收，免得解碼器銷毀時卡在 dma-buf 的 fence 上）
    std::vector<AVFrame*> m_WedgedFrames;

    XrSession m_Session = XR_NULL_HANDLE;

    // 影像 swapchain（render thread 擁有；m_SwMutex 保護「目前」與「退休」清單給 XR thread 讀）
    SwapchainSet m_Sw;                     // render thread 正在畫的一代
    int64_t m_SwFormat = 0;                // swapchain 的 VkFormat
    VkFormat m_ViewFormat = VK_FORMAT_UNDEFINED;  // libplacebo 包裝用的格式
    TargetMode m_Mode = TargetMode::None;
    bool m_LoggedFormat = false;
    std::atomic<bool> m_LoggedColor{false};  // mapFrame 由 render thread 與 testMap 呼叫
    uint64_t m_NextGeneration = 1;

    std::mutex m_SwMutex;
    bool m_HavePublished = false;          // 至少 release 過一次
    Current m_Published;                   // XR thread 讀的快照
    std::vector<SwapchainSet> m_Retired;   // 等 XR thread 送出新一代後銷毀
    uint64_t m_XrAckGeneration = 0;        // XR thread 最近一次 xrEndFrame 引用的 generation

    // mailbox（latest wins）
    std::mutex m_MailboxMutex;
    std::condition_variable m_MailboxCv;
    AVFrame* m_Pending = nullptr;
    bool m_StopThread = false;
    std::thread m_Thread;
    uint64_t m_FirstSubmitUs = 0;          // stall 測試的起點（submit 端）
    uint64_t m_TestStallDropped = 0;
    uint64_t m_NoMetaDropped = 0;  // M4a R4：requireMeta 時丟掉的無 meta 幀（累計）
    bool m_TestStallLogged = false;

    // 統計
    std::mutex m_StatsMutex;
    uint64_t m_Received = 0;
    uint64_t m_Overwritten = 0;
    uint64_t m_Drawn = 0;
    uint64_t m_RenderErrors = 0;
    std::vector<uint32_t> m_CpuUs;     // 本視窗每新幀 render thread 的 CPU 時間（不含等 GPU）
    std::vector<uint32_t> m_GpuUs;     // 本視窗每新幀等 GPU 做完的時間
    uint64_t m_TotalDrawn = 0;         // dump 用（XR thread）
    bool m_Dumped = false;
};
