/**
 * @file src/vr/vr_mv_codec.h
 * @brief VipleStream §VR-GAME-MV（M-MV1）：遊戲內 1/8 動態向量與深度量化、Planar SoA + ZigZag 水平差分 + LZ4 編解碼器。
 *
 * 設計依據：docs/specs/vr_game_motion_vectors.md §4.2。
 * 規則：
 *   - 可攜 C++20、不含任何 Win32 API（之後頭盔端共用解碼）。
 *   - half ↔ float 自行以純位元運算實作（不依賴 F16C 指令集）。
 *   - M-MV1 不扣頭部旋轉（headRotSubtracted = 0），死區只把剛好量化成 0 的保持 0。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "vr_mv_ipc_abi.h"

namespace vr::mv_codec {

  constexpr uint8_t k_version = 2u;  // v2：1/16 網格、1/4096 UV、死區、6 bit 深度（v1 是 1/8 網格、1/32768 UV、8 bit）
  constexpr uint8_t k_uv_scale_log2 = 12u;  ///< vx, vy 單位＝SBS UV / 2^12
  constexpr uint8_t k_depth_bits = 6u;  ///< 深度＝round(sqrt(z) × 63)
  constexpr uint32_t k_ds = 2u;  ///< 每眼 1/8 網格再 k_ds×k_ds 取前景一格 → 1/16
  constexpr int16_t k_deadzone = 1;  ///< |q| ≤ 1（約 0.7 px）歸 0：背景的浮點雜訊會把 LZ4 的連續 0 打斷

  /// 壓縮流標頭 flags 位元（對齊設計 §4.1：b0: hasDepth, b1: reset, b2: headRotSubtracted, b3..b4: codec(0=LZ4)）
  constexpr uint8_t k_flag_has_depth = 0x01u;
  constexpr uint8_t k_flag_reset = 0x02u;
  constexpr uint8_t k_flag_head_rot_subtracted = 0x04u;

  /**
   * @brief 壓縮流開頭的固定 12 B 小標頭（純 POD、little-endian）。
   *        注意：360×180×4 = 259,200、540×270×5 = 729,000，皆超過 uint16_t 上限 65535，
   *        因此 uncompressed_len 使用 uint32_t。
   */
  struct header_t {
    uint8_t version = k_version;
    uint8_t flags = 0;               ///< k_flag_*
    uint16_t grid_w = 0;             ///< 雙眼並排 1/8 網格總寬（偶數，<= VRMV_MAX_GRID_W）
    uint16_t grid_h = 0;             ///< 1/8 網格高度（<= VRMV_MAX_GRID_H）
    uint8_t uv_scale_log2 = k_uv_scale_log2;  ///< vx, vy 的單位＝SBS UV / 2^uv_scale_log2
    uint8_t depth_bits = k_depth_bits;  ///< 深度平面的位元數（sqrt 壓擴）
    uint32_t uncompressed_len = 0;   ///< grid_w * grid_h * (has_depth ? 5 : 4)
  };
  static_assert(sizeof(header_t) == 12, "vr::mv_codec::header_t must be 12 bytes");

  /// 解碼後的平面資料
  struct decoded_planes_t {
    header_t hdr {};
    std::vector<int16_t> vx;     ///< 長度 grid_w * grid_h，SBS UV × 2^uv_scale_log2（夾限 ±32767）
    std::vector<int16_t> vy;     ///< 長度 grid_w * grid_h，SBS UV × 2^uv_scale_log2（夾限 ±32767）
    std::vector<uint8_t> depth;  ///< 若 has_depth 為真，長度 grid_w * grid_h；否則為空
  };

  /**
   * @brief IEEE 754 binary16 (half) 轉 binary32 (float)，純軟實作（不依賴 F16C）。
   */
  inline float half_to_float(uint16_t h) noexcept {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x03FFu;
    uint32_t bits = 0;
    if (exp != 0 && exp != 31) [[likely]] {
      bits = sign | ((exp + 112u) << 23) | (mant << 13);
    } else if (exp == 0) {
      if (mant == 0) {
        bits = sign;
      } else {
        // 次正規數（subnormal）：(-1)^s * 2^-14 * (mant / 1024)
        const uint32_t magic_bits = 0x38800000u | (mant << 13);
        float f_sub = 0.0f;
        std::memcpy(&f_sub, &magic_bits, sizeof(f_sub));
        f_sub -= 6.103515625e-5f;  // 2^-14
        std::memcpy(&bits, &f_sub, sizeof(bits));
        bits |= sign;
      }
    } else {
      // Inf 或 NaN
      bits = sign | 0x7F800000u | (mant << 13);
    }
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
  }

  /**
   * @brief IEEE 754 binary32 (float) 轉 binary16 (half)，純軟實作（供單元測試構造 RGBA16F 輸入）。
   */
  uint16_t float_to_half(float f) noexcept;

  /**
   * @brief ZigZag 16-bit 編碼：將小正負有號數映射到無號低值。
   */
  inline uint16_t zigzag_encode16(int16_t v) noexcept {
    const uint16_t u = static_cast<uint16_t>(v);
    return static_cast<uint16_t>((u << 1) ^ static_cast<uint16_t>(v >> 15));
  }

  /**
   * @brief ZigZag 16-bit 解碼。
   */
  inline int16_t zigzag_decode16(uint16_t zz) noexcept {
    return static_cast<int16_t>((zz >> 1) ^ static_cast<uint16_t>(-static_cast<int16_t>(zz & 1u)));
  }

  /**
   * @brief 將單一浮點 SBS UV 位移量化為 int16（×2^k_uv_scale_log2、四捨五入、夾限 ±32767、NaN/Inf → 0）。
   */
  int16_t quantize_sbs_uv(float uv_sbs) noexcept;

  /**
   * @brief 將單一正規化逆深度 z 量化（round(sqrt(clamp(z, 0, 1)) × (2^k_depth_bits − 1))，NaN/Inf/<=0 → 0）。
   */
  uint8_t quantize_depth(float z) noexcept;

  /**
   * @brief 將 RGBA16F payload 依 vrmv_desc_t 量化為 int16 vx, vy 與選填的 uint8 depth 平面。
   * @param desc 描述符（提供 grid_w, grid_h, mv_scale, mv_tex_w, mv_tex_h, mv_units, flags）
   * @param rgba16f_payload 指向長度為 grid_w * grid_h * 8 bytes 的 RGBA16F 緩衝區
   * @param payload_bytes 位元組數（必須 == grid_w * grid_h * 8）
   * @param out_vx 輸出 int16 vx 平面（長度 grid_w * grid_h）
   * @param out_vy 輸出 int16 vy 平面（長度 grid_w * grid_h）
   * @param out_depth 若 (desc.flags & VRMV_F_HAS_DEPTH) != 0 且非 nullptr，輸出 uint8 depth 平面（長度 grid_w * grid_h）
   */
  /// 1/8 輸入網格（desc.grid_w × grid_h）對應的輸出網格尺寸（每眼 ceil(/k_ds)）
  bool output_grid_dims(const vrmv_desc_t &desc, uint16_t &out_w, uint16_t &out_h) noexcept;

  bool quantize_rgba16f_to_planes(
    const vrmv_desc_t &desc,
    const void *rgba16f_payload,
    size_t payload_bytes,
    int16_t *out_vx,
    int16_t *out_vy,
    uint8_t *out_depth = nullptr,
    float mv_gain = 1.0f  ///< 位移乘上的倍數（server 用來換算成「每個串流幀」的位移；見 display_vr 的 §VR-GAME-MV-GAIN）
  ) noexcept;

  /**
   * @brief 將已量化的 int16 vx, vy（與選填的 uint8 depth）平面編碼為「header_t + LZ4(Planar SoA 水平差分)」格式。
   * @param grid_w 網格寬度（偶數，2..VRMV_MAX_GRID_W）
   * @param grid_h 網格高度（1..VRMV_MAX_GRID_H）
   * @param codec_flags header_t.flags（k_flag_*；若 depth != nullptr 會自動設上 k_flag_has_depth）
   * @param vx 長度 grid_w * grid_h 的 int16 陣列
   * @param vy 長度 grid_w * grid_h 的 int16 陣列
   * @param depth 選填：長度 grid_w * grid_h 的 uint8 陣列（nullptr 表示無深度平面）
   * @param out_compressed 輸出壓縮後的位元組串（前 12 B 為 header_t，後面為 LZ4 payload）
   * @param scratch_planar 選填：呼叫端重複使用的工作緩衝區（避免每幀重新配置記憶體）
   */
  bool encode_planes(
    uint16_t grid_w,
    uint16_t grid_h,
    uint8_t codec_flags,
    const int16_t *vx,
    const int16_t *vy,
    const uint8_t *depth,
    std::vector<uint8_t> &out_compressed,
    std::vector<uint8_t> *scratch_planar = nullptr
  );

  /**
   * @brief 一趟完成 RGBA16F 量化 + Planar SoA 列內水平差分 + ZigZag + LZ4 壓縮。
   * @param desc 已驗證的 vrmv_desc_t 本地副本
   * @param rgba16f_payload 已透過 seqlock 複製到本地的 RGBA16F 資料
   * @param payload_bytes 位元組數
   * @param out_compressed 輸出含 header_t 的壓縮資料
   * @param scratch_planar 選填：呼叫端重複使用的工作緩衝區
   */
  bool encode_rgba16f(
    const vrmv_desc_t &desc,
    const void *rgba16f_payload,
    size_t payload_bytes,
    std::vector<uint8_t> &out_compressed,
    std::vector<uint8_t> *scratch_planar = nullptr,
    float mv_gain = 1.0f
  );

  /**
   * @brief 解碼「header_t + LZ4(Planar SoA 水平差分)」回 int16 vx, vy 與 uint8 depth 平面。
   * @param compressed 壓縮資料指標
   * @param compressed_size 壓縮資料總長度（含 header_t）
   * @param out 解碼輸出結構
   * @param scratch_planar 選填：呼叫端重複使用的解壓工作緩衝區
   */
  bool decode(
    const uint8_t *compressed,
    size_t compressed_size,
    decoded_planes_t &out,
    std::vector<uint8_t> *scratch_planar = nullptr
  );

}  // namespace vr::mv_codec
