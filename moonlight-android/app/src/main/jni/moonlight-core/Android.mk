# Android.mk for moonlight-core and binding
MY_LOCAL_PATH := $(call my-dir)

include $(call all-subdir-makefiles)

LOCAL_PATH := $(MY_LOCAL_PATH)

# ── VipleStream §Q: picoquic + picotls static libraries ──────
# When prebuilt .a exist under picoquic/$(TARGET_ARCH_ABI)/, enable
# VIPLE_MPQUIC and link them.  Otherwise the MPQUIC code paths are
# compiled out via #ifdef.
PICOQUIC_LIB_DIR := $(LOCAL_PATH)/picoquic/$(TARGET_ARCH_ABI)
PICOQUIC_AVAILABLE := $(wildcard $(PICOQUIC_LIB_DIR)/libpicoquic-core.a)

ifneq ($(PICOQUIC_AVAILABLE),)

include $(CLEAR_VARS)
LOCAL_MODULE := picoquic-core
LOCAL_SRC_FILES := picoquic/$(TARGET_ARCH_ABI)/libpicoquic-core.a
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/picoquic/include
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := picotls-openssl
LOCAL_SRC_FILES := picoquic/$(TARGET_ARCH_ABI)/libpicotls-openssl.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := picotls-core
LOCAL_SRC_FILES := picoquic/$(TARGET_ARCH_ABI)/libpicotls-core.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := picotls-minicrypto
LOCAL_SRC_FILES := picoquic/$(TARGET_ARCH_ABI)/libpicotls-minicrypto.a
include $(PREBUILT_STATIC_LIBRARY)

endif # PICOQUIC_AVAILABLE

# ── VipleStream PyroWave + Granite static library (arm64-v8a) ─
LOCAL_PATH := $(MY_LOCAL_PATH)

ifeq ($(TARGET_ARCH_ABI),arm64-v8a)
include $(CLEAR_VARS)
LOCAL_MODULE := pyrowave

PYROWAVE_DIR := pyrowave
LOCAL_SRC_FILES := \
    $(PYROWAVE_DIR)/pyrowave_encoder.cpp \
    $(PYROWAVE_DIR)/pyrowave_decoder.cpp \
    $(PYROWAVE_DIR)/pyrowave_common.cpp \
    $(PYROWAVE_DIR)/pyrowave_c.cpp \
    $(PYROWAVE_DIR)/Granite/video/scaler.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/context.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/device.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/wsi.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/wsi_pacer.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/buffer_pool.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/image.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/cookie.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/sampler.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/command_pool.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/fence_manager.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/descriptor_set.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/semaphore_manager.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/command_buffer.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/shader.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/render_pass.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/buffer.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/rtas.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/indirect_layout.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/pipeline_cache.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/semaphore.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/memory_allocator.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/fence.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/event_manager.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/breadcrumbs.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/pipeline_event.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/query_pool.cpp \
    $(PYROWAVE_DIR)/Granite/vulkan/texture/texture_format.cpp \
    $(PYROWAVE_DIR)/Granite/util/logging.cpp \
    $(PYROWAVE_DIR)/Granite/util/aligned_alloc.cpp \
    $(PYROWAVE_DIR)/Granite/util/timer.cpp \
    $(PYROWAVE_DIR)/Granite/util/thread_id.cpp \
    $(PYROWAVE_DIR)/Granite/util/string_helpers.cpp \
    $(PYROWAVE_DIR)/Granite/util/timeline_trace_file.cpp \
    $(PYROWAVE_DIR)/Granite/util/thread_name.cpp \
    $(PYROWAVE_DIR)/Granite/util/thread_priority.cpp \
    $(PYROWAVE_DIR)/Granite/util/cli_parser.cpp \
    $(PYROWAVE_DIR)/Granite/util/dynamic_library.cpp \
    $(PYROWAVE_DIR)/Granite/util/message_queue.cpp \
    $(PYROWAVE_DIR)/Granite/util/arena_allocator.cpp \
    $(PYROWAVE_DIR)/Granite/util/environment.cpp \
    $(PYROWAVE_DIR)/Granite/util/slab_allocator.cpp \
    $(PYROWAVE_DIR)/Granite/math/math.cpp \
    $(PYROWAVE_DIR)/Granite/math/frustum.cpp \
    $(PYROWAVE_DIR)/Granite/math/aabb.cpp \
    $(PYROWAVE_DIR)/Granite/math/interpolation.cpp \
    $(PYROWAVE_DIR)/Granite/math/muglm/muglm.cpp \
    $(PYROWAVE_DIR)/Granite/math/transforms.cpp \
    $(PYROWAVE_DIR)/Granite/application/global/global_managers.cpp \
    $(PYROWAVE_DIR)/Granite/third_party/volk/volk.cpp

PYROWAVE_INCLUDES := \
    $(LOCAL_PATH)/pyrowave \
    $(LOCAL_PATH)/pyrowave/shaders \
    $(LOCAL_PATH)/pyrowave/Granite/video \
    $(LOCAL_PATH)/pyrowave/Granite/vulkan \
    $(LOCAL_PATH)/pyrowave/Granite/vulkan/texture \
    $(LOCAL_PATH)/pyrowave/Granite/util \
    $(LOCAL_PATH)/pyrowave/Granite/math \
    $(LOCAL_PATH)/pyrowave/Granite/application/global \
    $(LOCAL_PATH)/pyrowave/Granite/third_party/volk \
    $(LOCAL_PATH)/pyrowave/Granite/third_party/khronos/vulkan-headers/include

LOCAL_C_INCLUDES := $(PYROWAVE_INCLUDES)
LOCAL_EXPORT_C_INCLUDES := $(PYROWAVE_INCLUDES)
LOCAL_CFLAGS := -DVK_NO_PROTOTYPES -DVK_USE_PLATFORM_ANDROID_KHR -DGRANITE_SHIPPING=1 -DPYROWAVE_PRECISION=1 -DVOLK_NAMESPACE -w
include $(BUILD_STATIC_LIBRARY)
endif # arm64-v8a

# ── moonlight-core ────────────────────────────────────────────
LOCAL_PATH := $(MY_LOCAL_PATH)

include $(CLEAR_VARS)
LOCAL_MODULE    := moonlight-core

LOCAL_SRC_FILES := moonlight-common-c/src/AudioStream.c \
                   moonlight-common-c/src/ByteBuffer.c \
                   moonlight-common-c/src/Connection.c \
                   moonlight-common-c/src/ConnectionTester.c \
                   moonlight-common-c/src/ControlStream.c \
                   moonlight-common-c/src/FakeCallbacks.c \
                   moonlight-common-c/src/HolePunch.c \
                   moonlight-common-c/src/InputStream.c \
                   moonlight-common-c/src/LinkedBlockingQueue.c \
                   moonlight-common-c/src/Misc.c \
                   moonlight-common-c/src/Platform.c \
                   moonlight-common-c/src/PlatformCrypto.c \
                   moonlight-common-c/src/PlatformSockets.c \
                   moonlight-common-c/src/RtpAudioQueue.c \
                   moonlight-common-c/src/RtpVideoQueue.c \
                   moonlight-common-c/src/rswrapper.c \
                   moonlight-common-c/src/RtspConnection.c \
                   moonlight-common-c/src/RtspParser.c \
                   moonlight-common-c/src/SdpGenerator.c \
                   moonlight-common-c/src/SimpleStun.c \
                   moonlight-common-c/src/VideoDepacketizer.c \
                   moonlight-common-c/src/VideoStream.c \
                   moonlight-common-c/src/VrMultiLink.c \
                   moonlight-common-c/src/PlatformNetIf.c \
                   moonlight-common-c/src/QuicTransport.c \
                   moonlight-common-c/enet/callbacks.c \
                   moonlight-common-c/enet/compress.c \
                   moonlight-common-c/enet/host.c \
                   moonlight-common-c/enet/list.c \
                   moonlight-common-c/enet/packet.c \
                   moonlight-common-c/enet/peer.c \
                   moonlight-common-c/enet/protocol.c \
                   moonlight-common-c/enet/unix.c \
                   moonlight-common-c/enet/win32.c \
                   simplejni.c \
                   callbacks.c \
                   minisdl.c \
                   vk_probe.c \
                   vk_backend.c \
                   pyrowave_android_bridge.cpp \


LOCAL_C_INCLUDES := $(LOCAL_PATH)/moonlight-common-c/enet/include \
                    $(LOCAL_PATH)/moonlight-common-c/nanors \
                    $(LOCAL_PATH)/moonlight-common-c/nanors/deps \
                    $(LOCAL_PATH)/moonlight-common-c/nanors/deps/obl \
                    $(LOCAL_PATH)/moonlight-common-c/src \

LOCAL_CFLAGS := -DHAS_SOCKLEN_T=1 -DLC_ANDROID -DHAVE_CLOCK_GETTIME=1 -DNDEBUG \
                -D__ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__

# VipleStream §Q: enable MP-QUIC when picoquic prebuilt is available
ifneq ($(PICOQUIC_AVAILABLE),)
LOCAL_CFLAGS += -DVIPLE_MPQUIC=1
LOCAL_C_INCLUDES += $(LOCAL_PATH)/picoquic/include
endif

ifeq ($(NDK_DEBUG),1)
LOCAL_CFLAGS += -DLC_DEBUG
endif

LOCAL_LDLIBS := -llog -landroid -ldl

LOCAL_STATIC_LIBRARIES := libopus libssl libcrypto cpufeatures
ifeq ($(TARGET_ARCH_ABI),arm64-v8a)
LOCAL_CFLAGS += -DVIPLESTREAM_HAVE_PYROWAVE=1 -DVK_NO_PROTOTYPES -DVK_USE_PLATFORM_ANDROID_KHR -DGRANITE_SHIPPING=1 -DPYROWAVE_PRECISION=1
LOCAL_CPPFLAGS += -DVOLK_NAMESPACE
LOCAL_STATIC_LIBRARIES += pyrowave
endif
ifneq ($(PICOQUIC_AVAILABLE),)
LOCAL_STATIC_LIBRARIES += picoquic-core picotls-openssl picotls-core picotls-minicrypto
endif
LOCAL_LDFLAGS += -Wl,--exclude-libs,ALL

LOCAL_BRANCH_PROTECTION := standard

include $(BUILD_SHARED_LIBRARY)

$(call import-module,android/cpufeatures)
