/**
 * @file src/vr/vr_padmode_map.h
 * @brief §VR-PADMODE：Frame 手把 → Xbox 手把的對應與「兩支選單鍵長按」偵測（純函式，可單元測試）。
 *
 * 不使用 VR 手把的遊戲（賽車、一般手把遊戲：Assetto Corsa、ACC、GRIP）在 VR 裡只讀 XInput，Frame 手把
 * 只交給 SteamVR 時完全沒反應。手把模式時 server 另外插一支虛擬 Xbox 360 手把，把兩手的狀態合成一支送過去。
 * 按鍵旗標與 platf::gamepad_state_t 同值（vr_padmode.cpp 有 static_assert）。
 */
#pragma once

#include <cmath>
#include <cstdint>

#include <optional>

#include <moonlight-common-c/src/VipleVr.h>

namespace vr::padmode {

  // 與 platform/common.h 的 platf::* 按鍵旗標同值
  constexpr uint32_t k_dpad_up = 0x0001;
  constexpr uint32_t k_dpad_down = 0x0002;
  constexpr uint32_t k_dpad_left = 0x0004;
  constexpr uint32_t k_dpad_right = 0x0008;
  constexpr uint32_t k_start = 0x0010;
  constexpr uint32_t k_back = 0x0020;
  constexpr uint32_t k_left_stick = 0x0040;
  constexpr uint32_t k_right_stick = 0x0080;
  constexpr uint32_t k_left_button = 0x0100;
  constexpr uint32_t k_right_button = 0x0200;
  constexpr uint32_t k_a = 0x1000;
  constexpr uint32_t k_b = 0x2000;
  constexpr uint32_t k_x = 0x4000;
  constexpr uint32_t k_y = 0x8000;

  struct xpad_t {
    uint32_t buttons = 0;
    uint8_t lt = 0;
    uint8_t rt = 0;
    int16_t lx = 0;
    int16_t ly = 0;
    int16_t rx = 0;
    int16_t ry = 0;

    bool operator==(const xpad_t &o) const {
      return buttons == o.buttons && lt == o.lt && rt == o.rt && lx == o.lx && ly == o.ly && rx == o.rx && ry == o.ry;
    }
  };

  /**
   * @brief 兩手 → 一支 Xbox 手把。in[0]＝左手、in[1]＝右手（0x5506 的順序）。
   *
   * Frame：右手 A／B／x／y（VIPLE_VR_BTN_A／B／C／D）→ A／B／X／Y；左手 dpad 下／右／左／上（同四個位元）→ 十字鍵；
   * bumper → LB／RB；扳機 → LT／RT；搖桿（含按下）→ LS／RS；右手 menu → Start、左手 view（MENU 位元）→ Back。
   * 握把與系統鍵不對應（留給 SteamVR）。左手不是 Frame 時（Touch 等）左手 A／B＝X／Y。
   * 沒在追蹤（flags 沒有 active）的那一手當成沒按。suppress_menu＝兩支選單鍵正在長按切換：不送 Start／Back。
   * 兩支搖桿同時按下＝client 的方向盤快捷鍵（§VR-WHEEL）：那段期間不送 LS／RS。
   */
  inline xpad_t map_frame(const VIPLE_VR_CONTROLLER_INPUT in[2], bool suppress_menu) {
    xpad_t o;
    const VIPLE_VR_CONTROLLER_INPUT &l = in[0];
    const VIPLE_VR_CONTROLLER_INPUT &r = in[1];
    const bool l_on = (l.flags & VIPLE_VR_CTRL_ACTIVE) != 0;
    const bool r_on = (r.flags & VIPLE_VR_CTRL_ACTIVE) != 0;
    const uint32_t lb = l_on ? l.buttons : 0;
    const uint32_t rb = r_on ? r.buttons : 0;
    if (rb & VIPLE_VR_BTN_A) o.buttons |= k_a;
    if (rb & VIPLE_VR_BTN_B) o.buttons |= k_b;
    if (rb & VIPLE_VR_BTN_C) o.buttons |= k_x;
    if (rb & VIPLE_VR_BTN_D) o.buttons |= k_y;
    if (l.profile == VIPLE_VR_CTRL_PROFILE_FRAME) {
      if (lb & VIPLE_VR_BTN_A) o.buttons |= k_dpad_down;
      if (lb & VIPLE_VR_BTN_B) o.buttons |= k_dpad_right;
      if (lb & VIPLE_VR_BTN_C) o.buttons |= k_dpad_left;
      if (lb & VIPLE_VR_BTN_D) o.buttons |= k_dpad_up;
    } else {
      if (lb & VIPLE_VR_BTN_A) o.buttons |= k_x;
      if (lb & VIPLE_VR_BTN_B) o.buttons |= k_y;
    }
    if (lb & VIPLE_VR_BTN_BUMPER) o.buttons |= k_left_button;
    if (rb & VIPLE_VR_BTN_BUMPER) o.buttons |= k_right_button;
    if (!((lb & VIPLE_VR_BTN_THUMBSTICK) && (rb & VIPLE_VR_BTN_THUMBSTICK))) {
      if (lb & VIPLE_VR_BTN_THUMBSTICK) o.buttons |= k_left_stick;
      if (rb & VIPLE_VR_BTN_THUMBSTICK) o.buttons |= k_right_stick;
    }
    if (!suppress_menu) {
      if (rb & VIPLE_VR_BTN_MENU) o.buttons |= k_start;
      if (lb & VIPLE_VR_BTN_MENU) o.buttons |= k_back;
    }
    if (l_on) {
      o.lt = static_cast<uint8_t>(l.trigger >> 8);
      o.lx = l.stickX;
      o.ly = l.stickY;
    }
    if (r_on) {
      o.rt = static_cast<uint8_t>(r.trigger >> 8);
      o.rx = r.stickX;
      o.ry = r.stickY;
    }
    return o;
  }

  /// 兩手的選單鍵（左手 view、右手 menu）是否同時按著
  inline bool both_menu(const VIPLE_VR_CONTROLLER_INPUT in[2]) {
    return (in[0].flags & VIPLE_VR_CTRL_ACTIVE) && (in[1].flags & VIPLE_VR_CTRL_ACTIVE) && (in[0].buttons & VIPLE_VR_BTN_MENU) &&
           (in[1].buttons & VIPLE_VR_BTN_MENU);
  }

  /**
   * @brief §VR-WHEEL：兩手握著一個看不見的方向盤，轉動的角度取代左搖桿 X。
   *
   * 角度＝右手減左手的向量在「方向盤平面」裡的傾角：平面的水平軸 axis 跟著兩手連線的水平方向（方向盤擺正時慢慢追，
   * 換姿勢、轉身也不會偏），垂直軸是世界的上方。逐樣本展開角度，所以轉超過 90 度（手交叉）也連續。右轉（右手往下）
   * 為正。任何一手沒在追蹤就失效（回 nullopt，維持搖桿），重新抓到時從當下的角度重新開始。
   */
  struct wheel_t {
    float axis_x = 0.0f;  ///< 方向盤平面的水平軸（單位向量，x／z）；0／0＝還沒建立
    float axis_z = 0.0f;
    float angle = 0.0f;  ///< 展開後的角度（弧度，右轉為正）
    bool valid = false;

    static constexpr float k_pi = 3.14159265358979f;
    static constexpr float k_min_span_m = 0.12f;  ///< 兩手距離小於這個值（手疊在一起）不更新
    static constexpr float k_follow = 0.02f;  ///< 擺正時水平軸追蹤的速度（每個樣本）
    static constexpr float k_follow_below = 0.35f;  ///< 角度小於約 20 度才追

    void reset() {
      valid = false;
      angle = 0.0f;
    }

    /**
     * @param l／r 左右手的位置（公尺，client 參考空間，y 朝上）
     * @return 展開後的角度（弧度）；nullopt＝目前算不出來
     */
    std::optional<float> step(const float l[3], const float r[3], bool both_tracked) {
      if (!both_tracked) {
        reset();
        return std::nullopt;
      }
      const float vx = r[0] - l[0];
      const float vy = r[1] - l[1];
      const float vz = r[2] - l[2];
      const float h = std::sqrt(vx * vx + vz * vz);
      if (std::sqrt(h * h + vy * vy) < k_min_span_m) {
        if (valid) {
          return angle;
        }
        return std::nullopt;
      }
      if (axis_x == 0.0f && axis_z == 0.0f) {
        if (h < k_min_span_m || std::fabs(vy) > h) {
          return std::nullopt;  // 第一次要兩手大致水平才建立方向盤平面
        }
        axis_x = vx / h;
        axis_z = vz / h;
      }
      const float along = vx * axis_x + vz * axis_z;
      const float raw = -std::atan2(vy, along);  // 右手往下＝右轉＝正
      if (!valid) {
        angle = raw;
        valid = true;
      } else {
        float d = raw - std::remainder(angle, 2.0f * k_pi);
        d = std::remainder(d, 2.0f * k_pi);
        angle += d;
      }
      // 擺正時讓水平軸慢慢對齊兩手連線（轉身、換座位姿勢）；只追同方向，免得轉 180 度時翻面
      if (std::fabs(angle) < k_follow_below && h >= k_min_span_m && along > 0.0f) {
        float nx = axis_x + (vx / h - axis_x) * k_follow;
        float nz = axis_z + (vz / h - axis_z) * k_follow;
        const float n = std::sqrt(nx * nx + nz * nz);
        if (n > 1e-4f) {
          axis_x = nx / n;
          axis_z = nz / n;
        }
      }
      return angle;
    }
  };

  /// 方向盤角度 → 左搖桿 X。range_deg＝左打到底到右打到底的總角度
  inline int16_t wheel_to_stick(float angle_rad, int range_deg) {
    const float half = static_cast<float>(range_deg > 0 ? range_deg : VIPLE_VR_WHEEL_RANGE_DEFAULT_DEG) * 0.5f * wheel_t::k_pi / 180.0f;
    float v = angle_rad / half;
    v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
    return static_cast<int16_t>(std::lround(v * 32767.0f));
  }

  /// input[0].reserved → 方向盤設定；回傳 0＝關，否則轉向總角度（度）
  inline int wheel_range_deg(const VIPLE_VR_CONTROLLER_INPUT in[2]) {
    const uint8_t b = in[0].reserved;
    if (!(b & VIPLE_VR_WHEEL_ON)) {
      return 0;
    }
    const int r = (b & VIPLE_VR_WHEEL_RANGE_MASK) * 10;
    return r > 0 ? r : VIPLE_VR_WHEEL_RANGE_DEFAULT_DEG;
  }

  /**
   * @brief 長按偵測：連續按住 hold_ms 觸發一次，放開後才能再觸發。
   */
  struct combo_t {
    int64_t since_ms = -1;
    bool fired = false;

    /// @return true＝這一刻觸發；held 回報目前是否正在長按（用來暫停 Start／Back）
    bool step(bool pressed, int64_t now_ms, int64_t hold_ms, bool *held = nullptr) {
      if (!pressed) {
        since_ms = -1;
        fired = false;
        if (held) *held = false;
        return false;
      }
      if (since_ms < 0) {
        since_ms = now_ms;
      }
      if (held) *held = true;
      if (!fired && now_ms - since_ms >= hold_ms) {
        fired = true;
        return true;
      }
      return false;
    }
  };

}  // namespace vr::padmode
