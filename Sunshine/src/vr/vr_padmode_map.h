/**
 * @file src/vr/vr_padmode_map.h
 * @brief §VR-PADMODE：Frame 手把 → Xbox 手把的對應與「兩支選單鍵長按」偵測（純函式，可單元測試）。
 *
 * 不使用 VR 手把的遊戲（賽車、一般手把遊戲：Assetto Corsa、ACC、GRIP）在 VR 裡只讀 XInput，Frame 手把
 * 只交給 SteamVR 時完全沒反應。手把模式時 server 另外插一支虛擬 Xbox 360 手把，把兩手的狀態合成一支送過去。
 * 按鍵旗標與 platf::gamepad_state_t 同值（vr_padmode.cpp 有 static_assert）。
 */
#pragma once

#include <cstdint>

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
    if (lb & VIPLE_VR_BTN_THUMBSTICK) o.buttons |= k_left_stick;
    if (rb & VIPLE_VR_BTN_THUMBSTICK) o.buttons |= k_right_stick;
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
