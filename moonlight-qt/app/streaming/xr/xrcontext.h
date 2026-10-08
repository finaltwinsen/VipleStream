// VipleStream 2.0 §VR M3a X1 — XrContext：OpenXR instance／system／Vulkan／session 與 XR frame thread。
// 只在 CONFIG+=openxr（HAVE_OPENXR）編譯。設計：vr_architecture.md §2.4（D4：Session 層
// XrContext 加 XrRenderer frontend）。
//
// X1 範圍：
//   - instance：擴充先列舉後啟用；XR_KHR_vulkan_enable2 必要（XR_KHR_vulkan_enable 備援尚未
//     實作，缺 enable2 時 bringUp 失敗並記原因）；cylinder／FB_display_refresh_rate／
//     EXT_local_floor 有就開。
//   - system（HMD）＋PRIMARY_STEREO；Vulkan instance／device 經 xrCreateVulkan*KHR 建立，保留
//     handle、graphics queue 與 queue mutex（X2 的 libplacebo pl_vulkan_import 用 lock_queue／
//     unlock_queue 共用這把鎖）。
//   - session 狀態機：READY→xrBeginSession、STOPPING→xrEndSession、EXITING／LOSS_PENDING 結束。
//   - XR frame thread（本類別擁有）：xrWaitFrame／BeginFrame／EndFrame；loading 環境＝world-locked
//     quad（前方 1.5 m、寬 60°）每幀以 vkCmdClearColorImage 清成深灰。每 10 s 印
//     `[VIPLE-XR] 10s` 彙總（frame 數、miss、shouldRender=false 數、session 狀態）。
//   - bringUp(timeoutMs)：instance→system→vulkan→session，等到 READY 並 xrBeginSession、frame
//     thread 起跑才回 true；shutdown() 可重入（destructor 也會呼叫）。
//
// X2（§VR M3a）：影像由 XrVideo（streaming/xr/xrvideo.*，HAVE_LIBPLACEBO_VULKAN 時）以 libplacebo
// pl_vulkan_import 共用本類別的 VkDevice；XrRenderer（ffmpeg frontend）只把幀放進 XrVideo 的
// mailbox，XR frame thread 取最新一幀畫進影像 quad swapchain。第一幀之前仍顯示 loading quad。
// runtime 在 xrBeginFrame／xrEndFrame／xrAcquire／ReleaseSwapchainImage 可能使用 queue，這些呼叫
// 一律持 queueMutex()（libplacebo 的 lock_queue 也是這把鎖）。
//
// 執行緒：bringUp／shutdown 在呼叫端執行緒（Session 的 AsyncConnectionStartThread 或 main）；
// frame thread 起跑後，xrPollEvent 與所有 frame 呼叫只在 frame thread 上。queue 的使用一律持
// queueMutex()。

#pragma once

#include <QImage>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <vulkan/vulkan.h>
#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "xrinput.h"
#include "xrvrcontrollers.h"

class XrVideo;
class XrDesktopScreen;

class XrContext
{
public:
    struct Options {
        const char* applicationName = "VipleStream";
        uint32_t quadWidth = 1280;   // loading quad 的 swapchain 尺寸（X2 會依串流解析度重建）
        uint32_t quadHeight = 720;
        float quadDistanceM = 1.5f;  // world-locked，前方距離
        float quadFovDeg = 60.0f;    // 水平張角
        // X2：建立 XrVideo（libplacebo）。xr-probe --session 可關掉。
        bool enableVideo = true;
        // X2（dev）：影像 quad 第 N 幀讀回存 PNG（最長邊 ≤ 1280），空字串＝不存
        QString dumpFramePath;
        int dumpAfterFrames = 300;
        // X3（dev）：第一幀影像 20 s 後丟 testStallMs 毫秒的幀（驗 stale 1 s／5 s 轉換）；0＝不用
        int testStallMs = 0;
        // X3（dev）：bring-up 後第 N 秒自動 recenter 一次；0＝不用
        int testRecenterSec = 0;
        // X4：射線滑鼠事件的出口（Session 提供，從 XR frame thread 呼叫；推 SDL_USEREVENT 到 main thread）
        XrInputSink inputSink;
        // X4（dev）：合成射線（不讀控制器），驗證求交→濾波→事件→Li* 路徑
        bool testPointer = false;
        // 虛擬鍵盤（dev）：--xr-test-keyboard 的文字（合成射線逐鍵點擊）、--xr-dump-keyboard 的 PNG 路徑
        QString testKeyboardText;
        QString dumpKeyboardPath;
        // X5：session 不是因 shutdown() 而結束時，frame thread 結束前呼叫一次（只推事件，不可阻塞）。
        // reason：kEndedLoss（LOSS_PENDING／instance loss／SESSION_LOST／INSTANCE_LOST）或
        // kEndedExit（runtime 發起的 EXITING，例如使用者關掉 SteamVR）。
        std::function<void(int reason)> onEnded;
        // X5（dev）失敗注入：testFailBringUp＝bringUp 直接失敗；testFailAfterSec>0＝bring-up 後 N 秒
        // 模擬 testFailKind（kEndedLoss：不經 runtime 直接當 LOSS_PENDING；kEndedExit：
        // xrRequestExitSession 後把 runtime 的正常結束當成 runtime 發起的 EXITING）
        bool testFailBringUp = false;
        int testFailAfterSec = 0;
        int testFailKind = 0;
        // M4a R1：PCVR 模式——影像以 projection layer 呈現（pose＝0x81 帶回的 renderPose∘eyeToHead），
        // 不建 β 的射線滑鼠／鍵盤；參考空間 STAGE→LOCAL_FLOOR→LOCAL；stale 500 ms 淡出、2000 ms loading。
        bool pcvr = false;
        // 2026-10-05（dev）：--vr-overscan。PCVR 每眼 FOV 四邊各放大這麼多度：/launch 的 vrFov 與 projection 用同一組
        // 放大後的值（vrOverscan 仍送 0），host 多畫一圈，轉頭時 Frame 端重投影不會露出沒畫面的邊。0＝不放大。
        float pcvrOverscanDeg = 0.0f;
        // 2026-10-08（§VR-LATE-LATCH）：PCVR 的 frame thread 在 xrWaitFrame 返回後先等這麼久，才挑最新的串流影像交給
        // runtime。我們的「算圖」只是把解碼好的影像貼上去，幾乎不花時間；runtime 卻在顯示前約 3 個週期就叫醒我們
        // （Frame 實測 predictAhead 約 26 ms）。晚一點挑，同一個顯示時刻就能用到更新的影像。0＝不等（舊行為）。
        uint32_t pcvrLatchDelayUs = 0;
        // 2026-10-08（§VR-TRACK-LOCK）：追蹤樣本的送出時刻鎖到顯示的節拍，相位依 MTP 自動找（見 pcvrTrackPhaseStep）
        bool pcvrTrackLock = false;
        // 2026-10-08（§VR-SYNTH）：PCVR 半速串流時，沒有新影像的那一格顯示合成影像（見 XrVideo::renderSynth）
        bool pcvrSynth = false;
        // M4a R2（dev）：PCVR 控制器按鍵改用合成序列（pose 仍來自 runtime），驗 0x5506 打包
        bool testVrInput = false;
        // M4a 收尾（Frame 實測）：>0＝bring-up 時經 XR_FB_display_refresh_rate 列出可用更新率，要求最接近
        // 這個值的那個（PCVR 用 90；runtime 不支援擴充就沿用目前的）。waitViews 會等 predictedDisplayPeriod
        // 穩定、與要求一致後才回報（不一致時以量到的週期為準）。
        float preferredRefreshHz = 0.0f;
    };

    // M4a R1：bring-up 後量到的顯示參數（/launch 的 vrFov、vrIpd、vrEyeToHead、vrHz、vrPeriodNs 來源）
    struct ViewInfo {
        bool valid = false;
        XrFovf fov[2] = {};
        XrPosef eyeToHead[2] = {};   // 每眼相對頭（VIEW space）的 pose
        float refreshHz = 0.0f;      // FB_display_refresh_rate 或由 period 推算
        uint64_t periodNs = 0;       // xrWaitFrame 的 predictedDisplayPeriod
        uint32_t recommendedWidth = 0, recommendedHeight = 0;
        QString trackingSpace;       // STAGE／LOCAL_FLOOR／LOCAL
        bool timeConversion = false; // 有 XrTime↔client 時鐘換算（tracking thread 模式）
    };

    static constexpr int kEndedLoss = 1;
    static constexpr int kEndedExit = 2;
    static constexpr int kTestGpuWedge = 100;  // dev：--xr-test-fail gpuwedge（只用在 testFailKind）

    struct Stats {
        uint64_t frames = 0;          // xrEndFrame 成功次數
        uint64_t missed = 0;          // 由 predictedDisplayTime 間隔推算的漏幀數（暖機期之後）
        uint64_t warmupMissed = 0;    // bring-up 後前 2 s 的漏幀（runtime 起步期，不計入 miss%）
        uint64_t notRendered = 0;     // shouldRender == false 的幀
        uint64_t endFrameErrors = 0;
        int highestState = 0;         // XrSessionState 的最大值（FOCUSED=5）
        int currentState = 0;
        double missPercent() const {
            const uint64_t total = frames + missed;
            return total == 0 ? 0.0 : 100.0 * static_cast<double>(missed) / static_cast<double>(total);
        }
    };

    XrContext();
    explicit XrContext(const Options& options);
    ~XrContext();

    XrContext(const XrContext&) = delete;
    XrContext& operator=(const XrContext&) = delete;

    // 成功回 true（session 已 begin、frame thread 已起跑）。失敗回 false，*error 帶原因，
    // 內部資源已全部釋放（可直接 delete，或再呼叫一次 bringUp）。
    bool bringUp(int timeoutMs, QString* error);

    // xrRequestExitSession → 等 STOPPING → xrEndSession → 停 frame thread → 釋放全部資源。
    // 可重入；沒 bringUp 過也安全。
    void shutdown();

    bool isRunning() const { return m_FrameThreadRunning.load(std::memory_order_acquire); }

    // X3：任何執行緒呼叫；frame thread 下一幀依當下頭部水平朝向把螢幕重擺到正前方
    void requestRecenter();
    // 虛擬鍵盤：任何執行緒呼叫（熱鍵 Ctrl+Alt+Shift+K），frame thread 下一幀切換開關
    void requestKeyboardToggle();
    bool isLost() const { return m_Lost.load(std::memory_order_acquire); }
    // X4：XR session 持有輸入焦點（FOCUSED）時，平面視窗的滑鼠事件一律忽略（鍵盤照常）
    bool inputFocused() const { return m_Input != nullptr && m_State.load() == XR_SESSION_STATE_FOCUSED; }
    Stats stats() const;

    // 需要 session 的量測（xr-probe --session）：refresh rate、每眼 FOV、reference space 清單、
    // 啟用的擴充、swapchain 格式等。frame thread 起跑後才有 FOV。
    QJsonObject describe() const;

    // ── X2 給 libplacebo 的 handle ──
    VkInstance vkInstance() const { return m_VkInstance; }
    VkPhysicalDevice vkPhysicalDevice() const { return m_VkPhysicalDevice; }
    VkDevice vkDevice() const { return m_VkDevice; }
    uint32_t vkQueueFamily() const { return m_QueueFamily; }
    VkQueue vkQueue() const { return m_Queue; }
    uint32_t vkApiVersion() const { return m_VkApiVersion; }
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr() const { return m_Gipa; }
    std::mutex& queueMutex() { return m_QueueMutex; }
    // M4a 收尾：runtime 在串流中死掉時，已送出的 GPU 工作可能永遠等不到結果（R4 在 S1 卡了 7 分鐘）。
    // 所有 GPU 等待改成有上限；等不到就標記 wedged，拆除時跳過無限期等待並刻意洩漏 device／libplacebo 物件。
    // timeline semaphore 到 value 或逾時；true＝已到達
    bool waitSemaphoreBounded(VkSemaphore sem, uint64_t value, uint64_t timeoutNs);
    bool gpuWedged() const { return m_GpuWedged.load(std::memory_order_acquire); }
    void markGpuWedged(const char* why);
    const QStringList& enabledExtensions() const { return m_EnabledExtensions; }
    // pl_vulkan_import 要求：device 建立時實際啟用的 device 擴充與 features（required∪recommended∩支援）
    const std::vector<const char*>& vkEnabledDeviceExtensions() const { return m_DevExts; }
    const VkPhysicalDeviceFeatures2* vkEnabledFeatures() const { return m_HaveFeatureChain ? &m_EnFeat2 : nullptr; }
    XrInstance xrInstance() const { return m_Instance; }
    XrSession xrSession() const { return m_Session; }

    // X2：影像路徑（沒有 libplacebo、或 pl_vulkan_import 失敗時為 nullptr）
    XrVideo* video() const { return m_Video; }

    // ── M4a R1（PCVR）──
    // 等 frame thread 量到每眼 FOV／eyeToHead／period（bring-up 後第一個 shouldRender 幀），最多 timeoutMs
    bool waitViews(ViewInfo* out, int timeoutMs);
    // §H.4-AUTO（XR 桌面）：等 predictedDisplayPeriod 穩定後回報實際顯示更新率（Hz；runtime 回報與量到的
    // 週期不一致時以週期為準，與 waitViews 同規則）。不需要 eyeToHead，XR 桌面模式也可用；拿不到回 0。
    float waitRefreshHz(int timeoutMs);
    // tracking 送出執行緒（任何執行緒）取 HMD 樣本：pose 在追蹤參考空間；predictNs＝目標顯示時間 − 取樣時間。
    // 有 XrTime 換算（Linux XR_KHR_convert_timespec_time／Windows XR_KHR_win32_convert_performance_counter_time）
    // 時當下直接 xrLocateSpace（thread 模式，2×Hz 都是新樣本）；沒有時沿用 frame loop 最近一次 locate
    // （frameloop 模式）。還沒有有效 pose 回 false。
    bool sampleHmd(float pos[3], float rot[4], float linVel[3], float angVel[3], uint32_t* predictNs);
    // M4a R2：tracking 送出執行緒填左右控制器（pose[LEFT/RIGHT]、input[0/1]、flags 的 LEFT/RIGHT）。
    // thread 模式當下 locate grip；frameloop 模式沿用 frame loop 最近一次的 grip pose。
    void sampleControllers(struct _VIPLE_VR_TRACKING* sample);
    // M4a R2：main thread 放進 haptic 佇列，XR frame thread 呼叫 xrApplyHapticFeedback。
    // device 1＝左、2＝右（VIPLE_VR_POSE_LEFT/RIGHT）
    void queueHaptic(uint8_t device, uint32_t durationUs, float frequencyHz, float amplitude);
    // §VR-FOVEA：server 在協商回應裡確認的注視點編碼強度（%，100＝關）。main thread 在 /launch 之後設，frame thread 讀
    void setPcvrFoveation(int pct) { m_PcvrFoveaPct.store(pct, std::memory_order_relaxed); }
    bool trackingThreadMode() const { return m_TimeConv != nullptr; }

    // 執行期載入 Vulkan loader（Linux：libvulkan.so.1；Windows：vulkan-1.dll），app 不連 -lvulkan。
    static PFN_vkGetInstanceProcAddr loadVulkanLoader(QString* error);

    static const char* sessionStateName(int state);

private:
    bool createInstance(QString* error);
    bool createSystem(QString* error);
    bool createVulkan(QString* error);
    bool createSession(QString* error);
    bool createQuadSwapchain(QString* error);
    bool createActions(QString* error);
    bool waitForReadyAndBegin(int timeoutMs, QString* error);
    void destroyAll();

    // 回傳 false 表示 instance 已失效（LOSS_PENDING／EXITING）
    bool pollEvents();
    void handleStateChange(XrSessionState state);
    void frameThreadMain();
    bool clearImage(VkImage img, float r, float g, float b, float a = 1.0f);
    // X3：loading／狀態 quad 只清一次色，之後的幀重用最後 release 的影像（a<1 時 rgb 應為預乘值）
    bool ensureSolidQuad(XrSwapchain sc, VkImage* images, uint32_t count, bool* ready, float r, float g, float b,
                         float a = 1.0f);
    // 虛擬鍵盤：把 QImage 經 staging buffer 複製進 swapchain 影像（格式需為 8-bit RGBA／BGRA）
    bool uploadImage(XrSwapchain sc, VkImage* images, uint32_t count, const QImage& img);
    bool createSolidSwapchain(uint32_t w, uint32_t h, XrSwapchain* sc, VkImage** images, uint32_t* count, const char* what);
    void maybeLogStats(uint64_t nowNs);
    // §XR-REFRESH-LATE：session 已在跑、量到的幀週期不是想要的更新率時，重新列舉並再要求一次（frame thread）
    void requestRefreshLate(double measuredHz);

    Options m_Options;

    XrInstance m_Instance = XR_NULL_HANDLE;
    XrSystemId m_SystemId = XR_NULL_SYSTEM_ID;
    XrSession m_Session = XR_NULL_HANDLE;
    XrSpace m_LocalSpace = XR_NULL_HANDLE;
    XrSpace m_ViewSpace = XR_NULL_HANDLE;
    XrSwapchain m_QuadSwapchain = XR_NULL_HANDLE;
    // action set：SteamVR 要 app 掛上 action set 並每幀 xrSyncActions 才給 FOCUSED。
    // X4：XrInput 擁有 action set／actions／aim space（射線滑鼠）；m_ActionSet 只是它的 handle 副本。
    XrActionSet m_ActionSet = XR_NULL_HANDLE;
    XrInput* m_Input = nullptr;
    // M4a R2：PCVR 模式的控制器（取代 XrInput；一個 session 只能 attach 一組 action set）
    XrVrControllers* m_VrCtl = nullptr;
    // X4：指標 quad（白色小方塊，畫在命中點）
    XrSwapchain m_PointerSwapchain = XR_NULL_HANDLE;
    uint32_t m_PointerImageCount = 0;
    VkImage* m_PointerImages = nullptr;
    bool m_PointerReady = false;
    static constexpr uint32_t kPointerPx = 16;
    // 虛擬鍵盤：整塊貼圖＋hover／按下高亮（半透明，預乘 alpha）
    XrSwapchain m_KbSwapchain = XR_NULL_HANDLE;
    uint32_t m_KbImageCount = 0;
    VkImage* m_KbImages = nullptr;
    bool m_KbUploaded = false;
    bool m_KbUnsupportedLogged = false;
    bool m_KbDumped = false;
    XrSwapchain m_KbHoverSwapchain = XR_NULL_HANDLE;
    uint32_t m_KbHoverImageCount = 0;
    VkImage* m_KbHoverImages = nullptr;
    bool m_KbHoverReady = false;
    XrSwapchain m_KbPressSwapchain = XR_NULL_HANDLE;
    uint32_t m_KbPressImageCount = 0;
    VkImage* m_KbPressImages = nullptr;
    bool m_KbPressReady = false;
    // 貼圖由背景執行緒畫：第一次 QPainter 畫字要初始化字型系統，Windows 上約 3 s，放在 XR thread
    // 會整段漏幀。bring-up 後先畫無修飾鍵版本（順便暖字型），其餘組合（黏滯修飾鍵變色／大寫）第一次
    // 用到時才畫；畫好之前沿用舊貼圖，基本貼圖好之前不顯示鍵盤。
    void keyboardRenderThreadMain();
    std::thread m_KbRenderThread;
    std::mutex m_KbRenderMutex;
    std::condition_variable m_KbRenderCv;
    int m_KbRenderRequest = -1;               // 要畫的修飾鍵組合（-1＝沒有）；受 m_KbRenderMutex 保護
    bool m_KbRenderCancel = false;            // 受 m_KbRenderMutex 保護
    std::atomic<uint32_t> m_KbReadyMask{0};   // bit m＝m_KbCache[m] 已畫好
    QImage m_KbCache[16];
    int m_KbUploadedMods = -1;                // 目前 swapchain 裡是哪一種組合
    VkBuffer m_Staging = VK_NULL_HANDLE;
    VkDeviceMemory m_StagingMem = VK_NULL_HANDLE;
    void* m_StagingPtr = nullptr;
    VkDeviceSize m_StagingSize = 0;
    int64_t m_QuadFormat = 0;
    uint32_t m_QuadImageCount = 0;
    VkImage* m_QuadImages = nullptr;  // new[]，m_QuadImageCount 個
    bool m_QuadReady = false;         // loading quad 已清色並 release 過（frame thread）
    // X3：stale 狀態 quad（影像超過 1 s 沒更新時疊在螢幕上緣之上）
    XrSwapchain m_StatusSwapchain = XR_NULL_HANDLE;
    uint32_t m_StatusImageCount = 0;
    VkImage* m_StatusImages = nullptr;
    bool m_StatusReady = false;
    static constexpr uint32_t kStatusW = 256;
    static constexpr uint32_t kStatusH = 32;
    XrDesktopScreen* m_Screen = nullptr;  // X3 擺放（frame thread 用；requestRecenter 任何執行緒）
    int m_StaleState = 0;                 // 0 無影像、1 正常、2 stale（>1 s）、3 lost（>5 s，顯示 loading）
    bool m_LoggedLayerKind = false;
    std::vector<uint32_t> m_XrCpuUs;      // 本視窗 XR thread 每幀 CPU 時間（xrWaitFrame 返回→xrEndFrame 返回）
    uint64_t m_BringUpNs = 0;
    bool m_TestRecenterDone = false;
    bool m_TestFailFired = false;          // X5（dev）
    std::atomic<bool> m_SimExit{false};    // X5（dev）：模擬的 runtime EXITING
    int m_MissEventsLogged = 0;
    uint64_t m_DiagWaitNs = 0, m_DiagLockNs = 0, m_DiagBeginNs = 0, m_DiagEndNs = 0;  // X3 漏幀診斷
    XrVersion m_ApiVersion = 0;
    QStringList m_EnabledExtensions;
    QStringList m_ReferenceSpaces;
    bool m_HasRefreshRate = false;

    PFN_vkGetInstanceProcAddr m_Gipa = nullptr;
    VkInstance m_VkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice m_VkPhysicalDevice = VK_NULL_HANDLE;
    VkDevice m_VkDevice = VK_NULL_HANDLE;
    uint32_t m_QueueFamily = 0;
    VkQueue m_Queue = VK_NULL_HANDLE;
    std::atomic<int> m_PcvrFoveaPct{100};
    std::atomic<bool> m_LostByGpu{false};  // frame thread 看到就當成 session loss
    std::atomic<bool> m_GpuWedged{false};  // M4a 收尾：GPU 等待逾時（見 waitSemaphoreBounded）
    uint32_t m_VkApiVersion = 0;
    VkCommandPool m_CmdPool = VK_NULL_HANDLE;
    VkCommandBuffer m_Cmd = VK_NULL_HANDLE;
    VkFence m_Fence = VK_NULL_HANDLE;
    std::mutex m_QueueMutex;
    std::vector<const char*> m_DevExts;
    VkPhysicalDeviceFeatures2 m_EnFeat2 = {};
    VkPhysicalDeviceVulkan11Features m_EnF11 = {};
    VkPhysicalDeviceVulkan12Features m_EnF12 = {};
    bool m_HaveFeatureChain = false;
    XrVideo* m_Video = nullptr;

    std::thread m_FrameThread;
    std::atomic<bool> m_FrameThreadRunning{false};
    std::atomic<bool> m_StopRequested{false};
    std::atomic<bool> m_ExitRequested{false};  // shutdown() 要求 frame thread 走正常退出流程
    std::atomic<bool> m_Lost{false};
    std::atomic<bool> m_SessionBegun{false};
    std::atomic<int> m_State{0};

    mutable std::mutex m_StatsMutex;
    Stats m_Stats;
    uint64_t m_LastDisplayTime = 0;
    uint64_t m_LastPeriod = 0;
    uint64_t m_StatsWindowStartNs = 0;
    uint64_t m_WarmupEndNs = 0;  // steady clock；bring-up 成功 + 2 s
    Stats m_StatsAtWindowStart;
    XrFovf m_Fov[2] = {};
    XrPosef m_EyePose[2] = {};
    bool m_HaveViews = false;
    float m_RefreshHz = 0.0f;       // m_StatsMutex 保護（REFRESH_RATE_CHANGED_FB 事件會更新）
    float m_RequestedHz = 0.0f;     // M4a 收尾：xrRequestDisplayRefreshRateFB 成功要求的值；0＝沒要求
    uint32_t m_StablePeriodFrames = 0;  // m_StatsMutex：predictedDisplayPeriod 連續不變（±1%）的幀數
    int m_LateRefreshTries = 0;         // frame thread：session 跑起來後再要求更新率的次數（最多 2 次）
    bool m_PeriodSettled = false;       // m_StatsMutex：週期已穩定（且符合要求或等夠久）→ waitViews 可回報

    // ── M4a R1（PCVR）──
    XrSpace m_TrackSpace = XR_NULL_HANDLE;   // PCVR：STAGE→LOCAL_FLOOR→LOCAL（β 不用）
    QString m_TrackSpaceName;
    XrPosef m_EyeToHead[2] = {};              // m_StatsMutex 保護（與 m_Fov 一起）
    bool m_HaveEyeToHead = false;
    uint32_t m_RecW = 0, m_RecH = 0;
    std::condition_variable m_ViewsCv;        // waitViews 等 m_HaveEyeToHead 與 m_PeriodSettled
    // XrTime 換算（沒有＝nullptr，tracking 走 frameloop 模式）：Linux timespec、Windows QPC
    void* m_TimeConv = nullptr;
    bool m_TimeConvQpc = false;
    bool nowXrTime(XrTime* out) const;
    void pcvrAfterWaitFrame(XrTime predictedDisplayTime, uint64_t periodNs);
    void pcvrTrackPhaseStep(std::vector<uint32_t>& mtpUs, uint64_t periodNs);
    bool buildProjection(XrSwapchain swapchain, const XrRect2Di& rect, const float renderRot[4], const float renderPos[3],
                         uint32_t echoSampleId, XrCompositionLayerProjection* proj,
                         XrCompositionLayerProjectionView views[2]);
    // frame loop 最近一次的 HMD locate（frameloop 模式樣本；thread 模式也用它的 predict 估計）
    struct HmdSample {
        bool valid = false;
        float pos[3] = {};
        float rot[4] = {0, 0, 0, 1};
        float linVel[3] = {};
        float angVel[3] = {};
        uint64_t predictNs = 0;
        int64_t displayTime = 0;  // frame loop locate 用的 predictedDisplayTime（XrTime）
    };
    mutable std::mutex m_HmdMutex;
    HmdSample m_Hmd;
    int64_t m_PredictAheadNs = 0;             // predictedDisplayTime − xrWaitFrame 返回時的 XrTime（EMA）
    // §VR-POSEERR（2026-10-03）：算繪姿態（0x81）對「這一幀實際顯示時的頭部姿態」的誤差與落後時間。
    // 頭盔的重投影只修轉動，平移落後多少就是近物晃動的幅度。全部只在 frame thread 存取。
    struct PoseTrailPoint {
        int64_t t = 0;      // predictedDisplayTime（XrTime，ns）
        float pos[3] = {};
    };
    static constexpr int kPoseTrailLen = 64;  // 約 0.53 s（120 Hz）
    PoseTrailPoint m_PoseTrail[kPoseTrailLen];
    int m_PoseTrailHead = 0;
    int m_PoseTrailCount = 0;
    std::vector<float> m_PoseErrPosMm;        // 10 s 視窗
    std::vector<float> m_PoseErrRotDeg;
    std::vector<float> m_PoseLagMs;           // 10 s 視窗；正值＝落後、負值＝超前，只在頭部有移動時估
    std::vector<float> m_PoseLag1s;           // §VR-PREDICT：1 s 視窗，隨 CLIENT_TIMING 回報給 server
    // 延後評估：等軌跡延伸到該幀顯示時間之後 100 ms 才找對應時間點，才看得出「超前」
    struct PosePending {
        int64_t t = 0;      // 該幀 predictedDisplayTime
        float pos[3] = {};  // 算繪位置
    };
    static constexpr int kPosePendingLen = 32;
    PosePending m_PosePending[kPosePendingLen];
    int m_PosePendingHead = 0;   // 最舊的一筆
    int m_PosePendingCount = 0;
    uint64_t m_PoseErrLastSeq = 0;  // 上一次算誤差的影像（XrVideo::Current::seq）：同一張重複顯示不估落後
    void pcvrPoseErr(const float renderRot[4], const float renderPos[3], bool freshFrame);
    void pcvrPoseLagEval(const PosePending& e);
    // PCVR 呈現：stale 淡出用的半透明黑 quad（head-locked）
    XrSwapchain m_FadeSwapchain = XR_NULL_HANDLE;
    uint32_t m_FadeImageCount = 0;
    VkImage* m_FadeImages = nullptr;
    bool m_FadeReady = false;
    bool m_LoggedProjection = false;
    uint64_t m_ProjFrames = 0;                // 10 s 視窗：送出 projection layer 的幀數（m_StatsMutex）
    uint64_t m_ProjNoMeta = 0;                // 影像有但沒有 pose meta 的幀
    int m_PcvrStale = 0;                      // 0 無影像、1 live、2 fade（>500 ms）、3 loading（>2000 ms）

    // M4a R3：LATCH（10 Hz）、CLIENT_TIMING（1 Hz）、MTP 統計。只在 frame thread 存取。
    struct PcvrTiming {
        uint64_t predictedDisplayClientNs = 0;  // 本幀預測顯示時間（client steady 時鐘）
        // §VR-TRACK-LOCK：相位搜尋／維持（只在 frame thread）
        int trkMode = 0;                 // 0 搜尋、1 維持
        int64_t trkPhaseNs = 0;
        int64_t trkLastGoodNs = 0;       // 搜尋時上一步的相位（還沒跳之前）
        uint32_t trkPrevMtpUs = 0;
        int trkSettle = 0;
        int trkWarm = 0;                 // 開場先等幾秒（host 的節拍還在收斂）
        int trkClean = 0;                // 維持時連續幾秒比鎖定時的水位高
        uint32_t trkLocks = 0;
        uint32_t trkBackoffs = 0;        // 維持中重新搜尋的次數
        double trkLateSum = 0.0;         // 10 s 視窗：每秒晚到比例的總和／秒數
        uint32_t trkLateN = 0;
        uint64_t lastLatchedSeq = 0;
        bool haveLatch = false;
        uint32_t lastFrameId = 0;
        int32_t lastSlackUs = 0;
        uint64_t lastLatchSendNs = 0;
        uint64_t lastTimingSendNs = 0;
        uint64_t mtpWindowStartNs = 0;
        uint32_t presentedTotal = 0;
        uint32_t metaMissTotal = 0;
        uint32_t latchSent = 0;
        uint32_t timingSent = 0;
        uint32_t mtpNoSample = 0;
        // 2026-10-05：10 s 視窗的新影像／重複（latch 時沒有新影像）／跳過（兩次 latch 之間到了兩張以上，舊的沒顯示）
        uint32_t new10s = 0;
        uint32_t repeat10s = 0;
        uint32_t skip10s = 0;
        // 2026-10-05：xrEndFrame 時 runtime 會用的影像（最後 release 的）不是 projection 姿態所屬的那張（錯配一幀）
        uint32_t mismatch10s = 0;
        std::vector<int32_t> slack1s;
        std::vector<uint32_t> render1s;
        std::vector<uint32_t> mtp1sUs;
        std::vector<uint32_t> mtp10sUs;
    } m_PcvrTiming;
    void pcvrTimingOnLatch(uint64_t seq, uint64_t lastDrawnUs, uint32_t renderUs, bool hasMeta,
                           uint32_t echoSampleId, uint64_t latchNs);
    void pcvrTimingTick(uint64_t nowNs, uint64_t periodNs);
};
