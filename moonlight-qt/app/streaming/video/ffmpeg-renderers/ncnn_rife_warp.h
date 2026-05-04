// VipleStream §I.F.A — RIFE custom warp layer for stock ncnn.
//
// rife-ncnn-vulkan adds a "rife.Warp" custom layer that does flow-
// based bilinear sampling (the W in motion-compensated F + W = next
// frame).  Stock ncnn doesn't ship this layer, so loading the
// flownet.param from any RIFE 4.x build fails with rc=-1 at parse.
//
// This file extracts the layer source from rife-ncnn-vulkan
// (BSD-3-Clause licensed, same author/maintainer as ncnn) so we can
// keep using stock ncnn.dll and register the layer at runtime via
// ncnn::Net::register_custom_layer("rife.Warp", ...).
//
// The .comp SPIR-V sources are embedded as raw string literals in
// the .cpp; ncnn::compile_spirv_module() compiles them at first
// dispatch via ncnn's bundled glslang (cost paid once per session).

#pragma once

// VipleStream §K.1: gate on VIPLESTREAM_HAVE_NCNN (set by app.pro when ncnn
// is available — Windows prebuilt libs/windows/ncnn always; Linux when
// /usr/local/include/ncnn/mat.h exists from source build).
#ifdef VIPLESTREAM_HAVE_NCNN

#include <ncnn/layer.h>
#include <ncnn/pipeline.h>

#include <vector>

namespace viple {

class RifeWarp : public ncnn::Layer
{
public:
    RifeWarp();

    int create_pipeline(const ncnn::Option& opt) override;
    int destroy_pipeline(const ncnn::Option& opt) override;

    int forward(const std::vector<ncnn::Mat>& bottom_blobs,
                std::vector<ncnn::Mat>& top_blobs,
                const ncnn::Option& opt) const override;

    int forward(const std::vector<ncnn::VkMat>& bottom_blobs,
                std::vector<ncnn::VkMat>& top_blobs,
                ncnn::VkCompute& cmd,
                const ncnn::Option& opt) const override;

private:
    ncnn::Pipeline* pipeline_warp       = nullptr;
    ncnn::Pipeline* pipeline_warp_pack4 = nullptr;
    ncnn::Pipeline* pipeline_warp_pack8 = nullptr;
};

// Factory used by ncnn::Net::register_custom_layer.
ncnn::Layer* createRifeWarp(void* /*userdata*/);
void         destroyRifeWarp(ncnn::Layer* layer, void* /*userdata*/);

} // namespace viple

#endif // VIPLESTREAM_HAVE_NCNN
