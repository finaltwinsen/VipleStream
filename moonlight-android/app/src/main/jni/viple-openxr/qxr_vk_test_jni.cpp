#include "qxr_vk_test.h"

#include "qxr_log.h"

#include <android/native_window_jni.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>

namespace {

constexpr uint32_t kMagic = 0x51585654u;

std::mutex g_mu;

struct Handle {
    std::atomic<uint32_t> magic{kMagic};
    std::atomic<bool> dying{false};
    QxrVideoTest test;
};

Handle* castHandle(jlong raw) {
    if (raw == 0) {
        return nullptr;
    }
    auto* handle = reinterpret_cast<Handle*>(static_cast<uintptr_t>(raw));
    if (handle->magic.load(std::memory_order_acquire) != kMagic) {
        return nullptr;
    }
    return handle;
}

// §QXR-VIDEO-TEST：這個 .so 的 JNI_OnLoad 在 qxr_jni.cpp。再定義一個會連結失敗，
// 所以用匯出名稱，不 RegisterNatives。

jlong nativeCreate(JNIEnv* env, jclass, jint streamW, jint streamH, jstring outDir, jboolean srgb) {
    if (outDir == nullptr) {
        QXR_LOGE("VTEST", "nativeCreate: outDir is null");
        return 0;
    }
    const char* utf = env->GetStringUTFChars(outDir, nullptr);
    if (utf == nullptr) {
        QXR_LOGE("VTEST", "nativeCreate: GetStringUTFChars failed");
        return 0;
    }
    const std::string dir(utf);
    env->ReleaseStringUTFChars(outDir, utf);

    Handle* handle = nullptr;
    try {
        handle = new Handle();
        if (!handle->test.create(static_cast<int>(streamW), static_cast<int>(streamH), dir.c_str(),
                                 srgb == JNI_TRUE)) {
            delete handle;
            return 0;
        }
    } catch (const std::exception& ex) {
        QXR_LOGE("VTEST", "nativeCreate threw: %s", ex.what());
        delete handle;
        return 0;
    }
    QXR_LOGI("VTEST", "nativeCreate %dx%d srgb=%d handle=%p", static_cast<int>(streamW), static_cast<int>(streamH),
             srgb == JNI_TRUE ? 1 : 0, static_cast<void*>(handle));
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(handle));
}

jobject nativeGetSurface(JNIEnv* env, jclass, jlong raw) {
    std::lock_guard<std::mutex> lock(g_mu);
    Handle* handle = castHandle(raw);
    if (handle == nullptr || handle->dying.load(std::memory_order_acquire)) {
        QXR_LOGE("VTEST", "nativeGetSurface: bad handle");
        return nullptr;
    }
    ANativeWindow* window = handle->test.decoderWindow();
    if (window == nullptr) {
        QXR_LOGE("VTEST", "nativeGetSurface: decoder window is null");
        return nullptr;
    }
    // §QXR-VIDEO-TEST：decoderWindow 已 acquire。toSurface 再加的引用屬於回傳的 Surface，這裡把多拿的那份放掉。
    jobject surface = ANativeWindow_toSurface(env, window);
    ANativeWindow_release(window);
    if (surface == nullptr) {
        QXR_LOGE("VTEST", "ANativeWindow_toSurface failed");
    }
    return surface;
}

void nativeStart(JNIEnv*, jclass, jlong raw) {
    std::lock_guard<std::mutex> lock(g_mu);
    Handle* handle = castHandle(raw);
    if (handle == nullptr || handle->dying.load(std::memory_order_acquire)) {
        QXR_LOGE("VTEST", "nativeStart: bad handle");
        return;
    }
    handle->test.start();
}

void nativeStop(JNIEnv*, jclass, jlong raw) {
    std::lock_guard<std::mutex> lock(g_mu);
    Handle* handle = castHandle(raw);
    if (handle == nullptr || handle->dying.load(std::memory_order_acquire)) {
        return;
    }
    handle->test.stop();
}

void nativeDestroy(JNIEnv*, jclass, jlong raw) {
    std::lock_guard<std::mutex> lock(g_mu);
    Handle* handle = castHandle(raw);
    if (handle == nullptr) {
        return;
    }
    if (handle->dying.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    handle->magic.store(0, std::memory_order_release);
    delete handle;
}

}  // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_limelight_vr_QxrVideoTest_nativeCreate(JNIEnv* env, jclass clazz, jint streamW, jint streamH, jstring outDir,
                                               jboolean srgb) {
    return nativeCreate(env, clazz, streamW, streamH, outDir, srgb);
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_limelight_vr_QxrVideoTest_nativeGetSurface(JNIEnv* env, jclass clazz, jlong handle) {
    return nativeGetSurface(env, clazz, handle);
}

extern "C" JNIEXPORT void JNICALL
Java_com_limelight_vr_QxrVideoTest_nativeStart(JNIEnv* env, jclass clazz, jlong handle) {
    nativeStart(env, clazz, handle);
}

extern "C" JNIEXPORT void JNICALL
Java_com_limelight_vr_QxrVideoTest_nativeStop(JNIEnv* env, jclass clazz, jlong handle) {
    nativeStop(env, clazz, handle);
}

extern "C" JNIEXPORT void JNICALL
Java_com_limelight_vr_QxrVideoTest_nativeDestroy(JNIEnv* env, jclass clazz, jlong handle) {
    nativeDestroy(env, clazz, handle);
}
