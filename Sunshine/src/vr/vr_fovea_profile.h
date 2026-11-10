/**
 * @file src/vr/vr_fovea_profile.h
 * @brief VipleStream 2.0 §VR-FOVEA-INSET（2026-11-09）：分段注視點曲線的參數（host 的協商與 driver 設定共用）。
 *
 * 舊的注視點曲線（§VR-FOVEA）是 t = a·e + (1−a)·e³：只有正中央那一點的密度是 1/a 倍，往外很快掉下去。每眼串流 1152、
 * 遊戲算圖 2154 時，達到原生密度（1.87 倍）的範圍只佔視野中央約一成，11-09 使用者：流暢很多但解析度明顯不如 Steam 內建。
 * Steam（推測）每眼送「整個視野的低解析度」＋「注視點附近、約一半視野寬的原生解析度」兩塊。
 *
 * 分段曲線（每一側各自以注視點中心 c 正規化成 e ∈ [−1, 1]，編碼影像 → 原圖）：
 *   |e| ≤ u0：t = k1·e          （k1 = 1/d，d＝中央密度＝算圖寬／串流寬：中央就是原生細節）
 *   |e| > u0：t = ±(k1·u0 + k2·(|e| − u0))，k2 = (1 − k1·u0)／(1 − u0)
 * 中央原生的範圍取視野的 kTargetT0（Steam 的 1152／2160），u0 上限 kMaxU0（周邊至少留四分之一的像素）。
 * 參數在協商時算好：回應的 fovea＝round(d×100)（client 用 a＝100／fovea＝k1），foveaInset＝round(u0×10000)。
 */
#pragma once

#include <algorithm>
#include <cmath>

namespace vr::fovea {

  constexpr float kTargetT0 = 0.533f;  ///< 中央原生的範圍：視野（每一側）的這個比例
  constexpr float kMaxU0 = 0.75f;      ///< 中央在編碼影像裡最多佔這個比例
  constexpr float kMinDensity = 1.1f;  ///< 算圖沒有比串流大到這個倍數就不用分段曲線（沒有細節可保留）

  /// 每眼建議算圖寬度（和 orchestrator 給 driver 的同一套規則：vr_render_scale_pct，夾在 vr_render_target_max）
  inline int render_width(int eye_w, int render_scale_pct, int render_target_max) {
    if (eye_w <= 0) {
      return 0;
    }
    int pct = std::clamp(render_scale_pct, 100, 250);
    const int cap = std::max(render_target_max, eye_w);
    if (eye_w * pct / 100 > cap) {
      pct = std::max(100, cap * 100 / eye_w);
    }
    return (eye_w * pct / 100) & ~1;
  }

  struct inset_t {
    bool on = false;
    int fovea_pct = 100;     ///< round(d×100)：協商回應的 fovea＝，driver 的 fovea_pct
    int u0_x10000 = 0;       ///< round(u0×10000)：協商回應的 foveaInset＝，driver 的 fovea_inset_u0
  };

  /// fovea_req＝client 要求的中央密度上限（%）。d 取 min(算圖寬／串流寬, fovea_req／100)
  inline inset_t compute_inset(int eye_w, int render_w, int fovea_req) {
    inset_t r;
    if (eye_w <= 0 || render_w <= eye_w || fovea_req <= 100) {
      return r;
    }
    const float d = std::min(static_cast<float>(render_w) / static_cast<float>(eye_w), static_cast<float>(fovea_req) / 100.0f);
    if (d < kMinDensity) {
      return r;
    }
    r.fovea_pct = std::clamp(static_cast<int>(std::lround(d * 100.0f)), 101, 250);
    const float k1 = 100.0f / static_cast<float>(r.fovea_pct);
    const float u0 = std::min(kTargetT0 / k1, kMaxU0);
    r.u0_x10000 = std::clamp(static_cast<int>(std::lround(u0 * 10000.0f)), 1000, 9000);
    r.on = true;
    return r;
  }

  // ── §VR-FOVEA-TILES（2026-11-10）：每眼兩塊影像（Steam 內建串流的排法）──────────────────────────
  //
  // 分段曲線把周邊壓到原生密度的 0.22（Steam 的全視野那塊是 0.53），交界兩側差 4.5 倍，配戴時交界很明顯；
  // 十字臂（一軸原生、一軸壓縮）又佔掉每眼 37% 的像素。改成不做幾何變形：
  //   上排：左、右眼的「全視野」各一塊（整張算圖影像縮到 tile_w×tile_h）
  //   下排：左、右眼的「注視點」各一塊（算圖影像裡 tile_w×tile_h 的原生裁切，跟著視線移，左上角對齊整數像素）
  // packed＝(2·tile_w)×(2·tile_h)。每格注視點那塊的左上角（算圖像素）放在 0x82 幀標頭。
  // 檔位只決定每塊多大：small＝像素量和不分塊差不多、large＝每塊就是串流的每眼尺寸（＝Steam）。

  constexpr int kTileAlign = 32;          ///< 每塊的寬高取這個的倍數（HEVC 編碼單元的邊界，塊與塊之間不共用一個單元）
  constexpr float kTileMaxCover = 0.8f;   ///< 注視點那塊最多佔算圖影像的這個比例（再大，全視野那塊就幾乎是多送的）
  constexpr int kTileMin = 256;
  /// 算圖至少要比串流的每眼尺寸大這個倍數才分塊：不然直接送整張（不分塊）就已經帶得到原生細節，分塊只會讓周邊變糊
  constexpr float kTileMinRenderScale = 1.25f;

  struct tiles_t {
    bool on = false;
    int tile_w = 0;    ///< 每塊的寬（全視野那塊、注視點那塊同尺寸）
    int tile_h = 0;
    int render_w = 0;  ///< 每眼算圖尺寸：注視點那塊＝這張影像裡 tile_w×tile_h 的裁切
    int render_h = 0;
  };

  /// tier：1 small（13/18，兩塊合計約 1.04 倍的每眼像素）、2 medium（5/6）、3 large（1）；其他值＝不用
  inline tiles_t compute_tiles(int eye_w, int eye_h, int render_w, int render_h, int tier) {
    tiles_t r;
    if (tier < 1 || tier > 3 || eye_w < kTileMin || eye_h < kTileMin || render_w <= 0 || render_h <= 0 || render_w > 65535 || render_h > 65535) {
      return r;  // 位置用 u16 的像素座標帶，算圖尺寸超過就不做
    }
    if (static_cast<float>(render_w) < static_cast<float>(eye_w) * kTileMinRenderScale || static_cast<float>(render_h) < static_cast<float>(eye_h) * kTileMinRenderScale) {
      return r;
    }
    const int num = tier == 1 ? 13 : tier == 2 ? 15 : 18;
    const auto size = [num](int v) {
      const int t = (v * num / 18 + kTileAlign / 2) / kTileAlign * kTileAlign;
      return std::max(t, kTileMin);
    };
    const int tw = size(eye_w), th = size(eye_h);
    if (static_cast<float>(tw) > static_cast<float>(render_w) * kTileMaxCover || static_cast<float>(th) > static_cast<float>(render_h) * kTileMaxCover) {
      return r;  // 算圖沒有比一塊大多少：注視點那塊沒有東西可以多帶
    }
    r.on = true;
    r.tile_w = tw;
    r.tile_h = th;
    r.render_w = render_w;
    r.render_h = render_h;
    return r;
  }

}  // namespace vr::fovea
