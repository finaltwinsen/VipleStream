/**
 * @file src/vr/vr_scene_pattern.h
 * @brief VipleStream §VR（M1b V4、§F.4 scene／§F.5 T4）：vr_probe scene 在每眼左上角畫的位元圖案，與 selftest 的解碼。
 *
 * 內容（72 bit = 9 byte）：frameCounter（16 bit，little-endian）＋ VipleVrPackQuat48(render pose)（6 byte）＋ CRC-8（前 8 byte；
 * 多項式 0x07、初值 0、最後 XOR 0x5A——全黑的區域不會碰巧通過）。位元 k 放在第 (k / 9) 列、第 (k % 9) 行的區塊；1 = 白、0 = 黑。
 * 區塊大小以「眼寬／108、眼高／108」定義（相對尺寸）：SteamVR 的超取樣會改 app 的 render target 大小，
 * 相對尺寸讓 driver 合成到 ring 之後仍在同一個位置（ring 的每眼寬 1728 時區塊 16×16 px）。
 * 設計原文寫「8×8 區塊」，但 16＋48＋8 = 72 bit 放不進 64 格，V4 改成 9×8（報告列為偏差）。
 *
 * 純 header：vr_probe（MSVC，/I src/vr）與 server（selftest T4）共用；不 include 任何平台 header。
 */
#pragma once

#include <cstdint>

namespace vr_scene_pattern {

  inline constexpr int k_cols = 9;
  inline constexpr int k_rows = 8;
  inline constexpr int k_bits = k_cols * k_rows;  // 72
  inline constexpr int k_bytes = k_bits / 8;  // 9
  inline constexpr int k_grid_div = 108;  // 區塊 = 眼寬／108 × 眼高／108
  inline constexpr uint8_t k_crc_xor = 0x5A;

  inline uint8_t crc8(const uint8_t *data, int len) {
    uint8_t c = 0;
    for (int i = 0; i < len; ++i) {
      c ^= data[i];
      for (int b = 0; b < 8; ++b) {
        c = (uint8_t) ((c & 0x80) ? ((c << 1) ^ 0x07) : (c << 1));
      }
    }
    return c;
  }

  /// counter＋48 bit 四元數 → 9 byte（最後一個是 CRC）
  inline void encode(uint16_t counter, const uint8_t q48[6], uint8_t out[k_bytes]) {
    out[0] = (uint8_t) (counter & 0xFF);
    out[1] = (uint8_t) (counter >> 8);
    for (int i = 0; i < 6; ++i) {
      out[2 + i] = q48[i];
    }
    out[8] = (uint8_t) (crc8(out, 8) ^ k_crc_xor);
  }

  inline bool bit(const uint8_t bytes[k_bytes], int k) {
    return ((bytes[k / 8] >> (k % 8)) & 1) != 0;
  }

  /// 區塊 k 在一眼內的像素範圍 [x0, x1) × [y0, y1)（eye_w／eye_h 是該眼影像的寬高）
  inline void block_rect(int k, uint32_t eye_w, uint32_t eye_h, uint32_t &x0, uint32_t &y0, uint32_t &x1, uint32_t &y1) {
    const int col = k % k_cols;
    const int row = k / k_cols;
    x0 = (uint32_t) ((uint64_t) col * eye_w / k_grid_div);
    x1 = (uint32_t) ((uint64_t) (col + 1) * eye_w / k_grid_div);
    y0 = (uint32_t) ((uint64_t) row * eye_h / k_grid_div);
    y1 = (uint32_t) ((uint64_t) (row + 1) * eye_h / k_grid_div);
  }

  /// 解碼時讀的區域大小（像素；左上角起）
  inline uint32_t region_w(uint32_t eye_w) {
    return (uint32_t) (((uint64_t) (k_cols + 1) * eye_w + k_grid_div - 1) / k_grid_div);
  }

  inline uint32_t region_h(uint32_t eye_h) {
    return (uint32_t) (((uint64_t) (k_rows + 1) * eye_h + k_grid_div - 1) / k_grid_div);
  }

  /**
   * @brief 解碼：luma(x, y) 回傳該像素的亮度 0..255（呼叫端從 staging 貼圖讀）。每個區塊取中心 2×2 的平均；
   *        門檻取 72 個區塊的（最亮＋最暗）／2——SteamVR 的淡入淡出與 compositor 的亮度處理會讓「白」不到 255
   *        （V4 host 實測約 190），固定 128 會在淡出時整幀失敗。亮暗差 < 32 視為沒有圖案。
   * @return CRC 通過為 true。
   */
  template <class LumaFn>
  bool decode(uint32_t eye_w, uint32_t eye_h, LumaFn luma, uint16_t &counter, uint8_t q48[6]) {
    uint32_t v[k_bits];
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (int k = 0; k < k_bits; ++k) {
      uint32_t x0, y0, x1, y1;
      block_rect(k, eye_w, eye_h, x0, y0, x1, y1);
      const uint32_t cx = (x0 + x1) / 2;
      const uint32_t cy = (y0 + y1) / 2;
      const uint32_t cx0 = cx > 0 ? cx - 1 : cx;
      const uint32_t cy0 = cy > 0 ? cy - 1 : cy;
      v[k] = luma(cx0, cy0) + luma(cx, cy0) + luma(cx0, cy) + luma(cx, cy);
      lo = v[k] < lo ? v[k] : lo;
      hi = v[k] > hi ? v[k] : hi;
    }
    if (hi < lo + 4 * 32) {
      return false;
    }
    const uint32_t threshold = (hi + lo) / 2;
    uint8_t bytes[k_bytes] = {};
    for (int k = 0; k < k_bits; ++k) {
      if (v[k] > threshold) {
        bytes[k / 8] = (uint8_t) (bytes[k / 8] | (1u << (k % 8)));
      }
    }
    if ((uint8_t) (crc8(bytes, 8) ^ k_crc_xor) != bytes[8]) {
      return false;
    }
    counter = (uint16_t) (bytes[0] | (bytes[1] << 8));
    for (int i = 0; i < 6; ++i) {
      q48[i] = bytes[2 + i];
    }
    return true;
  }

}  // namespace vr_scene_pattern
