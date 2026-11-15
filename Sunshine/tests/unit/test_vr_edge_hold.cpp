/**
 * @file tests/unit/test_vr_edge_hold.cpp
 * @brief Test src/vr/vr_edge_hold.h（§VR-EDGE-HOLD：晚到的按鍵變化先配上當時的姿態）。
 */
#include "../tests_common.h"

#include <src/vr/vr_edge_hold.h>
#include <vector>

using vr::edge_hold::hold_t;
using vr::edge_hold::inputs_t;

namespace {
  constexpr int64_t k_qpf = 10'000'000;  // 10 MHz（Windows 的 QPC）
  constexpr int64_t k_ms = k_qpf / 1000;
  constexpr int64_t k_dt = k_qpf / 576;  // 取樣間隔
  constexpr int64_t k_off = 6 * k_ms;  // 平常的提前量

  hold_t make(int hold_ms = 12) {
    hold_t h;
    h.hold_ticks = hold_ms * k_ms;
    h.late_ticks = 12 * k_ms;
    h.max_ticks = 100 * k_ms;
    return h;
  }

  inputs_t trig(bool down) {
    inputs_t in;
    in.buttons[0] = down ? 1u : 0u;
    in.trigger[0] = down ? 0xFFFF : 0;
    return in;
  }

  /// 準時的樣本連送 n 筆（扳機狀態固定），回傳最後的時刻
  int64_t feed_on_time(hold_t &h, int64_t now, int n, bool down) {
    for (int i = 0; i < n; ++i) {
      EXPECT_TRUE(h.on_sample(trig(down), k_off, now));
      now += k_dt;
    }
    return now;
  }
}  // namespace

TEST(VrEdgeHoldTest, OnTimeChangeIsNotHeld) {
  auto h = make();
  int64_t now = feed_on_time(h, 1000 * k_ms, 600, true);
  EXPECT_TRUE(h.on_sample(trig(false), k_off, now));  // 準時放開
  now += k_dt;
  for (int i = 0; i < 20; ++i) {
    EXPECT_TRUE(h.on_sample(trig(false), k_off, now));  // 之後每一筆都照常寫
    now += k_dt;
  }
  EXPECT_EQ(h.holds, 0u);
  EXPECT_EQ(h.deferred, 0u);
}

TEST(VrEdgeHoldTest, LateBurstHoldsTheReleaseSample) {
  auto h = make();
  int64_t now = feed_on_time(h, 1000 * k_ms, 600, true);
  // 40 ms 的空窗：空窗裡取樣的 23 筆在空窗結束時一起到（舊的在前）；第 10 筆起扳機放開
  now += 40 * k_ms;
  int written_after_release = 0;
  for (int i = 0; i < 23; ++i) {
    const int64_t off = k_off - (40 * k_ms - i * k_dt);  // 越舊的晚越多
    const bool down = i < 10;
    const bool w = h.on_sample(trig(down), off, now);
    if (i < 10) {
      EXPECT_TRUE(w) << i;  // 放開之前的照常寫（沒有變化、也沒有在處理）
    } else if (i == 10) {
      EXPECT_TRUE(w);  // 放開的那一筆
    } else {
      EXPECT_FALSE(w) << i;  // 同一批裡後面沒有變化的不寫
      written_after_release += w ? 1 : 0;
    }
  }
  EXPECT_EQ(h.holds, 1u);
  EXPECT_EQ(h.deferred, 12u);
  EXPECT_EQ(written_after_release, 0);
  EXPECT_NEAR((double) h.last_late_ticks / k_ms, 40.0 - 10 * 1000.0 / 576.0, 1.0);
  // 接下來準時到的樣本：12 ms 內不寫，之後照常
  int deferred_live = 0;
  for (int i = 0; i < 20; ++i) {
    now += k_dt;
    const bool w = h.on_sample(trig(false), k_off, now);
    if (!w) {
      ++deferred_live;
      EXPECT_LT(i, 7) << "held longer than 12 ms";  // 12 ms ÷ 1.74 ms ≈ 6.9
    }
  }
  EXPECT_GE(deferred_live, 6);
  EXPECT_LE(deferred_live, 7);
  EXPECT_EQ(h.holds, 1u);
}

TEST(VrEdgeHoldTest, AnalogSweepInALateBurstEndsOnTheLastChange) {
  auto h = make();
  int64_t now = feed_on_time(h, 1000 * k_ms, 600, true);
  now += 50 * k_ms;
  // 28 筆一起到：第 5～14 筆扳機從 1 放到 0（每筆 10%），按鍵位元在 50% 時變
  std::vector<bool> wrote;
  for (int i = 0; i < 28; ++i) {
    inputs_t in;
    double v = 1.0;
    if (i >= 5) {
      v = i >= 15 ? 0.0 : 1.0 - (i - 4) * 0.1;
    }
    in.trigger[0] = (uint16_t) (v * 65535.0);
    in.buttons[0] = v >= 0.5 ? 1u : 0u;
    wrote.push_back(h.on_sample(in, k_off - (50 * k_ms - i * k_dt), now));
  }
  for (int i = 0; i < 28; ++i) {
    if (i <= 14) {
      EXPECT_TRUE(wrote[i]) << i;  // 還在變的每一筆都寫（最後一筆是放到 0 的那一筆）
    } else {
      EXPECT_FALSE(wrote[i]) << i;  // 變化結束之後的不寫：遊戲下一格看到的是「放完」那一刻的手
    }
  }
  EXPECT_EQ(h.holds, 1u);
}

TEST(VrEdgeHoldTest, LateSamplesWithoutChangeAreWritten) {
  auto h = make();
  int64_t now = feed_on_time(h, 1000 * k_ms, 600, false);
  now += 40 * k_ms;
  for (int i = 0; i < 23; ++i) {
    EXPECT_TRUE(h.on_sample(trig(false), k_off - (40 * k_ms - i * k_dt), now));
  }
  EXPECT_EQ(h.holds, 0u);
}

TEST(VrEdgeHoldTest, DisabledWritesEverything) {
  auto h = make(0);
  int64_t now = feed_on_time(h, 1000 * k_ms, 600, true);
  now += 40 * k_ms;
  for (int i = 0; i < 23; ++i) {
    EXPECT_TRUE(h.on_sample(trig(i < 10), k_off - (40 * k_ms - i * k_dt), now));
  }
  EXPECT_EQ(h.holds, 0u);
  EXPECT_EQ(h.deferred, 0u);
}

TEST(VrEdgeHoldTest, JitterDoesNotCountAsLate) {
  auto h = make();
  // 提前量在 1～11 ms 之間跳（外地實測的範圍）、每 50 筆按一次或放一次：都不算晚到
  int64_t now = 1000 * k_ms;
  bool down = false;
  for (int i = 0; i < 6000; ++i) {
    if (i % 50 == 0) {
      down = !down;
    }
    const int64_t off = (1 + (i * 7) % 11) * k_ms;
    EXPECT_TRUE(h.on_sample(trig(down), off, now)) << i;
    now += k_dt;
  }
  EXPECT_EQ(h.holds, 0u);
}

TEST(VrEdgeHoldTest, ContinuousLateChangesAreBounded) {
  auto h = make();
  int64_t now = feed_on_time(h, 1000 * k_ms, 600, false);
  // 線路的延遲突然長了 60 ms，同時握把在慢慢按（每 4 筆變一次）：每一段處理不超過 100 ms，而且之後 100 ms 內不再開始
  int longest_gap = 0, gap = 0, written = 0;
  for (int i = 0; i < 600; ++i) {
    inputs_t in;
    in.grip[0] = (uint16_t) (((i / 4) % 16) * 0x1000);
    const bool w = h.on_sample(in, k_off - 60 * k_ms, now);
    if (w) {
      ++written;
      gap = 0;
    } else {
      ++gap;
      longest_gap = (std::max)(longest_gap, gap);
    }
    now += k_dt;
  }
  EXPECT_LE(longest_gap, 7);  // 沒寫的樣本最多連續 12 ms
  EXPECT_GT(written, 300);  // 大部分時間照常寫（「平常的提前量」跟上新的延遲之後就不算晚到了）
}

TEST(VrEdgeHoldTest, NoiseAroundZeroIsNotAChange) {
  auto h = make();
  int64_t now = feed_on_time(h, 1000 * k_ms, 600, false);
  // 放著不動的扳機在 0 附近跳一兩格；上行有空窗、樣本成批晚到：不算變化，每一筆都照常寫
  now += 40 * k_ms;
  for (int i = 0; i < 23; ++i) {
    inputs_t in;
    in.trigger[0] = (uint16_t) ((i % 3) * 0x40);
    in.grip[1] = (uint16_t) ((i % 2) * 0x100);
    EXPECT_TRUE(h.on_sample(in, k_off - (40 * k_ms - i * k_dt), now)) << i;
  }
  EXPECT_EQ(h.holds, 0u);
  // 真的按下去（超過 1.6%）才算
  inputs_t in;
  in.trigger[0] = 0x0500;
  EXPECT_TRUE(h.on_sample(in, k_off - 20 * k_ms, now));
  EXPECT_EQ(h.holds, 1u);
}

TEST(VrEdgeHoldTest, ResetForgetsEverything) {
  auto h = make();
  int64_t now = feed_on_time(h, 1000 * k_ms, 600, true);
  h.reset();
  // reset 之後第一筆就算晚了很多也不處理（還不知道平常的提前量、也沒有上一筆可比）
  EXPECT_TRUE(h.on_sample(trig(false), k_off - 40 * k_ms, now));
  EXPECT_TRUE(h.on_sample(trig(false), k_off - 40 * k_ms, now + k_dt));
  EXPECT_EQ(h.holds, 0u);
}
