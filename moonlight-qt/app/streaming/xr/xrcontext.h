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

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <vulkan/vulkan.h>
#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

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
    };

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
    bool isLost() const { return m_Lost.load(std::memory_order_acquire); }
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
    const QStringList& enabledExtensions() const { return m_EnabledExtensions; }
    // pl_vulkan_import 要求：device 建立時實際啟用的 device 擴充與 features（required∪recommended∩支援）
    const std::vector<const char*>& vkEnabledDeviceExtensions() const { return m_DevExts; }
    const VkPhysicalDeviceFeatures2* vkEnabledFeatures() const { return m_HaveFeatureChain ? &m_EnFeat2 : nullptr; }
    XrInstance xrInstance() const { return m_Instance; }
    XrSession xrSession() const { return m_Session; }

    // X2：影像路徑（沒有 libplacebo、或 pl_vulkan_import 失敗時為 nullptr）
    XrVideo* video() const { return m_Video; }

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
    bool clearImage(VkImage img, float r, float g, float b);
    // X3：loading／狀態 quad 只清一次色，之後的幀重用最後 release 的影像
    bool ensureSolidQuad(XrSwapchain sc, VkImage* images, uint32_t count, bool* ready, float r, float g, float b);
    void maybeLogStats(uint64_t nowNs);

    Options m_Options;

    XrInstance m_Instance = XR_NULL_HANDLE;
    XrSystemId m_SystemId = XR_NULL_SYSTEM_ID;
    XrSession m_Session = XR_NULL_HANDLE;
    XrSpace m_LocalSpace = XR_NULL_HANDLE;
    XrSpace m_ViewSpace = XR_NULL_HANDLE;
    XrSwapchain m_QuadSwapchain = XR_NULL_HANDLE;
    // 最小 action set（X1）：SteamVR 要 app 掛上 action set 並每幀 xrSyncActions 才給 FOCUSED。
    // X4（xrinput）接手擴充成射線／按鍵。
    XrActionSet m_ActionSet = XR_NULL_HANDLE;
    XrAction m_SelectAction = XR_NULL_HANDLE;
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
    float m_RefreshHz = 0.0f;
};
