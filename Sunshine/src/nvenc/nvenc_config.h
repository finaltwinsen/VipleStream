/**
 * @file src/nvenc/nvenc_config.h
 * @brief Declarations for NVENC encoder configuration.
 */
#pragma once

namespace nvenc {

  enum class nvenc_two_pass {
    disabled,  ///< Single pass, the fastest and no extra vram
    quarter_resolution,  ///< Larger motion vectors being caught, faster and uses less extra vram
    full_resolution,  ///< Better overall statistics, slower and uses more extra vram
  };

  /// §NVENC-SPLIT（實驗）：split-frame encoding，有 2～3 顆 NVENC 引擎的卡讓一幀由多顆引擎並行編碼。
  /// 只有 HEVC／AV1、驅動支援 API 13.0 以上時才生效；除了 disabled，其他值都會改用 13.0 的結構跟驅動溝通。
  enum class nvenc_split_encode {
    disabled,  ///< 預設：不設 splitEncodeMode，照舊以 11.0／12.0 的結構跟驅動溝通
    auto_mode,  ///< NV_ENC_SPLIT_AUTO_MODE：驅動依 preset、tuning、解析度自己決定要不要分
    auto_forced,  ///< NV_ENC_SPLIT_AUTO_FORCED_MODE：一定分，分幾條由驅動決定
    two,  ///< NV_ENC_SPLIT_TWO_FORCED_MODE：分兩條（只有一顆引擎時仍是一條）
    three,  ///< NV_ENC_SPLIT_THREE_FORCED_MODE：分三條（引擎不足三顆時用引擎數）
    native_off,  ///< NV_ENC_SPLIT_DISABLE_MODE：用 13.0 的結構但明確關閉 split（A/B 對照 API 版本本身的影響）
  };

  /**
   * @brief NVENC encoder configuration.
   */
  struct nvenc_config {
    // Quality preset from 1 to 7, higher is slower
    int quality_preset = 1;

    // Use optional preliminary pass for better motion vectors, bitrate distribution and stricter VBV(HRD), uses CUDA cores
    nvenc_two_pass two_pass = nvenc_two_pass::quarter_resolution;

    // Percentage increase of VBV/HRD from the default single frame, allows low-latency variable bitrate
    // [Raised to 20: allows bitrate bursts on scene changes / complex frames → better quality]
    int vbv_percentage_increase = 20;

    // Improves fades compression, uses CUDA cores
    bool weighted_prediction = false;

    // Allocate more bitrate to flat regions since they're visually more perceptible, uses CUDA cores
    // [Enabled by default: better perceptual quality in smooth gradients/skies/walls at no latency cost]
    bool adaptive_quantization = true;

    // Don't use QP below certain value, limits peak image quality to save bitrate
    bool enable_min_qp = false;

    // Min QP value for H.264 when enable_min_qp is selected
    unsigned min_qp_h264 = 19;

    // Min QP value for HEVC when enable_min_qp is selected
    unsigned min_qp_hevc = 23;

    // Min QP value for AV1 when enable_min_qp is selected
    unsigned min_qp_av1 = 23;

    // Use CAVLC entropy coding in H.264 instead of CABAC, not relevant and here for historical reasons
    bool h264_cavlc = false;

    // Add filler data to encoded frames to stay at target bitrate, mainly for testing
    bool insert_filler_data = false;

    // §NVENC-SPLIT（實驗，預設關）：sunshine.conf 的 nvenc_split_encode
    nvenc_split_encode split_encode = nvenc_split_encode::disabled;
  };

}  // namespace nvenc
