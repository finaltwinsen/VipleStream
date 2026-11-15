/**
 * @file tests/unit/test_vr_pace_auto.cpp
 * @brief Test src/vr/vr_pace_auto.h（§VR-PACE-AUTO：攤開送的自動開關與攤開比例）。
 */
#include "../tests_common.h"

#include <src/vr/vr_pace_auto.h>

using vr::pace::auto_t;
using vr::pace::pct_for_period;

TEST(VrPaceAutoTest, PctFollowsFramePeriod) {
  EXPECT_EQ(pct_for_period(6944), 85);  // 144 fps：6.5 ms 超過上限，夾在 85%
  EXPECT_EQ(pct_for_period(8333), 78);  // 120 fps
  EXPECT_EQ(pct_for_period(11111), 58);  // 90 fps
  EXPECT_EQ(pct_for_period(13889), 46);  // 72 fps（半速）：約 6.4 ms
  EXPECT_EQ(pct_for_period(16667), 38);  // 60 fps
  EXPECT_EQ(pct_for_period(40000), 20);  // 很低的幀率：不低於下限
  EXPECT_EQ(pct_for_period(0), 0);
  EXPECT_EQ(pct_for_period(-5), 0);
}

TEST(VrPaceAutoTest, StaysOffOnCleanPath) {
  auto_t a;
  for (int i = 0; i < 100; ++i) {
    EXPECT_FALSE(a.on_window(true, i % 7 == 0 ? 3u : 0u));  // 偶爾 3 個救不回：不到門檻
  }
  EXPECT_FALSE(a.on);
  EXPECT_EQ(a.switches, 0u);
}

TEST(VrPaceAutoTest, TurnsOnAfterTwoLossyWindows) {
  auto_t a;
  EXPECT_FALSE(a.on_window(true, 8));
  EXPECT_FALSE(a.on);
  EXPECT_TRUE(a.on_window(true, 5));
  EXPECT_TRUE(a.on);
  EXPECT_EQ(a.switches, 1u);
}

TEST(VrPaceAutoTest, OneLossyWindowIsNotEnough) {
  auto_t a;
  EXPECT_FALSE(a.on_window(true, 9));
  EXPECT_FALSE(a.on_window(true, 0));  // 中斷：重新數
  EXPECT_FALSE(a.on_window(true, 9));
  EXPECT_FALSE(a.on);
  EXPECT_TRUE(a.on_window(true, 4));
  EXPECT_TRUE(a.on);
}

TEST(VrPaceAutoTest, StaysOnWhileLossesContinueAtTheReducedLevel) {
  // 開著之後丟包會變少（實測每 10 秒 3 個上下）：不到「乾淨」的門檻，不可以關
  auto_t a;
  a.on_window(true, 8);
  a.on_window(true, 8);
  ASSERT_TRUE(a.on);
  for (int i = 0; i < 200; ++i) {
    EXPECT_FALSE(a.on_window(true, i % 3 == 0 ? 1u : 3u));
  }
  EXPECT_TRUE(a.on);
  EXPECT_EQ(a.switches, 1u);
}

TEST(VrPaceAutoTest, TurnsOffAfterTwoCleanMinutes) {
  auto_t a;
  a.on_window(true, 8);
  a.on_window(true, 8);
  ASSERT_TRUE(a.on);
  for (int i = 0; i < 11; ++i) {
    EXPECT_FALSE(a.on_window(true, 0));
  }
  EXPECT_TRUE(a.on);
  EXPECT_TRUE(a.on_window(true, 1));  // 第 12 個乾淨的視窗
  EXPECT_FALSE(a.on);
  EXPECT_EQ(a.switches, 2u);
  // 關掉之後要重新連續兩個視窗才會再開
  EXPECT_FALSE(a.on_window(true, 6));
  EXPECT_TRUE(a.on_window(true, 6));
}

TEST(VrPaceAutoTest, CleanRunIsResetByALossyWindow) {
  auto_t a;
  a.on_window(true, 8);
  a.on_window(true, 8);
  for (int i = 0; i < 11; ++i) {
    a.on_window(true, 0);
  }
  EXPECT_FALSE(a.on_window(true, 2));  // 不乾淨：重新數
  for (int i = 0; i < 11; ++i) {
    EXPECT_FALSE(a.on_window(true, 0));
  }
  EXPECT_TRUE(a.on);
  EXPECT_TRUE(a.on_window(true, 0));
  EXPECT_FALSE(a.on);
}

TEST(VrPaceAutoTest, AFewResendRequestsDoNotSwitchItOff) {
  // 11-14 外地半速兩場：開著時頭盔每 10 秒送 1～5 份補包請求。原本一份就關，10 分鐘切了 13 次
  auto_t a;
  a.on_window(true, 8, 0);
  a.on_window(true, 8, 2);
  ASSERT_TRUE(a.on);
  for (int i = 0; i < 60; ++i) {
    EXPECT_FALSE(a.on_window(true, 3 + i % 5, 1 + i % 5));
  }
  EXPECT_TRUE(a.on);
  EXPECT_EQ(a.switches, 1u);
  // 少量補包請求也不擋它打開
  auto_t b;
  EXPECT_FALSE(b.on_window(true, 10, 5));
  EXPECT_TRUE(b.on_window(true, 6, 19));
}

TEST(VrPaceAutoTest, BusyResendsSwitchItOffAfterTwoWindowsAndKeepItOff) {
  // 區網補包在用時每 10 秒 50～190 份：連續兩個視窗才關（單一個視窗的尖峰不算），而且補包在用期間不開
  auto_t a;
  a.on_window(true, 8, 0);
  a.on_window(true, 8, 0);
  ASSERT_TRUE(a.on);
  EXPECT_FALSE(a.on_window(true, 8, 90));
  EXPECT_FALSE(a.on_window(true, 8, 3));  // 中斷：重新數
  EXPECT_FALSE(a.on_window(true, 8, 60));
  EXPECT_TRUE(a.on_window(true, 8, 20));
  EXPECT_FALSE(a.on);
  EXPECT_STREQ(a.reason, "resends are in use");
  for (int i = 0; i < 20; ++i) {
    EXPECT_FALSE(a.on_window(true, 12, 80));
  }
  EXPECT_FALSE(a.on);
  // 補包安靜下來、丟包還在：重新連續兩個視窗才再開
  EXPECT_FALSE(a.on_window(true, 8, 4));
  EXPECT_TRUE(a.on_window(true, 8, 4));
}

TEST(VrPaceAutoTest, SecondLinkTurnsOffAtOnceAndBlocksTurningOn) {
  // 有第二條連線在送：馬上關，而且不管丟多少都不開
  auto_t a;
  a.on_window(true, 8);
  a.on_window(true, 8);
  ASSERT_TRUE(a.on);
  EXPECT_TRUE(a.on_window(false, 8));
  EXPECT_FALSE(a.on);
  for (int i = 0; i < 20; ++i) {
    EXPECT_FALSE(a.on_window(false, 20));
  }
  EXPECT_FALSE(a.on);
  // 條件恢復之後也要重新連續兩個視窗
  EXPECT_FALSE(a.on_window(true, 8));
  EXPECT_TRUE(a.on_window(true, 8));
}
