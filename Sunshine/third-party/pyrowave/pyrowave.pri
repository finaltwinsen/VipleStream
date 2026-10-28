# VipleStream PyroWave bundled static sources for qmake (moonlight-qt)
DEFINES += VIPLESTREAM_HAVE_PYROWAVE PYROWAVE_PRECISION=1 GRANITE_SHIPPING=1

INCLUDEPATH += \
    $$PWD \
    $$PWD/shaders \
    $$PWD/Granite/video \
    $$PWD/Granite/vulkan \
    $$PWD/Granite/vulkan/texture \
    $$PWD/Granite/util \
    $$PWD/Granite/math \
    $$PWD/Granite/application/global \
    $$PWD/Granite/third_party/volk \
    $$PWD/Granite/third_party/khronos/vulkan-headers/include

SOURCES += \
    $$PWD/pyrowave_encoder.cpp \
    $$PWD/pyrowave_decoder.cpp \
    $$PWD/pyrowave_common.cpp \
    $$PWD/pyrowave_c.cpp \
    $$PWD/Granite/video/scaler.cpp \
    $$PWD/Granite/vulkan/context.cpp \
    $$PWD/Granite/vulkan/device.cpp \
    $$PWD/Granite/vulkan/wsi.cpp \
    $$PWD/Granite/vulkan/wsi_pacer.cpp \
    $$PWD/Granite/vulkan/buffer_pool.cpp \
    $$PWD/Granite/vulkan/image.cpp \
    $$PWD/Granite/vulkan/cookie.cpp \
    $$PWD/Granite/vulkan/sampler.cpp \
    $$PWD/Granite/vulkan/command_pool.cpp \
    $$PWD/Granite/vulkan/fence_manager.cpp \
    $$PWD/Granite/vulkan/descriptor_set.cpp \
    $$PWD/Granite/vulkan/semaphore_manager.cpp \
    $$PWD/Granite/vulkan/command_buffer.cpp \
    $$PWD/Granite/vulkan/shader.cpp \
    $$PWD/Granite/vulkan/render_pass.cpp \
    $$PWD/Granite/vulkan/buffer.cpp \
    $$PWD/Granite/vulkan/rtas.cpp \
    $$PWD/Granite/vulkan/indirect_layout.cpp \
    $$PWD/Granite/vulkan/pipeline_cache.cpp \
    $$PWD/Granite/vulkan/semaphore.cpp \
    $$PWD/Granite/vulkan/memory_allocator.cpp \
    $$PWD/Granite/vulkan/fence.cpp \
    $$PWD/Granite/vulkan/event_manager.cpp \
    $$PWD/Granite/vulkan/breadcrumbs.cpp \
    $$PWD/Granite/vulkan/pipeline_event.cpp \
    $$PWD/Granite/vulkan/query_pool.cpp \
    $$PWD/Granite/vulkan/texture/texture_format.cpp \
    $$PWD/Granite/util/logging.cpp \
    $$PWD/Granite/util/aligned_alloc.cpp \
    $$PWD/Granite/util/timer.cpp \
    $$PWD/Granite/util/thread_id.cpp \
    $$PWD/Granite/util/string_helpers.cpp \
    $$PWD/Granite/util/timeline_trace_file.cpp \
    $$PWD/Granite/util/thread_name.cpp \
    $$PWD/Granite/util/thread_priority.cpp \
    $$PWD/Granite/util/cli_parser.cpp \
    $$PWD/Granite/util/dynamic_library.cpp \
    $$PWD/Granite/util/message_queue.cpp \
    $$PWD/Granite/util/arena_allocator.cpp \
    $$PWD/Granite/util/environment.cpp \
    $$PWD/Granite/util/slab_allocator.cpp \
    $$PWD/Granite/math/math.cpp \
    $$PWD/Granite/math/frustum.cpp \
    $$PWD/Granite/math/aabb.cpp \
    $$PWD/Granite/math/interpolation.cpp \
    $$PWD/Granite/math/muglm/muglm.cpp \
    $$PWD/Granite/math/transforms.cpp \
    $$PWD/Granite/application/global/global_managers.cpp \
    $$PWD/Granite/third_party/volk/volk.c
