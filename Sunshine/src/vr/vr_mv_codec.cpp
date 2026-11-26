/**
 * @file src/vr/vr_mv_codec.cpp
 * @brief VipleStream §VR-GAME-MV（M-MV1）：遊戲內 1/8 動態向量與深度量化、Planar SoA + ZigZag + LZ4 編解碼實作。
 */
#include "vr_mv_codec.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "third-party/lz4/lz4.h"

namespace vr::mv_codec {

  namespace {
    /// 快速建立 65536 項的 half -> float 查表：65536 * 4 B = 256 KB，在編碼 64,800～145,800 格時完全省去逐格位元拆解
    struct f16_lut_t {
      float table[65536];
      uint8_t depth_table[65536];

      f16_lut_t() noexcept {
        for (uint32_t i = 0; i < 65536; ++i) {
          const float f = half_to_float(static_cast<uint16_t>(i));
          table[i] = f;
          if (!std::isfinite(f) || f <= 0.0f) {
            depth_table[i] = 0;
          } else if (f >= 1.0f) {
            depth_table[i] = static_cast<uint8_t>((1u << k_depth_bits) - 1u);
          } else {
            depth_table[i] = static_cast<uint8_t>(std::lround(std::sqrt(f) * static_cast<float>((1u << k_depth_bits) - 1u)));
          }
        }
      }
    };

    const f16_lut_t &lut() noexcept {
      static const f16_lut_t instance;
      return instance;
    }

    inline int16_t quantize_scaled_fast(float scaled) noexcept {
      if (!std::isfinite(scaled)) [[unlikely]] {
        return 0;
      }
      if (scaled >= 32767.0f) [[unlikely]] {
        return 32767;
      }
      if (scaled <= -32767.0f) [[unlikely]] {
        return -32767;
      }
      // 四捨五入到最近整數（死區：剛好量化成 0 的保持 0）
      return static_cast<int16_t>(scaled >= 0.0f ? (scaled + 0.5f) : (scaled - 0.5f));
    }
  }  // namespace

  uint16_t float_to_half(float f) noexcept {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = bits & 0x007FFFFFu;

    if (((bits >> 23) & 0xFFu) == 0xFFu) {
      // Inf 或 NaN
      if (mant != 0) {
        return static_cast<uint16_t>(sign | 0x7E00u | (mant >> 13));
      }
      return static_cast<uint16_t>(sign | 0x7C00u);
    }
    if (exp <= 0) {
      if (exp < -10) {
        return static_cast<uint16_t>(sign);
      }
      mant |= 0x00800000u;
      const uint32_t shift = static_cast<uint32_t>(14 - exp);
      const uint32_t round_bit = 1u << (shift - 1);
      const uint32_t sticky = (mant & (round_bit - 1)) != 0 ? 1u : 0u;
      uint32_t half_mant = mant >> shift;
      if ((mant & round_bit) != 0 && (sticky != 0 || (half_mant & 1u) != 0)) {
        ++half_mant;
      }
      return static_cast<uint16_t>(sign | half_mant);
    }
    if (exp >= 31) {
      return static_cast<uint16_t>(sign | 0x7C00u);
    }
    uint32_t half_exp = static_cast<uint32_t>(exp) << 10;
    uint32_t half_mant = mant >> 13;
    const uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half_mant & 1u) != 0)) {
      ++half_mant;
      if (half_mant == 0x0400u) {
        half_mant = 0;
        half_exp += 0x0400u;
        if (half_exp >= 0x7C00u) {
          return static_cast<uint16_t>(sign | 0x7C00u);
        }
      }
    }
    return static_cast<uint16_t>(sign | half_exp | half_mant);
  }

  int16_t quantize_sbs_uv(float uv_sbs) noexcept {
    if (!std::isfinite(uv_sbs)) {
      return 0;
    }
    return quantize_scaled_fast(uv_sbs * static_cast<float>(1u << k_uv_scale_log2));
  }

  uint8_t quantize_depth(float z) noexcept {
    constexpr float k_max = static_cast<float>((1u << k_depth_bits) - 1u);
    if (!std::isfinite(z) || z <= 0.0f) {
      return 0;
    }
    if (z >= 1.0f) {
      return static_cast<uint8_t>(k_max);
    }
    return static_cast<uint8_t>(std::clamp<long>(std::lround(std::sqrt(z) * k_max), 0, static_cast<long>(k_max)));
  }

  bool output_grid_dims(const vrmv_desc_t &desc, uint16_t &out_w, uint16_t &out_h) noexcept {
    if (desc.grid_w < 2 || desc.grid_w > VRMV_MAX_GRID_W || (desc.grid_w & 1u) != 0 ||
        desc.grid_h < 1 || desc.grid_h > VRMV_MAX_GRID_H) {
      return false;
    }
    const uint32_t eye_w = desc.grid_w / 2u;
    out_w = static_cast<uint16_t>(2u * ((eye_w + k_ds - 1u) / k_ds));
    out_h = static_cast<uint16_t>((desc.grid_h + k_ds - 1u) / k_ds);
    return true;
  }

  // v2（M-MV1 實測後）：1/8 → 1/16（每眼各自 k_ds×k_ds 取前景那一格）、1/4096 UV、死區、6 bit 深度。
  // Palworld 30 幀實測：1/8＋1/32768 UV 時 LZ4 後 p50 約 75 KB（背景 ±1～2 單位的浮點雜訊把連續 0 打斷）；
  // 這組參數 MV 約 9 KB、深度約 4 KB。精度：每眼 1440 時水平約 0.7 px、垂直約 0.35 px，補幀只外插半格。
  bool quantize_rgba16f_to_planes(
    const vrmv_desc_t &desc,
    const void *rgba16f_payload,
    size_t payload_bytes,
    int16_t *out_vx,
    int16_t *out_vy,
    uint8_t *out_depth
  ) noexcept {
    if (!rgba16f_payload || !out_vx || !out_vy) {
      return false;
    }
    uint16_t ow = 0, oh = 0;
    if (!output_grid_dims(desc, ow, oh)) {
      return false;
    }
    const size_t src_w = desc.grid_w;
    const size_t src_h = desc.grid_h;
    if (payload_bytes < src_w * src_h * VRMV_CELL_BYTES) {
      return false;
    }
    if (desc.mv_units != VRMV_UNITS_MVTEX_PIXELS && desc.mv_units != VRMV_UNITS_EYE_UV) {
      return false;
    }
    if (desc.mv_units == VRMV_UNITS_MVTEX_PIXELS &&
        (desc.mv_tex_w == 0 || desc.mv_tex_h == 0 ||
         !std::isfinite(desc.mv_scale[0]) || !std::isfinite(desc.mv_scale[1]))) {
      return false;
    }

    // 單眼 UV → SBS UV（x × 0.5）→ × 2^k_uv_scale_log2
    const float scale = static_cast<float>(1u << k_uv_scale_log2);
    const float factor_x = (desc.mv_units == VRMV_UNITS_EYE_UV) ?
                             (0.5f * scale) :
                             ((0.5f * desc.mv_scale[0] / static_cast<float>(desc.mv_tex_w)) * scale);
    const float factor_y = (desc.mv_units == VRMV_UNITS_EYE_UV) ?
                             scale :
                             ((desc.mv_scale[1] / static_cast<float>(desc.mv_tex_h)) * scale);
    const bool has_depth = (desc.flags & VRMV_F_HAS_DEPTH) != 0;
    const bool want_depth = has_depth && out_depth != nullptr;

    const auto &t = lut();
    const auto *cells = static_cast<const uint16_t *>(rgba16f_payload);
    const size_t src_eye_w = src_w / 2u;
    const size_t out_eye_w = ow / 2u;

    for (size_t oy = 0; oy < oh; ++oy) {
      const size_t sy0 = oy * k_ds;
      const size_t sy1 = std::min(sy0 + k_ds, src_h);
      for (size_t eye = 0; eye < 2; ++eye) {
        for (size_t ox = 0; ox < out_eye_w; ++ox) {
          const size_t sx0 = ox * k_ds;
          const size_t sx1 = std::min(sx0 + k_ds, src_eye_w);
          // 前景優先：有深度時取逆深度最大（最近）那格；沒有深度時取位移最大那格（物體邊緣不被背景的 0 稀釋）
          size_t best = (sy0 * src_w) + eye * src_eye_w + sx0;
          float best_key = -1.0f;
          for (size_t sy = sy0; sy < sy1; ++sy) {
            for (size_t sx = sx0; sx < sx1; ++sx) {
              const size_t i = sy * src_w + eye * src_eye_w + sx;
              float key;
              if (has_depth) {
                key = t.table[cells[i * 4 + 2]];
              } else {
                const float ax = t.table[cells[i * 4 + 0]];
                const float ay = t.table[cells[i * 4 + 1]];
                key = ax * ax + ay * ay;
              }
              if (key > best_key) {  // NaN 不會大於任何值：不會被選中
                best_key = key;
                best = i;
              }
            }
          }
          const size_t o = oy * ow + eye * out_eye_w + ox;
          int16_t qx = quantize_scaled_fast(t.table[cells[best * 4 + 0]] * factor_x);
          int16_t qy = quantize_scaled_fast(t.table[cells[best * 4 + 1]] * factor_y);
          if (qx >= -k_deadzone && qx <= k_deadzone) {
            qx = 0;
          }
          if (qy >= -k_deadzone && qy <= k_deadzone) {
            qy = 0;
          }
          out_vx[o] = qx;
          out_vy[o] = qy;
          if (want_depth) {
            out_depth[o] = t.depth_table[cells[best * 4 + 2]];
          }
        }
      }
    }
    return true;
  }
  bool encode_planes(
    uint16_t grid_w,
    uint16_t grid_h,
    uint8_t codec_flags,
    const int16_t *vx,
    const int16_t *vy,
    const uint8_t *depth,
    std::vector<uint8_t> &out_compressed,
    std::vector<uint8_t> *scratch_planar
  ) {
    if (!vx || !vy || grid_w < 2 || grid_w > VRMV_MAX_GRID_W || (grid_w & 1u) != 0 ||
        grid_h < 1 || grid_h > VRMV_MAX_GRID_H) {
      return false;
    }

    const bool has_depth = (depth != nullptr);
    if (has_depth) {
      codec_flags |= k_flag_has_depth;
    } else {
      codec_flags &= static_cast<uint8_t>(~k_flag_has_depth);
    }
    // M-MV1 不扣頭部旋轉
    codec_flags &= static_cast<uint8_t>(~k_flag_head_rot_subtracted);

    const size_t n_cells = static_cast<size_t>(grid_w) * static_cast<size_t>(grid_h);
    const size_t uncompressed_len = n_cells * (has_depth ? 5u : 4u);

    std::vector<uint8_t> local_planar;
    std::vector<uint8_t> &planar = scratch_planar ? *scratch_planar : local_planar;
    planar.resize(uncompressed_len);

    uint8_t *p0_vx_lo = planar.data();
    uint8_t *p1_vx_hi = p0_vx_lo + n_cells;
    uint8_t *p2_vy_lo = p1_vx_hi + n_cells;
    uint8_t *p3_vy_hi = p2_vy_lo + n_cells;
    uint8_t *p4_depth = has_depth ? (p3_vy_hi + n_cells) : nullptr;

    for (uint16_t y = 0; y < grid_h; ++y) {
      const size_t row_off = static_cast<size_t>(y) * static_cast<size_t>(grid_w);
      uint16_t prev_x = 0;
      uint16_t prev_y = 0;
      uint8_t prev_d = 0;
      for (uint16_t x = 0; x < grid_w; ++x) {
        const size_t idx = row_off + x;
        const uint16_t cur_x = static_cast<uint16_t>(vx[idx]);
        const uint16_t cur_y = static_cast<uint16_t>(vy[idx]);
        const int16_t dx = static_cast<int16_t>(cur_x - prev_x);
        const int16_t dy = static_cast<int16_t>(cur_y - prev_y);
        prev_x = cur_x;
        prev_y = cur_y;

        const uint16_t zz_x = zigzag_encode16(dx);
        const uint16_t zz_y = zigzag_encode16(dy);
        p0_vx_lo[idx] = static_cast<uint8_t>(zz_x & 0xFFu);
        p1_vx_hi[idx] = static_cast<uint8_t>(zz_x >> 8);
        p2_vy_lo[idx] = static_cast<uint8_t>(zz_y & 0xFFu);
        p3_vy_hi[idx] = static_cast<uint8_t>(zz_y >> 8);

        if (has_depth) {
          const uint8_t cur_d = depth[idx];
          p4_depth[idx] = static_cast<uint8_t>(cur_d - prev_d);
          prev_d = cur_d;
        }
      }
    }

    const int max_lz4_bytes = LZ4_compressBound(static_cast<int>(uncompressed_len));
    if (max_lz4_bytes <= 0) {
      return false;
    }
    out_compressed.resize(sizeof(header_t) + static_cast<size_t>(max_lz4_bytes));

    header_t hdr {};
    hdr.version = k_version;
    hdr.flags = codec_flags;
    hdr.grid_w = grid_w;
    hdr.grid_h = grid_h;
    hdr.uv_scale_log2 = k_uv_scale_log2;
    hdr.depth_bits = k_depth_bits;
    hdr.uncompressed_len = static_cast<uint32_t>(uncompressed_len);
    std::memcpy(out_compressed.data(), &hdr, sizeof(header_t));

    const int comp_len = LZ4_compress_fast(
      reinterpret_cast<const char *>(planar.data()),
      reinterpret_cast<char *>(out_compressed.data() + sizeof(header_t)),
      static_cast<int>(uncompressed_len),
      max_lz4_bytes,
      1
    );
    if (comp_len <= 0) {
      out_compressed.clear();
      return false;
    }
    out_compressed.resize(sizeof(header_t) + static_cast<size_t>(comp_len));
    return true;
  }

  bool encode_rgba16f(
    const vrmv_desc_t &desc,
    const void *rgba16f_payload,
    size_t payload_bytes,
    std::vector<uint8_t> &out_compressed,
    std::vector<uint8_t> *scratch_planar
  ) {
    uint16_t ow = 0, oh = 0;
    if (!rgba16f_payload || !output_grid_dims(desc, ow, oh)) {
      return false;
    }
    const size_t n = static_cast<size_t>(ow) * oh;
    // 量化後的平面放在 thread_local（每個呼叫執行緒各一份；server 只有 vr_mvenc 一條）
    thread_local std::vector<int16_t> vx, vy;
    thread_local std::vector<uint8_t> dz;
    vx.resize(n);
    vy.resize(n);
    const bool has_depth = (desc.flags & VRMV_F_HAS_DEPTH) != 0;
    if (has_depth) {
      dz.resize(n);
    }
    if (!quantize_rgba16f_to_planes(desc, rgba16f_payload, payload_bytes, vx.data(), vy.data(), has_depth ? dz.data() : nullptr)) {
      return false;
    }
    uint8_t codec_flags = 0;
    if ((desc.flags & VRMV_F_RESET) != 0) {
      codec_flags |= k_flag_reset;
    }
    return encode_planes(ow, oh, codec_flags, vx.data(), vy.data(), has_depth ? dz.data() : nullptr, out_compressed, scratch_planar);
  }
  bool decode(
    const uint8_t *compressed,
    size_t compressed_size,
    decoded_planes_t &out,
    std::vector<uint8_t> *scratch_planar
  ) {
    if (!compressed || compressed_size <= sizeof(header_t)) {
      return false;
    }
    header_t hdr {};
    std::memcpy(&hdr, compressed, sizeof(header_t));
    if (hdr.version != k_version || hdr.uv_scale_log2 != k_uv_scale_log2 || hdr.depth_bits != k_depth_bits) {
      return false;
    }
    if (hdr.grid_w < 2 || hdr.grid_w > VRMV_MAX_GRID_W || (hdr.grid_w & 1u) != 0 ||
        hdr.grid_h < 1 || hdr.grid_h > VRMV_MAX_GRID_H) {
      return false;
    }
    const bool has_depth = (hdr.flags & k_flag_has_depth) != 0;
    const size_t n_cells = static_cast<size_t>(hdr.grid_w) * static_cast<size_t>(hdr.grid_h);
    const size_t expected_uncompressed = n_cells * (has_depth ? 5u : 4u);
    if (hdr.uncompressed_len != expected_uncompressed) {
      return false;
    }

    std::vector<uint8_t> local_planar;
    std::vector<uint8_t> &planar = scratch_planar ? *scratch_planar : local_planar;
    planar.resize(expected_uncompressed);

    const int decomp_len = LZ4_decompress_safe(
      reinterpret_cast<const char *>(compressed + sizeof(header_t)),
      reinterpret_cast<char *>(planar.data()),
      static_cast<int>(compressed_size - sizeof(header_t)),
      static_cast<int>(expected_uncompressed)
    );
    if (decomp_len != static_cast<int>(expected_uncompressed)) {
      return false;
    }

    out.hdr = hdr;
    out.vx.resize(n_cells);
    out.vy.resize(n_cells);
    if (has_depth) {
      out.depth.resize(n_cells);
    } else {
      out.depth.clear();
    }

    const uint8_t *p0_vx_lo = planar.data();
    const uint8_t *p1_vx_hi = p0_vx_lo + n_cells;
    const uint8_t *p2_vy_lo = p1_vx_hi + n_cells;
    const uint8_t *p3_vy_hi = p2_vy_lo + n_cells;
    const uint8_t *p4_depth = has_depth ? (p3_vy_hi + n_cells) : nullptr;

    int16_t *dst_vx = out.vx.data();
    int16_t *dst_vy = out.vy.data();
    uint8_t *dst_d = has_depth ? out.depth.data() : nullptr;

    for (uint16_t y = 0; y < hdr.grid_h; ++y) {
      const size_t row_off = static_cast<size_t>(y) * static_cast<size_t>(hdr.grid_w);
      uint16_t acc_x = 0;
      uint16_t acc_y = 0;
      uint8_t acc_d = 0;
      for (uint16_t x = 0; x < hdr.grid_w; ++x) {
        const size_t idx = row_off + x;
        const uint16_t zz_x = static_cast<uint16_t>(p0_vx_lo[idx]) | (static_cast<uint16_t>(p1_vx_hi[idx]) << 8);
        const uint16_t zz_y = static_cast<uint16_t>(p2_vy_lo[idx]) | (static_cast<uint16_t>(p3_vy_hi[idx]) << 8);
        const int16_t dx = zigzag_decode16(zz_x);
        const int16_t dy = zigzag_decode16(zz_y);
        acc_x = static_cast<uint16_t>(acc_x + static_cast<uint16_t>(dx));
        acc_y = static_cast<uint16_t>(acc_y + static_cast<uint16_t>(dy));
        dst_vx[idx] = static_cast<int16_t>(acc_x);
        dst_vy[idx] = static_cast<int16_t>(acc_y);

        if (has_depth) {
          acc_d = static_cast<uint8_t>(acc_d + p4_depth[idx]);
          dst_d[idx] = acc_d;
        }
      }
    }
    return true;
  }

}  // namespace vr::mv_codec
