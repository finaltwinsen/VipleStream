cmake_minimum_required(VERSION 3.20)
project(pyrowave_bundled LANGUAGES CXX C)

set(PYROWAVE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/pyrowave_encoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pyrowave_decoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pyrowave_common.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pyrowave_c.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/video/scaler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/context.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/device.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/wsi.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/wsi_pacer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/buffer_pool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/image.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/cookie.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/sampler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/command_pool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/fence_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/descriptor_set.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/semaphore_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/command_buffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/shader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/render_pass.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/buffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/rtas.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/indirect_layout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/pipeline_cache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/semaphore.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/memory_allocator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/fence.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/event_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/breadcrumbs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/pipeline_event.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/query_pool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/texture/texture_format.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/util/logging.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/util/aligned_alloc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/util/timer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/util/thread_id.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/util/string_helpers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/util/timeline_trace_file.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/util/thread_name.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/util/thread_priority.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/util/cli_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/util/dynamic_library.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/util/message_queue.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/util/arena_allocator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/util/environment.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/util/slab_allocator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/math/math.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/math/frustum.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/math/aabb.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/math/interpolation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/math/muglm/muglm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/math/transforms.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/application/global/global_managers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Granite/third_party/volk/volk.cpp
)

add_library(pyrowave STATIC ${PYROWAVE_SOURCES})
set_target_properties(pyrowave PROPERTIES
    CXX_STANDARD 17
    C_STANDARD 99
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(pyrowave
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Granite/third_party/khronos/vulkan-headers/include
        ${CMAKE_CURRENT_SOURCE_DIR}/Granite/third_party/volk
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders
        ${CMAKE_CURRENT_SOURCE_DIR}/Granite/video
        ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan
        ${CMAKE_CURRENT_SOURCE_DIR}/Granite/vulkan/texture
        ${CMAKE_CURRENT_SOURCE_DIR}/Granite/util
        ${CMAKE_CURRENT_SOURCE_DIR}/Granite/math
        ${CMAKE_CURRENT_SOURCE_DIR}/Granite/application/global
)

target_compile_definitions(pyrowave
    PRIVATE
        PYROWAVE_PRECISION=1
        GRANITE_SHIPPING=1
        VK_NO_PROTOTYPES
        VOLK_NAMESPACE
)

if(WIN32)
    target_compile_definitions(pyrowave PRIVATE VK_USE_PLATFORM_WIN32_KHR NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
    target_link_libraries(pyrowave PUBLIC dl pthread)
endif()

if(MSVC)
    target_compile_options(pyrowave PRIVATE /wd4267 /wd4244 /wd4309 /wd4005)
else()
    target_compile_options(pyrowave PRIVATE -w)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86)|(X86)|(amd64)|(AMD64)")
        target_compile_options(pyrowave PRIVATE -msse3)
    endif()
endif()
