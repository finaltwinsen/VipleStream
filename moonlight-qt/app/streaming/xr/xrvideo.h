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
//   - 鎖的順序（§VR-LATCH-RELATCH，2026-10-08）：XrContext::queueMutex() 與 m_SwMutex 原本從不巢狀持有。
//     Config::atomicPublish 開著時，影像 render thread 在 queueMutex 內 release 之後接著拿 m_SwMutex 發布
//     （queueMutex → m_SwMutex）——這是唯一的巢狀，而且只在影像 render thread。其他執行緒不可以持著 m_SwMutex
//     再去拿 queueMutex：XR frame thread 的 current()／peek()／frameEnded()／drainPublished() 都在 queue 鎖外呼叫。
//
// swapchain 格式：libplacebo 的 pl_fmt 沒有 sRGB 格式，所以
//   1. runtime 有 *_SRGB 且接受 MUTABLE_FORMAT：以對應的 UNORM 包裝，libplacebo 直接寫 sRGB 編碼值
//      （target transfer = sRGB），runtime 依 sRGB 解讀——8-bit 精度在感知空間；
//   2. 否則 RGBA16F：輸出線性光；
//   3. 否則 UNORM8：輸出線性光（OpenXR 把非 sRGB 格式當線性）。

#pragma once

#include "xrcontext.h"
#include "xrlatchstats.h"  // §VR-LDIAG：xrlatch::Jitter（dev 的到達抖動注入）
#include "streaming/video/ffmpeg-renderers/plvk_common.h"  // §SF-DMABUF-CACHE：DrmTexCache

#include <QImage>
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
        // §VR-SYNTH：合成中間格（只在 PCVR 半速串流時開）。swapchain 高度加倍，下半是合成影像
        bool synth = false;
        // §VR-LDIAG（dev，--vr-test-jitter）：畫每一張串流影像之前照這個字串延後（xrlatch::Jitter 的格式），空字串＝不注入
        QString testJitter;
        // §VR-LATCH-RELATCH：xrReleaseSwapchainImage 與發布放進同一段 queue 鎖。frame thread 在 queue 鎖內看到
        // releaseNo() 變了，就保證 peek() 拿到的已經是新的那一張。關著（預設）時照舊：先 release、放掉 queue 鎖再發布。
        // 不可以和 synth 一起開——§VR-SYNTH 的第二次發布不在 queue 鎖內，這個保證對它不成立（建構子會把它關掉）
        bool atomicPublish = false;
        // §VR-LATCH-WAIT：每發布一張新影像就叫醒 waitNewerThan()（frame thread 在等這一格的影像）。要和 atomicPublish
        // 一起開；同樣不可以和 synth 一起開（建構子會把它關掉）
        bool notifyPublish = false;
    };

    // §VR-LDIAG：每發布一張新影像記一筆（§VR-SYNTH 的第二次發布沿用同一個 seq，不另記）。時刻都是 steady 時鐘的微秒
    struct PubRec {
        uint64_t seq;       // 發布序號（＝Current::seq）
        int64_t pts90;      // 這張影像的 pts：host 的 Present 時刻，90 kHz
        uint64_t arriveUs;  // 影像 render thread 開始處理的時刻
        uint64_t gpuEndUs;  // GPU 做完的時刻
        uint64_t pubUs;     // xrReleaseSwapchainImage 返回的時刻（之後 frame thread 才挑得到）
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
        // 2026-10-05（§VR 錯配計數）：這張影像 xrReleaseSwapchainImage 後的 release 序號
        uint64_t releaseNo = 0;
        // §VR-LATCH-WAIT：這張影像發布的時刻（steady 時鐘，µs；和 PubRec::pubUs 是同一個值）
        uint64_t publishedUs = 0;
        // §VR-SYNTH：這張 swapchain 影像的下半有合成影像時，它的範圍與姿態（current() 視情況換進 subImage／meta）
        bool hasSynth = false;
        XrRect2Di synthRect = {};
        VIPLE_VR_FRAME_META synthMeta = {};
        bool synthShown = false;  // current() 這次回傳的是合成那一格
    };

    // 2026-10-05：目前最後一次 xrReleaseSwapchainImage 的序號（呼叫端持 XrContext::queueMutex() 讀，與 release 互斥）。
    // runtime 在 xrEndFrame 用的是「最後 release 的影像」；它跟 current() 拿到的 releaseNo 不同，就代表 projection 用的
    // render pose 屬於前一張影像（錯配一幀）。
    uint64_t releaseNo() const
    {
        return m_ReleaseNo.load(std::memory_order_relaxed);
    }

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
    // §VR-LATCH-RELATCH（XR frame thread）：current() 的無副作用版——只看目前發布的是哪一張，不算成「顯示過一次」
    // （不動 §VR-SYNTH 的輪替，回傳的一律是真的那一半）。不可以持著 queueMutex 呼叫
    bool peek(Current* out);
    // §VR-LATCH-WAIT（XR frame thread）：目前發布的是哪一張、什麼時候發布的；還沒有影像回 false
    bool latest(uint64_t* seq, uint64_t* pubUs);
    // §VR-LATCH-WAIT（XR frame thread）：等到發布的影像不是 seq 這一張，或到了 deadlineUs（steady 時鐘，µs）。
    // true＝有新的一張。等的時候不持鎖；呼叫時不可以持 queueMutex（影像 render thread 要它才 release 得出去）。
    // 要 Config::notifyPublish 才會被叫醒（否則只會等到逾時）。等不到的那一次多久才返回，取決於條件變數逾時的
    // 精度：Linux 預期數十微秒；Windows 是毫秒級，所以 XrContext 在 Windows 上不用它（pcvrLatchWaitSupported）
    bool waitNewerThan(uint64_t seq, uint64_t deadlineUs);
    // XR frame thread：xrEndFrame 已送出引用 generation 這一代 swapchain 的 layer
    void frameEnded(uint64_t generation);
    // §VR-LDIAG（XR frame thread）：依序抄出序號大於 afterSeq、而且在 notAfterUs 之前（含）發布的紀錄，最多 max 筆；
    // 回傳筆數。只保留最近 64 筆，更舊的抄不到
    size_t drainPublished(uint64_t afterSeq, uint64_t notAfterUs, PubRec* out, size_t max);
    // §VR-SYNTH：一隻眼的水平／垂直視角（左右、上下兩個 tan 的和）；用來把頭部轉動換算成影像位移（XR thread 每幀設）
    void setEyeFovTan(float tanW, float tanH)
    {
        m_FovTanW.store(tanW, std::memory_order_relaxed);
        m_FovTanH.store(tanH, std::memory_order_relaxed);
    }
    // §VR-FOVEA：a＝正前方的斜率（1＝關）、cxL／cxR、cyL／cyR＝正前方在左右眼影像裡的位置（0–1，y 從上緣算）。
    // 兩眼的上下視角可以不同（Steam Frame 就是），host 是每眼照自己的中心壓縮的，所以垂直中心也要每眼各給一個。
    // XR thread 每幀設
    void setFoveation(float a, float cxL, float cxR, float cyL, float cyR)
    {
        m_FvCxL.store(cxL, std::memory_order_relaxed);
        m_FvCxR.store(cxR, std::memory_order_relaxed);
        m_FvCyL.store(cyL, std::memory_order_relaxed);
        m_FvCyR.store(cyR, std::memory_order_relaxed);
        m_FvA.store(a, std::memory_order_relaxed);
    }
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
    bool renderSynth(pl_frame* mapped, const pl_frame* targetProto, pl_tex swTex, int fw, int fh,
                     bool hasMeta, const VIPLE_VR_FRAME_META& meta, int64_t pts, bool* synthOk, VIPLE_VR_FRAME_META* synthMeta);
    bool renderNewFrame(AVFrame* frame);
    // §VR-FOVEA：swapchain 裡一張影像的尺寸（還原後正前方的細節要有地方放，所以比解碼出來的影像大）
    void outputSize(int fw, int fh, int* ow, int* oh) const;
    bool ensureFull(int fw, int fh);
    // fv＝(a, cxL, cxR, cyL)（shader 的 vec4 vs_fv）、*cyR＝右眼的垂直中心（shader 的 float vs_cyr）
    void loadFoveation(float fv[4], float* cyR) const;
    bool renderUnwarp(pl_frame* mapped, const pl_frame* targetProto, pl_tex swTex, int fw, int fh);
    void dumpTexture(pl_tex tex, const QString& path);
    // dev（--xr-dump-frame）：把 tex 讀回成縮過的影像（最長邊 ≤ 1280；失敗回空的影像）；saveDump 存檔並印一行。
    // 分成兩步是為了 §VR-SYNTH：第一次發布的那一張要在 hold 之前讀回，等確定這一幀有合成影像才存。
    QImage readbackTexture(pl_tex tex);
    void saveDump(const QImage& img, const QString& path, int srcW, int srcH);
    // dev（--xr-dump-frame，補幀開著時）：把這一張估位移用到的東西也存下來——上一張與這一張的 1/8 縮圖（PNG）和位移場
    // （原始資料：寬×高×4 個 float16，xy＝剩餘位移（UV，上一張的位置 − 這一張的位置）、z＝比對成本）。存下來的那一組
    // 影像只看得出「合成的那一張和真的差在哪」，看不出在動的東西是不是往對的方向、推了對的距離；有這三個檔才能離線核對。
    void dumpSynthInputs(const QString& prevPath, const QString& curPath, const QString& flowPath);
    void renderThreadMain();
    void reapRetired();
    // §VR-LDIAG（dev）：這一張串流影像要延後幾微秒（沒開注入時恆為 0）。只在影像 render thread 呼叫
    uint32_t jitterNextUs();

    XrContext* m_Ctx;
    Config m_Config;
    std::atomic<bool> m_Usable{false};

    pl_log m_Log = nullptr;
    pl_vulkan m_Vulkan = nullptr;
    pl_renderer m_Renderer = nullptr;
    // §VR-SYNTH（都只在 render thread 用）
    pl_dispatch m_Dp = nullptr;
    pl_tex m_Full = nullptr;         // 這一張的 RGB（和寫進 swapchain 的值相同）
    pl_tex m_Small[2] = {};          // 1/8 尺寸，這一張與上一張（輪流）
    pl_tex m_Flow = nullptr;         // 1/8 尺寸的位移場（UV 單位）
    pl_tex m_Mid[2] = {};            // 1/16 尺寸（位移估計的粗層），這一張與上一張
    pl_tex m_FlowC = nullptr;        // 1/16 尺寸的位移場（粗層的結果，給 1/8 那一層當起點）
    int m_SmallCur = 0;
    bool m_HavePrevSmall = false;
    bool m_HavePrevMeta = false;
    VIPLE_VR_FRAME_META m_PrevMeta = {};
    int64_t m_PrevPts = 0;
    int m_FrameH = 0;                // 一張影像的高度（synth 時 swapchain 是它的兩倍）
    bool m_SynthFailed = false;      // shader 或材質建立失敗過：這個 session 不再試
    uint64_t m_FirstShownSeq = 0;    // XR thread（m_SwMutex 內）：已經顯示過一次的 seq
    uint64_t m_SynthDrawn = 0;       // m_StatsMutex
    std::atomic<uint64_t> m_SynthShown{0};
    std::atomic<float> m_FovTanW{3.2f};
    std::atomic<float> m_FovTanH{3.2f};
    std::atomic<float> m_FvA{1.0f};
    std::atomic<float> m_FvCxL{0.5f};
    std::atomic<float> m_FvCxR{0.5f};
    std::atomic<float> m_FvCyL{0.5f};
    std::atomic<float> m_FvCyR{0.5f};
    bool m_UnwarpFailed = false;     // 還原的 shader 或材質失敗過：這個 session 改回直接畫（影像會是壓縮過的樣子）
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
    std::condition_variable m_PubCv;       // §VR-LATCH-WAIT：發布了新影像（配 m_SwMutex）
    bool m_HavePublished = false;          // 至少 release 過一次
    Current m_Published;                   // XR thread 讀的快照
    std::atomic<uint64_t> m_ReleaseNo{0};  // 2026-10-05：xrReleaseSwapchainImage 次數（在 queueMutex 內遞增）
    std::vector<SwapchainSet> m_Retired;   // 等 XR thread 送出新一代後銷毀
    uint64_t m_XrAckGeneration = 0;        // XR thread 最近一次 xrEndFrame 引用的 generation
    // §VR-LDIAG：最近 kPubRing 筆發布紀錄（環狀；m_SwMutex 保護）
    static constexpr size_t kPubRing = 64;
    PubRec m_PubRing[kPubRing] = {};
    uint64_t m_PubHead = 0;                // 累計寫入筆數（下一筆寫在 m_PubHead % kPubRing）

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
    xrlatch::Jitter m_Jitter;      // §VR-LDIAG（dev）：只在影像 render thread 用（init() 設定，之後不變）

    // 統計
    std::mutex m_StatsMutex;
    uint64_t m_Received = 0;
    uint64_t m_Overwritten = 0;
    uint64_t m_Drawn = 0;
    uint64_t m_RenderErrors = 0;
    std::vector<uint32_t> m_CpuUs;     // 本視窗每新幀 render thread 的 CPU 時間（不含等 GPU）
    std::vector<uint32_t> m_GpuUs;     // 本視窗每新幀等 GPU 做完的時間
    std::vector<uint32_t> m_SynthGpuUs;  // §VR-SYNTH：真的那一張發布之後，到合成那一張做完的時間
    std::vector<uint32_t> m_JitUs;     // §VR-LDIAG（dev）：本視窗被注入延後的影像各延後多久
    uint64_t m_TotalDrawn = 0;         // dump 用（XR thread）
    int m_DumpsDone = 0;               // dev：已經存了幾組
    static constexpr int kSynthDumpPairs = 6;  // 補幀開著時 --xr-dump-frame 一共存幾組（每 480 張一組）
};
