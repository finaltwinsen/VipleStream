// VipleStream §I.F.A — RIFE custom warp layer for stock ncnn.
//
// Source extracted from rife-ncnn-vulkan (BSD-3-Clause, nihui/ncnn).
// .comp shaders embedded inline as raw string literals — ncnn's
// glslang compiles them once on first dispatch.

#ifdef _WIN32

// NOMINMAX must come before any header that pulls in <Windows.h> so
// std::min / std::max in <algorithm> aren't trampled by the Win32
// macros (we don't include <Windows.h> directly here, but ncnn's
// transitive includes might).
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ncnn_rife_warp.h"

#include <ncnn/gpu.h>
#include <ncnn/mat.h>

#include <algorithm>
#include <cmath>
#include <mutex>

using namespace ncnn;

namespace {

// ---- Embedded GLSL sources ----
// Verbatim copy from rife-ncnn-vulkan/src/{warp,warp_pack4,warp_pack8}.comp.
// ncnn::compile_spirv_module reads (data, size) so we don't need a
// trailing null; raw string literals give us exactly the right bytes.

static const char kWarpComp[] = R"GLSL(
#version 450

#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#endif
#if NCNN_fp16_arithmetic
#extension GL_EXT_shader_explicit_arithmetic_types_float16: require
#endif

layout (binding = 0) readonly buffer image_blob { sfp image_blob_data[]; };
layout (binding = 1) readonly buffer flow_blob { sfp flow_blob_data[]; };
layout (binding = 2) writeonly buffer top_blob { sfp top_blob_data[]; };

layout (push_constant) uniform parameter
{
    int w;
    int h;
    int c;
    int cstep;
} p;

void main()
{
    int gx = int(gl_GlobalInvocationID.x);
    int gy = int(gl_GlobalInvocationID.y);
    int gz = int(gl_GlobalInvocationID.z);

    if (gx >= p.w || gy >= p.h || gz >= p.c)
        return;

    afp flow_x = buffer_ld1(flow_blob_data, gy * p.w + gx);
    afp flow_y = buffer_ld1(flow_blob_data, p.cstep + gy * p.w + gx);

    afp sample_x = afp(gx) + flow_x;
    afp sample_y = afp(gy) + flow_y;

    afp v;
    {
        int x0 = int(floor(sample_x));
        int y0 = int(floor(sample_y));
        int x1 = x0 + 1;
        int y1 = y0 + 1;

        x0 = clamp(x0, 0, p.w - 1);
        y0 = clamp(y0, 0, p.h - 1);
        x1 = clamp(x1, 0, p.w - 1);
        y1 = clamp(y1, 0, p.h - 1);

        afp alpha = sample_x - afp(x0);
        afp beta = sample_y - afp(y0);

        afp v0 = buffer_ld1(image_blob_data, gz * p.cstep + y0 * p.w + x0);
        afp v1 = buffer_ld1(image_blob_data, gz * p.cstep + y0 * p.w + x1);
        afp v2 = buffer_ld1(image_blob_data, gz * p.cstep + y1 * p.w + x0);
        afp v3 = buffer_ld1(image_blob_data, gz * p.cstep + y1 * p.w + x1);

        afp v4 = v0 * (afp(1.f) - alpha) + v1 * alpha;
        afp v5 = v2 * (afp(1.f) - alpha) + v3 * alpha;

        v = v4 * (afp(1.f) - beta) + v5 * beta;
    }

    const int gi = gz * p.cstep + gy * p.w + gx;

    buffer_st1(top_blob_data, gi, v);
}
)GLSL";

static const char kWarpPack4Comp[] = R"GLSL(
#version 450

#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#endif
#if NCNN_fp16_arithmetic
#extension GL_EXT_shader_explicit_arithmetic_types_float16: require
#endif

layout (binding = 0) readonly buffer image_blob { sfpvec4 image_blob_data[]; };
layout (binding = 1) readonly buffer flow_blob { sfp flow_blob_data[]; };
layout (binding = 2) writeonly buffer top_blob { sfpvec4 top_blob_data[]; };

layout (push_constant) uniform parameter
{
    int w;
    int h;
    int c;
    int cstep;
} p;

void main()
{
    int gx = int(gl_GlobalInvocationID.x);
    int gy = int(gl_GlobalInvocationID.y);
    int gz = int(gl_GlobalInvocationID.z);

    if (gx >= p.w || gy >= p.h || gz >= p.c)
        return;

    afp flow_x = buffer_ld1(flow_blob_data, gy * p.w + gx);
    afp flow_y = buffer_ld1(flow_blob_data, p.cstep + gy * p.w + gx);

    afp sample_x = afp(gx) + flow_x;
    afp sample_y = afp(gy) + flow_y;

    afpvec4 v;
    {
        int x0 = int(floor(sample_x));
        int y0 = int(floor(sample_y));
        int x1 = x0 + 1;
        int y1 = y0 + 1;

        x0 = clamp(x0, 0, p.w - 1);
        y0 = clamp(y0, 0, p.h - 1);
        x1 = clamp(x1, 0, p.w - 1);
        y1 = clamp(y1, 0, p.h - 1);

        afp alpha = sample_x - afp(x0);
        afp beta = sample_y - afp(y0);

        afpvec4 v0 = buffer_ld4(image_blob_data, gz * p.cstep + y0 * p.w + x0);
        afpvec4 v1 = buffer_ld4(image_blob_data, gz * p.cstep + y0 * p.w + x1);
        afpvec4 v2 = buffer_ld4(image_blob_data, gz * p.cstep + y1 * p.w + x0);
        afpvec4 v3 = buffer_ld4(image_blob_data, gz * p.cstep + y1 * p.w + x1);

        afpvec4 v4 = v0 * (afp(1.f) - alpha) + v1 * alpha;
        afpvec4 v5 = v2 * (afp(1.f) - alpha) + v3 * alpha;

        v = v4 * (afp(1.f) - beta) + v5 * beta;
    }

    const int gi = gz * p.cstep + gy * p.w + gx;

    buffer_st4(top_blob_data, gi, v);
}
)GLSL";

static const char kWarpPack8Comp[] = R"GLSL(
#version 450

#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
struct sfpvec8 { f16vec4 abcd; f16vec4 efgh; };
#endif
#if NCNN_fp16_arithmetic
#extension GL_EXT_shader_explicit_arithmetic_types_float16: require
#endif

layout (binding = 0) readonly buffer image_blob { sfpvec8 image_blob_data[]; };
layout (binding = 1) readonly buffer flow_blob { sfp flow_blob_data[]; };
layout (binding = 2) writeonly buffer top_blob { sfpvec8 top_blob_data[]; };

layout (push_constant) uniform parameter
{
    int w;
    int h;
    int c;
    int cstep;
} p;

void main()
{
    int gx = int(gl_GlobalInvocationID.x);
    int gy = int(gl_GlobalInvocationID.y);
    int gz = int(gl_GlobalInvocationID.z);

    if (gx >= p.w || gy >= p.h || gz >= p.c)
        return;

    afp flow_x = buffer_ld1(flow_blob_data, gy * p.w + gx);
    afp flow_y = buffer_ld1(flow_blob_data, p.cstep + gy * p.w + gx);

    afp sample_x = afp(gx) + flow_x;
    afp sample_y = afp(gy) + flow_y;

    afpvec8 v;
    {
        int x0 = int(floor(sample_x));
        int y0 = int(floor(sample_y));
        int x1 = x0 + 1;
        int y1 = y0 + 1;

        x0 = clamp(x0, 0, p.w - 1);
        y0 = clamp(y0, 0, p.h - 1);
        x1 = clamp(x1, 0, p.w - 1);
        y1 = clamp(y1, 0, p.h - 1);

        afp alpha = sample_x - afp(x0);
        afp beta = sample_y - afp(y0);

        afpvec8 v0 = buffer_ld8(image_blob_data, gz * p.cstep + y0 * p.w + x0);
        afpvec8 v1 = buffer_ld8(image_blob_data, gz * p.cstep + y0 * p.w + x1);
        afpvec8 v2 = buffer_ld8(image_blob_data, gz * p.cstep + y1 * p.w + x0);
        afpvec8 v3 = buffer_ld8(image_blob_data, gz * p.cstep + y1 * p.w + x1);

        afpvec8 v4;
        afpvec8 v5;

        v4[0] = v0[0] * (afp(1.f) - alpha) + v1[0] * alpha;
        v4[1] = v0[1] * (afp(1.f) - alpha) + v1[1] * alpha;
        v5[0] = v2[0] * (afp(1.f) - alpha) + v3[0] * alpha;
        v5[1] = v2[1] * (afp(1.f) - alpha) + v3[1] * alpha;

        v[0] = v4[0] * (afp(1.f) - beta) + v5[0] * beta;
        v[1] = v4[1] * (afp(1.f) - beta) + v5[1] * beta;
    }

    const int gi = gz * p.cstep + gy * p.w + gx;

    buffer_st8(top_blob_data, gi, v);
}
)GLSL";

} // namespace

namespace viple {

RifeWarp::RifeWarp()
{
    support_vulkan = true;
}

int RifeWarp::create_pipeline(const ncnn::Option& opt)
{
    if (!vkdev) return 0;

    std::vector<vk_specialization_type> specializations(0);

    // pack1
    {
        static std::vector<uint32_t> spirv;
        static std::mutex lock;
        std::lock_guard<std::mutex> guard(lock);
        if (spirv.empty()) {
            compile_spirv_module(kWarpComp, sizeof(kWarpComp) - 1, opt, spirv);
        }
        pipeline_warp = new Pipeline(vkdev);
        pipeline_warp->set_optimal_local_size_xyz();
        pipeline_warp->create(spirv.data(), spirv.size() * 4, specializations);
    }

    // pack4
    {
        static std::vector<uint32_t> spirv;
        static std::mutex lock;
        std::lock_guard<std::mutex> guard(lock);
        if (spirv.empty()) {
            compile_spirv_module(kWarpPack4Comp, sizeof(kWarpPack4Comp) - 1, opt, spirv);
        }
        pipeline_warp_pack4 = new Pipeline(vkdev);
        pipeline_warp_pack4->set_optimal_local_size_xyz();
        pipeline_warp_pack4->create(spirv.data(), spirv.size() * 4, specializations);
    }

    // pack8 dropped — stock ncnn 1.20+ removed `use_shader_pack8`
    // and rebound the macros (`afpvec8`/`sfpvec8`) to require both
    // fp16_storage and fp16_arithmetic + a custom builder hook our
    // simplified runtime register doesn't replicate.  pack4 covers
    // every Tensor-Core-class GPU we care about; the only loss is
    // ~5-8% throughput on RTX 40-class with fp16e2m1, well below
    // measurement noise for FRUC's 30 fps frame budget.
    pipeline_warp_pack8 = nullptr;

    return 0;
}

int RifeWarp::destroy_pipeline(const ncnn::Option& /*opt*/)
{
    delete pipeline_warp;       pipeline_warp       = nullptr;
    delete pipeline_warp_pack4; pipeline_warp_pack4 = nullptr;
    delete pipeline_warp_pack8; pipeline_warp_pack8 = nullptr;
    return 0;
}

int RifeWarp::forward(const std::vector<Mat>& bottom_blobs,
                      std::vector<Mat>& top_blobs,
                      const ncnn::Option& opt) const
{
    // CPU fallback — only fires when the user disables Vulkan in
    // ncnn::Option (we never do, but ncnn calls forward() on cleanup
    // paths sometimes).  Plain bilinear sample, no SIMD.
    const Mat& image_blob = bottom_blobs[0];
    const Mat& flow_blob  = bottom_blobs[1];

    const int w = image_blob.w;
    const int h = image_blob.h;
    const int channels = image_blob.c;

    Mat& top_blob = top_blobs[0];
    top_blob.create(w, h, channels);
    if (top_blob.empty()) return -100;

    #pragma omp parallel for num_threads(opt.num_threads)
    for (int q = 0; q < channels; q++) {
        float* outptr = top_blob.channel(q);
        const Mat image = image_blob.channel(q);
        const float* fxptr = flow_blob.channel(0);
        const float* fyptr = flow_blob.channel(1);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const float sample_x = x + fxptr[0];
                const float sample_y = y + fyptr[0];
                int x0 = static_cast<int>(std::floor(sample_x));
                int y0 = static_cast<int>(std::floor(sample_y));
                int x1 = x0 + 1, y1 = y0 + 1;
                x0 = std::min(std::max(x0, 0), w - 1);
                y0 = std::min(std::max(y0, 0), h - 1);
                x1 = std::min(std::max(x1, 0), w - 1);
                y1 = std::min(std::max(y1, 0), h - 1);
                const float alpha = sample_x - x0;
                const float beta  = sample_y - y0;
                const float v0 = image.row(y0)[x0];
                const float v1 = image.row(y0)[x1];
                const float v2 = image.row(y1)[x0];
                const float v3 = image.row(y1)[x1];
                const float v4 = v0 * (1.f - alpha) + v1 * alpha;
                const float v5 = v2 * (1.f - alpha) + v3 * alpha;
                outptr[0] = v4 * (1.f - beta) + v5 * beta;
                outptr += 1;
                fxptr += 1;
                fyptr += 1;
            }
        }
    }
    return 0;
}

int RifeWarp::forward(const std::vector<VkMat>& bottom_blobs,
                      std::vector<VkMat>& top_blobs,
                      VkCompute& cmd,
                      const ncnn::Option& opt) const
{
    const VkMat& image_blob = bottom_blobs[0];
    const VkMat& flow_blob  = bottom_blobs[1];

    const int w = image_blob.w;
    const int h = image_blob.h;
    const int channels = image_blob.c;
    const size_t elemsize = image_blob.elemsize;
    const int elempack = image_blob.elempack;

    VkMat& top_blob = top_blobs[0];
    top_blob.create(w, h, channels, elemsize, elempack, opt.blob_vkallocator);
    if (top_blob.empty()) return -100;

    std::vector<VkMat> bindings(3);
    bindings[0] = image_blob;
    bindings[1] = flow_blob;
    bindings[2] = top_blob;

    std::vector<vk_constant_type> constants(4);
    constants[0].i = top_blob.w;
    constants[1].i = top_blob.h;
    constants[2].i = top_blob.c;
    constants[3].i = top_blob.cstep;

    // Pack8 path was dropped in create_pipeline; if ncnn somehow
    // hands us an elempack=8 blob (shouldn't happen for a custom
    // layer that didn't declare pack8 support), fall through to
    // pack4 and let ncnn re-pack on the boundary.  pack1 stays as
    // the single-channel scalar fallback.
    if (elempack == 4 || elempack == 8) {
        cmd.record_pipeline(pipeline_warp_pack4, bindings, constants, top_blob);
    } else {
        cmd.record_pipeline(pipeline_warp, bindings, constants, top_blob);
    }

    return 0;
}

ncnn::Layer* createRifeWarp(void* /*userdata*/)
{
    return new RifeWarp();
}

void destroyRifeWarp(ncnn::Layer* layer, void* /*userdata*/)
{
    delete layer;
}

} // namespace viple

#endif // _WIN32
