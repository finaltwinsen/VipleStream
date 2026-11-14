/**
 * @file tests/unit/test_desktop_fec.cpp
 * @brief 2D 串流的自適應 FEC 規則（src/desktop_fec.h）。
 */
#include "../tests_common.h"

#include <src/desktop_fec.h>

namespace df = desktop_fec;
using namespace std::chrono_literals;

namespace {
  /// 照 stream.cpp 的用法把規則串起來：有缺包的視窗（on_fec_status_locked）、丟包尾巴（ping tick 的 residual > 0）、
  /// 沒有缺包的一輪（兩條路徑共用 desktop_fec_decay_locked）
  struct sim_t {
    int base = 10;
    int adaptive = 0;  // 0＝還沒升過，用的是 base
    std::chrono::steady_clock::time_point now {1000s};
    std::chrono::steady_clock::time_point last_loss {};
    std::chrono::steady_clock::time_point last_decay {};
    int streak = 0;

    int cur() const {
      return adaptive != 0 ? adaptive : base;
    }

    void loss_window() {
      const int next = df::raised(cur(), base);
      if (next != cur()) {
        adaptive = next;
      }
      last_loss = now;
      streak = 0;
    }

    void loss_tail() {
      last_loss = now;
      streak = 0;
    }

    /// 回傳這一輪有沒有回降
    bool clean_round() {
      ++streak;
      const int next = df::decayed(adaptive, base, now - last_loss, streak, now - last_decay);
      if (next == adaptive) {
        return false;
      }
      adaptive = next;
      last_decay = now;
      return true;
    }
  };
}  // namespace

TEST(DesktopFec, RaiseStepsAndCap) {
  EXPECT_EQ(df::raised(10, 10), 20);
  EXPECT_EQ(df::raised(40, 10), 50);
  EXPECT_EQ(df::raised(50, 10), 50);  // 上限
  EXPECT_EQ(df::raised(45, 45), 50);  // 不滿一階也只到上限
  // 基準比上限高：維持基準，不可以「升」到 50
  EXPECT_EQ(df::raised(60, 60), 60);
  EXPECT_EQ(df::raised(55, 60), 60);
}

TEST(DesktopFec, DecayConditions) {
  // 還沒升過、或已經在基準：不動
  EXPECT_EQ(df::decayed(0, 10, 60s, 10, 60s), 0);
  EXPECT_EQ(df::decayed(10, 10, 60s, 10, 60s), 10);
  EXPECT_EQ(df::decayed(8, 10, 60s, 10, 60s), 8);
  // 距最後一次丟包要超過 5 個整秒
  EXPECT_EQ(df::decayed(30, 10, 5000ms, 10, 60s), 30);
  EXPECT_EQ(df::decayed(30, 10, 5999ms, 10, 60s), 30);
  EXPECT_EQ(df::decayed(30, 10, 6000ms, 10, 60s), 25);
  // 連續 3 輪沒有缺包
  EXPECT_EQ(df::decayed(30, 10, 60s, 2, 60s), 30);
  EXPECT_EQ(df::decayed(30, 10, 60s, 3, 60s), 25);
  // 兩次回降至少隔 500 ms
  EXPECT_EQ(df::decayed(30, 10, 60s, 10, 499ms), 30);
  EXPECT_EQ(df::decayed(30, 10, 60s, 10, 500ms), 25);
  // 不低於基準
  EXPECT_EQ(df::decayed(12, 10, 60s, 10, 60s), 10);
  EXPECT_EQ(df::decayed(25, 22, 60s, 10, 60s), 22);
}

TEST(DesktopFec, BurstThenDecayBackToBase) {
  sim_t s;
  // 2 秒的丟包：四個 500 ms 視窗都有缺包 → 10% 升到 50%
  for (int i = 0; i < 4; ++i) {
    s.now += 500ms;
    s.loss_window();
  }
  EXPECT_EQ(s.cur(), 50);
  s.now += 500ms;
  s.loss_window();
  EXPECT_EQ(s.cur(), 50);  // 上限

  // 之後只有 ping tick 的零缺包輪（500 ms 一輪）。滿 6 秒之前不降
  const auto stop = s.now;
  int first_decay_ms = -1;
  int back_to_base_ms = -1;
  for (int i = 0; i < 40 && back_to_base_ms < 0; ++i) {
    s.now += 500ms;
    const int before = s.cur();
    if (s.clean_round()) {
      EXPECT_EQ(s.cur(), before - 5);  // 一輪最多一階
      if (first_decay_ms < 0) {
        first_decay_ms = (int) std::chrono::duration_cast<std::chrono::milliseconds>(s.now - stop).count();
      }
    }
    if (s.cur() == s.base) {
      back_to_base_ms = (int) std::chrono::duration_cast<std::chrono::milliseconds>(s.now - stop).count();
    }
  }
  EXPECT_EQ(first_decay_ms, 6000);
  EXPECT_EQ(back_to_base_ms, 6000 + 7 * 500);  // 50 → 10 共 8 階
  // 回到基準之後不再動
  s.now += 500ms;
  EXPECT_FALSE(s.clean_round());
  EXPECT_EQ(s.cur(), 10);
}

TEST(DesktopFec, InterleavedPathsDecayAtMostOncePer500ms) {
  sim_t s;
  s.now += 500ms;
  s.loss_window();
  s.now += 500ms;
  s.loss_window();
  EXPECT_EQ(s.cur(), 30);
  s.now += 7s;
  s.streak = 5;
  EXPECT_TRUE(s.clean_round());  // ping tick
  EXPECT_EQ(s.cur(), 25);
  s.now += 200ms;
  EXPECT_FALSE(s.clean_round());  // 緊接著 FEC status 的零缺包視窗：還不到 500 ms
  EXPECT_EQ(s.cur(), 25);
  s.now += 300ms;
  EXPECT_TRUE(s.clean_round());
  EXPECT_EQ(s.cur(), 20);
}

TEST(DesktopFec, LossTailPostponesDecayWithoutRaising) {
  sim_t s;
  s.now += 500ms;
  s.loss_window();
  s.now += 500ms;
  s.loss_window();
  EXPECT_EQ(s.cur(), 30);
  s.now += 7s;
  s.streak = 5;
  EXPECT_TRUE(s.clean_round());
  EXPECT_EQ(s.cur(), 25);

  // 回降途中 ping tick 排到殘留的缺包：不升，但重新等 6 秒、重新數 3 輪
  s.now += 500ms;
  s.loss_tail();
  EXPECT_EQ(s.cur(), 25);
  for (int i = 0; i < 11; ++i) {  // 5.5 秒
    s.now += 500ms;
    EXPECT_FALSE(s.clean_round());
  }
  s.now += 500ms;  // 第 6 秒
  EXPECT_TRUE(s.clean_round());
  EXPECT_EQ(s.cur(), 20);
}

TEST(DesktopFec, NeverRaisedStaysUntouched) {
  sim_t s;
  for (int i = 0; i < 20; ++i) {
    s.now += 500ms;
    EXPECT_FALSE(s.clean_round());
  }
  EXPECT_EQ(s.adaptive, 0);
}
