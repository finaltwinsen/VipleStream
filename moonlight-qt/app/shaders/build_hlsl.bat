fxc /T vs_5_0 /O3 /Fo d3d11_vertex.fxc d3d11_vertex.hlsl

fxc /T ps_5_0 /O3 /Fo d3d11_overlay_pixel.fxc d3d11_overlay_pixel.hlsl
fxc /T ps_5_0 /O3 /Fo d3d11_yuv420_pixel.fxc d3d11_yuv420_pixel.hlsl
fxc /T ps_5_0 /O3 /Fo d3d11_ayuv_pixel.fxc d3d11_ayuv_pixel.hlsl
fxc /T ps_5_0 /O3 /Fo d3d11_y410_pixel.fxc d3d11_y410_pixel.hlsl
fxc /T ps_5_0 /O3 /Fo d3d11_bicubic_scale_pixel.fxc d3d11_bicubic_scale_pixel.hlsl

REM VipleStream §I (D3D11 Generic FRUC compute pipeline)
fxc /T cs_5_0 /O3 /Fo d3d11_warp_compute.fxc d3d11_warp_compute.hlsl
fxc /T cs_5_0 /O3 /Fo d3d11_motionest_compute.fxc d3d11_motionest_compute.hlsl
fxc /T cs_5_0 /O3 /Fo d3d11_mv_median.fxc d3d11_mv_median.hlsl
fxc /T cs_5_0 /O3 /Fo d3d11_fruc_blend_fp32.fxc d3d11_fruc_blend_fp32.hlsl

REM VipleStream §I (DirectML pack/unpack helpers)
fxc /T cs_5_0 /O3 /Fo d3d11_dml_pack_rgba8_fp16.fxc d3d11_dml_pack_rgba8_fp16.hlsl
fxc /T cs_5_0 /O3 /Fo d3d11_dml_unpack_fp16_rgba8.fxc d3d11_dml_unpack_fp16_rgba8.hlsl
