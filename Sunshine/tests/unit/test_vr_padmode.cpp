/**
 * @file tests/unit/test_vr_padmode.cpp
 * @brief §VR-PADMODE：Frame → Xbox 對應、長按偵測、手把模式的決策流程。
 */
#include "../tests_common.h"

#include <cmath>
#include <map>
#include <vector>

#include <src/vr/vr_padmode.h>

using namespace vr::padmode;

namespace {
  VIPLE_VR_CONTROLLER_INPUT hand(uint32_t buttons = 0, uint16_t trigger = 0, int16_t sx = 0, int16_t sy = 0) {
    VIPLE_VR_CONTROLLER_INPUT c {};
    c.buttons = buttons;
    c.trigger = trigger;
    c.stickX = sx;
    c.stickY = sy;
    c.flags = VIPLE_VR_CTRL_ACTIVE;
    c.profile = VIPLE_VR_CTRL_PROFILE_FRAME;
    return c;
  }

  struct fake_t {
    uint32_t pid = 0;
    std::map<uint32_t, app_t> apps;
    std::map<std::string, std::optional<bool>> tracked;
    std::map<std::string, bool> overrides;
    int next_id = 3;
    bool alloc_fails = false;
    std::vector<int> allocs, frees;
    std::vector<xpad_t> updates;
    std::vector<notice_e> notices;

    hooks_t hooks() {
      hooks_t h;
      h.scene_pid = [this]() {
        return pid;
      };
      h.resolve = [this](uint32_t p) -> std::optional<app_t> {
        const auto it = apps.find(p);
        if (it == apps.end()) return std::nullopt;
        return it->second;
      };
      h.tracked = [this](const std::string &id) -> std::optional<bool> {
        const auto it = tracked.find(id);
        return it == tracked.end() ? std::nullopt : it->second;
      };
      h.get_override = [this](const std::string &k) -> std::optional<bool> {
        const auto it = overrides.find(k);
        if (it == overrides.end()) return std::nullopt;
        return it->second;
      };
      h.set_override = [this](const std::string &k, bool v) {
        overrides[k] = v;
      };
      h.pad_alloc = [this]() {
        if (alloc_fails) return -1;
        allocs.push_back(next_id);
        return next_id;
      };
      h.pad_update = [this](int, const xpad_t &x) {
        updates.push_back(x);
      };
      h.pad_free = [this](int id) {
        frees.push_back(id);
      };
      h.notice = [this](notice_e n) {
        notices.push_back(n);
      };
      return h;
    }
  };
}  // namespace

TEST(VrPadmodeMap, FrameButtonsSticksTriggers) {
  VIPLE_VR_CONTROLLER_INPUT in[2] = {
    hand(VIPLE_VR_BTN_A | VIPLE_VR_BTN_D | VIPLE_VR_BTN_BUMPER | VIPLE_VR_BTN_MENU | VIPLE_VR_BTN_THUMBSTICK, 0xFFFF, -32768, 1000),
    hand(VIPLE_VR_BTN_A | VIPLE_VR_BTN_C | VIPLE_VR_BTN_MENU | VIPLE_VR_BTN_GRIP | VIPLE_VR_BTN_SYSTEM, 0x8000, 32767, -5),
  };
  const xpad_t x = map_frame(in, false);
  EXPECT_EQ(x.buttons, k_dpad_down | k_dpad_up | k_left_button | k_back | k_left_stick | k_a | k_x | k_start);
  EXPECT_EQ(x.lt, 255);
  EXPECT_EQ(x.rt, 128);
  EXPECT_EQ(x.lx, -32768);
  EXPECT_EQ(x.ly, 1000);
  EXPECT_EQ(x.rx, 32767);
  EXPECT_EQ(x.ry, -5);
}

TEST(VrPadmodeMap, InactiveHandAndSuppressedMenu) {
  VIPLE_VR_CONTROLLER_INPUT in[2] = {hand(VIPLE_VR_BTN_MENU, 0xFFFF, 100, 100), hand(VIPLE_VR_BTN_MENU | VIPLE_VR_BTN_B)};
  in[0].flags = 0;  // 左手沒在追蹤
  const xpad_t x = map_frame(in, false);
  EXPECT_EQ(x.buttons, k_start | k_b);
  EXPECT_EQ(x.lt, 0);
  EXPECT_EQ(x.lx, 0);
  in[0].flags = VIPLE_VR_CTRL_ACTIVE;
  EXPECT_EQ(map_frame(in, true).buttons, k_b);  // 長按切換中：不送 Start／Back
  EXPECT_TRUE(both_menu(in));
}

TEST(VrPadmodeMap, TouchLeftHandUsesXY) {
  VIPLE_VR_CONTROLLER_INPUT in[2] = {hand(VIPLE_VR_BTN_A | VIPLE_VR_BTN_B), hand()};
  in[0].profile = VIPLE_VR_CTRL_PROFILE_TOUCH;
  EXPECT_EQ(map_frame(in, false).buttons, k_x | k_y);
}

TEST(VrPadmodeCombo, FiresOnceAfterHoldAndRearmsOnRelease) {
  combo_t c;
  bool held = false;
  EXPECT_FALSE(c.step(true, 0, 2000, &held));
  EXPECT_TRUE(held);
  EXPECT_FALSE(c.step(true, 1999, 2000));
  EXPECT_TRUE(c.step(true, 2000, 2000));
  EXPECT_FALSE(c.step(true, 5000, 2000));
  EXPECT_FALSE(c.step(false, 5001, 2000, &held));
  EXPECT_FALSE(held);
  EXPECT_FALSE(c.step(true, 6000, 2000));
  EXPECT_TRUE(c.step(true, 8000, 2000));
}

TEST(VrPadmodeController, StoreSaysNoTrackedControllersTurnsPadOn) {
  fake_t f;
  f.apps[100] = {"805550", "805550", "ACC"};
  controller_t c(f.hooks(), false);
  f.pid = 100;
  c.poll_once();  // 商店結果還沒回來：維持 VR 手把
  EXPECT_FALSE(c.pad_active());
  f.tracked["805550"] = false;
  c.poll_once();
  EXPECT_TRUE(c.pad_active());
  ASSERT_EQ(f.notices.size(), 1u);
  EXPECT_EQ(f.notices[0], notice_e::pad_on_auto);

  // 狀態有變才送
  VIPLE_VR_CONTROLLER_INPUT in[2] = {hand(), hand(VIPLE_VR_BTN_A)};
  c.on_sample(in, 0);
  c.on_sample(in, 10);
  EXPECT_EQ(f.updates.size(), 1u);
  EXPECT_EQ(f.updates[0].buttons, k_a);

  // 遊戲結束：自動關、不提示
  f.pid = 0;
  c.poll_once();
  EXPECT_FALSE(c.pad_active());
  EXPECT_EQ(f.frees.size(), 1u);
  EXPECT_EQ(f.notices.size(), 1u);
}

TEST(VrPadmodeController, TrackedGameStaysOnVrControllers) {
  fake_t f;
  f.apps[7] = {"488310", "488310", "Eleven"};
  f.tracked["488310"] = true;
  f.pid = 7;
  controller_t c(f.hooks(), false);
  c.poll_once();
  EXPECT_FALSE(c.pad_active());
  EXPECT_TRUE(f.allocs.empty());
  EXPECT_TRUE(f.notices.empty());
}

TEST(VrPadmodeController, ManualToggleIsRememberedPerGame) {
  fake_t f;
  f.apps[7] = {"488310", "488310", "Eleven"};
  f.tracked["488310"] = true;
  f.pid = 7;
  controller_t c(f.hooks(), false);
  c.poll_once();
  VIPLE_VR_CONTROLLER_INPUT both[2] = {hand(VIPLE_VR_BTN_MENU), hand(VIPLE_VR_BTN_MENU)};
  c.on_sample(both, 0);
  c.on_sample(both, 2000);
  c.poll_once();
  EXPECT_TRUE(c.pad_active());
  EXPECT_EQ(f.overrides["488310"], true);
  ASSERT_FALSE(f.notices.empty());
  EXPECT_EQ(f.notices.back(), notice_e::pad_on_manual);

  // 下一場（新的 controller）：記住的選擇直接生效
  fake_t g = f;
  g.notices.clear();
  controller_t c2(g.hooks(), false);
  c2.poll_once();
  EXPECT_TRUE(c2.pad_active());

  // 再長按一次切回 VR 手把
  VIPLE_VR_CONTROLLER_INPUT none[2] = {hand(), hand()};
  c.on_sample(none, 2100);
  c.on_sample(both, 3000);
  c.on_sample(both, 5000);
  c.poll_once();
  EXPECT_FALSE(c.pad_active());
  EXPECT_EQ(f.overrides["488310"], false);
  EXPECT_EQ(f.notices.back(), notice_e::pad_off_manual);
}

TEST(VrPadmodeController, AllocFailureNoticesOnce) {
  fake_t f;
  f.apps[1] = {"244210", "244210", "AC"};
  f.tracked["244210"] = false;
  f.alloc_fails = true;
  controller_t c(f.hooks(), false);
  f.pid = 1;
  c.poll_once();
  f.pid = 2;
  c.poll_once();
  f.pid = 1;
  c.poll_once();
  EXPECT_FALSE(c.pad_active());
  ASSERT_EQ(f.notices.size(), 1u);
  EXPECT_EQ(f.notices[0], notice_e::pad_unavailable);
}

TEST(VrPadmodeController, DestructorFreesPad) {
  fake_t f;
  f.apps[1] = {"244210", "244210", "AC"};
  f.tracked["244210"] = false;
  f.pid = 1;
  {
    controller_t c(f.hooks(), false);
    c.poll_once();
    EXPECT_TRUE(c.pad_active());
  }
  EXPECT_EQ(f.frees.size(), 1u);
}

// §VR-WHEEL
namespace {
  /// 兩手在 y＝1、z＝-0.4 的平面握著直徑 0.36 m 的方向盤，轉 deg 度（右轉為正）；yaw＝整個人轉身的角度
  void wheel_hands(float deg, float l[3], float r[3], float yaw_deg = 0.0f) {
    const float a = deg * 3.14159265f / 180.0f;
    const float y = yaw_deg * 3.14159265f / 180.0f;
    const float rad = 0.18f;
    // 方向盤平面裡的右手位置（x 往右、y 往上）：右轉時右手往下
    const float px = rad * std::cos(a);
    const float py = -rad * std::sin(a);
    const float ax = std::cos(y);
    const float az = std::sin(y);
    r[0] = px * ax;
    r[1] = 1.0f + py;
    r[2] = -0.4f + px * az;
    l[0] = -px * ax;
    l[1] = 1.0f - py;
    l[2] = -0.4f - px * az;
  }
}  // namespace

TEST(VrPadmodeWheel, AngleFollowsRotationPast90Degrees) {
  wheel_t w;
  float l[3], r[3];
  for (int d = 0; d <= 200; d += 5) {
    wheel_hands(static_cast<float>(d), l, r);
    const auto a = w.step(l, r, true);
    ASSERT_TRUE(a.has_value());
    EXPECT_NEAR(*a * 180.0f / 3.14159265f, d, 0.5f) << "at " << d;
  }
  for (int d = 200; d >= -200; d -= 5) {
    wheel_hands(static_cast<float>(d), l, r);
    const auto a = w.step(l, r, true);
    ASSERT_TRUE(a.has_value());
    EXPECT_NEAR(*a * 180.0f / 3.14159265f, d, 0.5f) << "at " << d;
  }
}

TEST(VrPadmodeWheel, IndependentOfBodyYaw) {
  wheel_t w;
  float l[3], r[3];
  wheel_hands(0.0f, l, r, 40.0f);
  ASSERT_TRUE(w.step(l, r, true).has_value());
  wheel_hands(45.0f, l, r, 40.0f);
  EXPECT_NEAR(*w.step(l, r, true) * 180.0f / 3.14159265f, 45.0f, 0.5f);
  wheel_hands(-30.0f, l, r, 40.0f);
  EXPECT_NEAR(*w.step(l, r, true) * 180.0f / 3.14159265f, -30.0f, 0.5f);
}

TEST(VrPadmodeWheel, LostTrackingResetsAndNeedsLevelHandsFirst) {
  wheel_t w;
  float l[3], r[3];
  wheel_hands(70.0f, l, r);
  EXPECT_FALSE(w.step(l, r, true).has_value());  // 第一次兩手不水平（> 45 度）：還不建立
  wheel_hands(10.0f, l, r);
  EXPECT_TRUE(w.step(l, r, true).has_value());
  EXPECT_FALSE(w.step(l, r, false).has_value());
  EXPECT_FALSE(w.valid);
}

TEST(VrPadmodeWheel, StickMappingAndWireByte) {
  const float pi = 3.14159265f;
  EXPECT_EQ(wheel_to_stick(pi / 2, 180), 32767);
  EXPECT_EQ(wheel_to_stick(-pi, 180), -32767);
  EXPECT_EQ(wheel_to_stick(pi / 4, 180), 16384);
  EXPECT_EQ(wheel_to_stick(pi / 2, 360), 16384);
  VIPLE_VR_CONTROLLER_INPUT in[2] = {hand(), hand()};
  EXPECT_EQ(wheel_range_deg(in), 0);
  in[0].reserved = VIPLE_VR_WHEEL_ON;
  EXPECT_EQ(wheel_range_deg(in), 180);
  in[0].reserved = VIPLE_VR_WHEEL_ON | 54;
  EXPECT_EQ(wheel_range_deg(in), 540);
}

TEST(VrPadmodeMap, BothStickClicksAreSuppressed) {
  VIPLE_VR_CONTROLLER_INPUT in[2] = {hand(VIPLE_VR_BTN_THUMBSTICK), hand(VIPLE_VR_BTN_THUMBSTICK)};
  EXPECT_EQ(map_frame(in, false).buttons, 0u);
  in[1].buttons = 0;
  EXPECT_EQ(map_frame(in, false).buttons, k_left_stick);
}
