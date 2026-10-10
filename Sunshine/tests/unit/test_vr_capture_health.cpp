/**
 * @file tests/unit/test_vr_capture_health.cpp
 * @brief VipleStream 2.0 §VR-FENCE-LATE：VR 擷取端的等待預算、卡死／餓死、拆線退避與過載旗標
 *        （src/vr/vr_capture_health.h；全部用假的時間與假的影像序列驅動）。
 *
 * 這個檔刻意不 include ../tests_common.h：受測的標頭只用標準函式庫，測試也只需要 googletest，
 * 所以可以脫離整個 server 單獨編譯執行（-Wall -Wextra -Wshadow -Wconversion -Werror 也過得了；
 * tests_common.h 會拉進 logging／platform 的標頭，這組旗標過不了）。放進 test_sunshine 一起建也一樣能跑。
 *
 * 寫法上的兩個約定：
 *   - 規格的數字（3 s、5 s、30 張、5／15／30 s、30%／5%／2 s……）在這裡寫死，不引用標頭的常數：常數被改掉時測試要會失敗。
 *   - 沒有影像的時段用 sim_t::idle()，它照正式擷取迴圈的節拍（等 evtFrm 逾時，每 100 ms）呼叫 on_idle()，
 *     不是把時間一次跳過去——兩種走法在視窗的處理上不一樣，正式程式只有前一種。
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <ostream>
#include <src/vr/vr_capture_health.h>

using namespace vr::capture;

namespace vr::capture {
  // 斷言失敗時印名字（不然 googletest 只會印「1-byte object <02>」）
  void PrintTo(teardown_e e, std::ostream *os) {
    *os << teardown_name(e);
  }

  void PrintTo(outcome_e e, std::ostream *os) {
    *os << (e == outcome_e::on_time ? "on_time" : (e == outcome_e::late ? "late" : "abandoned"));
  }
}  // namespace vr::capture

namespace {
  constexpr int64_t kMs = 1'000'000;
  constexpr int64_t kSec = 1'000'000'000;
  constexpr int64_t kT120 = 8'333'333;  // 120 Hz
  constexpr int64_t kT90 = 11'111'111;  // 90 Hz
  constexpr int64_t kMax100 = 100 * kMs;  // vr_capture_late_max_ms 的預設
  constexpr int64_t kStall = 3 * kSec;
  constexpr int64_t kStarve = 5 * kSec;
  constexpr int64_t kStart = 5'000 * kSec;  // 任意的起點（不是 0）。剛好在整秒上；要測格子邊界的測試自己另外給起點
  constexpr int64_t kI64Max = std::numeric_limits<int64_t>::max();
  constexpr int64_t kI64Min = std::numeric_limits<int64_t>::min();

  /// 放棄的那一張，完成值的走法（複製成功的影像一定到值，這個參數對它沒有作用）
  enum class fence_e {
    reach,  ///< 到值：只能配複製成功的影像（預設值；配放棄的影像是測試寫錯）
    stuck,  ///< 完全不動
    lagging,  ///< 有在前進，但永遠比 descriptor 寫的值少（追不上）
  };

  /**
   * @brief 假的擷取迴圈：每呼叫一次 step()，就是過了一段時間、driver 又發布了幾張、server 取用最新的那一張。
   *        frame_id 與 descriptor 的 fence 值每發布一張各加 1（和真的 driver 一樣）。
   */
  struct sim_t {
    health_t h;
    int64_t now;
    uint64_t frame_id = 0;
    uint64_t fence = 1000;  // 最新一張 descriptor 寫的 fence 值
    uint64_t completed = 1000;  // 目前的完成值

    explicit sim_t(bool on = true, int64_t period_ns = kT120, int64_t max_wait_ns = kMax100, int64_t start_ns = kStart):
        now(start_ns) {
      h.configure(on, period_ns, max_wait_ns);
      h.begin_generation(now);
    }

    /// driver 重連：新的 generation（fence 物件是新的，值從別的地方開始）
    void new_generation(int64_t gap_ns = 500 * kMs) {
      now += gap_ns;
      frame_id = 0;
      fence = 77;
      completed = 77;
      h.begin_generation(now);
    }

    decision_t step(outcome_e o, fence_e fe = fence_e::reach, int64_t interval_ns = kT120, uint64_t publish = 1, int64_t wait_ns = 0) {
      now += interval_ns;
      frame_id += publish;
      fence += publish;
      if (o != outcome_e::abandoned) {
        completed = fence;
      } else if (fe == fence_e::lagging) {
        completed = fence - 3;  // 每張都比上一張大，但永遠到不了這一張的值
      } else if (fe == fence_e::reach) {
        ADD_FAILURE() << "an abandoned frame cannot have reached its fence value: pass fence_e::stuck or fence_e::lagging";
      }
      frame_t f;
      f.outcome = o;
      f.now_ns = now;
      f.wait_ns = wait_ns;
      f.present_ns = now - wait_ns - kMs;  // driver 在被取用前不久 Present
      f.frame_id = frame_id;
      f.observed = completed;
      return h.on_frame(f);
    }

    /// 連續 n 張同樣的結果；回傳最後一張的回覆
    decision_t run(int n, outcome_e o, fence_e fe = fence_e::reach, int64_t interval_ns = kT120, uint64_t publish = 1, int64_t wait_ns = 0) {
      decision_t d;
      for (int i = 0; i < n; ++i) {
        d = step(o, fe, interval_ns, publish, wait_ns);
      }
      return d;
    }

    /// 一直餵放棄的影像直到回報拆線（或超過 limit_ns）；回傳那一張的回覆，elapsed_ns＝從呼叫起經過多久
    decision_t run_until_teardown(fence_e fe, int64_t limit_ns, int64_t &elapsed_ns, int64_t interval_ns = kT120) {
      const int64_t t0 = now;
      decision_t d;
      while (now - t0 < limit_ns) {
        d = step(outcome_e::abandoned, fe, interval_ns);
        if (d.teardown != teardown_e::none) {
          break;
        }
      }
      elapsed_ns = now - t0;
      return d;
    }

    /// 沒有影像：照正式擷取迴圈的節拍每 tick_ns 呼叫一次 on_idle()
    void idle(int64_t dur_ns, int64_t tick_ns = 100 * kMs) {
      const int64_t end = now + dur_ns;
      while (end - now >= tick_ns) {
        now += tick_ns;
        h.on_idle(now);
      }
      now = end;
    }

    /// 先因卡死拆一次線（退避的階梯變成 1），再換新的 generation
    void tear_down_once_and_reconnect() {
      int64_t took = 0;
      ASSERT_EQ(run_until_teardown(fence_e::stuck, 20 * kSec, took).teardown, teardown_e::stall);
      new_generation();
    }

    /// 把時間往後推，讓「下一張以 interval_ns 餵進來的影像」剛好落在某一整秒之後 into_second_ns 的位置，
    /// 並從那裡重新開始這個 generation（健康格對齊整秒，要測格子邊界時用）
    void align_next_frame(int64_t into_second_ns, int64_t interval_ns = kT120) {
      const int64_t next = now + interval_ns;
      const int64_t into = ((next % kSec) + kSec) % kSec;
      now += (into_second_ns - into + 2 * kSec) % kSec;
      h.begin_generation(now);
    }
  };

  /// 兩個數差不到 tol（不叫 near：那是 Windows 標頭的巨集）
  bool close_to(int64_t a, int64_t b, int64_t tol) {
    return (a > b ? a - b : b - a) <= tol;
  }
}  // namespace

// ── (a) 等待預算 ─────────────────────────────────────────────────────────

TEST(VrCaptureHealthBudget, DefaultsToThreeDisplayPeriods) {
  sim_t s;
  EXPECT_EQ(s.h.wait_budget_ns(), 3 * kT120);  // 還沒有發布間隔的樣本
  EXPECT_EQ(s.h.interval_ema_ns(), 0);
  s.run(600, outcome_e::on_time);  // 每 8.33 ms 一張
  EXPECT_TRUE(close_to(s.h.interval_ema_ns(), kT120, 1000));
  EXPECT_TRUE(close_to(s.h.wait_budget_ns(), 3 * kT120, 3000));

  sim_t s90(true, kT90);
  EXPECT_EQ(s90.h.wait_budget_ns(), 3 * kT90);
}

TEST(VrCaptureHealthBudget, FollowsThePublishInterval) {
  sim_t s;
  // 遊戲每 16.67 ms 才交一幀（合成器的 Present 節拍跟著遊戲走）：預算跟著變成約 50 ms
  s.run(200, outcome_e::late, fence_e::reach, 2 * kT120);
  EXPECT_TRUE(close_to(s.h.interval_ema_ns(), 2 * kT120, 2000));
  EXPECT_TRUE(close_to(s.h.wait_budget_ns(), 6 * kT120, 6000));
  // 更慢（每 50 ms 一幀）：3 個間隔是 150 ms，夾在上限 100 ms
  s.run(200, outcome_e::late, fence_e::reach, 50 * kMs);
  EXPECT_EQ(s.h.wait_budget_ns(), kMax100);
  // 回到全速：預算回到 3 個顯示週期（不低於它）
  s.run(600, outcome_e::on_time);
  EXPECT_GE(s.h.wait_budget_ns(), 3 * kT120);
  EXPECT_TRUE(close_to(s.h.wait_budget_ns(), 3 * kT120, 3000));
}

TEST(VrCaptureHealthBudget, AveragesOverAboutEightSamples) {
  // 遊戲突然由 8.33 ms 變成 16.67 ms 一張：預算幾張之後跟到新目標的 90%。每筆佔 1/8 的指數平均要
  // ln(10)÷ln(8/7)≈17.2 筆（整數截斷後 19 上下）；每筆佔 1/4 會是 8 筆、1/16 會是 36 筆
  sim_t s;
  s.run(600, outcome_e::on_time);
  const int64_t target = 6 * kT120;
  int n = 0;
  while (s.h.wait_budget_ns() < target - (target - 3 * kT120) / 10 && n < 1000) {
    s.step(outcome_e::late, fence_e::reach, 2 * kT120, 1, 12 * kMs);
    ++n;
  }
  EXPECT_GE(n, 17);
  EXPECT_LE(n, 21);
}

TEST(VrCaptureHealthBudget, NeverBelowThreePeriodsNorAboveTheLimit) {
  // 發布間隔比顯示週期短（寫入方的時間戳亂跳）也不會讓預算低於 3 個顯示週期
  sim_t s;
  s.run(300, outcome_e::on_time, fence_e::reach, kT120 / 4);
  EXPECT_EQ(s.h.wait_budget_ns(), 3 * kT120);

  // 上限比 3 個顯示週期還短：上限優先（120 Hz 的 3 個週期是 25 ms，上限 20 ms）
  sim_t tight(true, kT120, 20 * kMs);
  EXPECT_EQ(tight.h.wait_budget_ns(), 20 * kMs);
  tight.run(100, outcome_e::late, fence_e::reach, 40 * kMs);
  EXPECT_EQ(tight.h.wait_budget_ns(), 20 * kMs);

  // 設定值超出範圍會被夾住；顯示週期不合理時用 90 Hz
  sim_t low(true, kT120, 1 * kMs);
  EXPECT_EQ(low.h.wait_budget_ns(), 20 * kMs);
  sim_t high(true, 100 * kMs, 10 * kSec);
  EXPECT_EQ(high.h.wait_budget_ns(), 250 * kMs);
  sim_t bad_period(true, 0, kMax100);
  EXPECT_EQ(bad_period.h.wait_budget_ns(), 3 * kT90);
  sim_t fast_period(true, 3 * kMs, kMax100);  // 333 Hz：在 2～100 ms 的範圍內，照用
  EXPECT_EQ(fast_period.h.wait_budget_ns(), 9 * kMs);
  sim_t too_fast(true, 2 * kMs - 1, kMax100);
  EXPECT_EQ(too_fast.h.wait_budget_ns(), 3 * kT90);
}

TEST(VrCaptureHealthBudget, SkippedFramesDivideTheSpan) {
  // 每次等完都跳過一張（latest-wins）：兩張被取用的影像相隔兩個發布間隔，估出來的仍是一個間隔
  sim_t s;
  s.run(300, outcome_e::late, fence_e::reach, 2 * kT120, 2);
  EXPECT_TRUE(close_to(s.h.interval_ema_ns(), kT120, 1000));
  EXPECT_TRUE(close_to(s.h.wait_budget_ns(), 3 * kT120, 3000));
}

TEST(VrCaptureHealthBudget, BogusTimestampsAndFrameIdsAreContained) {
  sim_t s;
  s.run(100, outcome_e::on_time);
  const int64_t before = s.h.interval_ema_ns();
  ASSERT_TRUE(close_to(before, kT120, 1000));

  frame_t f;
  f.outcome = outcome_e::on_time;
  f.observed = 5000;
  // present 倒退、frame_id 倒退：不取樣、發布張數當 1
  f.now_ns = s.now + kT120;
  f.present_ns = s.now - 10 * kSec;
  f.frame_id = 1;
  s.h.on_frame(f);
  EXPECT_EQ(s.h.interval_ema_ns(), before);
  // present 一次跳 20 s、frame_id 沒變：單筆夾在上限（100 ms），平均只往它走 1/8
  f.now_ns += kT120;
  f.present_ns = s.now + 10 * kSec;
  s.h.on_frame(f);
  EXPECT_EQ(s.h.interval_ema_ns(), before + (kMax100 - before) / 8);
  EXPECT_LE(s.h.wait_budget_ns(), kMax100);
  // frame_id 一次跳很遠：發布張數夾住（diag 的累計不會爆）
  const uint64_t published_before = s.h.diag(f.now_ns).published;
  f.now_ns += kT120;
  f.present_ns += kT120;
  f.frame_id = std::numeric_limits<uint64_t>::max();
  s.h.on_frame(f);
  EXPECT_EQ(s.h.diag(f.now_ns).published, published_before + 1024u);
}

// ── (b) 卡死 ─────────────────────────────────────────────────────────────

TEST(VrCaptureHealthStall, TearsDownAfterThreeSecondsWithoutFenceProgress) {
  sim_t s;
  s.run(240, outcome_e::on_time);
  int64_t took = 0;
  const auto d = s.run_until_teardown(fence_e::stuck, 20 * kSec, took);
  EXPECT_EQ(d.teardown, teardown_e::stall);
  EXPECT_EQ(d.allowed_in_ns, 5 * kSec);
  // 從「完成值沒前進的第一張」起算滿 3 s 的那一張
  EXPECT_GE(took, kStall);
  EXPECT_LT(took, kStall + 3 * kT120);
  EXPECT_EQ(s.h.teardowns(), 1u);
}

TEST(VrCaptureHealthStall, FiresOnTheFrameAtExactlyThreeSeconds) {
  // 每 100 ms 一張、完成值不動：第 1 張起算，第 31 張剛好 3.000 s
  sim_t s;
  int64_t first = 0;
  for (int k = 0; k <= 40; ++k) {
    const auto d = s.step(outcome_e::abandoned, fence_e::stuck, 100 * kMs);
    if (k == 0) {
      first = s.now;
    }
    if (k < 30) {
      ASSERT_EQ(d.teardown, teardown_e::none) << k;
      ASSERT_EQ(d.deferred, teardown_e::none) << k;
    } else {
      ASSERT_EQ(d.teardown, teardown_e::stall) << k;
      EXPECT_EQ(s.now - first, kStall);
      break;
    }
  }
  EXPECT_EQ(s.h.teardowns(), 1u);
}

TEST(VrCaptureHealthStall, NeedsThirtyPublishedFrames) {
  // 完成值不動，但 driver 每 200 ms 才發布一張：3 s 只有 15 張，不算；滿 30 張（6 s）才算
  sim_t s;
  int64_t took = 0;
  const auto d = s.run_until_teardown(fence_e::stuck, 60 * kSec, took, 200 * kMs);
  EXPECT_EQ(d.teardown, teardown_e::stall);
  EXPECT_EQ(took, 30 * 200 * kMs);

  // 每 150 ms 一張：3 s 在第 21 張就到，要等到第 30 張
  sim_t t;
  int fired = -1;
  for (int k = 1; k <= 40 && fired < 0; ++k) {
    if (t.step(outcome_e::abandoned, fence_e::stuck, 150 * kMs).teardown != teardown_e::none) {
      fired = k;
    }
  }
  EXPECT_EQ(fired, 30);
}

TEST(VrCaptureHealthStall, SkippedFramesCountTowardsThirty) {
  // 被 latest-wins 跳過的影像也是 driver 發布的：每 200 ms 取用一張、每次 frame_id 前進 3，
  // 30 張在第 11 張就滿了，所以由 3 s 決定——第 16 張（3.0 s）
  sim_t s;
  int fired = -1;
  for (int k = 1; k <= 40 && fired < 0; ++k) {
    if (s.step(outcome_e::abandoned, fence_e::stuck, 200 * kMs, 3).teardown != teardown_e::none) {
      fired = k;
    }
  }
  EXPECT_EQ(fired, 16);
}

TEST(VrCaptureHealthStall, FramesPublishedBeforeTheStreakDoNotCount) {
  // server 這一側停了一下（編碼器回堵、一串過期的影像），回來時 frame_id 已經跳了 500；這一張完成值沒前進，
  // 計時從它開始。那 500 張是計時開始之前發布的，不能拿來湊 30 張：之後 3 s 內 driver 只發布了 1 張，不算卡死
  sim_t s;
  s.run(240, outcome_e::on_time);
  auto d = s.step(outcome_e::abandoned, fence_e::stuck, kT120, 500);
  ASSERT_EQ(d.teardown, teardown_e::none);
  s.idle(3 * kSec + 10 * kMs);
  d = s.step(outcome_e::abandoned, fence_e::stuck);
  EXPECT_EQ(d.teardown, teardown_e::none);
  EXPECT_EQ(d.deferred, teardown_e::none);
  EXPECT_EQ(s.h.teardowns(), 0u);
}

TEST(VrCaptureHealthStall, ClockStartsAtTheFirstFrameWithoutProgress) {
  // 正常一陣子 → driver 整整 60 s 沒有發布（沒有任何影像）→ 恢復發布時完成值卡住。
  // 計時要從恢復後的第一張起算：不能因為「距上一次前進已經 60 s」就在第 30 張（0.25 s）拆線。
  sim_t s;
  s.run(240, outcome_e::on_time);
  s.idle(60 * kSec);
  int64_t took = 0;
  const auto d = s.run_until_teardown(fence_e::stuck, 20 * kSec, took);
  EXPECT_EQ(d.teardown, teardown_e::stall);
  EXPECT_GE(took, kStall);
  EXPECT_LT(took, kStall + 3 * kT120);
}

TEST(VrCaptureHealthStall, AnAdvanceRestartsTheClock) {
  // 不動 1.5 s → 前進一次（仍然沒到值）→ 再不動 2.5 s：合計 4 s 沒有複製，但完成值沒有連續 3 s 不動
  sim_t s;
  s.run(120, outcome_e::on_time);
  for (int i = 0; i < 180; ++i) {
    ASSERT_EQ(s.step(outcome_e::abandoned, fence_e::stuck).teardown, teardown_e::none) << i;
  }
  ASSERT_EQ(s.step(outcome_e::abandoned, fence_e::lagging).teardown, teardown_e::none);
  for (int i = 0; i < 300; ++i) {
    const auto d = s.step(outcome_e::abandoned, fence_e::stuck);
    ASSERT_EQ(d.teardown, teardown_e::none) << i;
    ASSERT_EQ(d.deferred, teardown_e::none) << i;
  }
  EXPECT_EQ(s.h.teardowns(), 0u);
}

TEST(VrCaptureHealthStall, AnIdleGapInsideTheStreakStillCounts) {
  // 完成值不動 0.33 s（40 張）→ 10 s 沒有影像 → 恢復後第一張完成值還是沒動：立刻判卡死。
  // 空檔前排著的 40 個 Signal 過了 10 s 都沒做完，就是卡住（和餓死不同：餓死的空檔不算，見下面）
  sim_t s;
  s.run(240, outcome_e::on_time);
  for (int i = 0; i < 40; ++i) {
    ASSERT_EQ(s.step(outcome_e::abandoned, fence_e::stuck).teardown, teardown_e::none) << i;
  }
  s.idle(10 * kSec);
  EXPECT_EQ(s.step(outcome_e::abandoned, fence_e::stuck).teardown, teardown_e::stall);
}

// ── (b) 餓死 ─────────────────────────────────────────────────────────────

TEST(VrCaptureHealthStarve, FenceMovingButNeverCatchingUpOnlyStarves) {
  // 完成值每張都在增加，但永遠到不了 descriptor 寫的值（driver 的計數與實際 Signal 脫節）：
  // 卡死條件抓不到（3 s 時不拆），5 s 由餓死拆
  sim_t s;
  s.run(240, outcome_e::on_time);
  const int64_t t0 = s.now;
  decision_t d;
  while (s.now - t0 < 4 * kSec) {
    d = s.step(outcome_e::abandoned, fence_e::lagging);
    ASSERT_EQ(d.teardown, teardown_e::none) << "at " << (s.now - t0) / kMs << " ms";
    ASSERT_EQ(d.deferred, teardown_e::none);
  }
  int64_t took = 0;
  d = s.run_until_teardown(fence_e::lagging, 20 * kSec, took);
  EXPECT_EQ(d.teardown, teardown_e::starve);
  EXPECT_GE(s.now - t0, kStarve);
  EXPECT_LT(s.now - t0, kStarve + 3 * kT120);
}

TEST(VrCaptureHealthStarve, FiresOnTheFrameAtExactlyFiveSeconds) {
  sim_t s;
  int64_t first = 0;
  for (int k = 0; k <= 60; ++k) {
    const auto d = s.step(outcome_e::abandoned, fence_e::lagging, 100 * kMs);
    if (k == 0) {
      first = s.now;
    }
    if (k < 50) {
      ASSERT_EQ(d.teardown, teardown_e::none) << k;
      ASSERT_EQ(d.deferred, teardown_e::none) << k;
    } else {
      ASSERT_EQ(d.teardown, teardown_e::starve) << k;
      EXPECT_EQ(s.now - first, kStarve);
      break;
    }
  }
  EXPECT_EQ(s.h.teardowns(), 1u);
}

TEST(VrCaptureHealthStarve, NeedsThirtyPublishedFrames) {
  // 每 250 ms 一張、完成值有在前進：5 s 在第 21 張就到，要等到第 30 張
  sim_t s;
  int fired = -1;
  teardown_e why = teardown_e::none;
  for (int k = 1; k <= 40 && fired < 0; ++k) {
    const auto d = s.step(outcome_e::abandoned, fence_e::lagging, 250 * kMs);
    if (d.teardown != teardown_e::none) {
      fired = k;
      why = d.teardown;
    }
  }
  EXPECT_EQ(fired, 30);
  EXPECT_EQ(why, teardown_e::starve);

  // 被跳過的影像也算：每 200 ms 取用一張、每次前進 3 張，30 張早就滿了，由 5 s 決定——第 26 張
  sim_t t;
  fired = -1;
  for (int k = 1; k <= 40 && fired < 0; ++k) {
    const auto d = t.step(outcome_e::abandoned, fence_e::lagging, 200 * kMs, 3);
    if (d.teardown != teardown_e::none) {
      fired = k;
      why = d.teardown;
    }
  }
  EXPECT_EQ(fired, 26);
  EXPECT_EQ(why, teardown_e::starve);
}

TEST(VrCaptureHealthStarve, FramesPublishedBeforeTheStreakDoNotCount) {
  // 連續失敗的第一張之前 frame_id 跳了 500：那些不算進 30 張。之後每 250 ms 一張，5 s 在第 21 張就到，
  // 但要到這段期間自己發布滿 30 張（第 30 張）才拆
  sim_t s;
  s.run(240, outcome_e::on_time);
  ASSERT_EQ(s.step(outcome_e::abandoned, fence_e::lagging, kT120, 500).teardown, teardown_e::none);
  int fired = -1;
  for (int k = 2; k <= 40 && fired < 0; ++k) {
    if (s.step(outcome_e::abandoned, fence_e::lagging, 250 * kMs).teardown != teardown_e::none) {
      fired = k;
    }
  }
  EXPECT_EQ(fired, 30);
}

TEST(VrCaptureHealthStarve, ACopyRestartsTheClock) {
  // 很久以前單獨逾時過一張，之後一路正常；10 s 後一次 0.5 s 的卡頓（60 張都沒趕上）離「連續 5 s」還很遠
  sim_t s;
  s.step(outcome_e::abandoned, fence_e::lagging);
  for (int i = 0; i < 120 * 10; ++i) {
    ASSERT_EQ(s.step(outcome_e::on_time).teardown, teardown_e::none);
  }
  for (int i = 0; i < 60; ++i) {
    const auto d = s.step(outcome_e::abandoned, fence_e::lagging);
    ASSERT_EQ(d.teardown, teardown_e::none) << i;
    ASSERT_EQ(d.deferred, teardown_e::none) << i;
  }
  EXPECT_EQ(s.h.teardowns(), 0u);
}

TEST(VrCaptureHealthStarve, AnIdleGapInsideTheStreakIsNotCounted) {
  // 0.33 s 的失敗（40 張，完成值有在前進）＋ 10 s 沒有影像 ＋ 恢復後第一張又沒趕上：實際沒複製成功的只有 41 張、
  // 0.34 s。空檔裡沒有影像可以複製，不算進「連續 5 s 一張都沒有複製成功」
  sim_t s;
  s.run(240, outcome_e::on_time);
  for (int i = 0; i < 40; ++i) {
    ASSERT_EQ(s.step(outcome_e::abandoned, fence_e::lagging).teardown, teardown_e::none) << i;
  }
  s.idle(10 * kSec);
  auto d = s.step(outcome_e::abandoned, fence_e::lagging);
  EXPECT_EQ(d.teardown, teardown_e::none);
  EXPECT_EQ(d.deferred, teardown_e::none);
  EXPECT_EQ(s.h.teardowns(), 0u);

  // 空檔不會讓連續紀錄重來：恢復後繼續失敗，累計（0.33 s＋空檔算的 0.5 s＋之後的）滿 5 s 就拆
  const int64_t t0 = s.now;
  int64_t took = 0;
  d = s.run_until_teardown(fence_e::lagging, 20 * kSec, took);
  EXPECT_EQ(d.teardown, teardown_e::starve);
  EXPECT_GE(s.now - t0, 5 * kSec - 325 * kMs - 500 * kMs);
  EXPECT_LT(s.now - t0, 5 * kSec - 325 * kMs - 500 * kMs + 3 * kT120);
}

TEST(VrCaptureHealthStarve, AGapCountsAsHalfASecondAtMost) {
  // 相鄰兩張之間最多算 0.5 s。每次前進 5 張，30 張在第 7 張就滿了，所以由累計的時間決定：
  //   每 400 ms 一張（沒超過上限，照實算）：0.4 s × 13 ＝ 5.2 s → 第 14 張
  //   每 500 ms 一張：0.5 s × 10 ＝ 5.0 s → 第 11 張
  //   每 1 s 一張（超過的部分不算）：一樣是第 11 張，不是第 6 張
  const int64_t gaps[] = {400 * kMs, 500 * kMs, 1000 * kMs};
  const int expect[] = {14, 11, 11};
  for (int c = 0; c < 3; ++c) {
    sim_t s;
    int fired = -1;
    teardown_e why = teardown_e::none;
    for (int k = 1; k <= 40 && fired < 0; ++k) {
      const auto d = s.step(outcome_e::abandoned, fence_e::lagging, gaps[c], 5);
      if (d.teardown != teardown_e::none) {
        fired = k;
        why = d.teardown;
      }
    }
    EXPECT_EQ(fired, expect[c]) << "gap " << gaps[c] / kMs << " ms";
    EXPECT_EQ(why, teardown_e::starve);
  }
}

TEST(VrCaptureHealthStarve, CreepingFenceNeverStallsButStarves) {
  // 完成值每 2.9 s 才加 1（永遠到不了期望值）：每次前進都讓卡死的計時重來，5 s 由餓死接手
  sim_t s;
  s.run(120, outcome_e::on_time);
  const int64_t t0 = s.now;
  int64_t last_creep = s.now;
  decision_t d;
  for (;;) {
    s.now += kT120;
    ++s.frame_id;
    ++s.fence;
    if (s.now - last_creep >= 2900 * kMs) {
      ++s.completed;
      last_creep = s.now;
    }
    frame_t f;
    f.outcome = outcome_e::abandoned;
    f.now_ns = s.now;
    f.present_ns = s.now - kMs;
    f.frame_id = s.frame_id;
    f.observed = s.completed;
    d = s.h.on_frame(f);
    if (d.teardown != teardown_e::none) {
      break;
    }
    ASSERT_LT(s.now - t0, 20 * kSec);
  }
  EXPECT_EQ(d.teardown, teardown_e::starve);
  EXPECT_GE(s.now - t0, kStarve);
  EXPECT_LT(s.now - t0, kStarve + 3 * kT120);
}

// ── 不該拆線的情形 ───────────────────────────────────────────────────────

TEST(VrCaptureHealth, LateFramesNeverTearDown) {
  // 這次事故的情形：每一張都晚到，但都有到（複製成功）。再久也不拆線。
  sim_t s;
  for (int i = 0; i < 120 * 120; ++i) {
    const auto d = s.step(outcome_e::late, fence_e::reach, 10 * kMs, 1, 9 * kMs);
    ASSERT_EQ(d.teardown, teardown_e::none);
    ASSERT_EQ(d.deferred, teardown_e::none);
  }
  EXPECT_EQ(s.h.teardowns(), 0u);
  EXPECT_TRUE(s.h.overload());
}

TEST(VrCaptureHealth, CopiedFramesAreProgressEvenIfTheFenceValueStandsStill) {
  // driver 先把 fence Signal 到很後面的值：每一張都到值、都複製成功，但讀到的完成值連續 10 s 都一樣。
  // 複製成功就算前進，不能判成卡死。
  sim_t s;
  frame_t f;
  f.outcome = outcome_e::on_time;
  f.observed = 1'000'000;
  for (int i = 1; i <= 1200; ++i) {
    f.now_ns = s.now + i * kT120;
    f.present_ns = f.now_ns - kMs;
    f.frame_id = static_cast<uint64_t>(i);
    const auto d = s.h.on_frame(f);
    ASSERT_EQ(d.teardown, teardown_e::none) << i;
    ASSERT_EQ(d.deferred, teardown_e::none) << i;
  }
  EXPECT_EQ(s.h.diag(f.now_ns).since_advance_ns, 0);
  EXPECT_EQ(s.h.teardowns(), 0u);
}

TEST(VrCaptureHealth, NothingPublishedNeverTearsDown) {
  // 一張影像都沒有：只有 on_idle，什麼都不會發生
  sim_t idle;
  idle.idle(300 * kSec);
  EXPECT_EQ(idle.h.teardowns(), 0u);
  EXPECT_FALSE(idle.h.overload());

  // 逾時了幾張（不到 30 張）之後 driver 就不再發布：隔 5 分鐘再來一張逾時的，時間早就超過門檻，但張數不夠
  sim_t s;
  s.run(10, outcome_e::abandoned, fence_e::stuck);
  s.idle(300 * kSec);
  const auto d = s.step(outcome_e::abandoned, fence_e::stuck);
  EXPECT_EQ(d.teardown, teardown_e::none);
  EXPECT_EQ(s.h.teardowns(), 0u);
}

TEST(VrCaptureHealth, DisabledOnlyCounts) {
  // 開關關著：不判卡死／餓死（由擷取端的舊規則處理）、過載旗標恆為 false；log 用的數字照算
  sim_t s(false);
  for (int i = 0; i < 120 * 30; ++i) {
    const auto d = s.step(outcome_e::abandoned, fence_e::stuck);
    ASSERT_EQ(d.teardown, teardown_e::none);
    ASSERT_EQ(d.deferred, teardown_e::none);
    ASSERT_FALSE(s.h.overload());
  }
  EXPECT_FALSE(s.h.snapshot().overload);
  EXPECT_EQ(s.h.snapshot().late_permille, 1000);
  EXPECT_EQ(s.h.snapshot().pushed_fps, 0);
  const auto g = s.h.diag(s.now);
  EXPECT_EQ(g.published, 120u * 30u);
  EXPECT_EQ(g.copied, 0u);
  EXPECT_EQ(s.h.teardowns(), 0u);
  EXPECT_FALSE(s.h.backoff().armed);
}

// ── (c) 退避 ─────────────────────────────────────────────────────────────

TEST(VrCaptureHealthBackoff, LadderIsFiveFifteenThirtyThirty) {
  sim_t s;
  int64_t took = 0;
  auto d = s.run_until_teardown(fence_e::stuck, 20 * kSec, took);
  ASSERT_EQ(d.teardown, teardown_e::stall);
  EXPECT_EQ(d.allowed_in_ns, 5 * kSec);
  int64_t last_teardown = s.now;

  const int64_t expect_gap[] = {5 * kSec, 15 * kSec, 30 * kSec, 30 * kSec};
  const int64_t expect_next[] = {15 * kSec, 30 * kSec, 30 * kSec, 30 * kSec};
  for (int round = 0; round < 4; ++round) {
    s.new_generation();  // driver 0.5 s 後重連，一樣卡住
    bool saw_deferred = false;
    int64_t deferred_first = 0;
    for (;;) {
      d = s.step(outcome_e::abandoned, fence_e::stuck);
      if (d.deferred != teardown_e::none && !saw_deferred) {
        saw_deferred = true;
        deferred_first = d.allowed_in_ns;
        EXPECT_EQ(d.deferred, teardown_e::stall);
      }
      if (d.teardown != teardown_e::none) {
        break;
      }
      ASSERT_LT(s.now - last_teardown, 120 * kSec) << "round " << round;
    }
    // 卡死的判定在新 generation 3 s 後就成立，但要等到退避期滿才拆
    EXPECT_TRUE(saw_deferred) << "round " << round;
    EXPECT_GT(deferred_first, 0);
    EXPECT_GE(s.now - last_teardown, expect_gap[round]) << "round " << round;
    EXPECT_LT(s.now - last_teardown, expect_gap[round] + 3 * kT120) << "round " << round;
    EXPECT_EQ(d.allowed_in_ns, expect_next[round]) << "round " << round;
    last_teardown = s.now;
  }
  EXPECT_EQ(s.h.teardowns(), 5u);
  EXPECT_EQ(s.h.backoff().step, 3u);  // 級數封頂在階梯的長度（log 的 backoffStep 與存出去的備份都是這個數字）
}

TEST(VrCaptureHealthBackoff, AllowsTheFrameAtExactlyTheDeadline) {
  // 100 ms 一張：第一次拆線在 T；0.5 s 後重連又卡住，3 s 後判定成立、一路 deferred，剛好 T+5.000 s 的那一張拆
  sim_t s;
  int64_t took = 0;
  ASSERT_EQ(s.run_until_teardown(fence_e::stuck, 20 * kSec, took, 100 * kMs).teardown, teardown_e::stall);
  const int64_t torn = s.now;
  ASSERT_EQ(s.h.backoff().next_allowed_ns, torn + 5 * kSec);
  s.new_generation(500 * kMs);
  int64_t prev_allowed_in = kI64Max;
  int deferred = 0;
  decision_t d;
  for (;;) {
    d = s.step(outcome_e::abandoned, fence_e::stuck, 100 * kMs);
    if (d.deferred != teardown_e::none) {
      ++deferred;
      EXPECT_EQ(d.allowed_in_ns, torn + 5 * kSec - s.now);  // 倒數正確
      EXPECT_LT(d.allowed_in_ns, prev_allowed_in);
      EXPECT_GT(d.allowed_in_ns, 0);
      prev_allowed_in = d.allowed_in_ns;
    }
    if (d.teardown != teardown_e::none) {
      break;
    }
    ASSERT_LT(s.now - torn, 20 * kSec);
  }
  EXPECT_EQ(s.now - torn, 5 * kSec);
  EXPECT_GT(deferred, 0);
  EXPECT_EQ(d.allowed_in_ns, 15 * kSec);
}

TEST(VrCaptureHealthBackoff, TenHealthySecondsResetTheLadder) {
  sim_t s;
  int64_t took = 0;
  ASSERT_EQ(s.run_until_teardown(fence_e::stuck, 20 * kSec, took).teardown, teardown_e::stall);
  s.new_generation();
  ASSERT_EQ(s.run_until_teardown(fence_e::stuck, 20 * kSec, took).teardown, teardown_e::stall);
  EXPECT_EQ(s.h.backoff().step, 2u);  // 下一次要等 30 s

  // 重連後好了：每一張都準時。開頭不滿一秒的那一格不算，之後 10 個完整的一秒格都健康才歸零（回報一次）：10～11 s
  s.new_generation();
  int resets = 0;
  int64_t reset_at = 0;
  const int64_t t0 = s.now;
  while (s.now - t0 < 13 * kSec) {
    if (s.step(outcome_e::on_time).backoff_reset) {
      ++resets;
      reset_at = s.now - t0;
    }
  }
  EXPECT_EQ(resets, 1);
  EXPECT_GE(reset_at, 10 * kSec);
  EXPECT_LE(reset_at, 11 * kSec + kT120);
  EXPECT_EQ(s.h.backoff(), backoff_t {});

  // 之後又卡住：和第一次一樣，3 s 就拆、退避從 5 s 開始
  const auto d = s.run_until_teardown(fence_e::stuck, 20 * kSec, took);
  EXPECT_EQ(d.teardown, teardown_e::stall);
  EXPECT_LT(took, kStall + 3 * kT120);
  EXPECT_EQ(d.allowed_in_ns, 5 * kSec);
}

TEST(VrCaptureHealthBackoff, TenHealthySecondsAreAtLeastTenSeconds) {
  // 健康格對齊整秒。第一張健康的影像落在某一秒的最後 10 ms：那一格只有 10 ms，不能算一秒——
  // 歸零要等到之後 10 個完整的格子都過了（10.01 s），不是 9.01 s
  {
    sim_t s;
    s.tear_down_once_and_reconnect();
    ASSERT_EQ(s.h.backoff().step, 1u);
    s.align_next_frame(990 * kMs);
    int64_t first = 0;
    int64_t reset_after = -1;
    for (int i = 0; i < 120 * 13 && reset_after < 0; ++i) {
      const auto d = s.step(outcome_e::on_time);
      if (i == 0) {
        first = s.now;
        ASSERT_EQ(first % kSec, 990 * kMs);
      }
      if (d.backoff_reset) {
        reset_after = s.now - first;
      }
    }
    EXPECT_GE(reset_after, 10 * kSec);
    EXPECT_LE(reset_after, 10 * kSec + 10 * kMs + kT120);
  }
  // 第一張落在某一秒的 0.4 s 處：那一格（0.6 s）不算，再 10 個完整的格子——10.6 s
  {
    sim_t s;
    s.tear_down_once_and_reconnect();
    s.align_next_frame(400 * kMs);
    int64_t first = 0;
    int64_t reset_after = -1;
    for (int i = 0; i < 120 * 13 && reset_after < 0; ++i) {
      const auto d = s.step(outcome_e::on_time);
      if (i == 0) {
        first = s.now;
      }
      if (d.backoff_reset) {
        reset_after = s.now - first;
      }
    }
    EXPECT_GE(reset_after, 10 * kSec + 600 * kMs);
    EXPECT_LE(reset_after, 10 * kSec + 600 * kMs + kT120);
  }
}

TEST(VrCaptureHealthBackoff, EightHealthySecondsAreNotEnough) {
  sim_t s;
  int64_t took = 0;
  ASSERT_EQ(s.run_until_teardown(fence_e::stuck, 20 * kSec, took).teardown, teardown_e::stall);
  const int64_t torn_at = s.now;
  s.new_generation();
  s.run(120 * 2, outcome_e::on_time);  // 只好了 2 s
  EXPECT_EQ(s.h.backoff().step, 1u);
  const auto d = s.run_until_teardown(fence_e::stuck, 60 * kSec, took);
  EXPECT_EQ(d.teardown, teardown_e::stall);
  EXPECT_GE(s.now - torn_at, 5 * kSec);  // 仍受第一級退避限制
  EXPECT_EQ(d.allowed_in_ns, 15 * kSec);  // 階梯沒有歸零：這一次之後是 15 s

  // 8 s 也不夠（之後卡住的那幾秒不健康，連續紀錄歸零）
  s.new_generation();
  for (int i = 0; i < 120 * 8; ++i) {
    ASSERT_FALSE(s.step(outcome_e::on_time).backoff_reset);
  }
  EXPECT_EQ(s.h.backoff().step, 2u);
}

TEST(VrCaptureHealthBackoff, AGapWithoutFramesBreaksTheHealthyStreak) {
  // 好了 6 s → 3 s 完全沒有影像（driver 不發布）→ 又好了 6 s：合計 12 s 但不連續，階梯不歸零；
  // 從空檔之後算起連續滿 10 s 才歸零
  sim_t s;
  s.tear_down_once_and_reconnect();
  for (int i = 0; i < 120 * 6; ++i) {
    ASSERT_FALSE(s.step(outcome_e::on_time).backoff_reset);
  }
  s.idle(3 * kSec);
  const int64_t t0 = s.now;
  for (int i = 0; i < 120 * 6; ++i) {
    ASSERT_FALSE(s.step(outcome_e::on_time).backoff_reset) << i;
  }
  EXPECT_EQ(s.h.backoff().step, 1u);
  bool reset = false;
  while (!reset && s.now - t0 < 13 * kSec) {
    reset = s.step(outcome_e::on_time).backoff_reset;
  }
  EXPECT_TRUE(reset);
  EXPECT_GE(s.now - t0, 10 * kSec);
  EXPECT_LE(s.now - t0, 11 * kSec + kT120);
}

TEST(VrCaptureHealthBackoff, HalfTheFramesCopiedStillCountsAsHealthy) {
  // 等得比較久、每次跳過一張（發布 2 張、複製 1 張）：剛好一半，算健康
  sim_t half;
  half.tear_down_once_and_reconnect();
  bool reset = false;
  for (int i = 0; i < 60 * 12; ++i) {
    reset = half.step(outcome_e::late, fence_e::reach, 2 * kT120, 2, 12 * kMs).backoff_reset || reset;
  }
  EXPECT_TRUE(reset);
  EXPECT_EQ(half.h.backoff().step, 0u);

  // 每發布 3 張只複製 1 張：不到一半，不算健康，階梯留著
  sim_t third;
  third.tear_down_once_and_reconnect();
  for (int i = 0; i < 40 * 12; ++i) {
    ASSERT_FALSE(third.step(outcome_e::late, fence_e::reach, 3 * kT120, 3, 20 * kMs).backoff_reset);
  }
  EXPECT_EQ(third.h.backoff().step, 1u);
}

TEST(VrCaptureHealthBackoff, HealthyStreakDoesNotSpanGenerations) {
  // 好了 6 s → driver 自己重連（中間沒有整秒的空檔）→ 又好了 6 s：連續紀錄不跨 generation
  sim_t s;
  s.tear_down_once_and_reconnect();
  for (int i = 0; i < 120 * 6; ++i) {
    ASSERT_FALSE(s.step(outcome_e::on_time).backoff_reset);
  }
  s.new_generation(50 * kMs);
  for (int i = 0; i < 120 * 6; ++i) {
    ASSERT_FALSE(s.step(outcome_e::on_time).backoff_reset) << i;
  }
  EXPECT_EQ(s.h.backoff().step, 1u);
}

TEST(VrCaptureHealthBackoff, ResetCanArriveOnAnAbandonedFrame) {
  // 健康＝每秒複製成功 ≥ 發布的一半，所以一半放棄、一半複製的串流也會歸零；一秒格在「下一格的第一張」結算，
  // 那一張可能是放棄的。呼叫端兩條路徑（複製成功、放棄）都要看 backoff_reset（display_vr.cpp 的 feed）。
  int on_abandoned = 0;
  int on_copied = 0;
  for (int phase = 0; phase < 40; ++phase) {
    sim_t s(true, kT120, kMax100, kStart + phase * 3 * kMs);  // 讓跨秒的那一張輪流落在放棄與複製上
    s.tear_down_once_and_reconnect();
    for (int i = 0; i < 120 * 13; ++i) {
      const outcome_e o = i % 2 ? outcome_e::late : outcome_e::abandoned;
      const auto d = s.step(o, fence_e::lagging);
      ASSERT_EQ(d.teardown, teardown_e::none);
      ASSERT_EQ(d.deferred, teardown_e::none);
      if (d.backoff_reset) {
        (o == outcome_e::abandoned ? on_abandoned : on_copied) += 1;
        break;
      }
    }
  }
  EXPECT_EQ(on_abandoned + on_copied, 40);
  EXPECT_GT(on_abandoned, 0);
  EXPECT_GT(on_copied, 0);
}

TEST(VrCaptureHealthBackoff, DeferredVerdictIsDroppedOnRecovery) {
  // 退避期內判定成立（deferred）之後恢復：判定要消掉，期滿時不會補拆
  sim_t s;
  int64_t took = 0;
  ASSERT_EQ(s.run_until_teardown(fence_e::stuck, 20 * kSec, took).teardown, teardown_e::stall);
  const int64_t torn = s.now;
  s.new_generation();
  bool deferred = false;
  decision_t d;
  while (s.now - torn < 4 * kSec) {
    d = s.step(outcome_e::abandoned, fence_e::stuck);
    ASSERT_EQ(d.teardown, teardown_e::none);
    deferred = deferred || d.deferred != teardown_e::none;
  }
  ASSERT_TRUE(deferred);
  // 恢復了：之後一路正常，跨過退避期限（T+5 s）也不拆
  while (s.now - torn < 8 * kSec) {
    d = s.step(outcome_e::on_time);
    ASSERT_EQ(d.teardown, teardown_e::none);
    ASSERT_EQ(d.deferred, teardown_e::none);
  }
  EXPECT_EQ(s.h.teardowns(), 1u);
}

TEST(VrCaptureHealthBackoff, SurvivesGenerationChangeAndRestore) {
  sim_t s;
  int64_t took = 0;
  ASSERT_EQ(s.run_until_teardown(fence_e::stuck, 20 * kSec, took).teardown, teardown_e::stall);
  const auto saved = s.h.backoff();
  EXPECT_EQ(saved.step, 1u);
  EXPECT_TRUE(saved.armed);
  EXPECT_EQ(saved.next_allowed_ns, s.now + 5 * kSec);
  s.new_generation();
  EXPECT_EQ(s.h.backoff(), saved);  // 換 generation 不動退避

  // display 物件被重建（capture_e::reinit）：新的物件把退避接回來，階梯與期限都延續
  sim_t fresh;
  fresh.now = s.now;
  fresh.h.restore_backoff(saved);
  fresh.h.begin_generation(fresh.now);
  const auto d = fresh.run_until_teardown(fence_e::stuck, 60 * kSec, took);
  EXPECT_EQ(d.teardown, teardown_e::stall);
  EXPECT_GE(fresh.now, saved.next_allowed_ns);
  EXPECT_EQ(d.allowed_in_ns, 15 * kSec);

  // 還原進來的階梯超出範圍也只會夾住；還原空的＝從頭開始
  health_t h;
  h.restore_backoff(backoff_t {1000, true, 0});
  EXPECT_EQ(h.backoff().step, 3u);
  h.restore_backoff(backoff_t {});
  EXPECT_EQ(h.backoff(), backoff_t {});
}

TEST(VrCaptureHealthBackoff, TeardownIsNotRepeatedOnTheNextFrame) {
  // 呼叫端回報拆線後就會放掉這個 generation；萬一多餵幾張，判定已經歸零：不會馬上再回報一次，
  // 也不會回報「成立但在退避期」（要重新累積 3 s）
  sim_t s;
  int64_t took = 0;
  ASSERT_EQ(s.run_until_teardown(fence_e::stuck, 20 * kSec, took).teardown, teardown_e::stall);
  for (int i = 0; i < 120; ++i) {
    const auto d = s.step(outcome_e::abandoned, fence_e::stuck);
    ASSERT_EQ(d.teardown, teardown_e::none) << i;
    ASSERT_EQ(d.deferred, teardown_e::none) << i;
  }
  EXPECT_EQ(s.h.teardowns(), 1u);
}

// ── (d) 過載旗標 ─────────────────────────────────────────────────────────

TEST(VrCaptureHealthOverload, SetsAtThirtyPercentAndClearsAfterTwoQuietSeconds) {
  sim_t s;
  s.run(240, outcome_e::on_time);
  EXPECT_FALSE(s.h.overload());

  // 每 3 張有 1 張晚到（33%）：1 s 內設起來
  const int64_t t_on = s.now;
  while (!s.h.overload() && s.now - t_on < 2 * kSec) {
    s.step(outcome_e::on_time);
    s.step(outcome_e::on_time);
    s.step(outcome_e::late, fence_e::reach, kT120, 1, 7 * kMs);
  }
  ASSERT_TRUE(s.h.overload());
  EXPECT_LE(s.now - t_on, kSec);
  EXPECT_TRUE(s.h.snapshot().overload);
  EXPECT_GE(s.h.snapshot().late_permille, 300);
  EXPECT_LE(s.h.snapshot().late_permille, 400);
  EXPECT_EQ(s.h.window_wait_max_ns(), 7 * kMs);

  // 之後全部準時：最後一張晚到的要先離開 1 s 的視窗（比例才降到 5% 以下），再持續 2 s 才清
  const int64_t t_last_bad = s.now;
  int64_t cleared_after = 0;
  while (s.now - t_last_bad < 5 * kSec) {
    s.step(outcome_e::on_time);
    if (!s.h.overload()) {
      cleared_after = s.now - t_last_bad;
      break;
    }
  }
  EXPECT_FALSE(s.h.overload());
  EXPECT_GT(cleared_after, 2 * kSec + 500 * kMs);
  EXPECT_LT(cleared_after, 3 * kSec + 200 * kMs);
}

TEST(VrCaptureHealthOverload, SetsAtExactlyThirtyPercent) {
  // 每秒 10 張（每格 1 張），每 10 張裡 3 張晚到＝剛好 30%：要設
  sim_t at;
  bool set = false;
  for (int i = 0; i < 200 && !set; ++i) {
    at.step(i % 10 < 3 ? outcome_e::late : outcome_e::on_time, fence_e::reach, 100 * kMs);
    set = at.h.overload();
  }
  EXPECT_TRUE(set);
  // 每 10 張裡 2 張（20%）：永遠不設
  sim_t below;
  for (int i = 0; i < 200; ++i) {
    below.step(i % 10 < 2 ? outcome_e::late : outcome_e::on_time, fence_e::reach, 100 * kMs);
    ASSERT_FALSE(below.h.overload()) << i;
  }
}

TEST(VrCaptureHealthOverload, ClearsBelowFivePercentButNotAtFive) {
  // 每 5 ms 一張（每格 20 張、視窗 181～200 張）。平均剛好 5%（每 20 張 1 張）：視窗裡的張數隨格子變動，比例在 5%
  // 上下跳，一跳回 5% 以上就重新計時，清不掉（所以規格的「5% 以下」含不含 5% 實際上分不出來）
  sim_t eq;
  eq.run(400, outcome_e::late, fence_e::reach, 5 * kMs);
  ASSERT_TRUE(eq.h.overload());
  for (int i = 0; i < 200 * 20; ++i) {
    eq.step(i % 20 == 0 ? outcome_e::late : outcome_e::on_time, fence_e::reach, 5 * kMs);
  }
  EXPECT_TRUE(eq.h.overload()) << "平均 5% 持續 20 s";
  // 每 25 張 1 張＝4%：視窗先花約 1 s 把之前晚到的排掉，再維持 2 s 就清
  sim_t lt;
  lt.run(400, outcome_e::late, fence_e::reach, 5 * kMs);
  ASSERT_TRUE(lt.h.overload());
  const int64_t t0 = lt.now;
  int64_t cleared = -1;
  for (int i = 0; i < 200 * 10 && cleared < 0; ++i) {
    lt.step(i % 25 == 24 ? outcome_e::late : outcome_e::on_time, fence_e::reach, 5 * kMs);
    if (!lt.h.overload()) {
      cleared = lt.now - t0;
    }
  }
  EXPECT_GT(cleared, 2 * kSec);
  EXPECT_LT(cleared, 3200 * kMs);
}

TEST(VrCaptureHealthOverload, HoldTimerRestartsWhenTheRatioComesBack) {
  sim_t s;
  s.run(240, outcome_e::late);
  ASSERT_TRUE(s.h.overload());
  // 安靜 2.0 s（視窗約 0.9 s 排空＋低於 5% 約 1.1 s，還沒滿 2 s）
  s.run(240, outcome_e::on_time);
  ASSERT_TRUE(s.h.overload());
  // 回到 10%（1.5 s）：不到 30%，但約 0.45 s 後視窗裡的比例就高於 5% → 計時作廢
  for (int i = 0; i < 180; ++i) {
    s.step(i % 10 == 0 ? outcome_e::late : outcome_e::on_time);
  }
  ASSERT_TRUE(s.h.overload());
  // 之後全部準時：要重新等「視窗排空約 1 s＋2 s」，不能因為之前已經安靜過就馬上清
  const int64_t t0 = s.now;
  int64_t cleared = -1;
  for (int i = 0; i < 120 * 6 && cleared < 0; ++i) {
    s.step(outcome_e::on_time);
    if (!s.h.overload()) {
      cleared = s.now - t0;
    }
  }
  EXPECT_GT(cleared, 2500 * kMs);
  EXPECT_LT(cleared, 3200 * kMs);
}

TEST(VrCaptureHealthOverload, HysteresisBetweenFiveAndThirtyPercent) {
  // 10% 晚到：沒設的不會設；已經設的不會清
  sim_t off;
  for (int i = 0; i < 120 * 10; ++i) {
    off.step(i % 10 == 0 ? outcome_e::late : outcome_e::on_time);
    ASSERT_FALSE(off.h.overload());
  }

  sim_t on;
  on.run(150, outcome_e::abandoned, fence_e::lagging);  // 放棄的也算
  ASSERT_TRUE(on.h.overload());
  for (int i = 0; i < 120 * 10; ++i) {
    on.step(i % 10 == 0 ? outcome_e::late : outcome_e::on_time);
    ASSERT_TRUE(on.h.overload()) << i;
  }
  // 降到 4% 以下（每 30 張 1 張）維持夠久才清
  for (int i = 0; i < 120 * 4; ++i) {
    on.step(i % 30 == 29 ? outcome_e::late : outcome_e::on_time);
  }
  EXPECT_FALSE(on.h.overload());
}

TEST(VrCaptureHealthOverload, AbandonedFramesCountToo) {
  // 一半放棄、一半準時：放棄的算進比例（只算晚到的話是 0%）
  sim_t s;
  s.run(240, outcome_e::on_time);
  for (int i = 0; i < 150; ++i) {
    s.step(i % 2 ? outcome_e::abandoned : outcome_e::on_time, fence_e::lagging);
  }
  EXPECT_TRUE(s.h.overload());
  EXPECT_GE(s.h.snapshot().late_permille, 450);
  EXPECT_LE(s.h.snapshot().late_permille, 550);
}

TEST(VrCaptureHealthOverload, NeedsAFullWindowOfFrames) {
  // generation 剛開：就算每一張都晚到，也要看滿 1 s 的影像才設（視窗裡只有幾張時比例沒有意義）
  sim_t s;
  const int64_t t0 = s.now;
  while (s.now - t0 < 950 * kMs) {
    s.step(outcome_e::late);
    ASSERT_FALSE(s.h.overload()) << (s.now - t0) / kMs << " ms";
  }
  s.run(12, outcome_e::late);
  EXPECT_TRUE(s.h.overload());

  // 每 4 張晚到 1 張（25%）：頭 9 張裡有 3 張（33%），但那時視窗還沒滿；滿了之後一直是 25%，整段都不設
  sim_t quarter;
  for (int i = 0; i < 120 * 5; ++i) {
    quarter.step(i % 4 == 0 ? outcome_e::late : outcome_e::on_time);
    ASSERT_FALSE(quarter.h.overload()) << i;
  }

  // 新的 generation 也要重新看滿（上一個 generation 的影像不算）
  sim_t gen;
  gen.run(240, outcome_e::on_time);
  gen.new_generation(100 * kMs);
  const int64_t t1 = gen.now;
  while (gen.now - t1 < 950 * kMs) {
    gen.step(outcome_e::late, fence_e::reach, kT120, 1, 7 * kMs);
    ASSERT_FALSE(gen.h.overload()) << (gen.now - t1) / kMs << " ms";
  }
  gen.run(12, outcome_e::late, fence_e::reach, kT120, 1, 7 * kMs);
  EXPECT_TRUE(gen.h.overload());
}

TEST(VrCaptureHealthOverload, AfterAnIdleGapNeedsAFullWindowAgain) {
  // 同一個 generation 裡 driver 停了一陣子沒有發布（遊戲載入、卡頓），恢復後的頭幾張本來就容易晚。
  // 視窗空了（整整 1 s 沒有影像）之後要重新看滿 1 s 才判——不論空檔是 on_idle 每 100 ms 一格一格走過去的
  // （正式的擷取迴圈），還是一次跳過去的
  for (const int64_t gap : {1500 * kMs, 3 * kSec}) {
    for (const bool ticks : {true, false}) {
      sim_t s;
      s.run(240, outcome_e::on_time);
      if (ticks) {
        s.idle(gap);
      } else {
        s.now += gap;
        s.h.on_idle(s.now);
      }
      ASSERT_FALSE(s.h.overload());
      EXPECT_EQ(s.h.snapshot().pushed_fps, 0);  // 視窗確實空了
      const int64_t t1 = s.now;
      while (s.now - t1 < 950 * kMs) {
        s.step(outcome_e::late, fence_e::reach, kT120, 1, 7 * kMs);
        ASSERT_FALSE(s.h.overload()) << "gap " << gap / kMs << " ms ticks=" << ticks << ": set " << (s.now - t1) / kMs << " ms after frames resumed";
      }
      s.run(12, outcome_e::late, fence_e::reach, kT120, 1, 7 * kMs);
      EXPECT_TRUE(s.h.overload());
    }
  }

  // 空檔之後每 4 張晚 1 張（25%，低於 30%）：不能因為頭 9 張裡有 3 張就設起來——設了之後 25% 高於 5%，永遠清不掉
  sim_t quarter;
  quarter.run(240, outcome_e::on_time);
  quarter.idle(3 * kSec);
  for (int i = 0; i < 120 * 5; ++i) {
    quarter.step(i % 4 == 0 ? outcome_e::late : outcome_e::on_time);
    ASSERT_FALSE(quarter.h.overload()) << i;
  }
}

TEST(VrCaptureHealthOverload, LowFrameRateStillTrips) {
  // 遊戲只有每秒 5 幀、每一張都晚到：視窗裡只有 5 張，照樣算過載
  sim_t slow;
  slow.run(15, outcome_e::late, fence_e::reach, 200 * kMs, 1, 90 * kMs);
  EXPECT_TRUE(slow.h.overload());
  // 每秒 5 幀、其中 1 張晚到（20%）：不算
  sim_t one;
  for (int i = 0; i < 50; ++i) {
    one.step(i % 5 == 0 ? outcome_e::late : outcome_e::on_time, fence_e::reach, 200 * kMs);
    ASSERT_FALSE(one.h.overload());
  }
  // 每秒只有 2 幀、都晚到：視窗裡不到 3 張，不設（張數太少，分不出是不是單獨的卡頓；這個幀率 VR 也已經不能用）
  sim_t two;
  for (int i = 0; i < 20; ++i) {
    two.step(outcome_e::late, fence_e::reach, 500 * kMs, 1, 90 * kMs);
    ASSERT_FALSE(two.h.overload());
  }
  // 每 400 ms 一張、都晚到：視窗裡剛好 3 張，夠了
  sim_t three;
  three.run(12, outcome_e::late, fence_e::reach, 400 * kMs, 1, 90 * kMs);
  EXPECT_TRUE(three.h.overload());
}

TEST(VrCaptureHealthOverload, IdleTicksClearIt) {
  // 過載中影像整個停了：沒有 on_frame，靠 on_idle 讓視窗往前走——最後一張離開視窗（0.9～1.0 s）之後再維持 2 s 才清
  sim_t s;
  s.run(150, outcome_e::late);
  ASSERT_TRUE(s.h.overload());
  const int64_t t0 = s.now;
  int64_t cleared_after = 0;
  while (s.now - t0 < 10 * kSec && cleared_after == 0) {
    s.now += 100 * kMs;
    s.h.on_idle(s.now);
    if (!s.h.overload()) {
      cleared_after = s.now - t0;
    }
  }
  EXPECT_GE(cleared_after, 2900 * kMs);
  EXPECT_LE(cleared_after, 3100 * kMs);
  EXPECT_EQ(s.h.snapshot().pushed_fps, 0);
  EXPECT_EQ(s.h.snapshot().late_permille, 0);
}

TEST(VrCaptureHealthOverload, DisablingClearsItImmediately) {
  sim_t s;
  s.run(200, outcome_e::late);
  ASSERT_TRUE(s.h.overload());
  s.h.configure(false, kT120, kMax100);
  EXPECT_FALSE(s.h.overload());
  EXPECT_FALSE(s.h.snapshot().overload);
  s.step(outcome_e::late);
  EXPECT_FALSE(s.h.overload());
  s.h.on_idle(s.now + kMs);
  EXPECT_FALSE(s.h.overload());
}

// ── generation 更換、時鐘、快照 ──────────────────────────────────────────

TEST(VrCaptureHealth, NewGenerationResetsEverythingButBackoff) {
  sim_t s;
  s.tear_down_once_and_reconnect();
  // 新 generation：慢的遊戲把預算拉長、過載設起來、再卡住 2.9 s（差一點就判卡死）
  s.run(200, outcome_e::late, fence_e::reach, 4 * kT120, 1, 20 * kMs);
  EXPECT_GT(s.h.wait_budget_ns(), 3 * kT120);
  EXPECT_TRUE(s.h.overload());
  const int64_t t0 = s.now;
  while (s.now - t0 < 2900 * kMs) {
    ASSERT_EQ(s.step(outcome_e::abandoned, fence_e::stuck).deferred, teardown_e::none);
  }
  const auto backoff_before = s.h.backoff();

  s.new_generation();
  EXPECT_EQ(s.h.wait_budget_ns(), 3 * kT120);  // 發布間隔重來
  EXPECT_EQ(s.h.interval_ema_ns(), 0);
  EXPECT_FALSE(s.h.overload());  // 過載重來
  EXPECT_EQ(s.h.snapshot(), snapshot_t {});
  const auto g = s.h.diag(s.now);
  EXPECT_EQ(g.published, 0u);
  EXPECT_EQ(g.copied, 0u);
  EXPECT_EQ(g.completed, 0u);
  EXPECT_EQ(g.since_copy_ns, 0);
  EXPECT_EQ(s.h.backoff(), backoff_before);  // 退避不動

  // 新 generation 的第一張沒有上一張可以比，不取發布間隔的樣本；第二張的樣本原樣當起點，不和上一個 generation 的平均混
  s.step(outcome_e::late, fence_e::reach, 2 * kT120, 1, 12 * kMs);
  EXPECT_EQ(s.h.interval_ema_ns(), 0);
  EXPECT_EQ(s.h.wait_budget_ns(), 3 * kT120);
  s.step(outcome_e::late, fence_e::reach, 2 * kT120, 1, 12 * kMs);
  EXPECT_EQ(s.h.interval_ema_ns(), 2 * kT120);
  EXPECT_EQ(s.h.diag(s.now).published, 2u);

  // 卡死的計時也重來：新 generation 的完成值從別的數字開始（比舊的小），卡住要重新滿 3 s 才成立
  const int64_t t1 = s.now;
  decision_t d;
  for (;;) {
    d = s.step(outcome_e::abandoned, fence_e::stuck);
    if (d.deferred != teardown_e::none || d.teardown != teardown_e::none) {
      break;
    }
    ASSERT_LT(s.now - t1, 10 * kSec);
  }
  EXPECT_GE(s.now - t1, kStall);
}

TEST(VrCaptureHealth, NewGenerationRestartsStallAndStarveClocks) {
  // 舊 generation 卡住 2.9 s（還沒判定）就被 driver 自己換掉（不是我們拆的，所以沒有退避）。新 generation 又卡住：
  // 要重新滿 3 s，而且新 generation 第一次讀到的完成值只當基準——剛好 3.000 s 的那一張拆，不會晚一張
  sim_t s;
  for (int i = 0; i < 29; ++i) {
    ASSERT_EQ(s.step(outcome_e::abandoned, fence_e::stuck, 100 * kMs).teardown, teardown_e::none) << i;
  }
  s.new_generation(100 * kMs);
  int64_t first = 0;
  decision_t d;
  for (int k = 0;; ++k) {
    d = s.step(outcome_e::abandoned, fence_e::stuck, 100 * kMs);
    if (k == 0) {
      first = s.now;
    }
    if (d.teardown != teardown_e::none) {
      break;
    }
    ASSERT_LT(k, 100);
  }
  EXPECT_EQ(d.teardown, teardown_e::stall);
  EXPECT_EQ(s.now - first, kStall);

  // 餓死同理：舊 generation 4.9 s 沒有複製（完成值有在動），新 generation 要重新累計 5 s
  sim_t v;
  for (int i = 0; i < 590; ++i) {
    ASSERT_EQ(v.step(outcome_e::abandoned, fence_e::lagging).teardown, teardown_e::none) << i;
  }
  v.new_generation(100 * kMs);
  const int64_t t1 = v.now;
  int64_t took = 0;
  d = v.run_until_teardown(fence_e::lagging, 20 * kSec, took);
  EXPECT_EQ(d.teardown, teardown_e::starve);
  EXPECT_GE(v.now - t1, kStarve);
  EXPECT_LT(v.now - t1, kStarve + 3 * kT120);
}

TEST(VrCaptureHealth, ClockGoingBackwardsOrStandingStillIsHarmless) {
  sim_t s;
  s.run(240, outcome_e::on_time);

  // 同一個時刻重複呼叫（present 也相同）：不會除以零，快照的數字在範圍內
  frame_t f;
  f.now_ns = s.now;
  f.present_ns = s.now;
  f.frame_id = s.frame_id;
  f.observed = s.completed;
  for (int i = 0; i < 2000; ++i) {
    f.outcome = i % 2 ? outcome_e::late : outcome_e::abandoned;
    const auto d = s.h.on_frame(f);
    ASSERT_EQ(d.teardown, teardown_e::none);  // 時間沒有前進：任何門檻都不會到
  }
  EXPECT_LE(s.h.snapshot().late_permille, 1000);
  EXPECT_GE(s.h.wait_budget_ns(), 3 * kT120);
  EXPECT_LE(s.h.wait_budget_ns(), kMax100);

  // 時間倒退（每張比上一張早 1 s）：經過時間都當成 0，不會算出負的、也不會因此拆線
  for (int i = 0; i < 2000; ++i) {
    f.now_ns -= kSec;
    f.present_ns -= kSec;
    f.outcome = outcome_e::abandoned;
    const auto d = s.h.on_frame(f);
    ASSERT_EQ(d.teardown, teardown_e::none);
    const auto g = s.h.diag(f.now_ns);
    ASSERT_GE(g.since_advance_ns, 0);
    ASSERT_GE(g.since_copy_ns, 0);
  }
  s.h.on_idle(f.now_ns);

  // 時間戳偶爾往回跳（每隔一張早 10 s）：那一張當成和上一張同一個時刻，不會把「距上一張多久」灌大——
  // 完成值追不上的串流仍然要實際滿 5 s 才判餓死，不是餵幾十張就判
  sim_t glitchy;
  glitchy.run(240, outcome_e::on_time);
  const int64_t t0 = glitchy.now;
  teardown_e why = teardown_e::none;
  int64_t fired_after = -1;
  for (int i = 0; i < 2000 && fired_after < 0; ++i) {
    glitchy.now += kT120;
    ++glitchy.frame_id;
    ++glitchy.fence;
    glitchy.completed = glitchy.fence - 3;
    frame_t x;
    x.outcome = outcome_e::abandoned;
    x.now_ns = (i % 2) ? glitchy.now - 10 * kSec : glitchy.now;
    x.present_ns = glitchy.now - kMs;
    x.frame_id = glitchy.frame_id;
    x.observed = glitchy.completed;
    const auto d = glitchy.h.on_frame(x);
    if (d.teardown != teardown_e::none) {
      why = d.teardown;
      fired_after = glitchy.now - t0;
    }
  }
  EXPECT_EQ(why, teardown_e::starve);
  EXPECT_GE(fired_after, kStarve);
  EXPECT_LT(fired_after, kStarve + 4 * kT120);
}

TEST(VrCaptureHealth, ExtremeTimestampsDoNotOverflow) {
  // 任何輸入都沒有有號溢位（用 -ftrapv 編也不會中止）。時鐘只往前，所以每一組極端值各用一個新的物件，
  // 不然餵過一次最大值之後，後面全都在同一個凍結的時刻上跑
  const int64_t extremes[] = {kI64Min, kI64Min + 1, -kSec, -1, 0, 1, kSec, kI64Max - 1, kI64Max};
  uint64_t id = 0;
  for (const int64_t t : extremes) {
    for (const int64_t p : extremes) {
      health_t h;
      h.configure(true, kT120, kMax100);
      h.begin_generation(t);
      for (int i = 0; i < 40; ++i) {
        frame_t x;
        x.outcome = (i % 3 == 0) ? outcome_e::abandoned : outcome_e::late;
        x.now_ns = t;
        x.present_ns = (i % 2) ? p : t;
        x.wait_ns = p;
        x.frame_id = ++id;
        x.observed = (i % 5 == 0) ? k_fence_lost : id / 2;
        h.on_frame(x);
        h.on_idle(p);
        const auto g = h.diag(p);
        ASSERT_GE(g.since_advance_ns, 0);
        ASSERT_GE(g.since_copy_ns, 0);
        ASSERT_GE(h.wait_budget_ns(), 20 * kMs);
        ASSERT_LE(h.wait_budget_ns(), kMax100);
        ASSERT_LE(h.snapshot().late_permille, 1000);
      }
    }
  }

  // 時刻由最小一路走到最大：每一站餵 40 張完成值不動的（同一個時刻），到下一站時「經過時間」是天文數字——
  // 卡死成立、拆線。最後第二站距最大值只有 2 s，那一次的退避期限（＋30 s）要飽和在最大值，不能溢位成負的
  // （那會變成「馬上可以再拆」）
  const int64_t stations[] = {kI64Min, kI64Min / 2, -1, kI64Max / 2, kI64Max - 40 * kSec, kI64Max - 2 * kSec, kI64Max};
  health_t h;
  h.configure(true, kT120, kMax100);
  h.begin_generation(stations[0]);
  uint64_t teardowns = 0;
  for (const int64_t t : stations) {
    for (int i = 0; i < 40; ++i) {
      frame_t x;
      x.outcome = outcome_e::abandoned;
      x.now_ns = t;
      x.present_ns = t;
      x.wait_ns = kMax100;
      x.frame_id = ++id;
      x.observed = 7;
      const auto d = h.on_frame(x);
      if (d.teardown != teardown_e::none) {
        ++teardowns;
        ASSERT_GE(h.backoff().next_allowed_ns, t) << "deadline wrapped around at t=" << t;
        ASSERT_GT(d.allowed_in_ns, 0);
      }
      if (d.deferred != teardown_e::none) {
        ASSERT_GT(d.allowed_in_ns, 0);
      }
      ASSERT_GE(h.diag(t).since_copy_ns, 0);
    }
    h.on_idle(t);
  }
  EXPECT_EQ(teardowns, 5u);  // 第 2～6 站各拆一次；最後一站距上一站只有 2 s，卡死還不成立
  EXPECT_EQ(h.backoff().next_allowed_ns, kI64Max);
}

TEST(VrCaptureHealth, NearInt64MaxSaturates) {
  // 時刻接近 INT64_MAX：拆線時 now＋5 s 超過 INT64_MAX，退避的期限要飽和
  sim_t s(true, kT120, kMax100, kI64Max - 4 * kSec);
  int64_t took = 0;
  ASSERT_EQ(s.run_until_teardown(fence_e::stuck, 3900 * kMs, took, 100 * kMs).teardown, teardown_e::stall);
  EXPECT_EQ(s.h.backoff().next_allowed_ns, kI64Max);
  // 之後（時間還沒到 INT64_MAX）仍在退避期內
  s.new_generation(100 * kMs);
  for (int i = 0; i < 5; ++i) {
    ASSERT_EQ(s.step(outcome_e::abandoned, fence_e::stuck, 100 * kMs).teardown, teardown_e::none);
  }
}

TEST(VrCaptureHealth, NegativeTimesBehaveLikePositive) {
  // 時刻是負的（格子編號往下取整）：行為與正的一樣
  sim_t s(true, kT120, kMax100, -7'000 * kSec + 123'456'789);
  s.run(240, outcome_e::on_time);
  int64_t first = 0;
  decision_t d;
  for (int k = 0;; ++k) {
    d = s.step(outcome_e::abandoned, fence_e::stuck, 100 * kMs);
    if (k == 0) {
      first = s.now;
    }
    if (d.teardown != teardown_e::none) {
      break;
    }
    ASSERT_LT(k, 100);
  }
  EXPECT_EQ(s.now - first, kStall);

  // 跨過 0 的那一刻，視窗的格子一樣是每格 100 ms、一樣是 10 格：−50 ms 的那一張在 +150 ms 時還在視窗裡，
  // 在 +950 ms（它那一格之後的第 10 格）已經離開
  sim_t z(true, kT120, kMax100, -50 * kMs - kT120);
  z.step(outcome_e::on_time);
  ASSERT_EQ(z.now, -50 * kMs);
  z.h.on_idle(150 * kMs);
  EXPECT_EQ(z.h.snapshot().pushed_fps, 1);
  z.h.on_idle(850 * kMs);
  EXPECT_EQ(z.h.snapshot().pushed_fps, 1);
  z.h.on_idle(950 * kMs);
  EXPECT_EQ(z.h.snapshot().pushed_fps, 0);

  // 一路晚到跨過 0：和其他時刻一樣，滿 1 s 才設
  sim_t late(true, kT120, kMax100, -2 * kSec);
  int late_set = -1;
  for (int i = 0; i < 600 && late_set < 0; ++i) {
    late.step(outcome_e::late);
    if (late.h.overload()) {
      late_set = i;
    }
  }
  EXPECT_GE(late_set, 100);
  EXPECT_LE(late_set, 125);
}

TEST(VrCaptureHealth, DiagTellsLateFromStuck) {
  // 開關關著（舊規則）也要能從一行 log 分辨「fence 晚到」和「fence 不動」
  sim_t late(false);
  late.run(120, outcome_e::abandoned, fence_e::lagging);  // 每張都逾時，但完成值一直在前進
  auto g = late.h.diag(late.now);
  EXPECT_EQ(g.since_advance_ns, 0);
  EXPECT_TRUE(close_to(g.since_copy_ns, 120 * kT120, kT120));
  EXPECT_EQ(g.published, 120u);
  EXPECT_EQ(g.copied, 0u);
  EXPECT_EQ(g.completed, late.completed);

  sim_t stuck(false);
  stuck.run(60, outcome_e::on_time);
  stuck.run(120, outcome_e::abandoned, fence_e::stuck);
  g = stuck.h.diag(stuck.now);
  EXPECT_TRUE(close_to(g.since_advance_ns, 120 * kT120, kT120));
  EXPECT_TRUE(close_to(g.since_copy_ns, 120 * kT120, kT120));
  EXPECT_EQ(g.published, 180u);
  EXPECT_EQ(g.copied, 60u);
  // diag 不改狀態；問一個比較早的時刻也不會是負的
  EXPECT_GE(stuck.h.diag(stuck.now - 3600 * kSec).since_copy_ns, 0);
}

TEST(VrCaptureHealth, UnreadableFenceValueIsIgnored) {
  // 完成值讀不到（UINT64_MAX）不能被當成「前進到最大值」：之後一路卡住仍然要判得出來
  sim_t s;
  s.run(60, outcome_e::on_time);
  frame_t f;
  f.outcome = outcome_e::abandoned;
  f.now_ns = s.now + kT120;
  f.present_ns = f.now_ns - kMs;
  f.frame_id = ++s.frame_id;
  ++s.fence;
  f.observed = k_fence_lost;
  s.now = f.now_ns;
  s.h.on_frame(f);
  EXPECT_EQ(s.h.diag(s.now).completed, s.completed);
  int64_t took = 0;
  EXPECT_EQ(s.run_until_teardown(fence_e::stuck, 20 * kSec, took).teardown, teardown_e::stall);
}

TEST(VrCaptureHealthSnapshot, PackRoundTripAndWindowNumbers) {
  EXPECT_EQ(pack(snapshot_t {}), 0u);
  EXPECT_EQ(unpack(0), snapshot_t {});
  const snapshot_t a {true, 1000, 65535};
  EXPECT_EQ(unpack(pack(a)), a);
  const snapshot_t b {false, 333, 97};
  EXPECT_EQ(unpack(pack(b)), b);
  EXPECT_NE(pack(a), pack(b));

  // 視窗的數字：每秒 100 張，其中每 4 張 1 張放棄
  sim_t s;
  for (int i = 0; i < 300; ++i) {
    s.step(i % 4 == 0 ? outcome_e::abandoned : outcome_e::on_time, fence_e::lagging, 10 * kMs);
  }
  const auto snap = s.h.snapshot();
  EXPECT_GE(snap.late_permille, 230);
  EXPECT_LE(snap.late_permille, 270);
  EXPECT_GE(snap.pushed_fps, 65);
  EXPECT_LE(snap.pushed_fps, 76);
  EXPECT_FALSE(snap.overload);  // 25% 不到 30%
}

TEST(VrCaptureHealthSnapshot, PushedFpsReadsUpToTenPercentLow) {
  // 視窗是「目前這一格＋前 9 格」，實際涵蓋 0.9～1.0 s：穩定每秒 120 張時 pushed_fps 讀到的是 109～120 的鋸齒。
  // 標頭與 log 的說明都這樣寫；視窗的定義改了的話這裡要跟著改
  sim_t s;
  s.run(240, outcome_e::on_time);
  uint16_t lo = 0xFFFF;
  uint16_t hi = 0;
  for (int i = 0; i < 1200; ++i) {
    s.step(outcome_e::on_time);
    lo = std::min(lo, s.h.snapshot().pushed_fps);
    hi = std::max(hi, s.h.snapshot().pushed_fps);
  }
  EXPECT_GE(lo, 108);
  EXPECT_LE(lo, 110);
  EXPECT_EQ(hi, 120);
}

// ── 亂數序列：不論怎麼走都要成立的性質 ───────────────────────────────────

namespace {
  struct rng_t {
    uint64_t s;

    uint32_t next() {
      s = s * 6364136223846793005ull + 1442695040888963407ull;
      return static_cast<uint32_t>(s >> 33);
    }

    /// [0, n)
    uint32_t below(uint32_t n) {
      return next() % n;
    }
  };

  struct fuzz_stats_t {
    uint64_t frames = 0;
    uint64_t stall = 0;
    uint64_t starve = 0;
    uint64_t deferred = 0;
    uint64_t resets = 0;
    uint64_t overload_flips = 0;
  };

  /**
   * @brief 固定種子的亂數事件序列，旁邊另外記一份帳（不看 health_t 的內部），每一張核對：
   *   I1 回報拆線的那一張一定是放棄的，而且開關開著；teardown 與 deferred 不會同時設
   *   I2 相鄰兩次拆線的間隔 ≥ 前一次回報的 allowed_in_ns（中間沒有回報 backoff_reset 的話）
   *   I3 stall：距上一次「複製成功或完成值創新高」≥ 3 s；starve：連續失敗期間有影像在來的時間（相鄰兩張最多算 0.5 s）
   *      累計 ≥ 5 s；兩者在連續失敗期間（第一張算 1 張）都發布了 ≥ 30 張
   *   I4 開關關著：永遠沒有拆線、沒有 deferred、過載旗標是 false
   *   I5 等待預算在 [min(3 個顯示週期, 上限), 上限] 之內；快照的千分比 ≤ 1000；diag 的經過時間 ≥ 0，複製成功後是 0
   */
  void fuzz(uint64_t seed, bool enabled, int64_t period_ns, int64_t max_wait_ns, int steps, fuzz_stats_t &st) {
    rng_t rng {seed};
    health_t h;
    h.configure(enabled, period_ns, max_wait_ns);
    int64_t now = 1'000 * kSec + static_cast<int64_t>(rng.below(1'000'000'000));
    h.begin_generation(now);

    // driver 的假狀態
    uint64_t frame_id = 0;
    uint64_t fence = 5000;
    uint64_t completed = 5000;
    // 另外記的帳
    bool have_base = false;
    uint64_t max_seen = 0;
    int64_t last_adv = now;
    bool have_fed = false;
    int64_t last_fed = now;
    bool no_adv_streak = false;
    uint64_t no_adv_published = 0;
    bool no_copy_streak = false;
    uint64_t no_copy_published = 0;
    int64_t no_copy_fed_ns = 0;
    bool have_teardown = false;
    int64_t last_teardown = 0;
    int64_t last_allowed_in = 0;
    bool reset_since_teardown = false;
    bool overload_prev = false;

    const auto new_generation = [&](int64_t gap_ns) {
      now += gap_ns;
      h.begin_generation(now);
      frame_id = 0;
      fence = 100 + rng.below(100000);
      completed = fence;
      have_base = false;
      last_adv = now;
      have_fed = false;
      no_adv_streak = false;
      no_copy_streak = false;
    };

    const int64_t period_used = period_ns >= 2 * kMs && period_ns <= 100 * kMs ? period_ns : kT90;
    const int64_t budget_hi = std::clamp<int64_t>(max_wait_ns, 20 * kMs, 250 * kMs);
    const int64_t budget_lo = std::min(3 * period_used, budget_hi);

    int regime = 0;  // 0 好、1 晚到、2 完成值不動、3 追不上、4 沒有影像、5 混合
    int regime_left = 0;
    for (int i = 0; i < steps; ++i) {
      if (regime_left <= 0) {
        regime = static_cast<int>(rng.below(6));
        regime_left = 1 + static_cast<int>(rng.below(regime == 4 ? 80 : 1500));
        if (rng.below(40) == 0) {
          new_generation(static_cast<int64_t>(rng.below(2000)) * kMs);  // driver 自己重連（不是我們拆的）
        }
      }
      --regime_left;
      if (regime == 4) {
        now += 100 * kMs + static_cast<int64_t>(rng.below(3)) * kMs;
        h.on_idle(now);
        ASSERT_FALSE(h.overload() && !enabled);
        continue;
      }
      // 這一張的間隔、跳過的張數、結果
      const int64_t dt = (1 + static_cast<int64_t>(rng.below(regime == 5 ? 700 : 30))) * kMs + static_cast<int64_t>(rng.below(1'000'000));
      const uint64_t publish = rng.below(8) == 0 ? 1 + rng.below(5) : 1;
      outcome_e o = outcome_e::on_time;
      bool moves = true;
      switch (regime) {
        case 0:
          break;
        case 1:
          o = rng.below(4) == 0 ? outcome_e::on_time : outcome_e::late;
          break;
        case 2:
          o = outcome_e::abandoned;
          moves = false;
          break;
        case 3:
          o = outcome_e::abandoned;
          moves = rng.below(3) != 0;
          break;
        default:
          o = static_cast<outcome_e>(rng.below(3));
          moves = rng.below(2) != 0;
          break;
      }
      now += dt;
      frame_id += publish;
      fence += publish;
      const bool copied = o != outcome_e::abandoned;
      if (copied) {
        completed = fence;
      } else if (moves && completed + 1 < fence) {
        completed += 1 + rng.below(static_cast<uint32_t>(std::min<uint64_t>(fence - completed - 1, 3)));
      }
      frame_t f;
      f.outcome = o;
      f.now_ns = now;
      f.wait_ns = static_cast<int64_t>(rng.below(100)) * kMs;
      f.present_ns = now - static_cast<int64_t>(rng.below(50)) * kMs;
      f.frame_id = frame_id;
      f.observed = rng.below(200) == 0 ? k_fence_lost : completed;

      // 另外記的帳（呼叫之前先更新：這一張本身算不算前進）
      bool adv = copied;
      if (f.observed != k_fence_lost) {
        if (!have_base) {
          have_base = true;
          max_seen = f.observed;
        } else if (f.observed > max_seen) {
          max_seen = f.observed;
          adv = true;
        }
      }
      const int64_t prev_adv = last_adv;
      const int64_t since_fed = have_fed ? std::min(now - last_fed, 500 * kMs) : 0;
      have_fed = true;
      last_fed = now;
      if (adv) {
        last_adv = now;
        no_adv_streak = false;
      } else if (!no_adv_streak) {
        no_adv_streak = true;
        no_adv_published = 1;
      } else {
        no_adv_published += publish;
      }
      if (copied) {
        no_copy_streak = false;
      } else if (!no_copy_streak) {
        no_copy_streak = true;
        no_copy_published = 1;
        no_copy_fed_ns = 0;
      } else {
        no_copy_published += publish;
        no_copy_fed_ns += since_fed;
      }

      const auto d = h.on_frame(f);
      ++st.frames;

      ASSERT_FALSE(d.teardown != teardown_e::none && d.deferred != teardown_e::none) << "seed " << seed << " step " << i;
      if (!enabled) {
        ASSERT_EQ(d.teardown, teardown_e::none);
        ASSERT_EQ(d.deferred, teardown_e::none);
        ASSERT_FALSE(h.overload());
      }
      if (d.backoff_reset) {
        ++st.resets;
        reset_since_teardown = true;
        ASSERT_EQ(h.backoff(), backoff_t {}) << "seed " << seed << " step " << i;
      }
      if (d.deferred != teardown_e::none) {
        ++st.deferred;
        ASSERT_GT(d.allowed_in_ns, 0);
        ASSERT_FALSE(copied);
      }
      if (d.teardown != teardown_e::none) {
        ASSERT_TRUE(enabled);
        ASSERT_FALSE(copied) << "seed " << seed << " step " << i;
        if (have_teardown && !reset_since_teardown) {
          ASSERT_GE(now - last_teardown, last_allowed_in) << "seed " << seed << " step " << i;
        }
        if (d.teardown == teardown_e::stall) {
          ++st.stall;
          ASSERT_GE(now - prev_adv, 3 * kSec) << "seed " << seed << " step " << i;
          ASSERT_GE(no_adv_published, 30u) << "seed " << seed << " step " << i;
        } else {
          ++st.starve;
          ASSERT_GE(no_copy_fed_ns, 5 * kSec) << "seed " << seed << " step " << i;
          ASSERT_GE(no_copy_published, 30u) << "seed " << seed << " step " << i;
        }
        ASSERT_TRUE(d.allowed_in_ns == 5 * kSec || d.allowed_in_ns == 15 * kSec || d.allowed_in_ns == 30 * kSec);
        have_teardown = true;
        last_teardown = now;
        last_allowed_in = d.allowed_in_ns;
        reset_since_teardown = false;
        // 呼叫端的動作：放掉這個 generation，0.5～3 s 後新的 generation
        new_generation(500 * kMs + static_cast<int64_t>(rng.below(2500)) * kMs);
      }
      const int64_t b = h.wait_budget_ns();
      ASSERT_GE(b, budget_lo) << "seed " << seed << " step " << i;
      ASSERT_LE(b, budget_hi) << "seed " << seed << " step " << i;
      ASSERT_LE(h.snapshot().late_permille, 1000);
      const auto g = h.diag(now);
      ASSERT_GE(g.since_advance_ns, 0);
      ASSERT_GE(g.since_copy_ns, 0);
      if (copied && d.teardown == teardown_e::none) {
        ASSERT_EQ(g.since_copy_ns, 0);
        ASSERT_EQ(g.since_advance_ns, 0);
      }
      if (h.overload() != overload_prev) {
        overload_prev = !overload_prev;
        ++st.overload_flips;
      }
    }
  }
}  // namespace

TEST(VrCaptureHealthFuzz, InvariantsHoldWithTheSwitchOn) {
  fuzz_stats_t st;
  for (uint64_t seed = 1; seed <= 24; ++seed) {
    const int64_t period = (seed % 4 == 0) ? kT90 : (seed % 4 == 1 ? kT120 : (seed % 4 == 2 ? 33'333'333 : 0));
    const int64_t max_wait = (20 + static_cast<int64_t>(seed * 37 % 231)) * kMs;
    fuzz(seed * 0x9E3779B97F4A7C15ull, true, period, max_wait, 40000, st);
    if (::testing::Test::HasFatalFailure()) {
      return;
    }
  }
  // 各種判定都真的走到了（數字只是下限，確認序列有覆蓋到）
  EXPECT_GT(st.stall, 30u);
  EXPECT_GT(st.starve, 30u);
  EXPECT_GT(st.deferred, 100u);
  EXPECT_GT(st.resets, 5u);
  EXPECT_GT(st.overload_flips, 100u);
}

TEST(VrCaptureHealthFuzz, InvariantsHoldWithTheSwitchOff) {
  fuzz_stats_t st;
  for (uint64_t seed = 101; seed <= 106; ++seed) {
    fuzz(seed * 0x9E3779B97F4A7C15ull, false, kT120, kMax100, 40000, st);
    if (::testing::Test::HasFatalFailure()) {
      return;
    }
  }
  EXPECT_GT(st.frames, 100000u);
  EXPECT_EQ(st.stall + st.starve, 0u);
  EXPECT_EQ(st.deferred, 0u);
  EXPECT_EQ(st.overload_flips, 0u);
}

// ── (e) §VR-CADENCE：LATCH／PREDICT 要不要取樣改看 Present 的節拍（config vr_capture_overload）──────────
//
// 節拍本身（locked／slipping／unsure／unknown）是 bridge 執行緒算的（test_vr_present_cadence.cpp）；這裡測的是擷取端
// 怎麼把它和 (d) 的旗標合併。規格的數字（2 s 的沿用、30%／5%／2 s、位元的位置）一樣寫死。

namespace vr::capture {
  void PrintTo(cadence_e e, std::ostream *os) {
    static constexpr const char *names[] = {"unknown", "locked", "slipping", "unsure"};
    *os << names[static_cast<uint8_t>(e) & 3u];
  }

  void PrintTo(overload_mode_e e, std::ostream *os) {
    *os << (e == overload_mode_e::late ? "late" : "cadence");
  }
}  // namespace vr::capture

namespace {
  constexpr overload_mode_e kLate = overload_mode_e::late;
  constexpr overload_mode_e kCadence = overload_mode_e::cadence;
  constexpr cadence_e kAllReports[] = {cadence_e::unknown, cadence_e::locked, cadence_e::slipping, cadence_e::unsure};

  /// 加 (e) 之前的打包公式：只有三個欄位
  constexpr uint64_t pack_old(bool overload, uint16_t late_permille, uint16_t pushed_fps) {
    return (overload ? uint64_t {1} : uint64_t {0}) | (static_cast<uint64_t>(late_permille) << 16) | (static_cast<uint64_t>(pushed_fps) << 32);
  }

  /// 假的擷取迴圈，每一步把 bridge 這一刻的報告（report）隨影像或 on_idle 帶進去——和 display_vr_t 的做法一樣
  struct cad_sim_t {
    health_t h;
    int64_t now = kStart;
    uint64_t frame_id = 0;
    uint64_t fence = 1000;
    cadence_e report = cadence_e::unknown;

    explicit cad_sim_t(overload_mode_e mode, bool on = true) {
      h.configure(on, kT120, kMax100, mode);
      h.begin_generation(now);
    }

    decision_t step(outcome_e o, int64_t interval_ns = kT120) {
      now += interval_ns;
      ++frame_id;
      ++fence;
      frame_t f;
      f.outcome = o;
      f.now_ns = now;
      f.present_ns = now - kMs;
      f.frame_id = frame_id;
      f.observed = o == outcome_e::abandoned ? fence - 3 : fence;  // 放棄的：完成值有在前進，只是到不了這一張的值
      f.cadence = report;
      return h.on_frame(f);
    }

    /// 連續餵同一種結果，到再餵一張就超過 dur_ns 為止
    void run_for(int64_t dur_ns, outcome_e o, int64_t interval_ns = kT120) {
      const int64_t end = now + dur_ns;
      while (now + interval_ns <= end) {
        step(o, interval_ns);
      }
    }

    /// 每 every 張裡前 bad 張是 o、其餘是 rest
    void run_mixed(int64_t dur_ns, outcome_e o, int bad, int every, outcome_e rest = outcome_e::on_time, int64_t interval_ns = kT120) {
      const int64_t end = now + dur_ns;
      for (int i = 0; now + interval_ns <= end; ++i) {
        step(i % every < bad ? o : rest, interval_ns);
      }
    }

    /// 沒有影像：每 100 ms 一次 on_idle（帶著目前的報告）
    void idle(int64_t dur_ns) {
      const int64_t end = now + dur_ns;
      while (end - now >= 100 * kMs) {
        now += 100 * kMs;
        h.on_idle(now, report);
      }
      now = end;
    }
  };
}  // namespace

TEST(VrCaptureHealthCadence, TruthTable) {
  // overload＝(d) 的旗標（晚到＋放棄 ≥ 30%）；abandon＝只算放棄的。最後兩欄是 cadence 模式的決定；
  // late 模式下兩個決定都等於 overload，不論節拍
  struct {
    cadence_e report;
    outcome_e load;  ///< late＝每一張都晚；abandoned＝每 10 張裡 4 張放棄、其餘準時
    bool overload;
    bool abandon;
    bool latch_hold;
    bool predict_hold;
  } rows[] = {
    // 每一張都晚（跑滿更新率、fence 固定晚幾毫秒）
    {cadence_e::locked, outcome_e::late, true, false, false, true},  // 相位取模後可鎖：LATCH 照追；絕對延遲不穩：PREDICT 不取樣
    {cadence_e::slipping, outcome_e::late, true, false, true, true},
    {cadence_e::unsure, outcome_e::late, true, false, true, true},  // 說不準：照 (d) 的旗標
    {cadence_e::unknown, outcome_e::late, true, false, true, true},
    // 全部準時
    {cadence_e::locked, outcome_e::on_time, false, false, false, false},
    {cadence_e::slipping, outcome_e::on_time, false, false, true, true},  // 節拍在滑動：一張都沒晚也不取樣
    {cadence_e::unsure, outcome_e::on_time, false, false, false, false},
    {cadence_e::unknown, outcome_e::on_time, false, false, false, false},
    // 40% 放棄（fence 真的不動：沒有新影像可量）
    {cadence_e::locked, outcome_e::abandoned, true, true, true, true},
    {cadence_e::slipping, outcome_e::abandoned, true, true, true, true},
    {cadence_e::unsure, outcome_e::abandoned, true, true, true, true},
    {cadence_e::unknown, outcome_e::abandoned, true, true, true, true},
  };
  for (const auto &r : rows) {
    for (const overload_mode_e mode : {kCadence, kLate}) {
      SCOPED_TRACE(::testing::Message() << "report " << static_cast<int>(r.report) << " load " << static_cast<int>(r.load) << " mode " << static_cast<int>(mode));
      cad_sim_t s(mode);
      s.report = r.report;
      if (r.load == outcome_e::abandoned) {
        s.run_mixed(3 * kSec, outcome_e::abandoned, 4, 10);
      } else {
        s.run_for(3 * kSec, r.load);
      }
      const auto snap = s.h.snapshot();
      EXPECT_EQ(s.h.mode(), mode);
      EXPECT_EQ(s.h.overload(), r.overload);
      EXPECT_EQ(snap.overload, r.overload);
      EXPECT_EQ(snap.abandon_overload, r.abandon);
      EXPECT_EQ(s.h.cadence(), r.report);  // 報告一直有：擷取端用的節拍就是它
      EXPECT_EQ(snap.cadence, r.report);  // late 模式照樣發布（影子）
      EXPECT_EQ(s.h.latch_hold_if_cadence(), r.latch_hold);  // cadence 模式會怎麼判：兩種模式都照算
      const bool want_latch = mode == kCadence ? r.latch_hold : r.overload;
      const bool want_predict = mode == kCadence ? r.predict_hold : r.overload;
      EXPECT_EQ(s.h.latch_hold(), want_latch);
      EXPECT_EQ(s.h.predict_hold(), want_predict);
      EXPECT_EQ(snap.latch_hold, want_latch);
      EXPECT_EQ(snap.predict_hold, want_predict);
    }
  }
}

TEST(VrCaptureHealthCadence, ACertainReportIsKeptForTwoSeconds) {
  // locked、每一張都晚：LATCH 照追。報告沒了（bridge 卡住、摘要過期）之後 2.0 s 內照舊，2.1 s 起照 (d) 的旗標
  cad_sim_t s(kCadence);
  s.report = cadence_e::locked;
  s.run_for(3 * kSec, outcome_e::late);
  ASSERT_TRUE(s.h.overload());
  ASSERT_FALSE(s.h.latch_hold());
  const int64_t t0 = s.now;  // 最後一次帶著 locked 的報告
  s.report = cadence_e::unknown;
  while (s.now + kT120 <= t0 + 2000 * kMs) {
    s.step(outcome_e::late);
    ASSERT_EQ(s.h.cadence(), cadence_e::locked) << (s.now - t0) / kMs << " ms";
    ASSERT_FALSE(s.h.latch_hold());
    ASSERT_TRUE(s.h.predict_hold());
    ASSERT_EQ(s.h.snapshot().cadence, cadence_e::locked);
  }
  while (s.now < t0 + 2100 * kMs) {
    s.step(outcome_e::late);
  }
  EXPECT_EQ(s.h.cadence(), cadence_e::unknown);
  EXPECT_TRUE(s.h.latch_hold());
  EXPECT_TRUE(s.h.predict_hold());
  EXPECT_EQ(s.h.snapshot().cadence, cadence_e::unknown);

  // slipping、全部準時、之後連影像也停了（只有 on_idle）：2.0 s 內還擋著，之後放開
  cad_sim_t t(kCadence);
  t.report = cadence_e::slipping;
  t.run_for(3 * kSec, outcome_e::on_time);
  ASSERT_FALSE(t.h.overload());
  ASSERT_TRUE(t.h.latch_hold());
  ASSERT_TRUE(t.h.predict_hold());
  t.report = cadence_e::unknown;
  t.idle(2 * kSec);
  EXPECT_EQ(t.h.cadence(), cadence_e::slipping);
  EXPECT_TRUE(t.h.latch_hold());
  EXPECT_TRUE(t.h.predict_hold());
  t.idle(100 * kMs);
  EXPECT_EQ(t.h.cadence(), cadence_e::unknown);
  EXPECT_FALSE(t.h.latch_hold());
  EXPECT_FALSE(t.h.predict_hold());

  // 界線：剛好 2 s 還沿用，多 1 ns 就不沿用
  cad_sim_t e(kCadence);
  e.report = cadence_e::locked;
  e.run_for(3 * kSec, outcome_e::late);
  const int64_t t2 = e.now;
  e.h.on_idle(t2 + 2 * kSec);
  EXPECT_EQ(e.h.cadence(), cadence_e::locked);
  e.h.on_idle(t2 + 2 * kSec + 1);
  EXPECT_EQ(e.h.cadence(), cadence_e::unknown);
  // 之後再有確定的報告就照它
  e.h.on_idle(t2 + 2 * kSec + 2, cadence_e::slipping);
  EXPECT_EQ(e.h.cadence(), cadence_e::slipping);
}

TEST(VrCaptureHealthCadence, UnsureIsTakenAtOnceAndDropsTheKeptValue) {
  cad_sim_t s(kCadence);
  s.report = cadence_e::locked;
  s.run_for(3 * kSec, outcome_e::late);
  ASSERT_TRUE(s.h.overload());
  ASSERT_FALSE(s.h.latch_hold());
  // 變成說不準：立刻照 (d) 的旗標，不沿用剛才的 locked
  s.report = cadence_e::unsure;
  s.step(outcome_e::late);
  EXPECT_EQ(s.h.cadence(), cadence_e::unsure);
  EXPECT_TRUE(s.h.latch_hold());
  EXPECT_EQ(s.h.snapshot().cadence, cadence_e::unsure);
  // 接著報告沒了：也不會回頭沿用更早的 locked
  s.report = cadence_e::unknown;
  s.step(outcome_e::late);
  EXPECT_EQ(s.h.cadence(), cadence_e::unknown);
  EXPECT_TRUE(s.h.latch_hold());
  // 再來一筆確定的報告就照它
  s.report = cadence_e::locked;
  s.step(outcome_e::late);
  EXPECT_EQ(s.h.cadence(), cadence_e::locked);
  EXPECT_FALSE(s.h.latch_hold());
  // on_idle 帶進來的 unsure 也一樣
  s.h.on_idle(s.now + kMs, cadence_e::unsure);
  EXPECT_EQ(s.h.cadence(), cadence_e::unsure);
  EXPECT_TRUE(s.h.latch_hold());
}

TEST(VrCaptureHealthCadence, NewGenerationStartsOver) {
  cad_sim_t s(kCadence);
  s.report = cadence_e::slipping;
  s.run_mixed(3 * kSec, outcome_e::abandoned, 4, 10);
  ASSERT_TRUE(s.h.snapshot().abandon_overload);
  ASSERT_TRUE(s.h.latch_hold());
  ASSERT_EQ(s.h.cadence(), cadence_e::slipping);
  // 換 generation：節拍、放棄的旗標都從頭來（2 s 的沿用不跨 generation），快照回到預設值
  s.h.begin_generation(s.now);
  EXPECT_EQ(s.h.snapshot(), snapshot_t {});
  EXPECT_EQ(pack(s.h.snapshot()), 0u);
  EXPECT_EQ(s.h.cadence(), cadence_e::unknown);
  EXPECT_FALSE(s.h.latch_hold());
  EXPECT_FALSE(s.h.predict_hold());
  EXPECT_FALSE(s.h.latch_hold_if_cadence());
  EXPECT_EQ(s.h.mode(), kCadence);  // 設定不動
  // 新 generation 的放棄旗標也要重新看滿 1 s
  s.report = cadence_e::locked;
  const int64_t t0 = s.now;
  for (int i = 0; s.now - t0 < 950 * kMs; ++i) {
    s.step(i % 2 ? outcome_e::abandoned : outcome_e::on_time);
    ASSERT_FALSE(s.h.snapshot().abandon_overload) << (s.now - t0) / kMs << " ms";
    ASSERT_FALSE(s.h.latch_hold());
  }
  for (int i = 0; i < 12; ++i) {
    s.step(i % 2 ? outcome_e::abandoned : outcome_e::on_time);
  }
  EXPECT_TRUE(s.h.snapshot().abandon_overload);
  EXPECT_TRUE(s.h.latch_hold());
}

TEST(VrCaptureHealthCadence, AbandonFlagFollowsTheThirtyAndFivePercentRule) {
  // 剛好 30%（每秒 10 張、每 10 張裡 3 張放棄）：要設
  cad_sim_t at(kCadence);
  bool set = false;
  for (int i = 0; i < 200 && !set; ++i) {
    at.step(i % 10 < 3 ? outcome_e::abandoned : outcome_e::on_time, 100 * kMs);
    set = at.h.snapshot().abandon_overload;
  }
  EXPECT_TRUE(set);
  // 20% 放棄、其餘全部晚到：(d) 的旗標設著（100%），放棄的旗標不設——只是晚到不擋 LATCH
  cad_sim_t below(kCadence);
  below.report = cadence_e::locked;
  for (int i = 0; i < 200; ++i) {
    below.step(i % 10 < 2 ? outcome_e::abandoned : outcome_e::late, 100 * kMs);
    ASSERT_FALSE(below.h.snapshot().abandon_overload) << i;
    ASSERT_FALSE(below.h.latch_hold()) << i;
  }
  EXPECT_TRUE(below.h.overload());
  EXPECT_TRUE(below.h.predict_hold());
  // 至少 3 張：每秒只有 2 張、都放棄，不設
  cad_sim_t two(kCadence);
  for (int i = 0; i < 8; ++i) {
    two.step(outcome_e::abandoned, 500 * kMs);
    ASSERT_FALSE(two.h.snapshot().abandon_overload) << i;
  }

  // 清除：設起來之後，10% 不清；降到 5% 以下持續 2 s 才清
  cad_sim_t s(kCadence);
  s.report = cadence_e::locked;
  s.run_mixed(3 * kSec, outcome_e::abandoned, 5, 10);
  ASSERT_TRUE(s.h.snapshot().abandon_overload);
  ASSERT_TRUE(s.h.latch_hold());
  for (int i = 0; i < 120 * 5; ++i) {
    s.step(i % 10 == 0 ? outcome_e::abandoned : outcome_e::on_time);
    ASSERT_TRUE(s.h.snapshot().abandon_overload) << i;
  }
  const int64_t t0 = s.now;
  int64_t cleared = -1;
  for (int i = 0; i < 120 * 6 && cleared < 0; ++i) {
    s.step(i % 30 == 29 ? outcome_e::abandoned : outcome_e::on_time);
    if (!s.h.snapshot().abandon_overload) {
      cleared = s.now - t0;
    }
  }
  EXPECT_GT(cleared, 2 * kSec);
  EXPECT_LT(cleared, 3300 * kMs);
  EXPECT_FALSE(s.h.latch_hold());

  // 影像整個停了：靠 on_idle 讓視窗往前走，最後一張離開視窗之後再 2 s 清（和 (d) 的旗標同時）
  cad_sim_t stop(kCadence);
  stop.report = cadence_e::locked;
  stop.run_mixed(3 * kSec, outcome_e::abandoned, 5, 10);
  ASSERT_TRUE(stop.h.snapshot().abandon_overload);
  const int64_t t1 = stop.now;
  int64_t cleared_after = 0;
  while (stop.now - t1 < 10 * kSec && cleared_after == 0) {
    stop.now += 100 * kMs;
    stop.h.on_idle(stop.now, cadence_e::locked);
    ASSERT_TRUE(!stop.h.snapshot().abandon_overload || stop.h.overload());
    if (!stop.h.snapshot().abandon_overload) {
      cleared_after = stop.now - t1;
    }
  }
  EXPECT_GE(cleared_after, 2900 * kMs);
  EXPECT_LE(cleared_after, 3100 * kMs);
  EXPECT_FALSE(stop.h.overload());
}

TEST(VrCaptureHealthCadence, WithTheSwitchOffEverythingNewIsFalse) {
  // vr_capture_late = disabled：不論模式、不論節拍，新欄位全是 false／unknown，打包的快照和加 (e) 之前相同
  for (const overload_mode_e mode : {kLate, kCadence}) {
    for (const cadence_e report : kAllReports) {
      SCOPED_TRACE(::testing::Message() << "mode " << static_cast<int>(mode) << " report " << static_cast<int>(report));
      cad_sim_t s(mode, false);
      s.report = report;
      s.run_mixed(3 * kSec, outcome_e::abandoned, 5, 10);
      s.idle(300 * kMs);
      s.run_mixed(kSec, outcome_e::abandoned, 5, 10);
      const auto snap = s.h.snapshot();
      EXPECT_FALSE(snap.overload);
      EXPECT_FALSE(snap.latch_hold);
      EXPECT_FALSE(snap.predict_hold);
      EXPECT_FALSE(snap.abandon_overload);
      EXPECT_EQ(snap.cadence, cadence_e::unknown);
      EXPECT_EQ(s.h.cadence(), cadence_e::unknown);
      EXPECT_FALSE(s.h.latch_hold());
      EXPECT_FALSE(s.h.predict_hold());
      EXPECT_FALSE(s.h.latch_hold_if_cadence());
      EXPECT_GT(snap.late_permille, 400);  // 數字照算
      EXPECT_EQ(pack(snap), pack_old(false, snap.late_permille, snap.pushed_fps));
    }
  }
  // 開著的時候關掉：新的旗標立刻清，之後怎麼餵都不會再設
  cad_sim_t s(kCadence);
  s.report = cadence_e::slipping;
  s.run_mixed(3 * kSec, outcome_e::abandoned, 5, 10);
  ASSERT_TRUE(s.h.latch_hold());
  ASSERT_TRUE(s.h.snapshot().abandon_overload);
  s.h.configure(false, kT120, kMax100, kCadence);
  EXPECT_EQ(pack(s.h.snapshot()) & 0xFFFFull, 0u);
  EXPECT_EQ(s.h.cadence(), cadence_e::unknown);
  EXPECT_FALSE(s.h.latch_hold_if_cadence());
  s.step(outcome_e::abandoned);
  s.h.on_idle(s.now + kMs, cadence_e::slipping);
  EXPECT_EQ(pack(s.h.snapshot()) & 0xFFFFull, 0u);
}

TEST(VrCaptureHealthCadence, ConfigureWithThreeArgumentsMeansLateMode) {
  // 現有的呼叫方式（3 個參數）＝late 模式；模式可以用第 4 個參數換，不清任何狀態
  cad_sim_t s(kCadence);
  s.report = cadence_e::locked;
  s.run_for(3 * kSec, outcome_e::late);
  ASSERT_TRUE(s.h.overload());
  ASSERT_FALSE(s.h.latch_hold());
  s.h.configure(true, kT120, kMax100);
  EXPECT_EQ(s.h.mode(), kLate);
  EXPECT_TRUE(s.h.overload());
  EXPECT_TRUE(s.h.latch_hold());  // late 模式：就是 (d) 的旗標
  EXPECT_FALSE(s.h.latch_hold_if_cadence());  // cadence 模式會怎麼判照算
  EXPECT_EQ(s.h.cadence(), cadence_e::locked);
  s.h.configure(true, kT120, kMax100, kCadence);
  EXPECT_FALSE(s.h.latch_hold());

  health_t fresh;
  EXPECT_EQ(fresh.mode(), kLate);
  EXPECT_EQ(fresh.cadence(), cadence_e::unknown);
  EXPECT_EQ(fresh.snapshot(), snapshot_t {});
}

TEST(VrCaptureHealthCadence, SnapshotPackLayout) {
  EXPECT_EQ(pack(snapshot_t {}), 0u);
  // 只給舊的三個欄位的寫法照樣可用，打包值和加 (e) 之前相同
  const snapshot_t old3 {true, 1000, 65535};
  EXPECT_EQ(pack(old3), 0x0000FFFF03E80001ull);
  EXPECT_EQ(pack(old3), pack_old(true, 1000, 65535));
  EXPECT_FALSE(old3.latch_hold);
  EXPECT_EQ(old3.cadence, cadence_e::unknown);
  // 新位元的位置：bit 1 latch_hold、bit 2 predict_hold、bit 3 abandon_overload、bit 4-5 cadence
  snapshot_t s;
  s.latch_hold = true;
  EXPECT_EQ(pack(s), 0x02u);
  s = snapshot_t {};
  s.predict_hold = true;
  EXPECT_EQ(pack(s), 0x04u);
  s = snapshot_t {};
  s.abandon_overload = true;
  EXPECT_EQ(pack(s), 0x08u);
  s = snapshot_t {};
  s.cadence = cadence_e::locked;
  EXPECT_EQ(pack(s), 0x10u);
  s.cadence = cadence_e::slipping;
  EXPECT_EQ(pack(s), 0x20u);
  s.cadence = cadence_e::unsure;
  EXPECT_EQ(pack(s), 0x30u);
  // 每一種組合來回相等；新位元不碰舊的三個欄位
  for (unsigned bits = 0; bits < 16; ++bits) {
    for (const cadence_e c : kAllReports) {
      snapshot_t x;
      x.overload = (bits & 1u) != 0;
      x.latch_hold = (bits & 2u) != 0;
      x.predict_hold = (bits & 4u) != 0;
      x.abandon_overload = (bits & 8u) != 0;
      x.cadence = c;
      x.late_permille = static_cast<uint16_t>(137 * bits);
      x.pushed_fps = static_cast<uint16_t>(65535 - bits);
      EXPECT_EQ(unpack(pack(x)), x) << bits;
      EXPECT_EQ(pack(x) & 0x0000FFFFFFFF0001ull, pack_old(x.overload, x.late_permille, x.pushed_fps)) << bits;
      EXPECT_EQ(pack(x) & ~0x0000FFFFFFFF003Full, 0u) << bits;
    }
  }
}

TEST(VrCaptureHealthCadence, LogNames) {
  // log 的詞彙（[VIPLE-VR-CAP] 各行的 cadence=、ovlMode=）：slipping 印成 slip，和 [VIPLE-VR-DRV] timing 10s 同一組詞
  EXPECT_STREQ(cadence_name(cadence_e::unknown), "unknown");
  EXPECT_STREQ(cadence_name(cadence_e::locked), "locked");
  EXPECT_STREQ(cadence_name(cadence_e::slipping), "slip");
  EXPECT_STREQ(cadence_name(cadence_e::unsure), "unsure");
  EXPECT_STREQ(overload_mode_name(kLate), "late");
  EXPECT_STREQ(overload_mode_name(kCadence), "cadence");
}

// ── (e) 的亂數序列 ───────────────────────────────────────────────────────

namespace {
  /// 參考模型：擷取端用的節拍（確定的報告沿用 2 s；unsure 立刻採用）。時鐘照 health_t 的規則只往前
  struct cad_ref_t {
    bool have = false;
    bool slip = false;
    int64_t seen = 0;
    cadence_e last = cadence_e::unknown;
    bool have_now = false;
    int64_t now = 0;

    int64_t tick(int64_t t) {
      if (!have_now || t > now) {
        have_now = true;
        now = t;
      }
      return now;
    }

    void begin(int64_t t) {
      tick(t);
      have = false;
      slip = false;
      last = cadence_e::unknown;
    }

    void note(cadence_e c, int64_t t) {
      const int64_t n = tick(t);
      last = c;
      if (c == cadence_e::locked || c == cadence_e::slipping) {
        have = true;
        slip = c == cadence_e::slipping;
        seen = n;
      } else if (c == cadence_e::unsure) {
        have = false;
      }
    }

    cadence_e used() const {
      if (have && now - seen <= 2 * kSec) {
        return slip ? cadence_e::slipping : cadence_e::locked;
      }
      return last == cadence_e::unsure ? cadence_e::unsure : cadence_e::unknown;
    }
  };

  /// 加 (e) 之前就有的每一個輸出，兩個物件是不是都相同
  bool same_old_outputs(const health_t &a, const health_t &b, int64_t now) {
    const auto ga = a.diag(now);
    const auto gb = b.diag(now);
    const auto sa = a.snapshot();
    const auto sb = b.snapshot();
    return a.enabled() == b.enabled() && a.overload() == b.overload() && a.wait_budget_ns() == b.wait_budget_ns() && a.interval_ema_ns() == b.interval_ema_ns() &&
           a.window_wait_max_ns() == b.window_wait_max_ns() && a.teardowns() == b.teardowns() && a.backoff() == b.backoff() &&
           ga.since_advance_ns == gb.since_advance_ns && ga.since_copy_ns == gb.since_copy_ns && ga.published == gb.published && ga.copied == gb.copied && ga.completed == gb.completed &&
           sa.overload == sb.overload && sa.late_permille == sb.late_permille && sa.pushed_fps == sb.pushed_fps;
  }

  bool same_decision(const decision_t &x, const decision_t &y) {
    return x.teardown == y.teardown && x.deferred == y.deferred && x.allowed_in_ns == y.allowed_in_ns && x.backoff_reset == y.backoff_reset;
  }

  struct cad_fuzz_stats_t {
    uint64_t steps = 0;
    uint64_t frames = 0;
    uint64_t teardowns = 0;
    uint64_t overload = 0;  ///< (d) 的旗標設著的步數
    uint64_t abandon = 0;  ///< 放棄的旗標設著的步數
    uint64_t latch_hold = 0;
    uint64_t predict_hold = 0;
    uint64_t latch_differs = 0;  ///< latch_hold 和 (d) 的旗標不同的步數
    uint64_t kept = 0;  ///< 這一次呼叫沒有報告、用的是沿用的值
    uint64_t seen[4] = {0, 0, 0, 0};  ///< 擷取端用的節拍
  };

  /**
   * @brief 固定種子的亂數影像序列（負載型態同上面的 fuzz()）加亂數的節拍報告。受測的物件 h 帶著節拍、用 4 個參數的
   *        configure；對照的物件 plain 餵同樣的影像，但節拍永遠是 unknown、用 3 個參數的 configure（＝加 (e) 之前的
   *        呼叫方式）。每一步核對：
   *   I6 節拍輸入不影響任何舊輸出、也不推進時鐘：h 與 plain 的回覆、(d) 的旗標、等待預算、發布間隔、視窗、退避、
   *      diag()、快照的舊三個欄位全部相同（兩種模式都要成立）
   *   I7 新輸出照真值表：late 模式 latch_hold == predict_hold == overload；cadence 模式看擷取端用的節拍（另外記的
   *      參考模型：確定的報告沿用 2 s、unsure 立刻採用）。latch_hold_if_cadence() 在兩種模式都是 cadence 那一欄
   *   I8 恆成立：latch_hold ⇒ predict_hold、abandon_overload ⇒ overload；開關關著時新欄位全是 false／unknown，
   *      打包的快照等於舊的打包公式
   */
  void cad_fuzz(uint64_t seed, bool enabled, overload_mode_e mode, int steps, cad_fuzz_stats_t &st) {
    rng_t rng {seed};
    health_t h;
    health_t plain;
    const int64_t period = seed % 3 == 0 ? kT90 : kT120;
    const int64_t max_wait = (20 + static_cast<int64_t>(seed % 231)) * kMs;
    h.configure(enabled, period, max_wait, mode);
    plain.configure(enabled, period, max_wait);
    int64_t now = 1'000 * kSec + static_cast<int64_t>(rng.below(1'000'000'000));
    h.begin_generation(now);
    plain.begin_generation(now);
    cad_ref_t ref;
    ref.begin(now);

    uint64_t frame_id = 0;
    uint64_t fence = 5000;
    uint64_t completed = 5000;
    cadence_e bridge = cadence_e::unknown;  // bridge 這一刻的狀態

    const auto new_generation = [&](int64_t gap_ns) {
      now += gap_ns;
      h.begin_generation(now);
      plain.begin_generation(now);
      ref.begin(now);
      frame_id = 0;
      fence = 100 + rng.below(100000);
      completed = fence;
    };

    // 每一步之後的核對；回 false＝有不成立的（已經記了一個失敗）
    const auto verify = [&](int i) -> bool {
      const auto snap = h.snapshot();
      const cadence_e used = ref.used();
      const bool want_cadence = used == cadence_e::slipping ? true : (used == cadence_e::locked ? snap.abandon_overload : snap.overload);
      bool ok = same_old_outputs(h, plain, now);  // I6
      ok = ok && unpack(pack(snap)) == snap && snap.latch_hold == h.latch_hold() && snap.predict_hold == h.predict_hold() && snap.cadence == h.cadence();
      ok = ok && (!snap.latch_hold || snap.predict_hold) && (!snap.abandon_overload || snap.overload);  // I8
      if (!enabled) {
        ok = ok && !snap.overload && !snap.latch_hold && !snap.predict_hold && !snap.abandon_overload && snap.cadence == cadence_e::unknown &&
             !h.latch_hold_if_cadence() && pack(snap) == pack_old(false, snap.late_permille, snap.pushed_fps);
      } else {
        ok = ok && snap.cadence == used && h.latch_hold_if_cadence() == want_cadence;  // I7
        if (mode == kLate) {
          ok = ok && snap.latch_hold == snap.overload && snap.predict_hold == snap.overload;
        } else {
          ok = ok && snap.latch_hold == want_cadence && snap.predict_hold == (snap.overload || used == cadence_e::slipping);
        }
      }
      if (!ok) {
        ADD_FAILURE() << "seed " << seed << " step " << i << " mode " << static_cast<int>(mode) << " enabled " << enabled << " used " << static_cast<int>(used)
                      << " snapshot 0x" << std::hex << pack(snap) << " plain 0x" << pack(plain.snapshot());
        return false;
      }
      ++st.steps;
      st.overload += snap.overload ? 1 : 0;
      st.abandon += snap.abandon_overload ? 1 : 0;
      st.latch_hold += snap.latch_hold ? 1 : 0;
      st.predict_hold += snap.predict_hold ? 1 : 0;
      st.latch_differs += snap.latch_hold != snap.overload ? 1 : 0;
      ++st.seen[static_cast<uint8_t>(snap.cadence) & 3u];
      return true;
    };

    int regime = 0;  // 0 好、1 晚到、2 完成值不動、3 追不上、4 沒有影像、5 混合
    int regime_left = 0;
    for (int i = 0; i < steps; ++i) {
      if (regime_left <= 0) {
        regime = static_cast<int>(rng.below(6));
        regime_left = 1 + static_cast<int>(rng.below(regime == 4 ? 80 : 1500));
        if (rng.below(40) == 0) {
          new_generation(static_cast<int64_t>(rng.below(2000)) * kMs);
        }
      }
      --regime_left;
      if (rng.below(200) == 0) {
        bridge = static_cast<cadence_e>(rng.below(4));
      }
      // 這一次呼叫帶的報告：每 4 次有 1 次沒有（摘要過期、bridge 卡住）
      const cadence_e in = rng.below(4) == 0 ? cadence_e::unknown : bridge;
      if (in == cadence_e::unknown && bridge != cadence_e::unknown && ref.used() != cadence_e::unknown) {
        ++st.kept;
      }
      if (regime == 4) {
        now += 100 * kMs + static_cast<int64_t>(rng.below(3)) * kMs;
        h.on_idle(now, in);
        plain.on_idle(now);
        ref.note(in, now);
        if (!verify(i)) {
          return;
        }
        continue;
      }
      if (rng.below(500) == 0) {
        // 時刻比上一次還早的 on_idle（時鐘只往前：兩個物件都把它當成和上一次相同）
        const int64_t early = now - static_cast<int64_t>(rng.below(50)) * kMs;
        h.on_idle(early, in);
        plain.on_idle(early);
        ref.note(in, early);
        if (!verify(i)) {
          return;
        }
      }
      const int64_t dt = (1 + static_cast<int64_t>(rng.below(regime == 5 ? 700 : 30))) * kMs + static_cast<int64_t>(rng.below(1'000'000));
      const uint64_t publish = rng.below(8) == 0 ? 1 + rng.below(5) : 1;
      outcome_e o = outcome_e::on_time;
      bool moves = true;
      switch (regime) {
        case 0:
          break;
        case 1:
          o = rng.below(4) == 0 ? outcome_e::on_time : outcome_e::late;
          break;
        case 2:
          o = outcome_e::abandoned;
          moves = false;
          break;
        case 3:
          o = outcome_e::abandoned;
          moves = rng.below(3) != 0;
          break;
        default:
          o = static_cast<outcome_e>(rng.below(3));
          moves = rng.below(2) != 0;
          break;
      }
      now += dt;
      frame_id += publish;
      fence += publish;
      if (o != outcome_e::abandoned) {
        completed = fence;
      } else if (moves && completed + 1 < fence) {
        completed += 1 + rng.below(static_cast<uint32_t>(std::min<uint64_t>(fence - completed - 1, 3)));
      }
      frame_t f;
      f.outcome = o;
      f.now_ns = now;
      f.wait_ns = static_cast<int64_t>(rng.below(100)) * kMs;
      f.present_ns = now - static_cast<int64_t>(rng.below(50)) * kMs;
      f.frame_id = frame_id;
      f.observed = rng.below(200) == 0 ? k_fence_lost : completed;
      frame_t with_cadence = f;
      with_cadence.cadence = in;
      const auto d = h.on_frame(with_cadence);
      const auto dp = plain.on_frame(f);
      ref.note(in, now);
      ++st.frames;
      if (!same_decision(d, dp)) {
        ADD_FAILURE() << "seed " << seed << " step " << i << ": the cadence input changed the verdict of on_frame()";
        return;
      }
      if (!verify(i)) {
        return;
      }
      if (d.teardown != teardown_e::none) {
        ++st.teardowns;
        new_generation(500 * kMs + static_cast<int64_t>(rng.below(2500)) * kMs);
        if (!verify(i)) {
          return;
        }
      }
    }
  }
}  // namespace

TEST(VrCaptureHealthCadenceFuzz, DefaultModeDecidesExactlyAsBefore) {
  // vr_capture_overload = late（預設）：節拍怎麼餵都不影響任何決定——stream.cpp 讀到的兩個旗標就是 (d) 的旗標
  cad_fuzz_stats_t st;
  for (uint64_t seed = 1; seed <= 16; ++seed) {
    cad_fuzz(seed * 0x9E3779B97F4A7C15ull, true, kLate, 40000, st);
    if (::testing::Test::HasFailure()) {
      return;
    }
  }
  EXPECT_GT(st.frames, 500'000u);
  EXPECT_EQ(st.latch_differs, 0u);
  EXPECT_EQ(st.latch_hold, st.overload);
  EXPECT_EQ(st.predict_hold, st.overload);
  // 序列真的有覆蓋到（數字只是下限）：旗標設著、放棄的旗標、拆線、四種節拍、沿用
  EXPECT_GT(st.overload, 100'000u);
  EXPECT_GT(st.abandon, 50'000u);
  EXPECT_GT(st.teardowns, 50u);
  EXPECT_GT(st.kept, 10'000u);
  for (const uint64_t n : st.seen) {
    EXPECT_GT(n, 10'000u);
  }
}

TEST(VrCaptureHealthCadenceFuzz, CadenceModeFollowsTheTruthTable) {
  cad_fuzz_stats_t st;
  for (uint64_t seed = 21; seed <= 36; ++seed) {
    cad_fuzz(seed * 0x9E3779B97F4A7C15ull, true, kCadence, 40000, st);
    if (::testing::Test::HasFailure()) {
      return;
    }
  }
  EXPECT_GT(st.frames, 500'000u);
  // 新模式真的有和 (d) 的旗標不同的時候（兩個方向都有：locked 而只是晚到→不擋；slipping 而沒有晚到→擋）
  EXPECT_GT(st.latch_differs, 20'000u);
  EXPECT_GE(st.predict_hold, st.latch_hold);
  EXPECT_GE(st.predict_hold, st.overload);
  EXPECT_GT(st.overload, 100'000u);
  EXPECT_GT(st.abandon, 50'000u);
  EXPECT_GT(st.teardowns, 50u);
  EXPECT_GT(st.kept, 10'000u);
  for (const uint64_t n : st.seen) {
    EXPECT_GT(n, 10'000u);
  }
}

TEST(VrCaptureHealthCadenceFuzz, SwitchOffIgnoresModeAndCadence) {
  cad_fuzz_stats_t st;
  for (uint64_t seed = 41; seed <= 44; ++seed) {
    cad_fuzz(seed * 0x9E3779B97F4A7C15ull, false, seed % 2 ? kCadence : kLate, 40000, st);
    if (::testing::Test::HasFailure()) {
      return;
    }
  }
  EXPECT_GT(st.frames, 100'000u);
  EXPECT_EQ(st.overload + st.abandon + st.latch_hold + st.predict_hold, 0u);
  EXPECT_EQ(st.seen[1] + st.seen[2] + st.seen[3], 0u);
  EXPECT_EQ(st.teardowns, 0u);
}
