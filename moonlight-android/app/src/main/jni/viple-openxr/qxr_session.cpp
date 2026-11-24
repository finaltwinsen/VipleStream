#ifndef XR_USE_PLATFORM_ANDROID
#define XR_USE_PLATFORM_ANDROID 1
#endif
#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN 1
#endif
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif

#include "qxr_session.h"

#include "qxr_framemeta.h"
#include "qxr_latch.h"
#include "qxr_log.h"
#include "qxr_tracking.h"
#include "qxr_video.h"
#include "qxr_vrmsg.h"
#include "qxr_xr_input.h"
#include "qxr_xr_pose.h"

#include "xrrateauto.h"

#include <Limelight.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <pthread.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr float kScreenDistanceM = 2.0f;
constexpr float kScreenWidthM = 2.4f;
// §QUEST-PCVR：跟 Frame xrcontext.cpp 2616–2626 同一組。500 ms 以內重投影還看得過去，超過 2 秒才換載入畫面。
constexpr uint64_t kPcvrLiveMs = 500;
constexpr uint64_t kPcvrFadeMs = 2000;
constexpr int32_t kPackedMax = 16384;
// §QUEST-RATE：Frame 預設開。streamingpreferences.h 的 vrRateAuto 初值是 true，
// session.cpp 的 createXrContext 只在 PCVR、而且沒有開場半速時才把 pcvrRateAuto 打開。
// Quest 沒有開場半速，/launch 的 vrHz 就是顯示更新率，所以這裡維持開，也不另做 Java 設定項。
constexpr bool kQxrRateAuto = true;

std::mutex g_loaderMu;
std::atomic<bool> g_loaderReady{false};
// Loader 的單例會一直留著這顆 jobject。Activity 結束時刪掉，同一個行程下一次建 instance 就是懸空的 global ref。
// 刻意不 DeleteGlobalRef，跟這個 .so 一起活到行程結束。
jobject g_loaderContext = nullptr;

// 正數也是成功。XR_TIMEOUT_EXPIRED 是 1，XR_SESSION_LOSS_PENDING 是 3，不能用 != XR_SUCCESS 當失敗。
bool xrOk(XrResult result) {
    return XR_SUCCEEDED(result);
}

// loss：這次呼叫已經成功，但 session 即將失效。呼叫端要走完已 begin 的幀再停。
// discarded：這幀不要畫，但仍要 xrEndFrame，layer 數為 0。
void xrNote(XrResult result, bool* lossPending, bool* discarded) {
    if (discarded != nullptr && result == XR_FRAME_DISCARDED) {
        *discarded = true;
    }
    if (lossPending != nullptr && result == XR_SESSION_LOSS_PENDING && !*lossPending) {
        *lossPending = true;
        QXR_LOGW("FRAME", "XR_SESSION_LOSS_PENDING");
    }
}

bool fail(std::string* error, const char* msg) {
    QXR_LOGE("SESSION", "%s", msg);
    if (error) {
        *error = msg;
    }
    return false;
}

void logXr(bool frame, XrInstance instance, const char* fn, XrResult result) {
    char text[XR_MAX_RESULT_STRING_SIZE] = {};
    const char* extra = "";
    if (instance != XR_NULL_HANDLE && xrOk(xrResultToString(instance, result, text))) {
        extra = text;
    }
    if (frame) {
        QXR_LOGE("FRAME", "%s failed: XrResult=%d %s", fn, static_cast<int>(result), extra);
    } else {
        QXR_LOGE("SESSION", "%s failed: XrResult=%d %s", fn, static_cast<int>(result), extra);
    }
}

bool failXr(std::string* error, XrInstance instance, const char* fn, XrResult result) {
    logXr(false, instance, fn, result);
    if (error) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "%s failed: XrResult=%d", fn, static_cast<int>(result));
        *error = buf;
    }
    return false;
}

bool failVk(std::string* error, const char* fn, VkResult result) {
    QXR_LOGE("SESSION", "%s failed: VkResult=%d", fn, static_cast<int>(result));
    if (error) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s failed: VkResult=%d", fn, static_cast<int>(result));
        *error = buf;
    }
    return false;
}

template <typename Fn>
bool loadProc(XrInstance instance, const char* name, Fn* out, std::string* error) {
    PFN_xrVoidFunction fn = nullptr;
    const XrResult result = xrGetInstanceProcAddr(instance, name, &fn);
    if (!xrOk(result) || fn == nullptr) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "xrGetInstanceProcAddr(%s) failed: XrResult=%d", name, static_cast<int>(result));
        return fail(error, buf);
    }
    *out = reinterpret_cast<Fn>(fn);
    return true;
}

uint32_t vkApiFromXr(XrVersion version) {
    uint32_t patch = static_cast<uint32_t>(XR_VERSION_PATCH(version));
    if (patch > 0xfffu) {
        patch = 0xfffu;
    }
    const uint32_t major = static_cast<uint32_t>(XR_VERSION_MAJOR(version));
    const uint32_t minor = static_cast<uint32_t>(XR_VERSION_MINOR(version));
#if defined(VK_MAKE_API_VERSION)
    return VK_MAKE_API_VERSION(0, major, minor, patch);
#else
    return VK_MAKE_VERSION(major, minor, patch);
#endif
}

const char* sessionStateName(XrSessionState state) {
    switch (state) {
    case XR_SESSION_STATE_IDLE: return "IDLE";
    case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
    case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "UNKNOWN";
    }
}

XrVector3f rotateQuat(const XrQuaternionf& q, const XrVector3f& v) {
    const float tx = 2.0f * (q.y * v.z - q.z * v.y);
    const float ty = 2.0f * (q.z * v.x - q.x * v.z);
    const float tz = 2.0f * (q.x * v.y - q.y * v.x);
    return {v.x + q.w * tx + (q.y * tz - q.z * ty),
            v.y + q.w * ty + (q.z * tx - q.x * tz),
            v.z + q.w * tz + (q.x * ty - q.y * tx)};
}

// §QUEST-PCVR：Hamilton a*b，儲存順序 xyzw。係數跟 Frame xrcontext.cpp 3878–3881 同一組，眼睛才是疊在頭的局部旋轉上。
XrQuaternionf quatMul(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// §QUEST-PCVR：網路上的算圖姿態。NaN 或長度是 0 不能送進 xrEndFrame。判斷跟 Frame 2218–2235 相同。
bool renderPoseUsable(const float rot[4], const float pos[3]) {
    float len2 = 0.0f;
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(rot[i])) {
            return false;
        }
        len2 += rot[i] * rot[i];
    }
    if (!(len2 > 1.0e-6f) || !std::isfinite(len2)) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(pos[i])) {
            return false;
        }
    }
    return true;
}

uint64_t monoNowNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

int32_t clampI32(int64_t value) {
    if (value > 2147483647ll) {
        return 2147483647;
    }
    if (value < -2147483647ll - 1) {
        return static_cast<int32_t>(-2147483647 - 1);
    }
    return static_cast<int32_t>(value);
}

bool extensionListed(const std::vector<XrExtensionProperties>& props, const char* name) {
    for (const XrExtensionProperties& prop : props) {
        if (std::strcmp(prop.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

bool deviceExtListed(const std::vector<VkExtensionProperties>& props, const char* name) {
    for (const VkExtensionProperties& prop : props) {
        if (std::strcmp(prop.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

// §QUEST-PCVR：舊的 1.0 標頭沒有 LOCAL_FLOOR 這個名字。數值來自 XR_EXT_local_floor，沒啟用擴充就不拿去建空間。
constexpr XrReferenceSpaceType kQxrLocalFloor = static_cast<XrReferenceSpaceType>(1000426000);

bool refSpaceListed(const std::vector<XrReferenceSpaceType>& types, XrReferenceSpaceType want) {
    for (XrReferenceSpaceType type : types) {
        if (type == want) {
            return true;
        }
    }
    return false;
}

}  // namespace

struct QxrSession::Impl {
    JavaVM* vm = nullptr;
    jobject activity = nullptr;
    int32_t streamWidth = 0;
    int32_t streamHeight = 0;
    int32_t preferredHz = 0;

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool sessionBegun = false;
    bool hasRefresh = false;
    bool created = false;

    VkInstance vkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool fencePending = false;

    std::vector<VkImageView> views;
    int64_t swapchainFormat = 0;
    uint32_t swapWidth = 0;
    uint32_t swapHeight = 0;
    QxrVideo video;

    std::vector<float> rates;
    float currentHz = 0.f;
    uint32_t eyeW = 0;
    uint32_t eyeH = 0;

    // §QUEST-PCVR：沒呼叫 enablePcvr 時這些都是空的，影格迴圈不 sync、quad 路徑不變。
    bool hasLocalFloor = false;
    bool hasTimeSpec = false;
    bool hasTouchPlus = false;
    std::vector<XrReferenceSpaceType> refSpaceTypes;
    std::atomic<bool> pcvr{false};
    std::unique_ptr<QxrXrControllers> controllers;
    std::unique_ptr<QxrXrPoseSource> poseSource;
    QxrConvertTimespecFn timeConvert = nullptr;
    XrSpace trackSpace = XR_NULL_HANDLE;
    bool trackIsStage = false;
    const char* trackSpaceName = "none";
    bool profileNoted = false;
    int syncFails = 0;
    std::atomic<bool> viewsLocked{false};
    mutable std::mutex viewMu;
    QxrViewConfig viewCache{};
    int viewLocateFails = 0;

    bool placed = false;
    bool needPlace = true;
    // 第一次擺放時 position 還沒有效。位置第一次變成有效才再擺一次，之後不再跟著頭動。
    bool awaitingPosition = false;
    bool forcePlace = false;
    bool lossPending = false;
    bool loggedLocate = false;
    float yaw = 0.f;
    XrPosef screenPose{};
    float quadWidthM = kScreenWidthM;
    float quadHeightM = 1.35f;

    std::mutex mu;
    std::condition_variable cv;
    std::atomic<bool> stopFlag{false};
    // join 不到自己。start 時 detach，stop 改看這兩個旗標等到迴圈結束。
    std::atomic<bool> workerSet{false};
    pthread_t worker{};
    int threadState = 0;
    std::function<void(int)> onEnded;
    bool endedNotified = false;
    std::atomic<bool> inRelease{false};

    uint64_t statLoops = 0;
    uint64_t statRenders = 0;
    uint64_t statLayers = 0;
    int64_t lastPeriodNs = 0;
    // predictedDisplayPeriod 取代擴充回報時只記一次，避免每一格都刷。
    bool loggedPeriodFallback = false;
    std::chrono::steady_clock::time_point statWindow{};
    int beginFails = 0;

    // §QUEST-PCVR：projection 沒建好之前，影格迴圈走原本的 quad。frameAlive 讓 arm 的等待在執行緒結束時醒得來。
    QxrLatch latch;
    QxrFrameMeta* frameMeta = nullptr;
    std::mutex timingMu;
    std::mutex armMu;
    std::condition_variable armCv;
    std::atomic<bool> armRequest{false};
    bool armDone = false;
    bool armOk = false;
    bool armWaiting = false;
    int32_t armW = 0;
    int32_t armH = 0;
    std::string armError;
    int32_t armedW = 0;
    int32_t armedH = 0;
    std::atomic<bool> projReady{false};
    std::atomic<bool> frameAlive{false};

    struct ColorChain {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        std::vector<VkImageView> views;
        std::vector<VkFramebuffer> framebuffers;
        uint32_t width = 0;
        uint32_t height = 0;
        bool ready = false;
    };
    struct StagedEye {
        XrPosef pose{};
        XrFovf fov{};
    };
    struct Staged {
        int64_t frameNumber = -2;
        bool poseValid = false;
        bool classified = false;
        bool fromMeta = false;
        uint32_t echoSampleId = 0;
        uint64_t sinkNs = 0;
        uint64_t noticedNs = 0;
        XrVector3f headPos{};
        XrQuaternionf headRot{0.f, 0.f, 0.f, 1.f};
        StagedEye eye[2]{};
    };
    struct PcvrSubmit {
        bool projection = false;
        bool fade = false;
        bool loading = false;
        XrCompositionLayerProjection proj{};
        XrCompositionLayerProjectionView views[2]{};
        XrCompositionLayerQuad fadeLayer{};
        XrCompositionLayerQuad loadLayer{};
    };

    ColorChain projChain;
    ColorChain fadeChain;
    ColorChain loadingChain;
    VkRenderPass solidPass = VK_NULL_HANDLE;
    // runtime 沒列 OPAQUE 時改用它列出來的第一個，xrEndFrame 才不會被拒。
    XrEnvironmentBlendMode blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    uint64_t lastReadyNs = 0;
    Staged pending;
    Staged shown;
    int64_t renderedFrame = -2;
    int64_t untimedGen = -3;
    bool loggedProjection = false;
    bool loggedUntimed = false;
    bool loggedFadeMiss = false;
    bool loggedLoadMiss = false;
    int pcvrStale = 0;
    uint64_t framePickNs = 0;
    uint64_t displayClientNs = 0;
    uint64_t statProj = 0;
    uint64_t statResent = 0;
    uint64_t statMetaHit = 0;
    uint64_t statMetaMiss = 0;
    uint64_t statFade = 0;
    uint64_t statLoading = 0;
    uint64_t statRenderUs = 0;
    uint64_t statRenderN = 0;
    uint64_t statEndFail = 0;

    // §QUEST-RATE：AutoRate 只在 frame thread 碰。訊息執行緒只寫下面兩個 atomic。
    xrrate::AutoRate rateAuto;
    std::atomic<int> rateDiv{1};
    std::atomic<uint32_t> rateReply{0};
    bool rateBound = false;
    bool rateDisabledLogged = false;
    // 到達時刻在影像 listener 寫下。這一格可能比那個 store 更早採用畫面，下一格再把訊號算回這張，避免把剛畫過的畫面當成還沒採用。
    bool rateSignalPending = false;
    uint64_t rateLastReadyNs = 0;
    uint64_t ratePrevRenderUs = 0;
    uint32_t rateSinceNew = 0;
    uint32_t rateUnknownSlots = 0;
    uint32_t ratePkts = 0;
    bool ratePktsInit = false;
    uint32_t rateLogDowns = 0;
    uint32_t rateLogUps = 0;

    bool build(const QxrSessionCreateInfo& info, std::string* error);
    void teardown();
    int frameLoop();
    bool enablePcvr(std::string* error);
    bool viewConfig(QxrViewConfig* out) const;
    bool armPcvrOutput(int32_t packedW, int32_t packedH, std::string* error);
    void bindPcvrTiming(QxrFrameMeta* meta, QxrSampleHistory* history);
    void unbindPcvrTiming();
    bool copyPcvrTiming(QxrClientTimingFill* out);
    // frame thread 結束時由外層的執行緒函式呼叫，要叫醒還在等 arm 的 JNI 呼叫端。
    void abandonArm();

private:
    bool createInstance(std::string* error);
    bool createSystem(std::string* error);
    bool createVulkan(std::string* error);
    bool createXrSession(std::string* error);
    bool createSwapchain(std::string* error);
    bool createGpuObjects(std::string* error);
    void tryBegin();
    bool poll(int* reason);
    void tryPlace(XrTime displayTime);
    bool drawFrame(const XrFrameState& frame, bool* sentLayer, int* fatalReason);
    bool waitGpu(int* fatalReason);
    bool recordAndSubmit(VkImageView target, uint32_t width, uint32_t height);
    void heartbeat();
    void logBlendModes();
    int syncPcvrActions();
    void captureViewConfig(XrTime displayTime);
    // 擴充回報跟 predictedDisplayPeriod 差超過 3% 時以週期為準。擴充沒有時 currentHz 是 0，也用週期。
    float effectiveRefreshHz() const;
    // LOCAL 在追蹤空間的高度夠才當坐姿零點，否則用當下的頭。兩者都失敗回 false。
    bool measureSeated(XrTime displayTime, float pos[3], float* yawRad, const char** source);
    int pumpArm();
    void finishArm(bool ok, const std::string& error);
    bool createProjOutput(int32_t packedW, int32_t packedH, std::string* error);
    void destroyProjOutput();
    bool ensureSolidPass(std::string* error);
    bool createChain(uint32_t width, uint32_t height, bool withFramebuffer, ColorChain* chain, std::string* error);
    void destroyChain(ColorChain* chain);
    bool paintSolid(ColorChain* chain, float r, float g, float b, float a, int* fatalReason);
    bool recordClear(VkFramebuffer framebuffer, uint32_t width, uint32_t height, float r, float g, float b, float a);
    void captureFrameClock(XrTime predicted, int64_t periodNs);
    bool copyViewEyes(XrPosef eye[2], XrFovf fov[2]) const;
    bool composeMeta(const VIPLE_VR_FRAME_META& meta, Staged* staged) const;
    void classifyPending(bool count);
    bool renderProjection(int* fatalReason, uint32_t* renderUs);
    void fillProjection(const Staged& src, PcvrSubmit* out);
    void fillFade(PcvrSubmit* out);
    void fillLoading(PcvrSubmit* out);
    bool drawPcvr(const XrFrameState& frame, PcvrSubmit* out, int* fatalReason);
    void noteStale(int state, uint64_t ageMs);
    void logProj();
    uint64_t ageMs(uint64_t noticedNs) const;
    void bindRateHandler();
    void unbindRateHandler();
    void noteRateApplied(int div, bool accepted, uint16_t seq);
    void applyRateReply();
    void pcvrRateOnSlot(bool newFrame, uint32_t periodUs, bool unadopted, uint64_t renderPlusFenceUs, bool decodeQueueEmpty,
                        bool packetStall);
    void logRate10s();
};

jobject loaderApplicationContext(JavaVM* vm, jobject activity, std::string* error) {
    if (g_loaderContext != nullptr) {
        return g_loaderContext;
    }
    JNIEnv* env = nullptr;
    bool attached = false;
    const jint envResult = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (envResult == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || env == nullptr) {
            fail(error, "AttachCurrentThread failed while reading the application context");
            return nullptr;
        }
        attached = true;
    } else if (envResult != JNI_OK || env == nullptr) {
        fail(error, "GetEnv failed while reading the application context");
        return nullptr;
    }
    jobject global = nullptr;
    jclass activityClass = env->GetObjectClass(activity);
    if (activityClass != nullptr) {
        jmethodID method = env->GetMethodID(activityClass, "getApplicationContext", "()Landroid/content/Context;");
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            method = nullptr;
        }
        if (method != nullptr) {
            jobject local = env->CallObjectMethod(activity, method);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                local = nullptr;
            }
            if (local != nullptr) {
                global = env->NewGlobalRef(local);
                env->DeleteLocalRef(local);
            }
        }
        env->DeleteLocalRef(activityClass);
    } else if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    if (attached) {
        vm->DetachCurrentThread();
    }
    if (global == nullptr) {
        fail(error, "activity.getApplicationContext failed");
        return nullptr;
    }
    g_loaderContext = global;
    return global;
}

bool QxrSession::Impl::createInstance(std::string* error) {
    {
        std::lock_guard<std::mutex> lock(g_loaderMu);
        if (!g_loaderReady.load(std::memory_order_relaxed)) {
            PFN_xrInitializeLoaderKHR init = nullptr;
            if (!loadProc(XR_NULL_HANDLE, "xrInitializeLoaderKHR", &init, error)) {
                return false;
            }
            // Application context 活過單一 Activity。傳 Activity 本身的話，第一次 nativeDestroy 刪掉 global ref，
            // loader 裡留的就是懸空參照。這顆 ref 不釋放，原因寫在 g_loaderContext。
            jobject appContext = loaderApplicationContext(vm, activity, error);
            if (appContext == nullptr) {
                return false;
            }
            XrLoaderInitInfoAndroidKHR loaderInfo{};
            loaderInfo.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
            loaderInfo.applicationVM = vm;
            loaderInfo.applicationContext = appContext;
            const XrResult result = init(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInfo));
            if (!xrOk(result)) {
                return failXr(error, XR_NULL_HANDLE, "xrInitializeLoaderKHR", result);
            }
            g_loaderReady.store(true, std::memory_order_relaxed);
        } else {
            // 同一個行程的 loader 只能初始化一次，之後的 Activity 沿用第一次的 VM。
            QXR_LOGI("SESSION", "xrInitializeLoaderKHR already completed in this process");
        }
    }

    uint32_t count = 0;
    XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    if (!xrOk(result)) {
        return failXr(error, XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", result);
    }
    std::vector<XrExtensionProperties> props(count);
    for (XrExtensionProperties& prop : props) {
        prop.type = XR_TYPE_EXTENSION_PROPERTIES;
        prop.next = nullptr;
    }
    if (count > 0) {
        result = xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data());
        if (!xrOk(result)) {
            return failXr(error, XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", result);
        }
        props.resize(count);
    }

    struct ExtWant {
        const char* name;
        bool required;
    };
    const ExtWant kExts[] = {
        {"XR_KHR_android_create_instance", true},
        {"XR_KHR_vulkan_enable2", true},
        {"XR_FB_display_refresh_rate", false},
        {"XR_KHR_composition_layer_cylinder", false},
        {"XR_EXT_local_floor", false},
        {"XR_KHR_convert_timespec_time", false},
        {"XR_META_touch_controller_plus", false},
    };
    std::vector<const char*> enabled;
    for (const ExtWant& ext : kExts) {
        if (extensionListed(props, ext.name)) {
            enabled.push_back(ext.name);
            continue;
        }
        if (ext.required) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "missing required instance extension %s", ext.name);
            return fail(error, buf);
        }
        QXR_LOGI("SESSION", "optional extension %s not present", ext.name);
    }
    hasRefresh = extensionListed(props, "XR_FB_display_refresh_rate");
    hasLocalFloor = extensionListed(props, "XR_EXT_local_floor");
    hasTimeSpec = extensionListed(props, "XR_KHR_convert_timespec_time");
    hasTouchPlus = extensionListed(props, "XR_META_touch_controller_plus");

    XrInstanceCreateInfoAndroidKHR androidInfo{};
    androidInfo.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
    androidInfo.applicationVM = vm;
    // Loader 用的是 Application context。這裡要當次的 Activity，runtime 才對得到這次的視窗。
    androidInfo.applicationActivity = activity;

    XrInstanceCreateInfo ci{};
    ci.type = XR_TYPE_INSTANCE_CREATE_INFO;
    ci.next = &androidInfo;
    std::snprintf(ci.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "VipleStream");
    std::snprintf(ci.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "VipleStream");
    ci.applicationInfo.applicationVersion = 1;
    ci.applicationInfo.engineVersion = 1;
    ci.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    ci.enabledExtensionNames = enabled.empty() ? nullptr : enabled.data();

    const XrVersion kApiTry[] = {XR_MAKE_VERSION(1, 1, 0), XR_MAKE_VERSION(1, 0, 0)};
    result = XR_ERROR_RUNTIME_UNAVAILABLE;
    for (XrVersion version : kApiTry) {
        ci.applicationInfo.apiVersion = version;
        result = xrCreateInstance(&ci, &instance);
        QXR_LOGI("SESSION", "xrCreateInstance api %d.%d -> XrResult=%d",
                 static_cast<int>(XR_VERSION_MAJOR(version)), static_cast<int>(XR_VERSION_MINOR(version)),
                 static_cast<int>(result));
        if (result != XR_ERROR_API_VERSION_UNSUPPORTED) {
            break;
        }
        instance = XR_NULL_HANDLE;
    }
    if (!xrOk(result) || instance == XR_NULL_HANDLE) {
        instance = XR_NULL_HANDLE;
        return failXr(error, XR_NULL_HANDLE, "xrCreateInstance", result);
    }

    std::string enabledList;
    for (size_t i = 0; i < enabled.size(); ++i) {
        if (i) enabledList += ",";
        enabledList += enabled[i];
    }
    XrInstanceProperties ip{};
    ip.type = XR_TYPE_INSTANCE_PROPERTIES;
    result = xrGetInstanceProperties(instance, &ip);
    if (!xrOk(result)) {
        logXr(false, instance, "xrGetInstanceProperties", result);
    } else {
        QXR_LOGI("SESSION", "runtime \"%s\" %u.%u.%u enabled=[%s]", ip.runtimeName,
                 static_cast<unsigned>(XR_VERSION_MAJOR(ip.runtimeVersion)),
                 static_cast<unsigned>(XR_VERSION_MINOR(ip.runtimeVersion)),
                 static_cast<unsigned>(XR_VERSION_PATCH(ip.runtimeVersion)), enabledList.c_str());
    }
    return true;
}

bool QxrSession::Impl::createSystem(std::string* error) {
    XrSystemGetInfo gi{};
    gi.type = XR_TYPE_SYSTEM_GET_INFO;
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    constexpr int kRetryMs = 500;
    constexpr int kBudgetMs = 10000;
    const auto start = std::chrono::steady_clock::now();
    auto waitedMs = [&]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    };
    int attempts = 1;
    XrResult result = xrGetSystem(instance, &gi, &systemId);
    while (result == XR_ERROR_FORM_FACTOR_UNAVAILABLE && waitedMs() + kRetryMs <= kBudgetMs) {
        if (attempts == 1) {
            QXR_LOGW("SESSION", "xrGetSystem: HMD not ready, retry every %d ms up to %d ms", kRetryMs, kBudgetMs);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kRetryMs));
        result = xrGetSystem(instance, &gi, &systemId);
        ++attempts;
    }
    if (!xrOk(result)) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "xrGetSystem(HMD) failed: XrResult=%d attempts=%d", static_cast<int>(result), attempts);
        logXr(false, instance, "xrGetSystem", result);
        if (error) *error = buf;
        return false;
    }
    if (attempts > 1) {
        QXR_LOGI("SESSION", "xrGetSystem: HMD ready after %lld ms (%d attempts)", static_cast<long long>(waitedMs()), attempts);
    }

    XrSystemProperties sp{};
    sp.type = XR_TYPE_SYSTEM_PROPERTIES;
    result = xrGetSystemProperties(instance, systemId, &sp);
    if (!xrOk(result)) {
        logXr(false, instance, "xrGetSystemProperties", result);
    } else {
        QXR_LOGI("SESSION", "system \"%s\" vendor=0x%x layers=%u", sp.systemName, sp.vendorId, sp.graphicsProperties.maxLayerCount);
    }

    uint32_t viewCount = 0;
    result = xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
    if (!xrOk(result)) {
        return failXr(error, instance, "xrEnumerateViewConfigurationViews", result);
    }
    if (viewCount == 0) {
        return fail(error, "xrEnumerateViewConfigurationViews returned no views");
    }
    std::vector<XrViewConfigurationView> viewsConfig(viewCount);
    for (XrViewConfigurationView& view : viewsConfig) {
        view.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        view.next = nullptr;
    }
    result = xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount,
                                               viewsConfig.data());
    if (!xrOk(result) || viewCount == 0) {
        return failXr(error, instance, "xrEnumerateViewConfigurationViews", result);
    }
    eyeW = viewsConfig[0].recommendedImageRectWidth;
    eyeH = viewsConfig[0].recommendedImageRectHeight;
    if (viewCount > 1 && (viewsConfig[1].recommendedImageRectWidth != eyeW || viewsConfig[1].recommendedImageRectHeight != eyeH)) {
        QXR_LOGW("SESSION", "eyes differ: %ux%u vs %ux%u", eyeW, eyeH, viewsConfig[1].recommendedImageRectWidth,
                 viewsConfig[1].recommendedImageRectHeight);
    }
    QXR_LOGI("SESSION", "recommended eye %ux%u", eyeW, eyeH);
    return true;
}

bool QxrSession::Impl::createVulkan(std::string* error) {
    PFN_xrGetVulkanGraphicsRequirements2KHR reqFn = nullptr;
    PFN_xrCreateVulkanInstanceKHR createInst = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR getDev = nullptr;
    PFN_xrCreateVulkanDeviceKHR createDev = nullptr;
    if (!loadProc(instance, "xrGetVulkanGraphicsRequirements2KHR", &reqFn, error) ||
        !loadProc(instance, "xrCreateVulkanInstanceKHR", &createInst, error) ||
        !loadProc(instance, "xrGetVulkanGraphicsDevice2KHR", &getDev, error) ||
        !loadProc(instance, "xrCreateVulkanDeviceKHR", &createDev, error)) {
        return false;
    }

    XrGraphicsRequirementsVulkan2KHR req{};
    req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR;
    XrResult result = reqFn(instance, systemId, &req);
    if (!xrOk(result)) {
        return failXr(error, instance, "xrGetVulkanGraphicsRequirements2KHR", result);
    }
    const uint32_t minApi = vkApiFromXr(req.minApiVersionSupported);
    const uint32_t maxApi = vkApiFromXr(req.maxApiVersionSupported);
    uint32_t api = VK_API_VERSION_1_1;
    if (minApi > api) {
        api = minApi;
    }
    if (maxApi != 0 && api > maxApi) {
        api = maxApi;
    }
    uint32_t loaderApi = 0;
    if (vkEnumerateInstanceVersion(&loaderApi) != VK_SUCCESS || loaderApi == 0) {
        loaderApi = VK_API_VERSION_1_0;
    }
    if (api > loaderApi) {
        api = loaderApi;
    }
    if (api < VK_API_VERSION_1_1) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "Vulkan API below 1.1 (min=0x%x max=0x%x loader=0x%x)", minApi, maxApi, loaderApi);
        return fail(error, buf);
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "VipleStream";
    app.pEngineName = "VipleStream";
    app.apiVersion = api;
    VkInstanceCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    vci.pApplicationInfo = &app;
    XrVulkanInstanceCreateInfoKHR xci{};
    xci.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR;
    xci.systemId = systemId;
    xci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xci.vulkanCreateInfo = &vci;
    VkResult vkResult = VK_SUCCESS;
    result = createInst(instance, &xci, &vkInstance, &vkResult);
    if (!xrOk(result) || vkResult != VK_SUCCESS || vkInstance == VK_NULL_HANDLE) {
        vkInstance = VK_NULL_HANDLE;
        char buf[192];
        std::snprintf(buf, sizeof(buf), "xrCreateVulkanInstanceKHR failed: XrResult=%d VkResult=%d", static_cast<int>(result),
                      static_cast<int>(vkResult));
        logXr(false, instance, "xrCreateVulkanInstanceKHR", result);
        if (vkResult != VK_SUCCESS) {
            QXR_LOGE("SESSION", "xrCreateVulkanInstanceKHR vk: VkResult=%d", static_cast<int>(vkResult));
        }
        if (error) *error = buf;
        return false;
    }

    XrVulkanGraphicsDeviceGetInfoKHR dgi{};
    dgi.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR;
    dgi.systemId = systemId;
    dgi.vulkanInstance = vkInstance;
    result = getDev(instance, &dgi, &physicalDevice);
    if (!xrOk(result) || physicalDevice == VK_NULL_HANDLE) {
        return failXr(error, instance, "xrGetVulkanGraphicsDevice2KHR", result);
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    if (props.apiVersion < VK_API_VERSION_1_1) {
        return fail(error, "GPU Vulkan API is below 1.1");
    }

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    if (familyCount > 0) {
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());
    }
    bool foundQueue = false;
    for (uint32_t i = 0; i < familyCount; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && families[i].queueCount > 0) {
            queueFamily = i;
            foundQueue = true;
            break;
        }
    }
    if (!foundQueue) {
        return fail(error, "no graphics queue family");
    }

    uint32_t extCount = 0;
    vkResult = vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, nullptr);
    if (vkResult != VK_SUCCESS) {
        return failVk(error, "vkEnumerateDeviceExtensionProperties", vkResult);
    }
    std::vector<VkExtensionProperties> deviceExts(extCount);
    if (extCount > 0) {
        vkResult = vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, deviceExts.data());
        if (vkResult != VK_SUCCESS) {
            return failVk(error, "vkEnumerateDeviceExtensionProperties", vkResult);
        }
        deviceExts.resize(extCount);
    }
    if (!deviceExtListed(deviceExts, VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME)) {
        return fail(error, "missing device extension VK_ANDROID_external_memory_android_hardware_buffer");
    }
    if (!deviceExtListed(deviceExts, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)) {
        return fail(error, "missing device extension VK_EXT_queue_family_foreign");
    }
    // 1.1 已升成核心的不放進 ppEnabledExtensionNames，否則部分驅動會拒。
    const char* kCore11[] = {
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_MAINTENANCE1_EXTENSION_NAME,
        VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
    };
    std::vector<const char*> enableExts;
    enableExts.push_back(VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
    enableExts.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
    if (api < VK_API_VERSION_1_1) {
        for (const char* name : kCore11) {
            if (!deviceExtListed(deviceExts, name)) {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "missing device extension %s", name);
                return fail(error, buf);
            }
            enableExts.push_back(name);
        }
    } else {
        for (const char* name : kCore11) {
            if (!deviceExtListed(deviceExts, name)) {
                QXR_LOGI("SESSION", "%s not advertised; using Vulkan 1.1 core", name);
            }
        }
    }

    VkPhysicalDeviceSamplerYcbcrConversionFeatures supportedYcbcr{};
    supportedYcbcr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    VkPhysicalDeviceFeatures2 queryFeatures{};
    queryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    queryFeatures.pNext = &supportedYcbcr;
    vkGetPhysicalDeviceFeatures2(physicalDevice, &queryFeatures);
    if (supportedYcbcr.samplerYcbcrConversion != VK_TRUE) {
        return fail(error, "samplerYcbcrConversion is not supported");
    }

    VkPhysicalDeviceSamplerYcbcrConversionFeatures enableYcbcr{};
    enableYcbcr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    enableYcbcr.samplerYcbcrConversion = VK_TRUE;
    VkPhysicalDeviceFeatures2 enableFeatures{};
    enableFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    enableFeatures.pNext = &enableYcbcr;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &enableFeatures;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &queueInfo;
    dci.enabledExtensionCount = static_cast<uint32_t>(enableExts.size());
    dci.ppEnabledExtensionNames = enableExts.data();
    // pEnabledFeatures 與 pNext 的 Features2 不能同時給，否則建立會被拒。
    dci.pEnabledFeatures = nullptr;

    XrVulkanDeviceCreateInfoKHR xdci{};
    xdci.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR;
    xdci.systemId = systemId;
    xdci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xdci.vulkanPhysicalDevice = physicalDevice;
    xdci.vulkanCreateInfo = &dci;
    vkResult = VK_SUCCESS;
    result = createDev(instance, &xdci, &device, &vkResult);
    // runtime 可能把 feature 鏈接到自己的 stack。這兩個結構只活在這次呼叫，返回後不再跟著 pNext 走。
    enableFeatures.pNext = &enableYcbcr;
    enableYcbcr.pNext = nullptr;
    if (!xrOk(result) || vkResult != VK_SUCCESS || device == VK_NULL_HANDLE) {
        device = VK_NULL_HANDLE;
        char buf[192];
        std::snprintf(buf, sizeof(buf), "xrCreateVulkanDeviceKHR failed: XrResult=%d VkResult=%d", static_cast<int>(result),
                      static_cast<int>(vkResult));
        logXr(false, instance, "xrCreateVulkanDeviceKHR", result);
        if (vkResult != VK_SUCCESS) {
            QXR_LOGE("SESSION", "xrCreateVulkanDeviceKHR vk: VkResult=%d", static_cast<int>(vkResult));
        }
        if (error) *error = buf;
        return false;
    }

    queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    if (queue == VK_NULL_HANDLE) {
        return fail(error, "vkGetDeviceQueue returned null");
    }
    QXR_LOGI("SESSION", "vulkan api=%u.%u queueFamily=%u gpu=\"%s\"", VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
             queueFamily, props.deviceName);
    return true;
}

bool QxrSession::Impl::createXrSession(std::string* error) {
    XrGraphicsBindingVulkan2KHR binding{};
    binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR;
    binding.instance = vkInstance;
    binding.physicalDevice = physicalDevice;
    binding.device = device;
    binding.queueFamilyIndex = queueFamily;
    binding.queueIndex = 0;
    XrSessionCreateInfo sci{};
    sci.type = XR_TYPE_SESSION_CREATE_INFO;
    sci.next = &binding;
    sci.systemId = systemId;
    XrResult result = xrCreateSession(instance, &sci, &session);
    if (!xrOk(result) || session == XR_NULL_HANDLE) {
        session = XR_NULL_HANDLE;
        return failXr(error, instance, "xrCreateSession", result);
    }

    uint32_t spaceCount = 0;
    result = xrEnumerateReferenceSpaces(session, 0, &spaceCount, nullptr);
    if (!xrOk(result)) {
        logXr(false, instance, "xrEnumerateReferenceSpaces", result);
    } else if (spaceCount > 0) {
        std::vector<XrReferenceSpaceType> spaces(spaceCount);
        result = xrEnumerateReferenceSpaces(session, spaceCount, &spaceCount, spaces.data());
        if (xrOk(result)) {
            xrNote(result, &lossPending, nullptr);
            spaces.resize(spaceCount);
            refSpaceTypes = spaces;
            std::string list;
            for (uint32_t i = 0; i < spaceCount; ++i) {
                if (i) list += ",";
                char one[16];
                std::snprintf(one, sizeof(one), "%d", static_cast<int>(spaces[i]));
                list += one;
            }
            QXR_LOGI("SESSION", "reference spaces=[%s]", list.c_str());
        } else {
            logXr(false, instance, "xrEnumerateReferenceSpaces", result);
        }
    }

    XrReferenceSpaceCreateInfo spaceInfo{};
    spaceInfo.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    result = xrCreateReferenceSpace(session, &spaceInfo, &localSpace);
    if (!xrOk(result)) {
        return failXr(error, instance, "xrCreateReferenceSpace(LOCAL)", result);
    }
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    result = xrCreateReferenceSpace(session, &spaceInfo, &viewSpace);
    if (!xrOk(result)) {
        return failXr(error, instance, "xrCreateReferenceSpace(VIEW)", result);
    }

    if (!hasRefresh) {
        return true;
    }
    PFN_xrEnumerateDisplayRefreshRatesFB enumRates = nullptr;
    PFN_xrGetDisplayRefreshRateFB getRate = nullptr;
    PFN_xrRequestDisplayRefreshRateFB requestRate = nullptr;
    if (!loadProc(instance, "xrEnumerateDisplayRefreshRatesFB", &enumRates, error) ||
        !loadProc(instance, "xrGetDisplayRefreshRateFB", &getRate, error) ||
        !loadProc(instance, "xrRequestDisplayRefreshRateFB", &requestRate, error)) {
        hasRefresh = false;
        if (error) error->clear();
        QXR_LOGW("SESSION", "display refresh procs missing; continuing without them");
        return true;
    }
    float hz = 0.0f;
    result = getRate(session, &hz);
    if (xrOk(result)) {
        currentHz = hz;
        xrNote(result, &lossPending, nullptr);
    } else {
        logXr(false, instance, "xrGetDisplayRefreshRateFB", result);
    }
    uint32_t rateCount = 0;
    result = enumRates(session, 0, &rateCount, nullptr);
    if (!xrOk(result)) {
        logXr(false, instance, "xrEnumerateDisplayRefreshRatesFB", result);
        return true;
    }
    if (rateCount > 0) {
        rates.assign(rateCount, 0.0f);
        result = enumRates(session, rateCount, &rateCount, rates.data());
        if (!xrOk(result)) {
            rates.clear();
            logXr(false, instance, "xrEnumerateDisplayRefreshRatesFB", result);
            return true;
        }
        rates.resize(rateCount);
    }
    std::string list;
    for (size_t i = 0; i < rates.size(); ++i) {
        if (i) list += ",";
        char one[32];
        std::snprintf(one, sizeof(one), "%.1f", static_cast<double>(rates[i]));
        list += one;
    }
    if (preferredHz > 0 && !rates.empty()) {
        float best = rates[0];
        const float want = static_cast<float>(preferredHz);
        for (float rate : rates) {
            const float dr = std::fabs(rate - want);
            const float db = std::fabs(best - want);
            if (dr < db || (dr == db && rate > best)) {
                best = rate;
            }
        }
        result = requestRate(session, best);
        if (!xrOk(result)) {
            logXr(false, instance, "xrRequestDisplayRefreshRateFB", result);
        } else {
            xrNote(result, &lossPending, nullptr);
        }
        QXR_LOGI("SESSION", "refresh rates=[%s] current=%.1f requested=%.1f (want %d) XrResult=%d", list.c_str(),
                 static_cast<double>(currentHz), static_cast<double>(best), preferredHz, static_cast<int>(result));
    } else {
        QXR_LOGI("SESSION", "refresh rates=[%s] current=%.1f (no request)", list.c_str(), static_cast<double>(currentHz));
    }
    return true;
}

bool QxrSession::Impl::createSwapchain(std::string* error) {
    uint32_t count = 0;
    XrResult result = xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
    if (!xrOk(result)) {
        return failXr(error, instance, "xrEnumerateSwapchainFormats", result);
    }
    std::vector<int64_t> formats(count);
    if (count > 0) {
        result = xrEnumerateSwapchainFormats(session, count, &count, formats.data());
        if (!xrOk(result)) {
            return failXr(error, instance, "xrEnumerateSwapchainFormats", result);
        }
        formats.resize(count);
    }
    const int64_t kWant[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM};
    for (int64_t want : kWant) {
        for (int64_t format : formats) {
            if (format == want) {
                swapchainFormat = want;
                break;
            }
        }
        if (swapchainFormat != 0) {
            break;
        }
    }
    if (swapchainFormat == 0) {
        std::string list;
        const size_t shown = formats.size() < 8 ? formats.size() : 8;
        for (size_t i = 0; i < shown; ++i) {
            if (i) list += ",";
            list += std::to_string(formats[i]);
        }
        char buf[256];
        std::snprintf(buf, sizeof(buf), "swapchain has neither R8G8B8A8_SRGB nor UNORM (have %s)", list.c_str());
        return fail(error, buf);
    }

    XrSwapchainCreateInfo ci{};
    ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = swapchainFormat;
    ci.sampleCount = 1;
    ci.width = static_cast<uint32_t>(streamWidth);
    ci.height = static_cast<uint32_t>(streamHeight);
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    result = xrCreateSwapchain(session, &ci, &swapchain);
    if (!xrOk(result) || swapchain == XR_NULL_HANDLE) {
        swapchain = XR_NULL_HANDLE;
        return failXr(error, instance, "xrCreateSwapchain", result);
    }
    swapWidth = ci.width;
    swapHeight = ci.height;

    uint32_t imageCount = 0;
    result = xrEnumerateSwapchainImages(swapchain, 0, &imageCount, nullptr);
    if (!xrOk(result) || imageCount == 0) {
        return failXr(error, instance, "xrEnumerateSwapchainImages", result);
    }
    std::vector<XrSwapchainImageVulkan2KHR> images(imageCount);
    for (XrSwapchainImageVulkan2KHR& image : images) {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
        image.next = nullptr;
    }
    result = xrEnumerateSwapchainImages(swapchain, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (!xrOk(result) || imageCount == 0) {
        return failXr(error, instance, "xrEnumerateSwapchainImages", result);
    }
    views.reserve(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = images[i].image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = static_cast<VkFormat>(swapchainFormat);
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        VkImageView view = VK_NULL_HANDLE;
        const VkResult vkResult = vkCreateImageView(device, &viewInfo, nullptr, &view);
        if (vkResult != VK_SUCCESS || view == VK_NULL_HANDLE) {
            return failVk(error, "vkCreateImageView(swapchain)", vkResult);
        }
        views.push_back(view);
    }
    QXR_LOGI("SESSION", "swapchain %ux%u format=%d images=%u", swapWidth, swapHeight, static_cast<int>(swapchainFormat), imageCount);
    return true;
}

bool QxrSession::Impl::createGpuObjects(std::string* error) {
    QxrVideoCreateInfo videoInfo{};
    videoInfo.physicalDevice = physicalDevice;
    videoInfo.device = device;
    videoInfo.queueFamily = queueFamily;
    videoInfo.targetFormat = static_cast<VkFormat>(swapchainFormat);
    videoInfo.streamWidth = streamWidth;
    videoInfo.streamHeight = streamHeight;
    const char* videoError = nullptr;
    if (!video.create(videoInfo, &videoError)) {
        return fail(error, videoError != nullptr ? videoError : "QxrVideo::create failed");
    }

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    VkResult vkResult = vkCreateCommandPool(device, &poolInfo, nullptr, &cmdPool);
    if (vkResult != VK_SUCCESS) {
        return failVk(error, "vkCreateCommandPool", vkResult);
    }
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = cmdPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    vkResult = vkAllocateCommandBuffers(device, &alloc, &cmd);
    if (vkResult != VK_SUCCESS) {
        return failVk(error, "vkAllocateCommandBuffers", vkResult);
    }
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkResult = vkCreateFence(device, &fenceInfo, nullptr, &fence);
    if (vkResult != VK_SUCCESS) {
        return failVk(error, "vkCreateFence", vkResult);
    }
    return true;
}

bool QxrSession::Impl::build(const QxrSessionCreateInfo& info, std::string* error) {
    if (info.vm == nullptr || info.activity == nullptr) {
        return fail(error, "activity or JavaVM is null");
    }
    if (info.streamWidth <= 0 || info.streamHeight <= 0) {
        return fail(error, "stream size is empty");
    }
    vm = info.vm;
    activity = info.activity;
    streamWidth = info.streamWidth;
    streamHeight = info.streamHeight;
    preferredHz = info.preferredHz;
    screenPose.orientation.w = 1.0f;
    quadWidthM = kScreenWidthM;
    quadHeightM = kScreenWidthM * static_cast<float>(streamHeight) / static_cast<float>(streamWidth);
    placed = false;
    needPlace = true;
    awaitingPosition = false;
    forcePlace = false;
    lossPending = false;
    loggedLocate = false;
    yaw = 0.0f;
    endedNotified = false;
    fencePending = false;
    sessionBegun = false;
    sessionState = XR_SESSION_STATE_UNKNOWN;
    beginFails = 0;
    rates.clear();
    currentHz = 0.0f;
    loggedPeriodFallback = false;
    blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    eyeW = 0;
    eyeH = 0;
    swapchainFormat = 0;
    hasLocalFloor = false;
    hasTimeSpec = false;
    hasTouchPlus = false;
    refSpaceTypes.clear();
    profileNoted = false;
    syncFails = 0;
    viewLocateFails = 0;
    viewsLocked.store(false, std::memory_order_release);
    trackIsStage = false;
    trackSpaceName = "none";
    timeConvert = nullptr;
    projReady.store(false, std::memory_order_release);
    frameAlive.store(false, std::memory_order_release);
    armRequest.store(false, std::memory_order_release);
    loggedProjection = false;
    loggedUntimed = false;
    loggedFadeMiss = false;
    loggedLoadMiss = false;
    pcvrStale = 0;
    renderedFrame = -2;
    untimedGen = -3;
    lastReadyNs = 0;
    framePickNs = 0;
    displayClientNs = 0;
    statProj = 0;
    statResent = 0;
    statMetaHit = 0;
    statMetaMiss = 0;
    statFade = 0;
    statLoading = 0;
    statRenderUs = 0;
    statRenderN = 0;
    statEndFail = 0;
    pending = {};
    shown = {};
    {
        std::lock_guard<std::mutex> lock(timingMu);
        frameMeta = nullptr;
    }

    if (!createInstance(error) || !createSystem(error) || !createVulkan(error) || !createXrSession(error) || !createSwapchain(error) ||
        !createGpuObjects(error)) {
        return false;
    }
    created = true;
    if (error) {
        error->clear();
    }
    QXR_LOGI("SESSION", "created %dx%d quad %.2fx%.2f m", streamWidth, streamHeight, static_cast<double>(quadWidthM),
             static_cast<double>(quadHeightM));
    return true;
}

void QxrSession::Impl::teardown() {
    const bool noisy = instance != XR_NULL_HANDLE || device != VK_NULL_HANDLE || session != XR_NULL_HANDLE;
    if (noisy) {
        QXR_LOGI("SESSION", "teardown");
    }
    if (device != VK_NULL_HANDLE) {
        const VkResult result = vkDeviceWaitIdle(device);
        if (result != VK_SUCCESS) {
            QXR_LOGE("SESSION", "vkDeviceWaitIdle failed: VkResult=%d", static_cast<int>(result));
        }
    }
    // 1 Hz 還會來抄 latch。先停收，拆 video 時才不會同時記樣本。
    latch.unbind();
    {
        std::lock_guard<std::mutex> lock(timingMu);
        frameMeta = nullptr;
    }
    destroyProjOutput();
    video.destroy();
    for (VkImageView view : views) {
        if (view != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
            vkDestroyImageView(device, view, nullptr);
        }
    }
    views.clear();
    if (swapchain != XR_NULL_HANDLE) {
        const XrResult result = xrDestroySwapchain(swapchain);
        if (!xrOk(result)) {
            logXr(false, instance, "xrDestroySwapchain", result);
        }
        swapchain = XR_NULL_HANDLE;
    }
    // §QUEST-PCVR：action space 與追蹤空間要在 xrDestroySession 之前拆。2D 的 LOCAL／VIEW 留到下面，quad 仍用那兩個。
    // 先把視角旗標清掉，viewConfig 才不會在 pose source 拆掉之後還進去讀遊玩區。
    {
        std::lock_guard<std::mutex> lock(viewMu);
        viewsLocked.store(false, std::memory_order_release);
    }
    pcvr.store(false, std::memory_order_release);
    if (poseSource) {
        poseSource->setSessionRunning(false);
    }
    poseSource.reset();
    controllers.reset();
    if (trackSpace != XR_NULL_HANDLE) {
        const XrResult result = xrDestroySpace(trackSpace);
        if (!xrOk(result)) {
            logXr(false, instance, "xrDestroySpace(track)", result);
        }
        trackSpace = XR_NULL_HANDLE;
    }
    trackIsStage = false;
    trackSpaceName = "none";
    timeConvert = nullptr;
    if (viewSpace != XR_NULL_HANDLE) {
        const XrResult result = xrDestroySpace(viewSpace);
        if (!xrOk(result)) {
            logXr(false, instance, "xrDestroySpace(VIEW)", result);
        }
        viewSpace = XR_NULL_HANDLE;
    }
    if (localSpace != XR_NULL_HANDLE) {
        const XrResult result = xrDestroySpace(localSpace);
        if (!xrOk(result)) {
            logXr(false, instance, "xrDestroySpace(LOCAL)", result);
        }
        localSpace = XR_NULL_HANDLE;
    }
    if (session != XR_NULL_HANDLE) {
        const XrResult result = xrDestroySession(session);
        if (!xrOk(result)) {
            logXr(false, instance, "xrDestroySession", result);
        }
        session = XR_NULL_HANDLE;
    }
    sessionBegun = false;
    if (cmdPool != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, cmdPool, nullptr);
    }
    cmdPool = VK_NULL_HANDLE;
    cmd = VK_NULL_HANDLE;
    if (fence != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroyFence(device, fence, nullptr);
    }
    fence = VK_NULL_HANDLE;
    fencePending = false;
    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    queue = VK_NULL_HANDLE;
    physicalDevice = VK_NULL_HANDLE;
    if (vkInstance != VK_NULL_HANDLE) {
        vkDestroyInstance(vkInstance, nullptr);
        vkInstance = VK_NULL_HANDLE;
    }
    if (instance != XR_NULL_HANDLE) {
        const XrResult result = xrDestroyInstance(instance);
        instance = XR_NULL_HANDLE;
        if (!xrOk(result)) {
            QXR_LOGE("SESSION", "xrDestroyInstance failed: XrResult=%d", static_cast<int>(result));
        }
    }
    created = false;
    {
        std::lock_guard<std::mutex> lock(mu);
        onEnded = nullptr;
    }
}

void QxrSession::Impl::tryBegin() {
    if (sessionBegun || session == XR_NULL_HANDLE) {
        return;
    }
    XrSessionBeginInfo info{};
    info.type = XR_TYPE_SESSION_BEGIN_INFO;
    info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    const XrResult result = xrBeginSession(session, &info);
    if (!xrOk(result)) {
        ++beginFails;
        if (beginFails == 1 || (beginFails % 20) == 0) {
            logXr(true, instance, "xrBeginSession", result);
        }
        return;
    }
    xrNote(result, &lossPending, nullptr);
    beginFails = 0;
    sessionBegun = true;
    // 上行的 sample() 在這之前回 false，不會對還沒 begin 的 session locate。
    if (poseSource) {
        poseSource->setSessionRunning(true);
    }
    QXR_LOGI("SESSION", "xrBeginSession");
}

void QxrSession::Impl::logBlendModes() {
    // 列舉失敗就維持 OPAQUE。Quest 有列；沒列的 runtime 用它列出來的第一個。
    blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    uint32_t count = 0;
    XrResult result = xrEnumerateEnvironmentBlendModes(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr);
    if (!xrOk(result)) {
        logXr(true, instance, "xrEnumerateEnvironmentBlendModes", result);
        return;
    }
    std::vector<XrEnvironmentBlendMode> modes(count);
    if (count > 0) {
        result = xrEnumerateEnvironmentBlendModes(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, modes.data());
        if (!xrOk(result)) {
            logXr(true, instance, "xrEnumerateEnvironmentBlendModes", result);
            return;
        }
        modes.resize(count);
    }
    for (XrEnvironmentBlendMode mode : modes) {
        if (mode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE) {
            return;
        }
    }
    if (!modes.empty()) {
        blendMode = modes[0];
        QXR_LOGW("FRAME", "runtime did not list OPAQUE blend; xrEndFrame uses enumerated mode %d", static_cast<int>(blendMode));
        return;
    }
    QXR_LOGE("FRAME", "runtime listed no environment blend modes; xrEndFrame keeps OPAQUE");
}

bool QxrSession::Impl::poll(int* reason) {
    for (;;) {
        XrEventDataBuffer event{};
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
        const XrResult result = xrPollEvent(instance, &event);
        if (result == XR_EVENT_UNAVAILABLE) {
            return true;
        }
        if (!xrOk(result)) {
            logXr(true, instance, "xrPollEvent", result);
            if (result == XR_ERROR_INSTANCE_LOST) {
                *reason = kQxrEndInstanceLoss;
                return false;
            }
            if (result == XR_ERROR_SESSION_LOST) {
                *reason = kQxrEndLossPending;
                return false;
            }
            return true;
        }
        xrNote(result, &lossPending, nullptr);
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            QXR_LOGW("FRAME", "instance loss pending");
            *reason = kQxrEndInstanceLoss;
            return false;
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            auto* changed = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
            if (session != XR_NULL_HANDLE && changed->session != session) {
                break;
            }
            QXR_LOGI("SESSION", "state %s -> %s", sessionStateName(sessionState), sessionStateName(changed->state));
            sessionState = changed->state;
            if (changed->state == XR_SESSION_STATE_READY) {
                tryBegin();
            } else if (changed->state == XR_SESSION_STATE_STOPPING) {
                if (sessionBegun && session != XR_NULL_HANDLE) {
                    // 先擋上行。xrEndSession 之後的 xrLocateSpace 是 XR_ERROR_SESSION_NOT_RUNNING。
                    if (poseSource) {
                        poseSource->setSessionRunning(false);
                    }
                    const XrResult endResult = xrEndSession(session);
                    if (!xrOk(endResult)) {
                        logXr(true, instance, "xrEndSession", endResult);
                    } else {
                        xrNote(endResult, &lossPending, nullptr);
                    }
                    sessionBegun = false;
                }
            } else if (changed->state == XR_SESSION_STATE_EXITING) {
                *reason = kQxrEndExiting;
                return false;
            } else if (changed->state == XR_SESSION_STATE_LOSS_PENDING) {
                *reason = kQxrEndLossPending;
                return false;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
            auto* changed = reinterpret_cast<XrEventDataDisplayRefreshRateChangedFB*>(&event);
            currentHz = changed->toDisplayRefreshRate;
            // 晚到的擴充事件不能把已經採用的週期後備蓋掉。兩邊差超過 3% 時仍以週期為準。
            const float hz = effectiveRefreshHz();
            if (viewsLocked.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lock(viewMu);
                if (viewsLocked.load(std::memory_order_relaxed)) {
                    viewCache.refreshHz = hz;
                }
            }
            QXR_LOGI("FRAME", "refresh %.1f -> %.1f Hz", static_cast<double>(changed->fromDisplayRefreshRate),
                     static_cast<double>(changed->toDisplayRefreshRate));
            if (hz > 0.0f && std::fabs(hz - currentHz) > 0.05f) {
                QXR_LOGI("FRAME", "view refresh uses predictedDisplayPeriod %.1f Hz", static_cast<double>(hz));
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
            auto* changed = reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&event);
            QXR_LOGI("FRAME", "reference space %d changed poseValid=%d", static_cast<int>(changed->referenceSpaceType),
                     changed->poseValid ? 1 : 0);
            needPlace = true;
            forcePlace = true;
            loggedLocate = false;
            if (pcvr.load(std::memory_order_acquire) && poseSource && trackIsStage) {
                poseSource->refreshPlayArea();
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
            // session 還沒 begin 時查 profile 會是 SESSION_NOT_RUNNING。等第一次 sync 成功再補一次。
            if (pcvr.load(std::memory_order_acquire) && controllers && sessionBegun) {
                controllers->onProfileChanged();
                profileNoted = true;
            }
            break;
        case XR_TYPE_EVENT_DATA_EVENTS_LOST:
            QXR_LOGW("FRAME", "runtime event queue overflowed");
            break;
        default:
            break;
        }
        if (lossPending) {
            *reason = kQxrEndLossPending;
            return false;
        }
    }
}

void QxrSession::Impl::tryPlace(XrTime displayTime) {
    XrSpaceLocation loc{};
    loc.type = XR_TYPE_SPACE_LOCATION;
    const XrResult result = xrLocateSpace(viewSpace, localSpace, displayTime, &loc);
    if (!xrOk(result)) {
        if (!loggedLocate) {
            loggedLocate = true;
            logXr(true, instance, "xrLocateSpace", result);
        }
        return;
    }
    xrNote(result, &lossPending, nullptr);
    const bool orientationValid = (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
    const bool positionValid = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
    if (!orientationValid) {
        if (!loggedLocate) {
            loggedLocate = true;
            QXR_LOGW("FRAME", "head orientation not valid yet flags=0x%x", static_cast<unsigned>(loc.locationFlags));
        }
        return;
    }
    // 已經用原點擺過、位置還沒回來：不要每幀跟著頭轉。空間基準變了才強制再擺一次。
    if (placed && awaitingPosition && !positionValid && !forcePlace) {
        return;
    }
    loggedLocate = false;
    const XrVector3f head = positionValid ? loc.pose.position : XrVector3f{0.0f, 0.0f, 0.0f};
    const XrVector3f forward = rotateQuat(loc.pose.orientation, {0.0f, 0.0f, -1.0f});
    const float horizontal = std::sqrt(forward.x * forward.x + forward.z * forward.z);
    if (horizontal > 1.0e-3f) {
        yaw = std::atan2(-forward.x / horizontal, -forward.z / horizontal);
    }
    screenPose.orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
    screenPose.position = {head.x - std::sin(yaw) * kScreenDistanceM, head.y, head.z - std::cos(yaw) * kScreenDistanceM};
    placed = true;
    forcePlace = false;
    if (positionValid) {
        awaitingPosition = false;
        needPlace = false;
    } else {
        // 原點、高度 0，螢幕中心仍在前方同樣的距離。位置第一次有效時再擺一次。
        awaitingPosition = true;
        needPlace = true;
    }
    QXR_LOGI("SESSION", "screen placed yaw=%.1f deg head=(%.2f, %.2f, %.2f) center=(%.2f, %.2f, %.2f) size=%.2fx%.2f m positionValid=%d",
             static_cast<double>(yaw * 180.0f / 3.14159265f), static_cast<double>(head.x),
             static_cast<double>(head.y), static_cast<double>(head.z),
             static_cast<double>(screenPose.position.x), static_cast<double>(screenPose.position.y),
             static_cast<double>(screenPose.position.z), static_cast<double>(quadWidthM), static_cast<double>(quadHeightM),
             positionValid ? 1 : 0);
}

bool QxrSession::Impl::waitGpu(int* fatalReason) {
    if (!fencePending) {
        return true;
    }
    VkResult result = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) {
        QXR_LOGE("FRAME", "vkWaitForFences failed: VkResult=%d", static_cast<int>(result));
        fencePending = false;
        if (fatalReason != nullptr) {
            *fatalReason = kQxrEndLossPending;
        }
        return false;
    }
    result = vkResetFences(device, 1, &fence);
    if (result != VK_SUCCESS) {
        QXR_LOGE("FRAME", "vkResetFences failed: VkResult=%d", static_cast<int>(result));
        fencePending = false;
        if (fatalReason != nullptr) {
            *fatalReason = kQxrEndLossPending;
        }
        return false;
    }
    fencePending = false;
    return true;
}

bool QxrSession::Impl::recordAndSubmit(VkImageView target, uint32_t width, uint32_t height) {
    VkResult result = vkResetCommandBuffer(cmd, 0);
    if (result != VK_SUCCESS) {
        QXR_LOGE("FRAME", "vkResetCommandBuffer failed: VkResult=%d", static_cast<int>(result));
        return false;
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(cmd, &begin);
    if (result != VK_SUCCESS) {
        QXR_LOGE("FRAME", "vkBeginCommandBuffer failed: VkResult=%d", static_cast<int>(result));
        return false;
    }
    // render pass 的 initial／final layout 已是 COLOR_ATTACHMENT_OPTIMAL，跟 swapchain acquire／release 相同，這裡不再轉。
    const bool drew = video.renderTo(cmd, target, width, height, video.sourceUv());
    result = vkEndCommandBuffer(cmd);
    if (result != VK_SUCCESS) {
        QXR_LOGE("FRAME", "vkEndCommandBuffer failed: VkResult=%d", static_cast<int>(result));
        return false;
    }
    if (!drew) {
        QXR_LOGE("FRAME", "QxrVideo::renderTo failed");
        return false;
    }
    // 只有這條 frame thread 碰這條 VkQueue。xrBeginFrame／xrEndFrame／xrAcquire／xrRelease 跟 submit 在同一條執行緒上前後發生，沒有第二條執行緒。
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    result = vkQueueSubmit(queue, 1, &submit, fence);
    if (result != VK_SUCCESS) {
        QXR_LOGE("FRAME", "vkQueueSubmit failed: VkResult=%d", static_cast<int>(result));
        return false;
    }
    fencePending = true;
    return true;
}

bool QxrSession::Impl::drawFrame(const XrFrameState& frame, bool* sentLayer, int* fatalReason) {
    *sentLayer = false;
    if (needPlace) {
        tryPlace(frame.predictedDisplayTime);
    }
    // consumeLatest 可能拆掉上一幀還掛在指令裡的 VkImage，要等這張 fence 才算做完。
    if (!waitGpu(fatalReason)) {
        return false;
    }
    video.consumeLatest();

    XrSwapchainImageAcquireInfo acquire{};
    acquire.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
    uint32_t index = 0;
    const XrResult acquireResult = xrAcquireSwapchainImage(swapchain, &acquire, &index);
    if (!xrOk(acquireResult)) {
        // 沒有 acquire 成功就不能 release，否則 runtime 回 XR_ERROR_CALL_ORDER_INVALID。
        logXr(true, instance, "xrAcquireSwapchainImage", acquireResult);
        if (acquireResult == XR_ERROR_SESSION_LOST) {
            *fatalReason = kQxrEndLossPending;
            return false;
        }
        if (acquireResult == XR_ERROR_INSTANCE_LOST) {
            *fatalReason = kQxrEndInstanceLoss;
            return false;
        }
        return true;
    }
    xrNote(acquireResult, &lossPending, nullptr);

    XrSwapchainImageWaitInfo wait{};
    wait.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
    wait.timeout = XR_INFINITE_DURATION;
    const XrResult waitResult = xrWaitSwapchainImage(swapchain, &wait);
    // XR_TIMEOUT_EXPIRED 是成功碼，但這張還沒就緒，不能畫。acquire 已成功就一定要 release。
    const bool imageReady = xrOk(waitResult) && waitResult != XR_TIMEOUT_EXPIRED;
    if (!xrOk(waitResult)) {
        logXr(true, instance, "xrWaitSwapchainImage", waitResult);
    } else {
        xrNote(waitResult, &lossPending, nullptr);
        if (waitResult == XR_TIMEOUT_EXPIRED) {
            QXR_LOGW("FRAME", "xrWaitSwapchainImage returned XR_TIMEOUT_EXPIRED");
        }
    }
    bool submitted = false;
    if (imageReady) {
        if (index >= views.size() || views[index] == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE) {
            QXR_LOGE("FRAME", "swapchain image index %u out of range (count=%u)", index, static_cast<unsigned>(views.size()));
        } else {
            submitted = recordAndSubmit(views[index], swapWidth, swapHeight);
        }
    }

    XrSwapchainImageReleaseInfo release{};
    release.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
    const XrResult releaseResult = xrReleaseSwapchainImage(swapchain, &release);
    if (!xrOk(releaseResult)) {
        logXr(true, instance, "xrReleaseSwapchainImage", releaseResult);
        return true;
    }
    xrNote(releaseResult, &lossPending, nullptr);
    if (submitted && placed) {
        *sentLayer = true;
    }
    return true;
}

void QxrSession::Impl::heartbeat() {
    const auto now = std::chrono::steady_clock::now();
    video.pumpStats();
    if (now - statWindow < std::chrono::seconds(10)) {
        return;
    }
    const double periodMs = static_cast<double>(lastPeriodNs) / 1.0e6;
    QXR_LOGI("FRAME", "loops=%llu shouldRender=%llu layers=%llu period=%.2f ms hz=%.1f (10s)",
             static_cast<unsigned long long>(statLoops), static_cast<unsigned long long>(statRenders),
             static_cast<unsigned long long>(statLayers), periodMs, static_cast<double>(currentHz));
    statLoops = 0;
    statRenders = 0;
    statLayers = 0;
    if (projReady.load(std::memory_order_acquire)) {
        logProj();
        logRate10s();
    }
    statWindow = now;
}

int QxrSession::Impl::frameLoop() {
    statWindow = std::chrono::steady_clock::now();
    statLoops = 0;
    statRenders = 0;
    statLayers = 0;
    statProj = 0;
    statResent = 0;
    statMetaHit = 0;
    statMetaMiss = 0;
    statFade = 0;
    statLoading = 0;
    statRenderUs = 0;
    statRenderN = 0;
    statEndFail = 0;
    logBlendModes();
    while (true) {
        const int armFatal = pumpArm();
        if (armFatal != 0) {
            return armFatal;
        }
        heartbeat();
        ++statLoops;
        if (stopFlag.load(std::memory_order_acquire)) {
            return 0;
        }
        int reason = 0;
        if (!poll(&reason)) {
            return reason;
        }
        if (lossPending) {
            return kQxrEndLossPending;
        }
        if (!sessionBegun) {
            if (sessionState == XR_SESSION_STATE_READY) {
                tryBegin();
            }
            if (lossPending) {
                return kQxrEndLossPending;
            }
            // 還沒 begin 就不會畫。排空 reader，解碼器才有輸出緩衝。這裡不匯入，也不碰快取。
            video.drainReader();
            std::unique_lock<std::mutex> lock(mu);
            cv.wait_for(lock, std::chrono::milliseconds(50), [&] {
                return stopFlag.load(std::memory_order_acquire) || armRequest.load(std::memory_order_acquire);
            });
            continue;
        }

        if (pcvr.load(std::memory_order_acquire)) {
            // §QUEST-PCVR：action state 只在 xrSyncActions 更新。放在 xrWaitFrame 之前，上行才不會送上一格的按鍵。
            const int syncReason = syncPcvrActions();
            if (syncReason != 0) {
                return syncReason;
            }
        }

        XrFrameWaitInfo wait{};
        wait.type = XR_TYPE_FRAME_WAIT_INFO;
        XrFrameState frame{};
        frame.type = XR_TYPE_FRAME_STATE;
        const XrResult waitResult = xrWaitFrame(session, &wait, &frame);
        if (!xrOk(waitResult)) {
            logXr(true, instance, "xrWaitFrame", waitResult);
            if (waitResult == XR_ERROR_SESSION_LOST) {
                return kQxrEndLossPending;
            }
            if (waitResult == XR_ERROR_INSTANCE_LOST) {
                return kQxrEndInstanceLoss;
            }
            video.drainReader();
            std::unique_lock<std::mutex> lock(mu);
            cv.wait_for(lock, std::chrono::milliseconds(50), [&] {
                return stopFlag.load(std::memory_order_acquire) || armRequest.load(std::memory_order_acquire);
            });
            continue;
        }
        xrNote(waitResult, &lossPending, nullptr);
        lastPeriodNs = frame.predictedDisplayPeriod;
        if (pcvr.load(std::memory_order_acquire) && poseSource) {
            poseSource->onFrameTiming(frame.predictedDisplayTime, frame.predictedDisplayPeriod);
            captureViewConfig(frame.predictedDisplayTime);
        }
        if (projReady.load(std::memory_order_acquire)) {
            captureFrameClock(frame.predictedDisplayTime, frame.predictedDisplayPeriod);
            latch.noteDisplay(frame.predictedDisplayTime, frame.predictedDisplayPeriod);
        }

        XrFrameBeginInfo begin{};
        begin.type = XR_TYPE_FRAME_BEGIN_INFO;
        const XrResult beginResult = xrBeginFrame(session, &begin);
        if (!xrOk(beginResult)) {
            logXr(true, instance, "xrBeginFrame", beginResult);
            video.drainReader();
            if (beginResult == XR_ERROR_SESSION_LOST) {
                return kQxrEndLossPending;
            }
            if (beginResult == XR_ERROR_INSTANCE_LOST) {
                return kQxrEndInstanceLoss;
            }
            // begin 失敗就不能 end。
            continue;
        }
        bool discarded = false;
        xrNote(beginResult, &lossPending, &discarded);
        if (discarded) {
            QXR_LOGW("FRAME", "xrBeginFrame returned XR_FRAME_DISCARDED");
        }

        bool sentLayer = false;
        int fatalReason = 0;
        bool fatal = false;
        const bool output = projReady.load(std::memory_order_acquire);
        PcvrSubmit pcvrOut{};
        if (!output) {
            if (!discarded && frame.shouldRender) {
                ++statRenders;
                if (!drawFrame(frame, &sentLayer, &fatalReason)) {
                    fatal = true;
                }
            } else {
                // 不畫的時候也要把 reader 排到只剩最新一張。fence 可能還沒到，不能拆快取。
                video.drainReader();
            }
        } else if (!discarded && frame.shouldRender) {
            ++statRenders;
            if (!drawPcvr(frame, &pcvrOut, &fatalReason)) {
                fatal = true;
            }
        } else {
            // 不畫的時候也要把 reader 排到只剩最新一張。fence 可能還沒到，不能拆快取。
            video.drainReader();
        }

        XrCompositionLayerQuad quad{};
        quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        const XrCompositionLayerBaseHeader* headers[2] = {};
        uint32_t layerCount = 0;
        if (!output) {
            if (sentLayer) {
                ++statLayers;
                // 不設 XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT。清除色是不透明的，交給 runtime 直接覆蓋。
                quad.layerFlags = 0;
                quad.space = localSpace;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain = swapchain;
                quad.subImage.imageRect.offset = {0, 0};
                quad.subImage.imageRect.extent.width = static_cast<int32_t>(swapWidth);
                quad.subImage.imageRect.extent.height = static_cast<int32_t>(swapHeight);
                quad.subImage.imageArrayIndex = 0;
                quad.pose = screenPose;
                quad.size.width = quadWidthM;
                quad.size.height = quadHeightM;
                headers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
                layerCount = 1;
            }
        } else {
            if (pcvrOut.projection) {
                headers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&pcvrOut.proj);
            }
            if (pcvrOut.fade && layerCount < 2) {
                headers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&pcvrOut.fadeLayer);
            }
            if (pcvrOut.loading && layerCount < 2) {
                headers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&pcvrOut.loadLayer);
            }
            statLayers += layerCount;
            // §QUEST-PCVR：影像是這條 frame thread 畫的，xrEndFrame 前不會有另一條 thread release 新的 swapchain，所以不做 relatch。
            const int64_t period = frame.predictedDisplayPeriod > 0 ? frame.predictedDisplayPeriod : 0;
            latch.tick(monoNowNs(), static_cast<uint64_t>(period));
        }
        XrFrameEndInfo end{};
        end.type = XR_TYPE_FRAME_END_INFO;
        end.displayTime = frame.predictedDisplayTime;
        end.environmentBlendMode = blendMode;
        end.layerCount = layerCount;
        end.layers = layerCount != 0 ? headers : nullptr;
        const XrResult endResult = xrEndFrame(session, &end);
        if (!xrOk(endResult)) {
            logXr(true, instance, "xrEndFrame", endResult);
            if (output && statEndFail != ~0ull) {
                ++statEndFail;
            }
            if (endResult == XR_ERROR_SESSION_LOST) {
                return kQxrEndLossPending;
            }
            if (endResult == XR_ERROR_INSTANCE_LOST) {
                return kQxrEndInstanceLoss;
            }
        } else {
            xrNote(endResult, &lossPending, nullptr);
        }
        if (fatal) {
            return fatalReason;
        }
        if (lossPending) {
            return kQxrEndLossPending;
        }
    }
}

int QxrSession::Impl::pumpArm() {
    if (!armRequest.load(std::memory_order_acquire)) {
        return 0;
    }
    int32_t width = 0;
    int32_t height = 0;
    {
        std::lock_guard<std::mutex> lock(armMu);
        if (!armRequest.load(std::memory_order_acquire)) {
            return 0;
        }
        width = armW;
        height = armH;
        armRequest.store(false, std::memory_order_release);
    }
    int fatal = 0;
    if (!waitGpu(&fatal)) {
        finishArm(false, "vkWaitForFences failed before projection arm");
        return fatal;
    }
    bool ok = false;
    std::string err;
    try {
        ok = createProjOutput(width, height, &err);
    } catch (const std::exception& ex) {
        ok = false;
        err = ex.what();
    }
    if (!ok) {
        destroyProjOutput();
    }
    finishArm(ok, err);
    return 0;
}

void QxrSession::Impl::finishArm(bool ok, const std::string& error) {
    std::lock_guard<std::mutex> lock(armMu);
    armOk = ok;
    armError = error;
    if (ok) {
        armedW = armW;
        armedH = armH;
        projReady.store(true, std::memory_order_release);
    }
    armDone = true;
    armCv.notify_all();
}

void QxrSession::Impl::abandonArm() {
    std::lock_guard<std::mutex> lock(armMu);
    if (!armDone && (armWaiting || armRequest.load(std::memory_order_acquire))) {
        armRequest.store(false, std::memory_order_release);
        armOk = false;
        if (armError.empty()) {
            armError = "frame thread stopped";
        }
        armDone = true;
    }
    armCv.notify_all();
}

bool QxrSession::Impl::ensureSolidPass(std::string* error) {
    if (solidPass != VK_NULL_HANDLE) {
        return true;
    }
    if (device == VK_NULL_HANDLE || swapchainFormat == 0) {
        return fail(error, "solid render pass before the swapchain format is chosen");
    }
    // §QUEST-PCVR：swapchain 交出來已是 COLOR_ATTACHMENT_OPTIMAL。用 transfer clear 會把 layout 弄亂，所以跟影像同一種 loadOp。
    VkAttachmentDescription attachment{};
    attachment.format = static_cast<VkFormat>(swapchainFormat);
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference reference{};
    reference.attachment = 0;
    reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    // 出去不用 BOTTOM_OF_PIPE：它當 destination 不會擋住後面較早的階段。
    // 同一條 command buffer 裡的 copy 要 TRANSFER read。swapchain 交給 runtime 之後，合成器以 SAMPLED 在 fragment shader 讀。
    // 外部依賴要同時涵蓋這兩種，否則取樣可能讀到還沒寫完的像素。
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &attachment;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = dependencies;
    const VkResult result = vkCreateRenderPass(device, &info, nullptr, &solidPass);
    if (result != VK_SUCCESS || solidPass == VK_NULL_HANDLE) {
        solidPass = VK_NULL_HANDLE;
        return failVk(error, "vkCreateRenderPass(solid)", result);
    }
    return true;
}

void QxrSession::Impl::destroyChain(ColorChain* chain) {
    if (chain == nullptr) {
        return;
    }
    if (device != VK_NULL_HANDLE) {
        for (VkFramebuffer framebuffer : chain->framebuffers) {
            if (framebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, framebuffer, nullptr);
            }
        }
        for (VkImageView view : chain->views) {
            if (view != VK_NULL_HANDLE) {
                vkDestroyImageView(device, view, nullptr);
            }
        }
    }
    chain->framebuffers.clear();
    chain->views.clear();
    if (chain->swapchain != XR_NULL_HANDLE) {
        const XrResult result = xrDestroySwapchain(chain->swapchain);
        if (!xrOk(result)) {
            logXr(false, instance, "xrDestroySwapchain", result);
        }
        chain->swapchain = XR_NULL_HANDLE;
    }
    chain->width = 0;
    chain->height = 0;
    chain->ready = false;
}

bool QxrSession::Impl::createChain(uint32_t width, uint32_t height, bool withFramebuffer, ColorChain* chain, std::string* error) {
    if (chain == nullptr || session == XR_NULL_HANDLE || device == VK_NULL_HANDLE) {
        return fail(error, "createChain before the session device exists");
    }
    destroyChain(chain);
    XrSwapchainCreateInfo ci{};
    ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = swapchainFormat;
    ci.sampleCount = 1;
    ci.width = width;
    ci.height = height;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XrResult result = xrCreateSwapchain(session, &ci, &chain->swapchain);
    if (!xrOk(result) || chain->swapchain == XR_NULL_HANDLE) {
        chain->swapchain = XR_NULL_HANDLE;
        return failXr(error, instance, "xrCreateSwapchain", result);
    }
    chain->width = width;
    chain->height = height;
    uint32_t imageCount = 0;
    result = xrEnumerateSwapchainImages(chain->swapchain, 0, &imageCount, nullptr);
    if (!xrOk(result) || imageCount == 0) {
        destroyChain(chain);
        return failXr(error, instance, "xrEnumerateSwapchainImages", result);
    }
    std::vector<XrSwapchainImageVulkan2KHR> images(imageCount);
    for (XrSwapchainImageVulkan2KHR& image : images) {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
        image.next = nullptr;
    }
    result = xrEnumerateSwapchainImages(chain->swapchain, imageCount, &imageCount,
                                        reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (!xrOk(result) || imageCount == 0) {
        destroyChain(chain);
        return failXr(error, instance, "xrEnumerateSwapchainImages", result);
    }
    chain->views.reserve(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = images[i].image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = static_cast<VkFormat>(swapchainFormat);
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        VkImageView view = VK_NULL_HANDLE;
        const VkResult vkResult = vkCreateImageView(device, &viewInfo, nullptr, &view);
        if (vkResult != VK_SUCCESS || view == VK_NULL_HANDLE) {
            destroyChain(chain);
            return failVk(error, "vkCreateImageView(projection)", vkResult);
        }
        chain->views.push_back(view);
    }
    if (!withFramebuffer) {
        return true;
    }
    if (solidPass == VK_NULL_HANDLE) {
        destroyChain(chain);
        return fail(error, "solid render pass is missing");
    }
    chain->framebuffers.reserve(chain->views.size());
    for (VkImageView view : chain->views) {
        VkFramebufferCreateInfo fb{};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = solidPass;
        fb.attachmentCount = 1;
        fb.pAttachments = &view;
        fb.width = width;
        fb.height = height;
        fb.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        const VkResult vkResult = vkCreateFramebuffer(device, &fb, nullptr, &framebuffer);
        if (vkResult != VK_SUCCESS || framebuffer == VK_NULL_HANDLE) {
            destroyChain(chain);
            return failVk(error, "vkCreateFramebuffer(solid)", vkResult);
        }
        chain->framebuffers.push_back(framebuffer);
    }
    return true;
}

void QxrSession::Impl::destroyProjOutput() {
    unbindRateHandler();
    // §QUEST-PCVR：清色若還在 GPU 上，先等完再拆 framebuffer。這條不在每圈的熱路徑。
    // wait idle 之後 fence 是 signaled，不 reset 的話下一筆 submit 會被拒。
    if (device != VK_NULL_HANDLE && fence != VK_NULL_HANDLE &&
        (fencePending || projChain.swapchain != XR_NULL_HANDLE || fadeChain.swapchain != XR_NULL_HANDLE ||
         loadingChain.swapchain != XR_NULL_HANDLE)) {
        const VkResult result = vkDeviceWaitIdle(device);
        if (result != VK_SUCCESS) {
            QXR_LOGE("PROJ", "vkDeviceWaitIdle failed: VkResult=%d", static_cast<int>(result));
        }
        const VkResult reset = vkResetFences(device, 1, &fence);
        if (reset != VK_SUCCESS) {
            QXR_LOGE("PROJ", "vkResetFences failed: VkResult=%d", static_cast<int>(reset));
        }
        fencePending = false;
    }
    // 投影的 image view 等一下就拆。快取裡的 framebuffer 還指著它們，先拆掉，否則下一張 view 會對到舊的 framebuffer。
    video.clearTargetFramebuffers();
    destroyChain(&projChain);
    destroyChain(&fadeChain);
    destroyChain(&loadingChain);
    if (solidPass != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, solidPass, nullptr);
    }
    solidPass = VK_NULL_HANDLE;
    projReady.store(false, std::memory_order_release);
    pending = {};
    shown = {};
    renderedFrame = -2;
    loggedProjection = false;
    lastReadyNs = 0;
}

bool QxrSession::Impl::recordClear(VkFramebuffer framebuffer, uint32_t width, uint32_t height, float r, float g, float b, float a) {
    VkResult result = vkResetCommandBuffer(cmd, 0);
    if (result != VK_SUCCESS) {
        QXR_LOGE("PROJ", "vkResetCommandBuffer failed: VkResult=%d", static_cast<int>(result));
        return false;
    }
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(cmd, &beginInfo);
    if (result != VK_SUCCESS) {
        QXR_LOGE("PROJ", "vkBeginCommandBuffer failed: VkResult=%d", static_cast<int>(result));
        return false;
    }
    VkClearValue clear{};
    clear.color.float32[0] = r;
    clear.color.float32[1] = g;
    clear.color.float32[2] = b;
    clear.color.float32[3] = a;
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = solidPass;
    pass.framebuffer = framebuffer;
    pass.renderArea.extent = VkExtent2D{width, height};
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(cmd);
    result = vkEndCommandBuffer(cmd);
    if (result != VK_SUCCESS) {
        QXR_LOGE("PROJ", "vkEndCommandBuffer failed: VkResult=%d", static_cast<int>(result));
        return false;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    result = vkQueueSubmit(queue, 1, &submit, fence);
    if (result != VK_SUCCESS) {
        QXR_LOGE("PROJ", "vkQueueSubmit failed: VkResult=%d", static_cast<int>(result));
        return false;
    }
    fencePending = true;
    return true;
}

bool QxrSession::Impl::paintSolid(ColorChain* chain, float r, float g, float b, float a, int* fatalReason) {
    if (chain == nullptr || chain->swapchain == XR_NULL_HANDLE || solidPass == VK_NULL_HANDLE) {
        return false;
    }
    if (chain->ready) {
        return true;
    }
    if (!waitGpu(fatalReason)) {
        return false;
    }
    XrSwapchainImageAcquireInfo acquire{};
    acquire.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
    uint32_t index = 0;
    const XrResult acquireResult = xrAcquireSwapchainImage(chain->swapchain, &acquire, &index);
    if (!xrOk(acquireResult)) {
        logXr(true, instance, "xrAcquireSwapchainImage", acquireResult);
        if (fatalReason != nullptr && acquireResult == XR_ERROR_SESSION_LOST) {
            *fatalReason = kQxrEndLossPending;
        }
        if (fatalReason != nullptr && acquireResult == XR_ERROR_INSTANCE_LOST) {
            *fatalReason = kQxrEndInstanceLoss;
        }
        return false;
    }
    xrNote(acquireResult, &lossPending, nullptr);

    XrSwapchainImageWaitInfo wait{};
    wait.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
    wait.timeout = XR_INFINITE_DURATION;
    const XrResult waitResult = xrWaitSwapchainImage(chain->swapchain, &wait);
    const bool imageReady = xrOk(waitResult) && waitResult != XR_TIMEOUT_EXPIRED;
    if (!xrOk(waitResult)) {
        logXr(true, instance, "xrWaitSwapchainImage", waitResult);
    } else {
        xrNote(waitResult, &lossPending, nullptr);
        if (waitResult == XR_TIMEOUT_EXPIRED) {
            QXR_LOGW("PROJ", "xrWaitSwapchainImage returned XR_TIMEOUT_EXPIRED");
        }
    }
    bool submitted = false;
    if (imageReady) {
        if (index >= chain->framebuffers.size() || chain->framebuffers[index] == VK_NULL_HANDLE) {
            QXR_LOGE("PROJ", "solid image index %u out of range (count=%u)", index, static_cast<unsigned>(chain->framebuffers.size()));
        } else {
            submitted = recordClear(chain->framebuffers[index], chain->width, chain->height, r, g, b, a);
        }
    }
    XrSwapchainImageReleaseInfo release{};
    release.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
    const XrResult releaseResult = xrReleaseSwapchainImage(chain->swapchain, &release);
    if (!xrOk(releaseResult)) {
        logXr(true, instance, "xrReleaseSwapchainImage", releaseResult);
        return false;
    }
    xrNote(releaseResult, &lossPending, nullptr);
    if (!submitted) {
        return false;
    }
    // §QUEST-PCVR：清色只做一次。等 GPU 做完再放下一圈去共用同一條 command buffer。
    if (!waitGpu(fatalReason)) {
        return false;
    }
    chain->ready = true;
    return true;
}

bool QxrSession::Impl::createProjOutput(int32_t packedW, int32_t packedH, std::string* error) {
    destroyProjOutput();
    if (!ensureSolidPass(error)) {
        return false;
    }
    if (!createChain(static_cast<uint32_t>(packedW), static_cast<uint32_t>(packedH), false, &projChain, error)) {
        destroyProjOutput();
        return false;
    }
    if (!createChain(8, 8, true, &fadeChain, error) || !createChain(8, 8, true, &loadingChain, error)) {
        destroyProjOutput();
        return false;
    }
    int fatal = 0;
    paintSolid(&fadeChain, 0.0f, 0.0f, 0.0f, 0.6f, &fatal);
    if (fatal != 0) {
        destroyProjOutput();
        return fail(error, "fade clear failed");
    }
    fatal = 0;
    paintSolid(&loadingChain, 0.07f, 0.07f, 0.08f, 1.0f, &fatal);
    if (fatal != 0) {
        destroyProjOutput();
        return fail(error, "loading clear failed");
    }
    QXR_LOGI("PROJ", "projection swapchain %ux%u images=%u fade=%d loading=%d", projChain.width, projChain.height,
             static_cast<unsigned>(projChain.views.size()), fadeChain.ready ? 1 : 0, loadingChain.ready ? 1 : 0);
    bindRateHandler();
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void QxrSession::Impl::captureFrameClock(XrTime predicted, int64_t periodNs) {
    // §QUEST-PCVR：跟 Frame pcvrAfterWaitFrame 一樣，現在與換算用同一筆 CLOCK_MONOTONIC。沒有換算就用兩個顯示週期。
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    framePickNs = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
    int64_t ahead = periodNs > 0 ? periodNs * 2 : 0;
    if (timeConvert != nullptr && instance != XR_NULL_HANDLE) {
        XrTime xrNow = 0;
        if (xrOk(timeConvert(instance, &ts, &xrNow)) && predicted > xrNow) {
            ahead = static_cast<int64_t>(predicted - xrNow);
        }
    }
    if (ahead < 0) {
        ahead = 0;
    }
    displayClientNs = framePickNs + static_cast<uint64_t>(ahead);
}

bool QxrSession::Impl::copyViewEyes(XrPosef eye[2], XrFovf fov[2]) const {
    if (!viewsLocked.load(std::memory_order_acquire)) {
        return false;
    }
    QxrViewConfig cfg;
    {
        std::lock_guard<std::mutex> lock(viewMu);
        if (!viewsLocked.load(std::memory_order_relaxed)) {
            return false;
        }
        cfg = viewCache;
    }
    eye[0].position = {cfg.leftPosX, cfg.leftPosY, cfg.leftPosZ};
    eye[0].orientation = {cfg.leftRotX, cfg.leftRotY, cfg.leftRotZ, cfg.leftRotW};
    fov[0] = {cfg.leftAngleLeft, cfg.leftAngleRight, cfg.leftAngleUp, cfg.leftAngleDown};
    eye[1].position = {cfg.rightPosX, cfg.rightPosY, cfg.rightPosZ};
    eye[1].orientation = {cfg.rightRotX, cfg.rightRotY, cfg.rightRotZ, cfg.rightRotW};
    fov[1] = {cfg.rightAngleLeft, cfg.rightAngleRight, cfg.rightAngleUp, cfg.rightAngleDown};
    return true;
}

bool QxrSession::Impl::composeMeta(const VIPLE_VR_FRAME_META& meta, Staged* staged) const {
    XrPosef eye[2]{};
    XrFovf fov[2]{};
    if (!copyViewEyes(eye, fov)) {
        return false;
    }
    XrPosef head{};
    head.orientation = {meta.renderRot[0], meta.renderRot[1], meta.renderRot[2], meta.renderRot[3]};
    head.position = {meta.renderPos[0], meta.renderPos[1], meta.renderPos[2]};
    // §QUEST-PCVR：解包端已把 renderPos 換成公尺。VIPLE_VR_FF_POS_RELATIVE 定義了，Frame 的 buildProjection 沒有套，這裡也不套。
    // 沒有注視點 quad。Frame 在 xrcontext.cpp 2647 把 layersMode 強制成 0，眼睛位置留在算圖當下。
    // 順序跟 5548–5551：方向是 q_head * q_eye，位置是頭再加上轉進世界的眼偏移。OpenXR 右手座標、+Y 朝上。
    for (int e = 0; e < 2; ++e) {
        const XrVector3f off = rotateQuat(head.orientation, eye[e].position);
        staged->eye[e].pose.orientation = quatMul(head.orientation, eye[e].orientation);
        staged->eye[e].pose.position = {head.position.x + off.x, head.position.y + off.y, head.position.z + off.z};
        staged->eye[e].fov = fov[e];
    }
    staged->headPos = head.position;
    staged->headRot = head.orientation;
    return true;
}

void QxrSession::Impl::classifyPending(bool count) {
    if (!viewsLocked.load(std::memory_order_acquire)) {
        return;
    }
    pending.classified = true;
    VIPLE_VR_FRAME_META meta{};
    uint64_t sinkNs = 0;
    bool usable = false;
    if (pending.frameNumber >= 0) {
        QxrFrameMeta* src = nullptr;
        {
            std::lock_guard<std::mutex> lock(timingMu);
            src = frameMeta;
        }
        if (src != nullptr && src->lookup(pending.frameNumber, &meta, &sinkNs)) {
            pending.sinkNs = sinkNs;
            // 沒有 VIPLE_VR_FF_POSE_VALID 的新畫面不能送進 projection。顯示時刻的頭貼在舊畫面上，ATW 會補成 0°。
            usable = meta.present != 0 && (meta.vrFlags & VIPLE_VR_FF_POSE_VALID) != 0 &&
                     renderPoseUsable(meta.renderRot, meta.renderPos) && composeMeta(meta, &pending);
        }
    }
    if (usable) {
        pending.fromMeta = true;
        pending.poseValid = true;
        pending.echoSampleId = meta.echoSampleId;
        if (count) {
            ++statMetaHit;
        }
        return;
    }
    pending.fromMeta = false;
    pending.poseValid = false;
    pending.echoSampleId = 0;
    if (count) {
        ++statMetaMiss;
        latch.noteMetaMiss();
    }
}

bool QxrSession::Impl::renderProjection(int* fatalReason, uint32_t* renderUs) {
    if (renderUs != nullptr) {
        *renderUs = 0;
    }
    if (projChain.swapchain == XR_NULL_HANDLE) {
        return false;
    }
    XrSwapchainImageAcquireInfo acquire{};
    acquire.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
    uint32_t index = 0;
    const XrResult acquireResult = xrAcquireSwapchainImage(projChain.swapchain, &acquire, &index);
    if (!xrOk(acquireResult)) {
        logXr(true, instance, "xrAcquireSwapchainImage", acquireResult);
        if (fatalReason != nullptr && acquireResult == XR_ERROR_SESSION_LOST) {
            *fatalReason = kQxrEndLossPending;
        }
        if (fatalReason != nullptr && acquireResult == XR_ERROR_INSTANCE_LOST) {
            *fatalReason = kQxrEndInstanceLoss;
        }
        return false;
    }
    xrNote(acquireResult, &lossPending, nullptr);

    XrSwapchainImageWaitInfo wait{};
    wait.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
    wait.timeout = XR_INFINITE_DURATION;
    const XrResult waitResult = xrWaitSwapchainImage(projChain.swapchain, &wait);
    const bool imageReady = xrOk(waitResult) && waitResult != XR_TIMEOUT_EXPIRED;
    if (!xrOk(waitResult)) {
        logXr(true, instance, "xrWaitSwapchainImage", waitResult);
    } else {
        xrNote(waitResult, &lossPending, nullptr);
        if (waitResult == XR_TIMEOUT_EXPIRED) {
            QXR_LOGW("PROJ", "xrWaitSwapchainImage returned XR_TIMEOUT_EXPIRED");
        }
    }
    bool submitted = false;
    uint32_t micros = 0;
    if (imageReady) {
        if (index >= projChain.views.size() || projChain.views[index] == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE) {
            QXR_LOGE("PROJ", "projection image index %u out of range (count=%u)", index, static_cast<unsigned>(projChain.views.size()));
        } else {
            // §QUEST-PCVR：等這張 fence 會把顯示整格往後推。CPU 包住 submit，當 render 時間。
            // 打包影像的有效範圍是 sourceUv（crop），左右眼用 subImage 切，不把 uv 切半。
            const auto started = std::chrono::steady_clock::now();
            submitted = recordAndSubmit(projChain.views[index], projChain.width, projChain.height);
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
            if (elapsed > 0) {
                micros = elapsed > 0xffffffffll ? 0xffffffffu : static_cast<uint32_t>(elapsed);
            }
        }
    }
    XrSwapchainImageReleaseInfo release{};
    release.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
    const XrResult releaseResult = xrReleaseSwapchainImage(projChain.swapchain, &release);
    if (!xrOk(releaseResult)) {
        logXr(true, instance, "xrReleaseSwapchainImage", releaseResult);
        return false;
    }
    xrNote(releaseResult, &lossPending, nullptr);
    if (!submitted) {
        return false;
    }
    if (renderUs != nullptr) {
        *renderUs = micros;
    }
    return true;
}

void QxrSession::Impl::fillProjection(const Staged& src, PcvrSubmit* out) {
    const int32_t fullW = static_cast<int32_t>(projChain.width);
    // §QUEST-PCVR：奇數寬時左右各用 w/2，中間那一像素不進任一眼。跟 Frame xrcontext.cpp 5556 一樣。
    const int32_t half = fullW / 2;
    const int32_t height = static_cast<int32_t>(projChain.height);
    out->proj.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    out->proj.next = nullptr;
    out->proj.layerFlags = 0;
    out->proj.space = trackSpace;
    out->proj.viewCount = 2;
    out->proj.views = out->views;
    for (int e = 0; e < 2; ++e) {
        XrCompositionLayerProjectionView& view = out->views[e];
        view.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        view.next = nullptr;
        view.pose = src.eye[e].pose;
        view.fov = src.eye[e].fov;
        view.subImage.swapchain = projChain.swapchain;
        view.subImage.imageArrayIndex = 0;
        view.subImage.imageRect.offset = {e == 0 ? 0 : half, 0};
        view.subImage.imageRect.extent = {half, height};
    }
    out->projection = true;
    if (!loggedProjection) {
        loggedProjection = true;
        QXR_LOGI("PROJ",
                 "pcvr projection: %dx%d per eye from %dx%d SBS, space=%s, meta=%d pose (%.3f, %.3f, %.3f | %.3f, %.3f, %.3f, %.3f) echo=%u",
                 half, height, fullW, height, trackSpaceName != nullptr ? trackSpaceName : "none", src.fromMeta ? 1 : 0,
                 static_cast<double>(src.headPos.x), static_cast<double>(src.headPos.y), static_cast<double>(src.headPos.z),
                 static_cast<double>(src.headRot.x), static_cast<double>(src.headRot.y), static_cast<double>(src.headRot.z),
                 static_cast<double>(src.headRot.w), src.echoSampleId);
    }
}

void QxrSession::Impl::fillFade(PcvrSubmit* out) {
    XrCompositionLayerQuad& quad = out->fadeLayer;
    quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    quad.next = nullptr;
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = viewSpace;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = fadeChain.swapchain;
    quad.subImage.imageRect.offset = {0, 0};
    quad.subImage.imageRect.extent = {8, 8};
    quad.subImage.imageArrayIndex = 0;
    quad.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    quad.pose.position = {0.0f, 0.0f, -0.3f};
    quad.size = {3.0f, 3.0f};
    out->fade = true;
}

void QxrSession::Impl::fillLoading(PcvrSubmit* out) {
    // §QUEST-PCVR：擺法跟 2D quad 相同。顏色用沒有影像時的清除色，不再把過期的雙眼影像畫上去。
    XrCompositionLayerQuad& quad = out->loadLayer;
    quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    quad.next = nullptr;
    quad.layerFlags = 0;
    quad.space = localSpace;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = loadingChain.swapchain;
    quad.subImage.imageRect.offset = {0, 0};
    quad.subImage.imageRect.extent = {8, 8};
    quad.subImage.imageArrayIndex = 0;
    quad.pose = screenPose;
    quad.size = {quadWidthM, quadHeightM};
    out->loading = true;
}

uint64_t QxrSession::Impl::ageMs(uint64_t noticedNs) const {
    if (noticedNs == 0 || framePickNs < noticedNs) {
        return 0;
    }
    return (framePickNs - noticedNs) / 1000000ull;
}

void QxrSession::Impl::noteStale(int state, uint64_t age) {
    if (state < 0 || state > 3) {
        state = 3;
    }
    if (state == pcvrStale) {
        return;
    }
    static const char* const kName[] = {"no-video", "live", "fade", "loading"};
    QXR_LOGI("PROJ", "pcvr video state %s -> %s (age %llu ms)", kName[pcvrStale], kName[state],
             static_cast<unsigned long long>(age));
    pcvrStale = state;
}

void QxrSession::Impl::logProj() {
    const uint64_t known = statMetaHit + statMetaMiss;
    const double pct = known > 0 ? 100.0 * static_cast<double>(statMetaHit) / static_cast<double>(known) : 0.0;
    const double avgMs = statRenderN > 0 ? (static_cast<double>(statRenderUs) / static_cast<double>(statRenderN)) / 1000.0 : 0.0;
    QXR_LOGI("PROJ",
             "presented=%llu resent=%llu metaHit=%.1f%% (%llu/%llu) fade=%llu loading=%llu renderToAvg=%.2f ms endFrameFail=%llu",
             static_cast<unsigned long long>(statProj), static_cast<unsigned long long>(statResent), pct,
             static_cast<unsigned long long>(statMetaHit), static_cast<unsigned long long>(known),
             static_cast<unsigned long long>(statFade), static_cast<unsigned long long>(statLoading), avgMs,
             static_cast<unsigned long long>(statEndFail));
    statProj = 0;
    statResent = 0;
    statMetaHit = 0;
    statMetaMiss = 0;
    statFade = 0;
    statLoading = 0;
    statRenderUs = 0;
    statRenderN = 0;
    statEndFail = 0;
}

void QxrSession::Impl::bindRateHandler() {
    if (!kQxrRateAuto || rateBound) {
        return;
    }
    rateAuto = xrrate::AutoRate();
    rateDiv.store(1, std::memory_order_relaxed);
    rateReply.store(0, std::memory_order_relaxed);
    rateDisabledLogged = false;
    rateSignalPending = false;
    rateLastReadyNs = 0;
    ratePrevRenderUs = 0;
    rateSinceNew = 0;
    rateUnknownSlots = 0;
    ratePkts = 0;
    ratePktsInit = false;
    rateLogDowns = 0;
    rateLogUps = 0;
    QxrVrMsg::setRateHandler([this](int div, bool accepted, uint16_t seq) { noteRateApplied(div, accepted, seq); });
    rateBound = true;
    QXR_LOGI("RATE", "automatic frame rate on: drops to half while the network is unstable");
}

void QxrSession::Impl::unbindRateHandler() {
    if (!rateBound) {
        return;
    }
    QxrVrMsg::setRateHandler(nullptr);
    rateBound = false;
}

void QxrSession::Impl::noteRateApplied(int div, bool accepted, uint16_t seq) {
    div = div >= 2 ? 2 : 1;
    if (accepted) {
        LiVrSetStreamDiv(static_cast<uint32_t>(div));
        rateDiv.store(div, std::memory_order_release);
    }
    const uint32_t word = 0x80000000u | (accepted ? 0x01000000u : 0u) | (static_cast<uint32_t>(div) << 16) |
                          static_cast<uint32_t>(seq);
    rateReply.store(word, std::memory_order_release);
    QXR_LOGI("RATE", "host %s div=%d (seq=%u): stream %s", accepted ? "applied" : "refused", div,
             static_cast<unsigned>(seq), div == 2 ? "half rate, each frame shown twice" : "full rate");
}

void QxrSession::Impl::applyRateReply() {
    uint64_t nowUs = framePickNs / 1000ull;
    // AutoRate 把 0 當成還沒開始，每一格都會重設。單調時鐘不該是 0；擋一下避免穩定期永遠重來。
    if (nowUs == 0) {
        nowUs = 1;
    }
    const int divBefore = rateAuto.div();
    const uint32_t reply = rateReply.exchange(0, std::memory_order_acquire);
    if ((reply & 0x80000000u) == 0u) {
        return;
    }
    rateAuto.onApplied(static_cast<int>((reply >> 16) & 0xffu), (reply & 0x01000000u) != 0u, nowUs);
    if (rateAuto.div() != divBefore) {
        rateSinceNew = 0;
    }
}

void QxrSession::Impl::pcvrRateOnSlot(bool newFrame, uint32_t periodUs, bool unadopted, uint64_t renderPlusFenceUs,
                                      bool decodeQueueEmpty, bool packetStall) {
    uint64_t nowUs = framePickNs / 1000ull;
    if (nowUs == 0) {
        nowUs = 1;
    }
    const bool headsetLate = !newFrame && (unadopted || (periodUs > 0 && renderPlusFenceUs > periodUs));
    const bool network = !newFrame && !headsetLate && decodeQueueEmpty && packetStall;
    if (newFrame) {
        rateSinceNew = 0;
    }
    else {
        ++rateSinceNew;
        const int div = rateAuto.div() >= 2 ? 2 : 1;
        // §QUEST-RATE：對不上頭盔，也對不上「佇列空且收包停頓」。AutoRate 沒有第三個原因，這種格仍會進它的網路計數。
        if (rateSinceNew >= static_cast<uint32_t>(div) && !headsetLate && !network) {
            ++rateUnknownSlots;
        }
    }
    const bool repairReleased = newFrame && LiGetVrRepairQuietMs() < 50u;
    // §QUEST-RATE：HostCadence 要 90 kHz Present 時戳。QxrFrameMeta 只有幀號、vrFlags、sink 的 CLOCK_MONOTONIC，
    // RTP 時戳沒寫進環。不拿 sink 時間冒充 Present，hostSlots 固定 0，host 沒出畫這類標成未知。
    rateAuto.onSlot(newFrame, nowUs, headsetLate, repairReleased, 0u);
    const xrrate::AutoRate::Request request = rateAuto.poll(nowUs);
    if (request.div != 0) {
        VIPLE_VR_TLV_RATE req{};
        req.div = static_cast<uint8_t>(request.div);
        req.reason = request.reason == xrrate::AutoRate::Reason::Up ? VIPLE_VR_RATE_REASON_AUTO_UP
                                                                     : VIPLE_VR_RATE_REASON_AUTO_DOWN;
        req.seq = rateAuto.seq();
        uint8_t tlv[2 + sizeof(req)];
        tlv[0] = VIPLE_VR_C2S_RATE;
        tlv[1] = static_cast<uint8_t>(sizeof(req));
        std::memcpy(tlv + 2, &req, sizeof(req));
        const int rc = LiSendVrMessage(tlv, static_cast<int>(sizeof(tlv)), true);
        const char* why = request.reason == xrrate::AutoRate::Reason::Up            ? "network stable, trying full rate"
                           : request.reason == xrrate::AutoRate::Reason::ProbeFail   ? "full rate did not hold, back to half"
                           : request.reason == xrrate::AutoRate::Reason::HeadsetDown ? "headset cannot keep up (frames arrive but decode late)"
                                                                                     : "network unstable";
        QXR_LOGI("RATE",
                 "request div=%d seq=%u%s (%s): short repeats last 15 s=%u (headset %u), last 5 s=%u stalls=%u lateHost=%u "
                 "(15 s, not counted); next try after %llu s%s",
                 request.div, static_cast<unsigned>(req.seq), request.retry ? " (resend)" : "", why, rateAuto.recentSum(3),
                 rateAuto.recentHeadsetSum(3), rateAuto.lastWindow().shortRep, rateAuto.lastWindow().stalls,
                 rateAuto.recentHostSum(3), static_cast<unsigned long long>(rateAuto.upDwellUs() / 1000000ull),
                 rc == 0 ? "" : " - send failed");
    }
    if (rateAuto.disabled() && !rateDisabledLogged) {
        rateDisabledLogged = true;
        QXR_LOGW("RATE", "host did not answer the rate request - automatic frame rate is off for this session (host too old?)");
    }
}

void QxrSession::Impl::logRate10s() {
    if (!kQxrRateAuto || !rateBound) {
        return;
    }
    const uint32_t downs = rateAuto.downs();
    const uint32_t ups = rateAuto.ups();
    const uint32_t down10 = downs >= rateLogDowns ? downs - rateLogDowns : downs;
    const uint32_t up10 = ups >= rateLogUps ? ups - rateLogUps : ups;
    rateLogDowns = downs;
    rateLogUps = ups;
    const uint32_t unknown = rateUnknownSlots;
    rateUnknownSlots = 0;
    QXR_LOGI("RATE",
             "div=%d down10s=%u up10s=%u short15s=%u downs=%u ups=%u probeFails=%u outageSkips=%u%s lateNet=%llu "
             "lateHeadset=%llu lateHost=%llu fnBreaks=unknown hostCause=unknown unclassified=%u",
             rateAuto.div(), down10, up10, rateAuto.recentSum(3), downs, ups, rateAuto.probeFails(), rateAuto.outageSkips(),
             rateAuto.disabled() ? " off" : "", static_cast<unsigned long long>(rateAuto.totalNetSlots()),
             static_cast<unsigned long long>(rateAuto.totalHeadsetSlots()),
             static_cast<unsigned long long>(rateAuto.totalHostSlots()), unknown);
}

bool QxrSession::Impl::drawPcvr(const XrFrameState& frame, PcvrSubmit* out, int* fatalReason) {
    if (out == nullptr) {
        return true;
    }
    *out = {};
    const auto fenceT0 = std::chrono::steady_clock::now();
    if (!waitGpu(fatalReason)) {
        return false;
    }
    uint64_t fenceUs = 0;
    const auto fenceElapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - fenceT0).count();
    if (fenceElapsed > 0) {
        fenceUs = static_cast<uint64_t>(fenceElapsed);
    }
    // §QUEST-RATE：fence 在下一格開頭才等完。上一格 renderTo 的 CPU 時間要加這一格的等待，才是「上一圈畫完沒有」。
    const uint64_t renderPlusFenceUs = ratePrevRenderUs + fenceUs;
    const int64_t numBefore = video.hasFrame() ? video.frameNumber() : -3;
    video.consumeLatest();
    bool unadopted = false;
    bool decodeQueueEmpty = true;
    bool packetStall = false;
    int divForStale = 1;
    if (kQxrRateAuto && rateBound) {
        const int64_t numAfter = video.hasFrame() ? video.frameNumber() : -3;
        const uint64_t readyAfter = video.lastImageAvailableNs();
        // §QUEST-RATE：沒有時間戳的影像幀號一直是 -1，分不出這格有沒有換張，不能把它當成還沒採用。
        const bool gotNew = numAfter >= 0 && numAfter != numBefore;
        if (gotNew) {
            if (readyAfter != 0) {
                rateLastReadyNs = readyAfter;
                rateSignalPending = false;
            }
            else {
                rateSignalPending = true;
            }
        }
        else if (numAfter >= 0 && rateSignalPending && readyAfter != 0 && readyAfter != rateLastReadyNs) {
            rateLastReadyNs = readyAfter;
            rateSignalPending = false;
        }
        else if (numAfter >= 0 && !rateSignalPending && readyAfter != 0 && readyAfter != rateLastReadyNs) {
            unadopted = true;
        }
        // DIRECT_SUBMIT 時這條佇列通常是空的。AImageReader 裡還沒採用的畫面另看 unadopted。
        decodeQueueEmpty = !unadopted && LiGetPendingVideoFrames() <= 0;
        if (const RTP_VIDEO_STATS* stats = LiGetRTPVideoStats()) {
            if (!ratePktsInit) {
                ratePktsInit = true;
                ratePkts = stats->packetCountVideo;
            }
            else {
                packetStall = stats->packetCountVideo == ratePkts;
                ratePkts = stats->packetCountVideo;
            }
        }
        applyRateReply();
        divForStale = std::max(1, rateAuto.div());
    }
    // §QUEST-RATE：分頻 2 時沒有新畫面的那一格本來就會重送上一張。門檻不跟著乘，半速會比全速更早被當成過期。
    const uint64_t liveMs = kPcvrLiveMs * static_cast<uint64_t>(divForStale);
    const uint64_t fadeMs = kPcvrFadeMs * static_cast<uint64_t>(divForStale);
    if (video.hasFrame()) {
        int64_t number = video.frameNumber();
        bool fresh = false;
        if (number < 0) {
            if (!loggedUntimed) {
                loggedUntimed = true;
                QXR_LOGW("PROJ", "video frame has no timestamp; pose lookup disabled");
            }
            number = --untimedGen;
            fresh = true;
        } else if (number != pending.frameNumber) {
            fresh = true;
        }
        if (fresh) {
            const uint64_t noticed = framePickNs;
            pending = {};
            pending.frameNumber = number;
            pending.noticedNs = noticed;
            classifyPending(true);
        } else if (!pending.poseValid) {
            classifyPending(!pending.classified);
        }
    }

    bool renderedThis = false;
    bool renderBroken = false;
    uint32_t renderUs = 0;
    if (pending.poseValid && pending.frameNumber != renderedFrame && video.hasFrame() && ageMs(pending.noticedNs) < fadeMs) {
        if (!renderProjection(fatalReason, &renderUs)) {
            if (fatalReason != nullptr && *fatalReason != 0) {
                return false;
            }
            // §QUEST-PCVR：acquire 成功就一定 release。沒畫成的那張變成最新影像，這一格不能再拿上一張的 pose 送 projection。
            renderBroken = true;
        } else {
            uint32_t frameId = 0;
            if (pending.frameNumber >= 0 && pending.frameNumber <= 0xffffffffll) {
                frameId = static_cast<uint32_t>(pending.frameNumber);
            }
            const uint64_t ready = video.lastImageAvailableNs();
            bool haveSlack = false;
            int32_t slackUs = 0;
            if (ready != 0 && ready != lastReadyNs) {
                lastReadyNs = ready;
                slackUs = clampI32(static_cast<int64_t>(framePickNs / 1000ull) - static_cast<int64_t>(ready / 1000ull));
                haveSlack = true;
            }
            bool haveDecode = false;
            uint32_t decodeUs = 0;
            if (haveSlack && pending.sinkNs != 0 && ready >= pending.sinkNs) {
                const uint64_t deltaUs = (ready - pending.sinkNs) / 1000ull;
                // §QUEST-PCVR：sink 在送進解碼器之前。這段含排隊，不是純 codec；超過 500 ms 當成對不上。
                if (deltaUs < 500000ull) {
                    haveDecode = true;
                    decodeUs = deltaUs > 0xffffffffull ? 0xffffffffu : static_cast<uint32_t>(deltaUs);
                }
            }
            latch.onNewFrame(frameId, haveSlack, slackUs, true, renderUs, pending.fromMeta, pending.echoSampleId, displayClientNs,
                             haveDecode, decodeUs);
            ++statRenderN;
            statRenderUs += renderUs;
            shown = pending;
            renderedFrame = pending.frameNumber;
            renderedThis = true;
        }
    }

    if (kQxrRateAuto && rateBound) {
        if (renderedThis) {
            ratePrevRenderUs = renderUs;
        }
        else {
            ratePrevRenderUs = 0;
        }
        uint32_t periodUs = 0;
        if (frame.predictedDisplayPeriod > 0) {
            const int64_t us = frame.predictedDisplayPeriod / 1000;
            periodUs = us > 0xffffffffll ? 0xffffffffu : static_cast<uint32_t>(us);
        }
        pcvrRateOnSlot(renderedThis, periodUs, unadopted, renderPlusFenceUs, decodeQueueEmpty, packetStall);
    }

    if (!renderBroken && shown.poseValid) {
        const uint64_t age = ageMs(shown.noticedNs);
        if (age < fadeMs) {
            fillProjection(shown, out);
            if (renderedThis) {
                ++statProj;
            } else {
                ++statResent;
            }
            if (age >= liveMs) {
                if (!fadeChain.ready && !paintSolid(&fadeChain, 0.0f, 0.0f, 0.0f, 0.6f, fatalReason)) {
                    if (fatalReason != nullptr && *fatalReason != 0) {
                        return false;
                    }
                }
                if (fadeChain.ready) {
                    fillFade(out);
                    ++statFade;
                    noteStale(2, age);
                } else {
                    if (!loggedFadeMiss) {
                        loggedFadeMiss = true;
                        QXR_LOGW("PROJ", "fade quad unavailable");
                    }
                    noteStale(1, age);
                }
            } else {
                noteStale(1, age);
            }
            return true;
        }
    }

    noteStale(3, shown.poseValid ? ageMs(shown.noticedNs) : 0);
    if (!loadingChain.ready && !paintSolid(&loadingChain, 0.07f, 0.07f, 0.08f, 1.0f, fatalReason)) {
        if (fatalReason != nullptr && *fatalReason != 0) {
            return false;
        }
    }
    if (needPlace) {
        tryPlace(frame.predictedDisplayTime);
    }
    if (loadingChain.ready && placed) {
        fillLoading(out);
        ++statLoading;
    } else if (!loggedLoadMiss) {
        loggedLoadMiss = true;
        QXR_LOGW("PROJ", "loading quad not sent");
    }
    return true;
}

bool QxrSession::Impl::armPcvrOutput(int32_t packedW, int32_t packedH, std::string* error) {
    if (!pcvr.load(std::memory_order_acquire)) {
        return fail(error, "armPcvrOutput before enablePcvr");
    }
    if (packedW < 2 || packedH < 1 || packedW > kPackedMax || packedH > kPackedMax) {
        return fail(error, "packed size is invalid");
    }
    {
        std::lock_guard<std::mutex> life(mu);
        if (threadState != 1) {
            return fail(error, "armPcvrOutput before the frame thread is running");
        }
    }
    {
        std::lock_guard<std::mutex> lock(armMu);
        if (projReady.load(std::memory_order_acquire) && armedW == packedW && armedH == packedH) {
            if (error != nullptr) {
                error->clear();
            }
            return true;
        }
        if (projReady.load(std::memory_order_acquire)) {
            return fail(error, "projection swapchain size is already armed");
        }
        armW = packedW;
        armH = packedH;
        armError.clear();
        armOk = false;
        armDone = false;
        armWaiting = true;
        armRequest.store(true, std::memory_order_release);
    }
    cv.notify_all();
    std::unique_lock<std::mutex> lock(armMu);
    armCv.wait(lock, [&] { return armDone || !frameAlive.load(std::memory_order_acquire); });
    armWaiting = false;
    if (!armDone || !armOk) {
        const std::string message = armError.empty() ? std::string("frame thread stopped") : armError;
        return fail(error, message.c_str());
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void QxrSession::Impl::bindPcvrTiming(QxrFrameMeta* meta, QxrSampleHistory* history) {
    {
        std::lock_guard<std::mutex> lock(timingMu);
        frameMeta = meta;
    }
    latch.bind(history);
}

void QxrSession::Impl::unbindPcvrTiming() {
    // §QUEST-PCVR：上行停了影像可能還在。meta 指標留著，pose 才不會改用當下的頭、畫面才不會漂。
    latch.unbind();
}

bool QxrSession::Impl::copyPcvrTiming(QxrClientTimingFill* out) {
    return latch.copyTiming(out);
}

bool QxrSession::Impl::enablePcvr(std::string* error) {
    std::lock_guard<std::mutex> lock(mu);
    if (!created || session == XR_NULL_HANDLE || viewSpace == XR_NULL_HANDLE) {
        return fail(error, "enablePcvr before create");
    }
    if (threadState == 1) {
        return fail(error, "enablePcvr while the frame thread is running");
    }
    if (sessionBegun) {
        return fail(error, "enablePcvr after xrBeginSession");
    }
    if (pcvr.load(std::memory_order_relaxed) && poseSource && controllers) {
        if (error) {
            error->clear();
        }
        return true;
    }

    if (trackSpace == XR_NULL_HANDLE) {
        const bool haveStage = refSpaceListed(refSpaceTypes, XR_REFERENCE_SPACE_TYPE_STAGE);
        const bool haveFloor = hasLocalFloor && refSpaceListed(refSpaceTypes, kQxrLocalFloor);
        struct Cand {
            XrReferenceSpaceType type;
            const char* name;
            bool ok;
            bool stage;
        };
        // 另建一顆空間，即使最後退回 LOCAL 也不跟 quad 的 localSpace 共用，拆的時候才不會打兩次。
        const Cand cands[] = {
            {XR_REFERENCE_SPACE_TYPE_STAGE, "STAGE", haveStage, true},
            {kQxrLocalFloor, "LOCAL_FLOOR", haveFloor, false},
            {XR_REFERENCE_SPACE_TYPE_LOCAL, "LOCAL", true, false},
        };
        XrReferenceSpaceCreateInfo spaceInfo{};
        spaceInfo.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
        spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
        for (const Cand& cand : cands) {
            if (!cand.ok) {
                continue;
            }
            spaceInfo.referenceSpaceType = cand.type;
            const XrResult result = xrCreateReferenceSpace(session, &spaceInfo, &trackSpace);
            if (xrOk(result) && trackSpace != XR_NULL_HANDLE) {
                xrNote(result, &lossPending, nullptr);
                trackIsStage = cand.stage;
                trackSpaceName = cand.name;
                QXR_LOGI("SESSION", "PCVR tracking reference space %s", cand.name);
                break;
            }
            trackSpace = XR_NULL_HANDLE;
            char fn[64];
            std::snprintf(fn, sizeof(fn), "xrCreateReferenceSpace(%s)", cand.name);
            logXr(false, instance, fn, result);
        }
        if (trackSpace == XR_NULL_HANDLE) {
            return fail(error, "enablePcvr: no usable tracking reference space");
        }
    }

    if (timeConvert == nullptr && hasTimeSpec) {
        PFN_xrVoidFunction raw = nullptr;
        const XrResult result = xrGetInstanceProcAddr(instance, "xrConvertTimespecTimeToTimeKHR", &raw);
        if (!xrOk(result) || raw == nullptr) {
            logXr(false, instance, "xrGetInstanceProcAddr(xrConvertTimespecTimeToTimeKHR)", result);
            timeConvert = nullptr;
        }
        else {
            timeConvert = reinterpret_cast<QxrConvertTimespecFn>(raw);
        }
    }

    if (!controllers) {
        controllers = std::make_unique<QxrXrControllers>();
        if (!controllers->create(instance, session, hasTouchPlus, error)) {
            // attach 沒成功才會走到這裡。掛上之後 create() 會留下 PCVR，即使 action space 只有警告。
            controllers.reset();
            if (trackSpace != XR_NULL_HANDLE) {
                const XrResult result = xrDestroySpace(trackSpace);
                if (!xrOk(result)) {
                    logXr(false, instance, "xrDestroySpace(track)", result);
                }
                trackSpace = XR_NULL_HANDLE;
            }
            trackIsStage = false;
            trackSpaceName = "none";
            return false;
        }
    }

    if (!poseSource) {
        poseSource = std::make_unique<QxrXrPoseSource>(instance, session, trackSpace, viewSpace, controllers.get(), timeConvert,
                                                       trackIsStage, hasTimeSpec);
    }
    // pose source 與 controllers 先發布，再把旗標放開。viewConfig／上行看到旗標時這兩個已經寫好。
    pcvr.store(true, std::memory_order_release);
    if (error) {
        error->clear();
    }
    QXR_LOGI("SESSION", "PCVR input enabled: space=%s touchPlus=%d timeConv=%d", trackSpaceName, hasTouchPlus ? 1 : 0,
             timeConvert != nullptr ? 1 : 0);
    return true;
}

bool QxrSession::Impl::viewConfig(QxrViewConfig* out) const {
    if (out == nullptr || !viewsLocked.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(viewMu);
    if (!viewsLocked.load(std::memory_order_relaxed)) {
        return false;
    }
    *out = viewCache;
    float width = 0.0f;
    float depth = 0.0f;
    if (poseSource) {
        poseSource->copyPlayArea(&width, &depth);
    }
    out->playAreaWidthM = width;
    out->playAreaDepthM = depth;
    // 換算函式是 enablePcvr 才載的，不跟視角一起鎖。沒有時上行必須留在影格執行緒。
    out->timeConversion = timeConvert != nullptr ? 1.0f : 0.0f;
    return true;
}

int QxrSession::Impl::syncPcvrActions() {
    if (!controllers || session == XR_NULL_HANDLE || controllers->actionSet() == XR_NULL_HANDLE) {
        return 0;
    }
    XrActiveActionSet active{};
    active.actionSet = controllers->actionSet();
    XrActionsSyncInfo sync{};
    sync.type = XR_TYPE_ACTIONS_SYNC_INFO;
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    const XrResult result = xrSyncActions(session, &sync);
    if (!xrOk(result)) {
        ++syncFails;
        if (syncFails == 1 || (syncFails % 300) == 0) {
            logXr(true, instance, "xrSyncActions", result);
        }
        if (result == XR_ERROR_SESSION_LOST) {
            return kQxrEndLossPending;
        }
        if (result == XR_ERROR_INSTANCE_LOST) {
            return kQxrEndInstanceLoss;
        }
        return 0;
    }
    syncFails = 0;
    xrNote(result, &lossPending, nullptr);
    // XR_SESSION_NOT_FOCUSED 是成功碼。沒有焦點也要快照，按鍵才會被清成放開，pressCtr 則留著。
    controllers->updateButtons(sessionState == XR_SESSION_STATE_FOCUSED);
    controllers->applyPendingHaptics();
    if (!profileNoted) {
        controllers->onProfileChanged();
        profileNoted = true;
    }
    return 0;
}

float QxrSession::Impl::effectiveRefreshHz() const {
    const float periodHz = lastPeriodNs > 0 ? 1.0e9f / static_cast<float>(lastPeriodNs) : 0.0f;
    // Frame waitViews：兩邊都有、差超過週期的 3% 就信週期。擴充拿不到時 currentHz 是 0，同樣用週期。
    if (periodHz > 0.0f && currentHz > 0.0f && std::fabs(periodHz - currentHz) > 0.03f * periodHz) {
        return periodHz;
    }
    return currentHz > 0.0f ? currentHz : periodHz;
}

bool QxrSession::Impl::measureSeated(XrTime displayTime, float pos[3], float* yawRad, const char** source) {
    if (pos == nullptr || yawRad == nullptr || source == nullptr) {
        return false;
    }
    *source = nullptr;
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    auto take = [&](const XrPosef& pose, const char* name) -> bool {
        if (!std::isfinite(pose.position.x) || !std::isfinite(pose.position.y) || !std::isfinite(pose.position.z)) {
            return false;
        }
        pos[0] = pose.position.x;
        pos[1] = pose.position.y;
        pos[2] = pose.position.z;
        const float x = pose.orientation.x;
        const float y = pose.orientation.y;
        const float z = pose.orientation.z;
        const float w = pose.orientation.w;
        const float siny = 2.0f * (w * y + x * z);
        const float cosy = 1.0f - 2.0f * (x * x + y * y);
        float yaw = std::atan2(siny, cosy);
        if (!std::isfinite(yaw)) {
            yaw = 0.0f;
        }
        *yawRad = yaw;
        *source = name;
        return true;
    };
    // 追蹤空間是 STAGE 時，LOCAL 原點在追蹤空間裡的高度就是坐姿零點。
    // 追蹤空間本身就是 LOCAL 時，這個相對位置接近原點、高度不夠，改用當下的頭。
    if (localSpace != XR_NULL_HANDLE && trackSpace != XR_NULL_HANDLE) {
        XrSpaceLocation loc{};
        loc.type = XR_TYPE_SPACE_LOCATION;
        const XrResult result = xrLocateSpace(localSpace, trackSpace, displayTime, &loc);
        if (xrOk(result)) {
            xrNote(result, &lossPending, nullptr);
            if ((loc.locationFlags & need) == need && loc.pose.position.y >= 0.5f && take(loc.pose, "local")) {
                return true;
            }
        }
    }
    if (viewSpace != XR_NULL_HANDLE && trackSpace != XR_NULL_HANDLE) {
        XrSpaceLocation loc{};
        loc.type = XR_TYPE_SPACE_LOCATION;
        const XrResult result = xrLocateSpace(viewSpace, trackSpace, displayTime, &loc);
        if (xrOk(result)) {
            xrNote(result, &lossPending, nullptr);
            if ((loc.locationFlags & need) == need && take(loc.pose, "head")) {
                return true;
            }
        }
    }
    return false;
}

void QxrSession::Impl::captureViewConfig(XrTime displayTime) {
    if (viewsLocked.load(std::memory_order_acquire)) {
        return;
    }
    if (sessionState != XR_SESSION_STATE_VISIBLE && sessionState != XR_SESSION_STATE_FOCUSED) {
        return;
    }
    if (session == XR_NULL_HANDLE || viewSpace == XR_NULL_HANDLE) {
        return;
    }
    XrViewLocateInfo info{};
    info.type = XR_TYPE_VIEW_LOCATE_INFO;
    info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    info.displayTime = displayTime;
    info.space = viewSpace;
    XrViewState state{};
    state.type = XR_TYPE_VIEW_STATE;
    XrView viewsOut[2]{};
    viewsOut[0].type = XR_TYPE_VIEW;
    viewsOut[1].type = XR_TYPE_VIEW;
    uint32_t count = 0;
    const XrResult result = xrLocateViews(session, &info, &state, 2, &count, viewsOut);
    if (!xrOk(result)) {
        ++viewLocateFails;
        if (viewLocateFails == 1 || (viewLocateFails % 300) == 0) {
            logXr(true, instance, "xrLocateViews", result);
        }
        return;
    }
    xrNote(result, &lossPending, nullptr);
    const XrViewStateFlags need = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    if (count != 2 || (state.viewStateFlags & need) != need) {
        ++viewLocateFails;
        if (viewLocateFails == 1 || (viewLocateFails % 300) == 0) {
            QXR_LOGW("SESSION", "xrLocateViews not usable yet: count=%u flags=0x%x (occurrence %d)", count,
                     static_cast<unsigned>(state.viewStateFlags), viewLocateFails);
        }
        return;
    }
    const float hz = effectiveRefreshHz();
    if (!(hz > 0.0f)) {
        // 0 不能鎖進 /launch。等 predictedDisplayPeriod 或擴充回報。
        return;
    }
    if (!loggedPeriodFallback) {
        const float periodHz = lastPeriodNs > 0 ? 1.0e9f / static_cast<float>(lastPeriodNs) : 0.0f;
        const bool periodWins = periodHz > 0.0f && (currentHz <= 0.0f || std::fabs(periodHz - currentHz) > 0.03f * periodHz);
        if (periodWins) {
            loggedPeriodFallback = true;
            if (currentHz <= 0.0f) {
                QXR_LOGW("SESSION",
                         "XR_FB_display_refresh_rate unavailable; using predictedDisplayPeriod %.2f ms (%.1f Hz)",
                         static_cast<double>(lastPeriodNs) / 1.0e6, static_cast<double>(periodHz));
            } else {
                QXR_LOGW("SESSION",
                         "refresh mismatch: runtime reports %.1f Hz but predictedDisplayPeriod=%.2f ms (%.1f Hz) - using the measured period",
                         static_cast<double>(currentHz), static_cast<double>(lastPeriodNs) / 1.0e6, static_cast<double>(periodHz));
            }
        }
    }
    QxrViewConfig cfg{};
    cfg.eyeW = static_cast<float>(eyeW);
    cfg.eyeH = static_cast<float>(eyeH);
    cfg.refreshHz = hz;
    cfg.leftAngleLeft = viewsOut[0].fov.angleLeft;
    cfg.leftAngleRight = viewsOut[0].fov.angleRight;
    cfg.leftAngleUp = viewsOut[0].fov.angleUp;
    cfg.leftAngleDown = viewsOut[0].fov.angleDown;
    cfg.leftPosX = viewsOut[0].pose.position.x;
    cfg.leftPosY = viewsOut[0].pose.position.y;
    cfg.leftPosZ = viewsOut[0].pose.position.z;
    cfg.leftRotX = viewsOut[0].pose.orientation.x;
    cfg.leftRotY = viewsOut[0].pose.orientation.y;
    cfg.leftRotZ = viewsOut[0].pose.orientation.z;
    cfg.leftRotW = viewsOut[0].pose.orientation.w;
    cfg.rightAngleLeft = viewsOut[1].fov.angleLeft;
    cfg.rightAngleRight = viewsOut[1].fov.angleRight;
    cfg.rightAngleUp = viewsOut[1].fov.angleUp;
    cfg.rightAngleDown = viewsOut[1].fov.angleDown;
    cfg.rightPosX = viewsOut[1].pose.position.x;
    cfg.rightPosY = viewsOut[1].pose.position.y;
    cfg.rightPosZ = viewsOut[1].pose.position.z;
    cfg.rightRotX = viewsOut[1].pose.orientation.x;
    cfg.rightRotY = viewsOut[1].pose.orientation.y;
    cfg.rightRotZ = viewsOut[1].pose.orientation.z;
    cfg.rightRotW = viewsOut[1].pose.orientation.w;
    float seatPos[3] = {};
    float seatYaw = 0.0f;
    const char* seatSource = nullptr;
    if (measureSeated(displayTime, seatPos, &seatYaw, &seatSource)) {
        cfg.seatedValid = 1.0f;
        cfg.seatedX = seatPos[0];
        cfg.seatedY = seatPos[1];
        cfg.seatedZ = seatPos[2];
        cfg.seatedYawRad = seatYaw;
        QXR_LOGI("SESSION", "seated zero from %s: (%.3f, %.3f, %.3f) yaw=%.3f rad",
                 seatSource != nullptr ? seatSource : "unknown", static_cast<double>(seatPos[0]), static_cast<double>(seatPos[1]),
                 static_cast<double>(seatPos[2]), static_cast<double>(seatYaw));
    } else {
        QXR_LOGW("SESSION", "seated zero unavailable; vrSeated omitted");
    }
    {
        std::lock_guard<std::mutex> lock(viewMu);
        viewCache = cfg;
        // 第一次量到的眼睛相對頭就鎖住。這輪沒有 overscan，/launch 之後整場都用這一組。
        viewsLocked.store(true, std::memory_order_release);
    }
    QXR_LOGI("SESSION",
             "per-eye views locked: L fov %.3f/%.3f/%.3f/%.3f R fov %.3f/%.3f/%.3f/%.3f rad eye %.0fx%.0f hz=%.1f",
             static_cast<double>(cfg.leftAngleLeft), static_cast<double>(cfg.leftAngleRight), static_cast<double>(cfg.leftAngleUp),
             static_cast<double>(cfg.leftAngleDown), static_cast<double>(cfg.rightAngleLeft),
             static_cast<double>(cfg.rightAngleRight), static_cast<double>(cfg.rightAngleUp),
             static_cast<double>(cfg.rightAngleDown), static_cast<double>(cfg.eyeW), static_cast<double>(cfg.eyeH),
             static_cast<double>(cfg.refreshHz));
}

QxrSession::QxrSession() : impl(std::make_shared<Impl>()) {}

QxrSession::~QxrSession() {
    destroy();
}

bool QxrSession::create(const QxrSessionCreateInfo& info, std::string* error) {
    if (!impl) {
        impl = std::make_shared<Impl>();
    }
    {
        std::lock_guard<std::mutex> lock(impl->mu);
        if (impl->threadState == 1) {
            return fail(error, "create while the frame thread is running");
        }
    }
    release();
    if (!impl->build(info, error)) {
        release();
        return false;
    }
    return true;
}

ANativeWindow* QxrSession::decoderWindow() {
    if (!impl) {
        return nullptr;
    }
    return impl->video.decoderWindow();
}

bool QxrSession::start(std::string* error) {
    if (!impl || !impl->created) {
        return fail(error, "start before create");
    }
    {
        std::lock_guard<std::mutex> lock(impl->mu);
        if (impl->threadState == 1) {
            return fail(error, "frame thread already running");
        }
        impl->stopFlag.store(false, std::memory_order_release);
        impl->frameAlive.store(true, std::memory_order_release);
        impl->threadState = 1;
    }
    try {
        std::thread([s = impl]() { run(s); }).detach();
    } catch (const std::exception& ex) {
        std::lock_guard<std::mutex> lock(impl->mu);
        impl->frameAlive.store(false, std::memory_order_release);
        impl->threadState = 0;
        return fail(error, ex.what());
    }
    if (error) {
        error->clear();
    }
    return true;
}

void QxrSession::stop() {
    auto s = impl;
    if (!s) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s->mu);
        s->stopFlag.store(true, std::memory_order_release);
    }
    s->cv.notify_all();
    if (s->workerSet.load(std::memory_order_acquire) && pthread_equal(pthread_self(), s->worker)) {
        return;
    }
    std::unique_lock<std::mutex> lock(s->mu);
    s->cv.wait(lock, [&] { return s->threadState != 1; });
}

void QxrSession::destroy() {
    stop();
    release();
}

void QxrSession::release() {
    auto s = impl;
    if (!s) {
        return;
    }
    if (s->inRelease.exchange(true)) {
        return;
    }
    s->teardown();
    s->inRelease.store(false);
}

std::vector<float> QxrSession::refreshRates() const {
    if (!impl) {
        return {};
    }
    return impl->rates;
}

void QxrSession::recommendedEyeSize(uint32_t* w, uint32_t* h) const {
    if (w) {
        *w = impl ? impl->eyeW : 0;
    }
    if (h) {
        *h = impl ? impl->eyeH : 0;
    }
}

void QxrSession::setEndedCallback(std::function<void(int)> callback) {
    if (!impl) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl->mu);
    impl->onEnded = std::move(callback);
}

bool QxrSession::enablePcvr(std::string* error) {
    if (!impl) {
        return fail(error, "enablePcvr before create");
    }
    return impl->enablePcvr(error);
}

bool QxrSession::armPcvrOutput(int32_t packedW, int32_t packedH, std::string* error) {
    if (!impl) {
        return fail(error, "armPcvrOutput before create");
    }
    return impl->armPcvrOutput(packedW, packedH, error);
}

void QxrSession::bindPcvrTiming(QxrFrameMeta* meta, QxrSampleHistory* history) {
    if (!impl) {
        return;
    }
    impl->bindPcvrTiming(meta, history);
}

void QxrSession::unbindPcvrTiming() {
    if (!impl) {
        return;
    }
    impl->unbindPcvrTiming();
}

bool QxrSession::copyPcvrTiming(QxrClientTimingFill* out) {
    if (!impl) {
        return false;
    }
    return impl->copyPcvrTiming(out);
}

bool QxrSession::viewConfig(QxrViewConfig* out) const {
    if (!impl) {
        return false;
    }
    return impl->viewConfig(out);
}

QxrPoseSource* QxrSession::pcvrPoseSource() const {
    if (!impl || !impl->pcvr.load(std::memory_order_acquire)) {
        return nullptr;
    }
    return impl->poseSource.get();
}

void QxrSession::queuePcvrHaptic(int hand, int64_t durationNs, float frequencyHz, float amplitude) {
    if (!impl || !impl->pcvr.load(std::memory_order_acquire) || !impl->controllers) {
        return;
    }
    impl->controllers->queueHaptic(hand, durationNs, frequencyHz, amplitude);
}

bool QxrSession::pcvrTimeConversion() const {
    if (!impl || !impl->pcvr.load(std::memory_order_acquire) || !impl->poseSource) {
        return false;
    }
    return impl->poseSource->hasTimeConversion();
}

void QxrSession::run(const std::shared_ptr<Impl>& impl) {
    impl->worker = pthread_self();
    impl->workerSet.store(true, std::memory_order_release);
    pthread_setname_np(pthread_self(), "qxr-frame");
    int reason = 0;
    try {
        reason = impl->frameLoop();
    } catch (const std::exception& ex) {
        QXR_LOGE("FRAME", "frame thread exception: %s", ex.what());
        reason = kQxrEndLossPending;
    }
    // §QUEST-PCVR：ended 回呼可能在這條執行緒上進 nativeDestroy，那時 JNI 還握著 pcvrMu 等 arm。要先叫醒它。
    impl->frameAlive.store(false, std::memory_order_release);
    impl->abandonArm();
    std::function<void(int)> callback;
    {
        std::lock_guard<std::mutex> lock(impl->mu);
        callback = impl->onEnded;
    }
    if (reason != 0 && !impl->endedNotified) {
        impl->endedNotified = true;
        if (callback) {
            callback(reason);
        }
    }
    callback = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl->mu);
        impl->workerSet.store(false, std::memory_order_release);
        impl->threadState = 2;
    }
    impl->cv.notify_all();
}
