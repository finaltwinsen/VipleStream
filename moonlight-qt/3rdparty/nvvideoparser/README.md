# nvvideoparser — third-party H.265 bitstream parser

Source: NVIDIA / Khronos `vk_video_samples` repo  
Upstream: <https://github.com/nvpro-samples/vk_video_samples>  
License: Apache 2.0 (see `LICENSE`)  
Imported: 2026-04-30

## What this is

VipleStream §J.3.e.2.i.8 native VK_KHR_video_decode path 需要把 H.265
bitstream 的 VPS/SPS/PPS 解析成 `StdVideoH265*ParameterSet` 結構交給
Vulkan driver.  自寫 ~600 LOC parser 風險高（HEVC 邊角條件多，錯一
個欄位 driver 直接 reject），改從 NVIDIA `vk_video_samples` repo 移植
production-tested 的 parser.

## What's included (minimal H.265 subset)

```
include/
  ByteStreamParser.h            ── bit reader (Exp-Golomb)
  VulkanH265Decoder.h           ── H.265 parser class + std structs
  VulkanH26xDecoder.h           ── common HEVC base
  VulkanVideoDecoder.h          ── parser framework base
  cpudetect.h                   ── runtime SIMD dispatch
  nvVulkanh265ScalingList.h     ── HEVC default scaling lists
  VkVideoRefCountBase.h         ── ref-counting base for shared objects
  VulkanBitstreamBuffer.h       ── bitstream buffer interface
  VulkanVideoParser*.h          ── public parser interface

src/
  VulkanH265Parser.cpp          ── 3214 LOC, the core parser
  VulkanVideoDecoder.cpp        ── parser framework base
  cpudetect.cpp                 ── CPUID for SIMD dispatch
  NextStartCodeC.cpp            ── plain-C start-code scanner (SIMD variants
                                   not included; the C path always works)
```

## What's intentionally NOT included

- H.264 parser (`VulkanH264Parser.cpp`, ~4700 LOC) — Phase 2 後加
- AV1 parser (`VulkanAV1Decoder.cpp`, ~2500 LOC) — Phase 3 後加
- VP9 parser — VipleStream 不打算支援
- SIMD start-code scanners (AVX2 / AVX512 / NEON / SSSE3 / SVE) —
  C 版本足夠快 for 1080p60；之後性能 profile 缺再補 AVX2
- Frame buffer / decoder framework (`VkVideoDecoder/`) — VipleStream
  自己 manage decode session + DPB

## License compliance

Apache 2.0 跟 GPLv3 單向相容（Apache 2.0 → GPLv3 OK）.
- 所有檔頭保留原 NVIDIA copyright + Apache notice
- 此目錄保留完整 LICENSE
- VipleStream 整體用 GPLv3 license（含此 3rd-party Apache code）
