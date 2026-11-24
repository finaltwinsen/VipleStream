#include "qxr_session.h"

#include "qxr_framemeta.h"
#include "qxr_log.h"
#include "qxr_synthetic.h"
#include "qxr_tracking.h"
#include "qxr_vrmsg.h"
#include "viple_vr_bridge.h"

#include <android/native_window_jni.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>

namespace {

constexpr uint32_t kMagic = 0x51585231u;
constexpr uint32_t kEmuMagic = 0x51584531u;

std::mutex g_errorMu;
std::string g_lastError;
jclass g_class = nullptr;
jmethodID g_onEnded = nullptr;

struct QxrNativeHandle {
    // onFrameSink 在別的執行緒讀。普通 uint32_t 和 nativeDestroy 的寫入會資料競爭。
    std::atomic<uint32_t> magic{kMagic};
    std::atomic<bool> dying{false};
    JavaVM* vm = nullptr;
    jobject activity = nullptr;
    QxrSession session;
    // §QUEST-VR：64 格環沒有讀取端，改成跟模擬模式同一個配對統計。
    // 這條 OpenXR 路徑還沒有追蹤環，echoKnown 維持 0。
    QxrFrameMeta frameMeta{nullptr};
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

void nativeStop(JNIEnv*, jclass, jlong raw) {
    QxrNativeHandle* handle = castHandle(raw);
    if (handle == nullptr || handle->dying.load(std::memory_order_acquire)) {
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

// §QUEST-VR：手機模擬。不建構 QxrSession，函式裡也不能呼叫 xr*。
// 這個 .so 連結了 OpenXR loader，但載入跟 JNI_OnLoad 都不會去建立 instance。
struct QxrEmuHandle {
    std::atomic<uint32_t> magic{kEmuMagic};
    std::atomic<bool> dying{false};
    std::mutex lifeMu;
    bool started = false;
    int refreshHz = 72;
    std::unique_ptr<QxrSyntheticPose> source;
    std::unique_ptr<QxrSampleHistory> history;
    std::unique_ptr<QxrTrackingSender> tracking;
    std::unique_ptr<QxrFrameMeta> frameMeta;
    std::unique_ptr<QxrVrMsg> vrMsg;
};

QxrEmuHandle* castEmu(jlong raw) {
    if (raw == 0) {
        return nullptr;
    }
    auto* handle = reinterpret_cast<QxrEmuHandle*>(static_cast<uintptr_t>(raw));
    if (handle->magic.load(std::memory_order_acquire) != kEmuMagic) {
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
        handle->source = std::make_unique<QxrSyntheticPose>(motion);
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
    setError("");
    QXR_LOGI("EMU", "nativeEmuCreate hz=%d motion=%s handle=%p", hz, handle->source->motionName(),
             static_cast<void*>(handle));
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(handle));
}

void nativeEmuStart(JNIEnv*, jclass, jlong raw) {
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
    handle->tracking->start(handle->refreshHz, handle->source->motionName());
    handle->vrMsg->start();
    handle->started = true;
}

void nativeEmuStop(JNIEnv*, jclass, jlong raw) {
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
    QxrEmuHandle* handle = castEmu(raw);
    if (handle == nullptr) {
        return;
    }
    if (handle->dying.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(handle->lifeMu);
        if (handle->started) {
            handle->tracking->stop();
            handle->vrMsg->stop();
            handle->started = false;
        }
    }
    // 先等進行中的 sink 返回，再清 magic、再釋放。回呼裡不能反過來呼叫這兩個 Set。
    VipleVrBridgeSetFrameSink(nullptr, nullptr);
    VipleVrBridgeSetMessageSink(nullptr, nullptr);
    handle->magic.store(0, std::memory_order_release);
    QXR_LOGI("EMU", "nativeEmuDestroy handle=%p", static_cast<void*>(handle));
    delete handle;
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
        {"nativeEmuCreate", "(ILjava/lang/String;)J", reinterpret_cast<void*>(nativeEmuCreate)},
        {"nativeEmuStart", "(J)V", reinterpret_cast<void*>(nativeEmuStart)},
        {"nativeEmuStop", "(J)V", reinterpret_cast<void*>(nativeEmuStop)},
        {"nativeEmuDestroy", "(J)V", reinterpret_cast<void*>(nativeEmuDestroy)},
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
