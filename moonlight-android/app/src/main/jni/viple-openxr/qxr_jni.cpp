#include "qxr_session.h"

#include "qxr_framemeta.h"
#include "qxr_log.h"
#include "qxr_imu_pose.h"
#include "qxr_synthetic.h"
#include "qxr_tracking.h"
#include "qxr_vrmsg.h"
#include "viple_vr_bridge.h"

#include <android/native_window_jni.h>

#include <android/log.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {

constexpr uint32_t kMagic = 0x51585231u;
constexpr uint32_t kEmuMagic = 0x51584531u;
// OpenXR 的 XR_MIN_HAPTIC_DURATION。這個檔沒有 include openxr.h。0 微秒要送這個，不是停止。
constexpr int64_t kMinHapticDurationNs = -1;

// §VITURE：push 約 400 Hz，不能拿 lifeMu。stop() 會握著 lifeMu 等上行執行緒結束。
// 存活旗標放在物件外面。delete 之後再解參考 magic 就是 use-after-free。
// castEmu、push、start、stop、destroy 都要先拿這把鎖，slot 的檢查和後續使用才不會分開。
std::mutex g_imuLifeMu;
constexpr int kEmuSlotCount = 4;
std::atomic<uintptr_t> g_emuLive[kEmuSlotCount]{};

std::mutex g_errorMu;
std::string g_lastError;
jclass g_class = nullptr;
jmethodID g_onEnded = nullptr;
jmethodID g_onVrServerError = nullptr;

struct QxrNativeHandle {
    // onFrameSink 在別的執行緒讀。普通 uint32_t 和 nativeDestroy 的寫入會資料競爭。
    std::atomic<uint32_t> magic{kMagic};
    std::atomic<bool> dying{false};
    JavaVM* vm = nullptr;
    jobject activity = nullptr;
    QxrSession session;
    // §QUEST-PCVR：追蹤環在 nativePcvrStart 才掛上。沒掛的時候 echoKnown 維持 0。
    QxrFrameMeta frameMeta{nullptr};
    std::mutex pcvrMu;
    bool pcvrStarted = false;
    bool messageSinkSet = false;
    int pcvrHz = 72;
    std::unique_ptr<QxrSampleHistory> history;
    std::unique_ptr<QxrTrackingSender> tracking;
    std::unique_ptr<QxrVrMsg> vrMsg;
};

void setError(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_errorMu);
    g_lastError = message;
}

std::string lastError() {
    std::lock_guard<std::mutex> lock(g_errorMu);
    return g_lastError;
}

QxrNativeHandle* castHandle(jlong raw) {
    if (raw == 0) {
        return nullptr;
    }
    auto* handle = reinterpret_cast<QxrNativeHandle*>(static_cast<uintptr_t>(raw));
    if (handle->magic.load(std::memory_order_acquire) != kMagic) {
        return nullptr;
    }
    return handle;
}

void notifyEnded(QxrNativeHandle* handle, int reason) {
    // method id 在 JNI_OnLoad 取好。frame thread 上 FindClass 應用程式類別會拿到錯的 class loader。
    if (handle == nullptr || handle->vm == nullptr || g_class == nullptr || g_onEnded == nullptr) {
        QXR_LOGE("JNI", "onSessionEnded skipped");
        return;
    }
    JavaVM* vm = handle->vm;
    JNIEnv* env = nullptr;
    bool attached = false;
    const jint envResult = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (envResult == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || env == nullptr) {
            QXR_LOGE("JNI", "AttachCurrentThread failed");
            return;
        }
        attached = true;
    } else if (envResult != JNI_OK || env == nullptr) {
        QXR_LOGE("JNI", "GetEnv failed: %d", static_cast<int>(envResult));
        return;
    }
    // 回呼返回後 frame thread 才把自己標成結束。這裡若同步等另一條正卡在 nativeStop／nativeDestroy 的執行緒，兩邊會互等。
    env->CallStaticVoidMethod(g_class, g_onEnded, static_cast<jlong>(reinterpret_cast<uintptr_t>(handle)), reason);
    if (env->ExceptionCheck()) {
        QXR_LOGE("JNI", "onSessionEnded threw");
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    if (attached) {
        vm->DetachCurrentThread();
    }
}

void notifyVrServerError(QxrNativeHandle* handle, int code) {
    // 跟 onSessionEnded 一樣，method id 在 JNI_OnLoad 取好。回呼在 message sink 裡，返回後 bridge 才放讀鎖。
    if (handle == nullptr || handle->vm == nullptr || g_class == nullptr || g_onVrServerError == nullptr) {
        QXR_LOGE("JNI", "onVrServerError skipped");
        return;
    }
    JavaVM* vm = handle->vm;
    JNIEnv* env = nullptr;
    bool attached = false;
    const jint envResult = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (envResult == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || env == nullptr) {
            QXR_LOGE("JNI", "AttachCurrentThread failed");
            return;
        }
        attached = true;
    } else if (envResult != JNI_OK || env == nullptr) {
        QXR_LOGE("JNI", "GetEnv failed: %d", static_cast<int>(envResult));
        return;
    }
    env->CallStaticVoidMethod(g_class, g_onVrServerError, static_cast<jlong>(reinterpret_cast<uintptr_t>(handle)), code);
    if (env->ExceptionCheck()) {
        QXR_LOGE("JNI", "onVrServerError threw");
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    if (attached) {
        vm->DetachCurrentThread();
    }
}

void onFrameSink(void* ctx, int frameNumber, const VIPLE_VR_FRAME_META* meta, uint64_t, uint64_t, uint32_t, int) {
    auto* handle = static_cast<QxrNativeHandle*>(ctx);
    if (handle == nullptr || handle->magic.load(std::memory_order_acquire) != kMagic) {
        return;
    }
    // 不可呼叫 VipleVrBridgeSet*：bridge 取消時會等這次回呼結束，再拿寫鎖會死結。
    handle->frameMeta.onFrame(frameNumber, meta);
}

jlong nativeCreate(JNIEnv* env, jclass, jobject activity, jint streamWidth, jint streamHeight, jint preferredHz) {
    if (activity == nullptr) {
        setError("activity is null");
        return 0;
    }
    JavaVM* vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK || vm == nullptr) {
        setError("GetJavaVM failed");
        return 0;
    }
    jobject global = env->NewGlobalRef(activity);
    if (global == nullptr) {
        setError("NewGlobalRef(activity) failed");
        return 0;
    }
    QxrNativeHandle* handle = nullptr;
    try {
        handle = new QxrNativeHandle();
        handle->vm = vm;
        handle->activity = global;
        QxrSessionCreateInfo info{};
        info.vm = vm;
        info.activity = global;
        info.streamWidth = streamWidth;
        info.streamHeight = streamHeight;
        info.preferredHz = preferredHz;
        std::string error;
        if (!handle->session.create(info, &error)) {
            setError(error.empty() ? "QxrSession::create failed" : error);
            env->DeleteGlobalRef(global);
            delete handle;
            return 0;
        }
        handle->session.setEndedCallback([handle](int reason) { notifyEnded(handle, reason); });
    } catch (const std::exception& ex) {
        setError(ex.what());
        env->DeleteGlobalRef(global);
        delete handle;
        return 0;
    }
    VipleVrBridgeSetFrameSink(onFrameSink, handle);
    setError("");
    QXR_LOGI("JNI", "nativeCreate %dx%d hz=%d handle=%p", static_cast<int>(streamWidth), static_cast<int>(streamHeight),
             static_cast<int>(preferredHz), static_cast<void*>(handle));
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(handle));
}

jstring nativeLastError(JNIEnv* env, jclass) {
    const std::string message = lastError();
    return env->NewStringUTF(message.c_str());
}

jobject nativeGetDecoderSurface(JNIEnv* env, jclass, jlong raw) {
    QxrNativeHandle* handle = castHandle(raw);
    if (handle == nullptr || handle->dying.load(std::memory_order_acquire)) {
        return nullptr;
    }
    ANativeWindow* window = handle->session.decoderWindow();
    if (window == nullptr) {
        return nullptr;
    }
    return ANativeWindow_toSurface(env, window);
}

jboolean nativeStart(JNIEnv*, jclass, jlong raw) {
    QxrNativeHandle* handle = castHandle(raw);
    if (handle == nullptr || handle->dying.load(std::memory_order_acquire)) {
        setError("nativeStart: bad handle");
        return JNI_FALSE;
    }
    std::string error;
    if (!handle->session.start(&error)) {
        setError(error.empty() ? "QxrSession::start failed" : error);
        return JNI_FALSE;
    }
    setError("");
    return JNI_TRUE;
}

void onPcvrMessage(void* ctx, const uint8_t* tlv, int length) {
    auto* handle = static_cast<QxrNativeHandle*>(ctx);
    if (handle == nullptr || handle->magic.load(std::memory_order_acquire) != kMagic) {
        return;
    }
    // 不拿 pcvrMu。stop 握著那把鎖等這個回呼結束，再拿一次會死結。
    if (handle->vrMsg != nullptr) {
        handle->vrMsg->onMessage(tlv, length);
    }
}

void stopPcvrUplink(QxrNativeHandle* handle) {
    if (handle == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(handle->pcvrMu);
    if (handle->messageSinkSet) {
        // bridge 會等進行中的 onPcvrMessage 返回。回呼裡不能再呼叫 Set。
        VipleVrBridgeSetMessageSink(nullptr, nullptr);
        handle->messageSinkSet = false;
    }
    handle->session.unbindPcvrTiming();
    if (handle->vrMsg) {
        handle->vrMsg->setTimingFill(nullptr);
    }
    handle->frameMeta.setHistory(nullptr);
    if (handle->tracking) {
        handle->tracking->stop();
    }
    if (handle->vrMsg) {
        handle->vrMsg->stop();
    }
    handle->pcvrStarted = false;
}

void nativeStop(JNIEnv*, jclass, jlong raw) {
    QxrNativeHandle* handle = castHandle(raw);
    if (handle == nullptr || handle->dying.load(std::memory_order_acquire)) {
        return;
    }
    stopPcvrUplink(handle);
    if (handle->dying.load(std::memory_order_acquire)) {
        return;
    }
    handle->session.stop();
}

void nativeDestroy(JNIEnv* env, jclass, jlong raw) {
    QxrNativeHandle* handle = castHandle(raw);
    if (handle == nullptr) {
        return;
    }
    if (handle->dying.exchange(true)) {
        return;
    }
    // 上行先停。frame sink 仍留著，等下面卸掉；這時 history 已經摘掉，onFrame 不會再讀環。
    stopPcvrUplink(handle);
    // 先卸 sink。bridge 會等進行中的 onFrameSink 返回，之後才沒有人讀 magic。
    // 反過來寫的話，回呼執行緒和這裡會同時碰到 magic。
    VipleVrBridgeSetFrameSink(nullptr, nullptr);
    handle->magic.store(0, std::memory_order_release);
    handle->session.destroy();
    if (handle->activity != nullptr && env != nullptr) {
        env->DeleteGlobalRef(handle->activity);
        handle->activity = nullptr;
    }
    delete handle;
}

jfloatArray nativeGetRefreshRates(JNIEnv* env, jclass, jlong raw) {
    QxrNativeHandle* handle = castHandle(raw);
    std::vector<float> rates;
    if (handle != nullptr && !handle->dying.load(std::memory_order_acquire)) {
        rates = handle->session.refreshRates();
    }
    jfloatArray array = env->NewFloatArray(static_cast<jsize>(rates.size()));
    if (array == nullptr) {
        return nullptr;
    }
    if (!rates.empty()) {
        env->SetFloatArrayRegion(array, 0, static_cast<jsize>(rates.size()), rates.data());
    }
    return array;
}

jintArray nativeGetRecommendedEyeSize(JNIEnv* env, jclass, jlong raw) {
    uint32_t width = 0;
    uint32_t height = 0;
    QxrNativeHandle* handle = castHandle(raw);
    if (handle != nullptr && !handle->dying.load(std::memory_order_acquire)) {
        handle->session.recommendedEyeSize(&width, &height);
    }
    jint values[2] = {static_cast<jint>(width), static_cast<jint>(height)};
    jintArray array = env->NewIntArray(2);
    if (array == nullptr) {
        return nullptr;
    }
    env->SetIntArrayRegion(array, 0, 2, values);
    return array;
}

jboolean nativeEnablePcvr(JNIEnv*, jclass, jlong raw) {
    QxrNativeHandle* handle = castHandle(raw);
    if (handle == nullptr || handle->dying.load(std::memory_order_acquire)) {
        setError("nativeEnablePcvr: bad handle");
        return JNI_FALSE;
    }
    std::string error;
    try {
        if (!handle->session.enablePcvr(&error)) {
            const std::string message = error.empty() ? "QxrSession::enablePcvr failed" : error;
            setError(message);
            QXR_LOGE("JNI", "nativeEnablePcvr failed: %s", message.c_str());
            return JNI_FALSE;
        }
    }
    catch (const std::exception& ex) {
        setError(ex.what());
        QXR_LOGE("JNI", "nativeEnablePcvr failed: %s", ex.what());
        return JNI_FALSE;
    }
    setError("");
    QXR_LOGI("JNI", "nativeEnablePcvr handle=%p timeConv=%d", static_cast<void*>(handle),
             handle->session.pcvrTimeConversion() ? 1 : 0);
    return JNI_TRUE;
}

jfloatArray nativeGetViewConfig(JNIEnv* env, jclass, jlong raw) {
    QxrNativeHandle* handle = castHandle(raw);
    if (handle == nullptr || handle->dying.load(std::memory_order_acquire)) {
        return nullptr;
    }
    QxrViewConfig cfg{};
    if (!handle->session.viewConfig(&cfg)) {
        return nullptr;
    }
    // 0..26 不變。27 起是坐姿零點與時間換算，Java 的 VIEW_* 要同一順序。
    const jfloat values[33] = {
        cfg.eyeW,
        cfg.eyeH,
        cfg.refreshHz,
        cfg.leftAngleLeft,
        cfg.leftAngleRight,
        cfg.leftAngleUp,
        cfg.leftAngleDown,
        cfg.leftPosX,
        cfg.leftPosY,
        cfg.leftPosZ,
        cfg.leftRotX,
        cfg.leftRotY,
        cfg.leftRotZ,
        cfg.leftRotW,
        cfg.rightAngleLeft,
        cfg.rightAngleRight,
        cfg.rightAngleUp,
        cfg.rightAngleDown,
        cfg.rightPosX,
        cfg.rightPosY,
        cfg.rightPosZ,
        cfg.rightRotX,
        cfg.rightRotY,
        cfg.rightRotZ,
        cfg.rightRotW,
        cfg.playAreaWidthM,
        cfg.playAreaDepthM,
        cfg.seatedValid,
        cfg.seatedX,
        cfg.seatedY,
        cfg.seatedZ,
        cfg.seatedYawRad,
        cfg.timeConversion,
    };
    jfloatArray array = env->NewFloatArray(33);
    if (array == nullptr) {
        return nullptr;
    }
    env->SetFloatArrayRegion(array, 0, 33, values);
    return array;
}

jboolean nativePcvrStart(JNIEnv*, jclass, jlong raw, jint refreshHz, jint packedW, jint packedH) {
    QxrNativeHandle* handle = castHandle(raw);
    if (handle == nullptr || handle->dying.load(std::memory_order_acquire)) {
        setError("nativePcvrStart: bad handle");
        return JNI_FALSE;
    }
    QxrPoseSource* pose = handle->session.pcvrPoseSource();
    if (pose == nullptr) {
        setError("nativePcvrStart: PCVR mode is not enabled");
        QXR_LOGE("JNI", "nativePcvrStart refused: PCVR mode is not enabled");
        return JNI_FALSE;
    }
    int hz = static_cast<int>(refreshHz);
    if (hz <= 0) {
        QXR_LOGI("JNI", "nativePcvrStart refreshHz=%d invalid; using 72", hz);
        hz = 72;
    }
    std::lock_guard<std::mutex> guard(handle->pcvrMu);
    if (handle->dying.load(std::memory_order_acquire)) {
        return JNI_FALSE;
    }
    if (handle->pcvrStarted) {
        return JNI_TRUE;
    }
    const int flags = LiGetVrFlags();
    if ((flags & VIPLE_VR_SF_ENABLED) == 0) {
        QXR_LOGE("JNI", "nativePcvrStart refused: vrFlags=0x%x (VIPLE_VR_SF_ENABLED not set)", flags);
        setError("nativePcvrStart: VIPLE_VR_SF_ENABLED is not set");
        return JNI_FALSE;
    }
    const char* sourceName = handle->session.pcvrTimeConversion() ? "xr-thread" : "xr-frameloop";
    try {
        if (!handle->history) {
            handle->history = std::make_unique<QxrSampleHistory>();
        }
        if (!handle->tracking) {
            handle->tracking = std::make_unique<QxrTrackingSender>(pose, handle->history.get());
        }
        if (handle->vrMsg && handle->pcvrHz != hz) {
            handle->vrMsg->stop();
            handle->vrMsg.reset();
        }
        if (!handle->vrMsg) {
            handle->vrMsg = std::make_unique<QxrVrMsg>(hz);
            handle->vrMsg->setHapticCallback([handle](const VIPLE_VR_TLV_HAPTIC& haptic) {
                int hand = -1;
                if (haptic.device == VIPLE_VR_POSE_LEFT) {
                    hand = 0;
                }
                else if (haptic.device == VIPLE_VR_POSE_RIGHT) {
                    hand = 1;
                }
                else {
                    return;
                }
                // durationUs 0 是最短脈衝，不是停止。停止只看振幅。
                const int64_t durationNs = haptic.durationUs == 0
                    ? kMinHapticDurationNs
                    : static_cast<int64_t>(haptic.durationUs) * 1000;
                handle->session.queuePcvrHaptic(hand, durationNs, haptic.frequencyHz, haptic.amplitude);
            });
        }
        // 模擬與 IMU 不掛這個。回呼不可拿 pcvrMu，也不可呼叫 VipleVrBridgeSet*。
        handle->vrMsg->setServerErrorCallback([handle](uint16_t code) {
            notifyVrServerError(handle, static_cast<int>(code));
        });
        handle->vrMsg->setTimingFill([handle](QxrClientTimingFill* out) {
            return handle->session.copyPcvrTiming(out);
        });
        handle->session.bindPcvrTiming(&handle->frameMeta, handle->history.get());
        handle->pcvrHz = hz;
        handle->frameMeta.setHistory(handle->history.get());
        handle->messageSinkSet = true;
        VipleVrBridgeSetMessageSink(onPcvrMessage, handle);
        handle->tracking->start(hz, sourceName);
        handle->vrMsg->start();
        std::string armError;
        if (!handle->session.armPcvrOutput(static_cast<int32_t>(packedW), static_cast<int32_t>(packedH), &armError)) {
            throw std::runtime_error(armError.empty() ? "armPcvrOutput failed" : armError);
        }
        handle->pcvrStarted = true;
    }
    catch (const std::exception& ex) {
        handle->session.unbindPcvrTiming();
        if (handle->vrMsg) {
            handle->vrMsg->setTimingFill(nullptr);
        }
        handle->frameMeta.setHistory(nullptr);
        if (handle->messageSinkSet) {
            VipleVrBridgeSetMessageSink(nullptr, nullptr);
            handle->messageSinkSet = false;
        }
        if (handle->tracking) {
            handle->tracking->stop();
        }
        if (handle->vrMsg) {
            handle->vrMsg->stop();
        }
        handle->pcvrStarted = false;
        setError(ex.what());
        QXR_LOGE("JNI", "nativePcvrStart failed: %s", ex.what());
        return JNI_FALSE;
    }
    setError("");
    QXR_LOGI("JNI", "nativePcvrStart hz=%d packed=%dx%d source=%s handle=%p", hz, static_cast<int>(packedW),
             static_cast<int>(packedH), sourceName, static_cast<void*>(handle));
    return JNI_TRUE;
}

void nativePcvrStop(JNIEnv*, jclass, jlong raw) {
    QxrNativeHandle* handle = castHandle(raw);
    if (handle == nullptr || handle->dying.load(std::memory_order_acquire)) {
        return;
    }
    stopPcvrUplink(handle);
}

// §QUEST-VR：手機模擬。不建構 QxrSession，函式裡也不能呼叫 xr*。
// 這個 .so 連結了 OpenXR loader，但載入跟 JNI_OnLoad 都不會去建立 instance。
struct QxrEmuHandle {
    std::atomic<uint32_t> magic{kEmuMagic};
    std::atomic<bool> dying{false};
    std::mutex lifeMu;
    bool started = false;
    int refreshHz = 72;
    std::unique_ptr<QxrPoseSource> source;
    const char* sourceName = "sine";
    QxrImuPoseSource* imu = nullptr;
    std::unique_ptr<QxrSampleHistory> history;
    std::unique_ptr<QxrTrackingSender> tracking;
    std::unique_ptr<QxrFrameMeta> frameMeta;
    std::unique_ptr<QxrVrMsg> vrMsg;
};

bool claimEmuSlot(QxrEmuHandle* handle) {
    const auto key = reinterpret_cast<uintptr_t>(handle);
    for (auto& slot : g_emuLive) {
        uintptr_t expected = 0;
        if (slot.compare_exchange_strong(expected, key, std::memory_order_acq_rel)) {
            return true;
        }
    }
    return false;
}

void releaseEmuSlot(uintptr_t key) {
    for (auto& slot : g_emuLive) {
        uintptr_t expected = key;
        if (slot.compare_exchange_strong(expected, static_cast<uintptr_t>(0), std::memory_order_acq_rel)) {
            return;
        }
    }
}

bool emuSlotLive(uintptr_t key) {
    for (const auto& slot : g_emuLive) {
        if (slot.load(std::memory_order_acquire) == key) {
            return true;
        }
    }
    return false;
}

// 呼叫端已持有 g_imuLifeMu。先看物件外的原子旗標，再看 magic 與 dying。
QxrEmuHandle* castEmu(jlong raw) {
    if (raw == 0) {
        return nullptr;
    }
    const auto key = static_cast<uintptr_t>(raw);
    if (!emuSlotLive(key)) {
        return nullptr;
    }
    auto* handle = reinterpret_cast<QxrEmuHandle*>(key);
    if (handle->magic.load(std::memory_order_acquire) != kEmuMagic) {
        return nullptr;
    }
    if (handle->dying.load(std::memory_order_acquire)) {
        return nullptr;
    }
    return handle;
}

void onEmuFrame(void* ctx, int frameNumber, const VIPLE_VR_FRAME_META* meta, uint64_t, uint64_t, uint32_t, int) {
    auto* handle = static_cast<QxrEmuHandle*>(ctx);
    if (handle == nullptr || handle->magic.load(std::memory_order_acquire) != kEmuMagic) {
        return;
    }
    if (handle->frameMeta != nullptr) {
        handle->frameMeta->onFrame(frameNumber, meta);
    }
}

void onEmuMessage(void* ctx, const uint8_t* tlv, int length) {
    auto* handle = static_cast<QxrEmuHandle*>(ctx);
    if (handle == nullptr || handle->magic.load(std::memory_order_acquire) != kEmuMagic) {
        return;
    }
    if (handle->vrMsg != nullptr) {
        handle->vrMsg->onMessage(tlv, length);
    }
}

void logEmuHaptic(const VIPLE_VR_TLV_HAPTIC& haptic) {
    const char* hand = haptic.device == VIPLE_VR_POSE_LEFT ? "left"
                       : haptic.device == VIPLE_VR_POSE_RIGHT ? "right"
                                                              : "other";
    QXR_LOGI("EMU", "haptic hand=%s durationUs=%u freq=%.1f Hz amp=%.2f id=%u",
             hand, haptic.durationUs, static_cast<double>(haptic.frequencyHz),
             static_cast<double>(haptic.amplitude), haptic.eventId);
}

jlong nativeEmuCreate(JNIEnv* env, jclass, jint refreshHz, jstring motionText) {
    int hz = static_cast<int>(refreshHz);
    if (hz <= 0) {
        QXR_LOGI("EMU", "nativeEmuCreate refreshHz=%d invalid; using 72", hz);
        hz = 72;
    }
    std::string motion = "sine";
    if (motionText != nullptr) {
        const char* utf = env->GetStringUTFChars(motionText, nullptr);
        if (utf != nullptr) {
            motion.assign(utf);
            env->ReleaseStringUTFChars(motionText, utf);
        }
    }

    QxrEmuHandle* handle = nullptr;
    try {
        handle = new QxrEmuHandle();
        handle->refreshHz = hz;
        handle->history = std::make_unique<QxrSampleHistory>();
        auto synthetic = std::make_unique<QxrSyntheticPose>(motion);
        handle->sourceName = synthetic->motionName();
        handle->source = std::move(synthetic);
        handle->tracking = std::make_unique<QxrTrackingSender>(handle->source.get(), handle->history.get());
        handle->frameMeta = std::make_unique<QxrFrameMeta>(handle->history.get());
        handle->vrMsg = std::make_unique<QxrVrMsg>(hz);
        handle->vrMsg->setHapticCallback(logEmuHaptic);
    } catch (const std::exception& ex) {
        setError(ex.what());
        delete handle;
        return 0;
    } catch (...) {
        setError("nativeEmuCreate failed");
        delete handle;
        return 0;
    }

    // 先把物件建好再登記。sink 回呼只讀這個 handle，不呼叫 VipleVrBridgeSet*。
    VipleVrBridgeSetFrameSink(onEmuFrame, handle);
    VipleVrBridgeSetMessageSink(onEmuMessage, handle);
    bool claimed = false;
    {
        std::lock_guard<std::mutex> imuGuard(g_imuLifeMu);
        claimed = claimEmuSlot(handle);
    }
    if (!claimed) {
        // slot 滿了就不能把指標交出去。先等進行中的回呼結束再放掉。
        VipleVrBridgeSetFrameSink(nullptr, nullptr);
        VipleVrBridgeSetMessageSink(nullptr, nullptr);
        delete handle;
        setError("nativeEmuCreate no slot");
        QXR_LOGE("EMU", "nativeEmuCreate no live slot");
        return 0;
    }
    setError("");
    QXR_LOGI("EMU", "nativeEmuCreate hz=%d motion=%s handle=%p", hz, handle->sourceName,
             static_cast<void*>(handle));
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(handle));
}

void nativeEmuStart(JNIEnv*, jclass, jlong raw) {
    std::lock_guard<std::mutex> imuGuard(g_imuLifeMu);
    QxrEmuHandle* handle = castEmu(raw);
    if (handle == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(handle->lifeMu);
    if (handle->dying.load(std::memory_order_acquire) || handle->started) {
        return;
    }
    const int flags = LiGetVrFlags();
    if ((flags & VIPLE_VR_SF_ENABLED) == 0) {
        QXR_LOGE("EMU", "nativeEmuStart refused: vrFlags=0x%x (VIPLE_VR_SF_ENABLED not set)", flags);
        return;
    }
    handle->tracking->start(handle->refreshHz, handle->sourceName);
    handle->vrMsg->start();
    handle->started = true;
}

void nativeEmuStop(JNIEnv*, jclass, jlong raw) {
    std::lock_guard<std::mutex> imuGuard(g_imuLifeMu);
    QxrEmuHandle* handle = castEmu(raw);
    if (handle == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(handle->lifeMu);
    if (!handle->started) {
        return;
    }
    handle->tracking->stop();
    handle->vrMsg->stop();
    handle->started = false;
}

void nativeEmuDestroy(JNIEnv*, jclass, jlong raw) {
    if (raw == 0) {
        return;
    }
    QxrEmuHandle* handle = nullptr;
    {
        // 清掉 slot 與 magic 之後才放開鎖。push 醒來時不會再解參考這塊記憶體。
        std::lock_guard<std::mutex> imuGuard(g_imuLifeMu);
        const auto key = static_cast<uintptr_t>(raw);
        if (!emuSlotLive(key)) {
            return;
        }
        handle = reinterpret_cast<QxrEmuHandle*>(key);
        if (handle->magic.load(std::memory_order_acquire) != kEmuMagic) {
            releaseEmuSlot(key);
            return;
        }
        if (handle->dying.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        releaseEmuSlot(key);
        handle->imu = nullptr;
        handle->magic.store(0, std::memory_order_release);
        std::lock_guard<std::mutex> guard(handle->lifeMu);
        if (handle->started) {
            handle->tracking->stop();
            handle->vrMsg->stop();
            handle->started = false;
        }
    }
    // 先等進行中的 sink 返回再釋放。回呼裡不能反過來呼叫這兩個 Set。
    VipleVrBridgeSetFrameSink(nullptr, nullptr);
    VipleVrBridgeSetMessageSink(nullptr, nullptr);
    QXR_LOGI("EMU", "nativeEmuDestroy handle=%p", static_cast<void*>(handle));
    delete handle;
}

jlong nativeImuCreate(JNIEnv*, jclass, jint refreshHz) {
    int hz = static_cast<int>(refreshHz);
    if (hz <= 0) {
        __android_log_print(ANDROID_LOG_INFO, "VipleViture",
                            "[VIPLE-VITURE] nativeImuCreate refreshHz=%d invalid; using 60", hz);
        hz = 60;
    }
    QxrEmuHandle* handle = nullptr;
    try {
        handle = new QxrEmuHandle();
        handle->refreshHz = hz;
        handle->history = std::make_unique<QxrSampleHistory>();
        auto imu = std::make_unique<QxrImuPoseSource>();
        handle->imu = imu.get();
        handle->source = std::move(imu);
        handle->sourceName = "imu";
        handle->tracking = std::make_unique<QxrTrackingSender>(handle->source.get(), handle->history.get());
        handle->frameMeta = std::make_unique<QxrFrameMeta>(handle->history.get());
        handle->vrMsg = std::make_unique<QxrVrMsg>(hz);
        handle->vrMsg->setHapticCallback(logEmuHaptic);
    } catch (const std::exception& ex) {
        setError(ex.what());
        delete handle;
        return 0;
    } catch (...) {
        setError("nativeImuCreate failed");
        delete handle;
        return 0;
    }
    VipleVrBridgeSetFrameSink(onEmuFrame, handle);
    VipleVrBridgeSetMessageSink(onEmuMessage, handle);
    bool claimed = false;
    {
        std::lock_guard<std::mutex> imuGuard(g_imuLifeMu);
        claimed = claimEmuSlot(handle);
    }
    if (!claimed) {
        VipleVrBridgeSetFrameSink(nullptr, nullptr);
        VipleVrBridgeSetMessageSink(nullptr, nullptr);
        delete handle;
        setError("nativeImuCreate no slot");
        __android_log_print(ANDROID_LOG_INFO, "VipleViture",
                            "[VIPLE-VITURE] nativeImuCreate no live slot");
        return 0;
    }
    setError("");
    __android_log_print(ANDROID_LOG_INFO, "VipleViture",
                        "[VIPLE-VITURE] nativeImuCreate handle=%p", static_cast<void*>(handle));
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(handle));
}

void nativeImuPush(JNIEnv*, jclass, jlong raw, jfloat qw, jfloat qx, jfloat qy, jfloat qz,
                   jfloat wx, jfloat wy, jfloat wz, jlong sourceTimeNs) {
    std::lock_guard<std::mutex> life(g_imuLifeMu);
    QxrEmuHandle* handle = castEmu(raw);
    if (handle == nullptr || handle->imu == nullptr) {
        return;
    }
    handle->imu->push(qw, qx, qy, qz, wx, wy, wz, static_cast<int64_t>(sourceTimeNs));
}

bool cacheJavaClass(JNIEnv* env) {
    jclass local = env->FindClass("com/limelight/vr/QxrNative");
    if (local == nullptr) {
        QXR_LOGE("JNI", "FindClass com/limelight/vr/QxrNative failed");
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        return false;
    }
    g_class = reinterpret_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    if (g_class == nullptr) {
        return false;
    }
    g_onEnded = env->GetStaticMethodID(g_class, "onSessionEnded", "(JI)V");
    if (g_onEnded == nullptr) {
        QXR_LOGE("JNI", "GetStaticMethodID onSessionEnded failed");
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        return false;
    }
    g_onVrServerError = env->GetStaticMethodID(g_class, "onVrServerError", "(JI)V");
    if (g_onVrServerError == nullptr) {
        QXR_LOGE("JNI", "GetStaticMethodID onVrServerError failed");
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        return false;
    }
    return true;
}

}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) {
        return JNI_ERR;
    }
    if (!cacheJavaClass(env)) {
        return JNI_ERR;
    }
    JNINativeMethod methods[] = {
        {"nativeCreate", "(Landroid/app/Activity;III)J", reinterpret_cast<void*>(nativeCreate)},
        {"nativeLastError", "()Ljava/lang/String;", reinterpret_cast<void*>(nativeLastError)},
        {"nativeGetDecoderSurface", "(J)Landroid/view/Surface;", reinterpret_cast<void*>(nativeGetDecoderSurface)},
        {"nativeStart", "(J)Z", reinterpret_cast<void*>(nativeStart)},
        {"nativeStop", "(J)V", reinterpret_cast<void*>(nativeStop)},
        {"nativeDestroy", "(J)V", reinterpret_cast<void*>(nativeDestroy)},
        {"nativeGetRefreshRates", "(J)[F", reinterpret_cast<void*>(nativeGetRefreshRates)},
        {"nativeGetRecommendedEyeSize", "(J)[I", reinterpret_cast<void*>(nativeGetRecommendedEyeSize)},
        {"nativeEnablePcvr", "(J)Z", reinterpret_cast<void*>(nativeEnablePcvr)},
        {"nativeGetViewConfig", "(J)[F", reinterpret_cast<void*>(nativeGetViewConfig)},
        {"nativePcvrStart", "(JIII)Z", reinterpret_cast<void*>(nativePcvrStart)},
        {"nativePcvrStop", "(J)V", reinterpret_cast<void*>(nativePcvrStop)},
        {"nativeEmuCreate", "(ILjava/lang/String;)J", reinterpret_cast<void*>(nativeEmuCreate)},
        {"nativeEmuStart", "(J)V", reinterpret_cast<void*>(nativeEmuStart)},
        {"nativeEmuStop", "(J)V", reinterpret_cast<void*>(nativeEmuStop)},
        {"nativeEmuDestroy", "(J)V", reinterpret_cast<void*>(nativeEmuDestroy)},
        {"nativeImuCreate", "(I)J", reinterpret_cast<void*>(nativeImuCreate)},
        {"nativeImuPush", "(JFFFFFFFJ)V", reinterpret_cast<void*>(nativeImuPush)},
    };
    if (env->RegisterNatives(g_class, methods, static_cast<jint>(sizeof(methods) / sizeof(methods[0]))) != JNI_OK) {
        QXR_LOGE("JNI", "RegisterNatives failed");
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        return JNI_ERR;
    }
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNI_OnUnload(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) {
        return;
    }
    if (g_class != nullptr) {
        env->DeleteGlobalRef(g_class);
        g_class = nullptr;
    }
    g_onEnded = nullptr;
}
