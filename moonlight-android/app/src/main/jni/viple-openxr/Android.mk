# quad.vert.spv.h、quad.frag.spv.h 要先跑 shaders\build_shaders.cmd 產生並簽入。
# 裝置上沒有 glslc，缺這兩個標頭時 qxr_video.cpp 無法編譯。

ifeq ($(TARGET_ARCH_ABI),arm64-v8a)

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := viple-openxr
LOCAL_SRC_FILES := qxr_video.cpp qxr_session.cpp qxr_synthetic.cpp qxr_tracking.cpp qxr_framemeta.cpp qxr_vrmsg.cpp qxr_jni.cpp

# moonlight-core 沒有 LOCAL_EXPORT_C_INCLUDES，所以標頭路徑寫在 LOCAL_C_INCLUDES，不改那個 module。
LOCAL_SHARED_LIBRARIES := moonlight-core openxr_loader
LOCAL_LDLIBS := -lvulkan -lmediandk -landroid -llog

# moonlight-qt/app/streaming/xr：之後要直接 include 那邊只用標準庫的標頭，這次不 include。
LOCAL_C_INCLUDES := \
    $(LOCAL_PATH) \
    $(LOCAL_PATH)/shaders \
    $(LOCAL_PATH)/../moonlight-core \
    $(LOCAL_PATH)/../moonlight-core/moonlight-common-c/src \
    $(LOCAL_PATH)/../../../../../../moonlight-qt/app/streaming/xr

LOCAL_CPPFLAGS := -DXR_USE_PLATFORM_ANDROID -DXR_USE_GRAPHICS_API_VULKAN -DVK_USE_PLATFORM_ANDROID_KHR

# APP_PLATFORM 是 28，但有用到 API 31 的 AHardwareBuffer_getId（以 __builtin_available 保護）。
# 這個巨集要在任何 NDK 標頭之前定義才有效，所以放在編譯旗標，不放在原始碼裡。
LOCAL_CFLAGS := -D__ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__

include $(BUILD_SHARED_LIBRARY)

# prefab 套件名是 OpenXR、模組名是 openxr_loader（看 build/.cxx/**/prefab/<abi>/prefab/ 底下的目錄）。
$(call import-module,prefab/OpenXR)

endif
