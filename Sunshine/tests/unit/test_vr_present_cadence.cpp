/**
 * @file tests/unit/test_vr_present_cadence.cpp
 * @brief VipleStream 2.0 §VR-CADENCE：從 driver 的 timing ring 判斷合成器的 Present 鎖不鎖得住虛擬 vsync
 *        （src/vr/vr_present_cadence.h；全部用假的時間與假的 Present 序列驅動）。
 *
 * 和 test_vr_capture_health.cpp 同樣的約定：
 *   - 不 include ../tests_common.h：受測的標頭只用標準函式庫，測試也只需要 googletest，可以脫離整個 server 單獨編譯執行
 *     （-Wall -Wextra -Wshadow -Wconversion -Werror 也過得了）。放進 test_sunshine 一起建也一樣能跑。
 *   - 規格的數字（24 筆、10 s 內 4 個、12 s、3 s、16 ms、事件代碼……）在這裡寫死，不引用標頭的常數：常數被改掉時測試要會失敗。
 *   - 沒有紀錄的時段用 sim_t::advance()，它照正式的節拍呼叫 on_tick()（bridge 執行緒每 2 ms 一輪，這裡每 5 ms），
 *     不是把時間一次跳過去。
 *
 * 讀測試時要記得的兩件事：
 *   - 「一張跨 m 格的 Present」＝時間前進 m × 週期之後呼叫 on_present()；settle()＝每格一張跑 11 s（過了 24 筆與 10 s 的觀察期）。
 *   - 一個滑動要等下一筆 Present 進來（那一段同樣格數的連續紀錄結束）才記，事件的時刻是那時的 now。
 */
#include <gtest/gtest.h>

#ifdef _WIN32
  // 刻意放在受測標頭之前：Windows 的標頭把 near、far、interface、ERROR……定義成巨集，受測標頭的識別字撞到任何一個
  // 就編不過（這個標頭會被先 include <Windows.h> 的 vr_bridge_win.cpp、display_vr.cpp include）
  #include <windows.h>
#endif

#include <algorithm>
#include <cstdint>
#include <limits>
#include <ostream>
#include <random>
#include <src/vr/vr_present_cadence.h>

using namespace vr::cadence;

namespace vr::cadence {
  // 斷言失敗時印名字（不然 googletest 只會印「1-byte object <02>」）
  void PrintTo(state_e e, std::ostream *os) {
    static constexpr const char *names[] = {"unknown", "locked", "slipping", "unsure"};
    *os << names[static_cast<uint8_t>(e) & 3u];
  }

  void PrintTo(why_e e, std::ostream *os) {
    static constexpr const char *names[] = {"none", "marginal", "heavy", "recovering", "recent", "fraction", "suspect", "proving"};
    *os << names[static_cast<uint8_t>(e) & 7u];
  }
}  // namespace vr::cadence

namespace {
  constexpr int64_t kMs = 1'000'000;
  constexpr int64_t kSec = 1'000'000'000;
  constexpr int64_t kT144 = 6'944'444;  // 144 Hz
  constexpr int64_t kT120 = 8'333'333;  // 120 Hz
  constexpr int64_t kT90 = 11'111'111;  // 90 Hz
  constexpr int64_t kT60 = 16'666'666;  // 60 Hz
  constexpr int64_t kStart = 5'000 * kSec + 123 * kMs;  // 任意的起點（不是 0，也不在整秒上）
  constexpr int64_t kI64Max = std::numeric_limits<int64_t>::max();
  constexpr int64_t kI64Min = std::numeric_limits<int64_t>::min();

  // timing ring 的事件代碼（vr_ipc_abi.h 的 VRIPC_TE_*）。寫死在這裡：標頭的常數被改掉時下面的 static_assert 會擋住
  constexpr uint32_t kEvVsyncVirtual = 1;
  constexpr uint32_t kEvPresentEnter = 3;
  constexpr uint32_t kEvPresentExit = 4;
  constexpr uint32_t kEvPostPresentEnter = 5;
  constexpr uint32_t kEvPostPresentExit = 6;
  constexpr int64_t kExitNoTarget = 4;  // PRESENT_EXIT.arg0：沒有輸出目標
  static_assert(k_ev_vsync_virtual == kEvVsyncVirtual && k_ev_present_enter == kEvPresentEnter && k_ev_present_exit == kEvPresentExit &&
                k_ev_postpresent_enter == kEvPostPresentEnter && k_ev_postpresent_exit == kEvPostPresentExit && k_exit_no_target == kExitNoTarget);

  /// 假的 bridge 執行緒：時間往前走、定期 on_tick()、每次 PostPresent 餵一筆
  struct sim_t {
    detector_t d;
    int64_t now = kStart;
    int64_t period;

    explicit sim_t(int64_t period_ns = kT120):
        period(period_ns) {
      d.reset(period, now);
    }

    /// 時間前進 dt_ns；這段期間沒有紀錄，照正式的節拍呼叫 on_tick()
    void advance(int64_t dt_ns) {
      const int64_t end = now + dt_ns;
      while (now + 5 * kMs < end) {
        now += 5 * kMs;
        d.on_tick(now);
      }
      now = end;
      d.on_tick(now);
    }

    /// 一張跨 slots 格的 Present：時間前進 slots × 週期後餵進去（slots 不合理時只前進一格）
    void present(int64_t slots = 1, int64_t throttle = 0, int64_t exit_code = 0) {
      advance((slots > 0 && slots < 1000 ? slots : 1) * period);
      present_t p;
      p.now_ns = now;
      p.slots = slots;
      p.exit_code = exit_code;
      p.throttle = throttle;
      d.on_present(p);
    }

    /// 連續 n 張
    void run(int n, int64_t slots = 1, int64_t throttle = 0) {
      for (int i = 0; i < n; ++i) {
        present(slots, throttle);
      }
    }

    /// 連續餵到再餵一張就會超過 dur_ns 為止
    void run_for(int64_t dur_ns, int64_t slots = 1, int64_t throttle = 0) {
      const int64_t end = now + dur_ns;
      while (now + slots * period <= end) {
        present(slots, throttle);
      }
    }

    /// 每格一張跑 11 s：locked，而且過了觀察期
    void settle() {
      run_for(11 * kSec);
    }

    state_e st() const {
      return d.state();
    }

    why_e why() const {
      return d.why();
    }
  };
}  // namespace

// ── 打包與新鮮度 ─────────────────────────────────────────────────────────

TEST(VrPresentCadencePack, DefaultIsZeroAndEveryStateRoundTrips) {
  EXPECT_EQ(pack(info_t {}), 0u);
  EXPECT_EQ(unpack(0), info_t {});
  for (int st = 0; st < 4; ++st) {
    for (int wy = 0; wy < 8; ++wy) {
      info_t i;
      i.state = static_cast<state_e>(st);
      i.why = static_cast<why_e>(wy);
      i.divisor = static_cast<uint8_t>(st + 1);
      i.slips_10s = static_cast<uint8_t>(wy * 4);
      i.stamp_ms = 0xFFFFFF00u + static_cast<uint32_t>(wy);
      EXPECT_EQ(unpack(pack(i)), i) << "state " << st << " why " << wy;
    }
  }
  // 位元的位置：bit 0-1 state、2-4 why、5-7 divisor、8-13 slips_10s、32-63 stamp_ms
  info_t i;
  i.state = state_e::slipping;  // 2
  i.why = why_e::heavy;  // 2
  i.divisor = 5;
  i.slips_10s = 32;
  i.stamp_ms = 0x89ABCDEFu;
  EXPECT_EQ(pack(i), 0x89ABCDEF000020AAull);  // 2 | 2 << 2 | 5 << 5 | 32 << 8
  i = info_t {};
  i.state = state_e::unsure;  // 3
  i.why = why_e::proving;  // 7
  EXPECT_EQ(pack(i), 0x1Full);
  // 讀的人只看低 32 bit 有沒有變（bridge 用它決定要不要重寫）：stamp 不在裡面
  i.stamp_ms = 0xFFFFFFFFu;
  EXPECT_EQ(static_cast<uint32_t>(pack(i)), 0x1Fu);
}

TEST(VrPresentCadencePack, FreshnessWindow) {
  info_t i;
  i.state = state_e::locked;
  i.stamp_ms = 5000;
  EXPECT_TRUE(fresh(i, 5000));
  EXPECT_TRUE(fresh(i, 6000));  // 1000 ms：還算新
  EXPECT_FALSE(fresh(i, 6001));
  // 讀的人的時鐘比寫的人蓋的時戳早：100 ms 以內算新
  EXPECT_TRUE(fresh(i, 4900));
  EXPECT_FALSE(fresh(i, 4899));
  // 可以另外指定期限
  EXPECT_TRUE(fresh(i, 5200, 200));
  EXPECT_FALSE(fresh(i, 5201, 200));
  // 時戳只留低 32 bit：跨過 2^32 也一樣
  i.stamp_ms = 0xFFFFFFF0u;
  EXPECT_TRUE(fresh(i, 100));
  EXPECT_TRUE(fresh(i, 0xFFFFFFF0u + 1000u));
  EXPECT_FALSE(fresh(i, 0xFFFFFFF0u + 1001u));
  EXPECT_TRUE(fresh(i, 0xFFFFFFF0u - 100u));
  EXPECT_FALSE(fresh(i, 0xFFFFFFF0u - 101u));
  // unknown 永遠不算新；unsure 可以（它是一個有內容的報告）
  i.state = state_e::unknown;
  EXPECT_FALSE(fresh(i, 0xFFFFFFF0u));
  i.state = state_e::unsure;
  EXPECT_TRUE(fresh(i, 0xFFFFFFF0u));
  i.state = state_e::slipping;
  EXPECT_TRUE(fresh(i, 0xFFFFFFF0u));
  EXPECT_FALSE(fresh(info_t {}, 0));
}

TEST(VrPresentCadencePack, LogNames) {
  // log 的詞彙（[VIPLE-VR-DRV] timing 10s 的 cadence=／why=）：slipping 印成 slip
  EXPECT_STREQ(state_name(state_e::unknown), "unknown");
  EXPECT_STREQ(state_name(state_e::locked), "locked");
  EXPECT_STREQ(state_name(state_e::slipping), "slip");
  EXPECT_STREQ(state_name(state_e::unsure), "unsure");
  const char *whys[] = {"none", "marginal", "heavy", "recovering", "recent", "fraction", "suspect", "proving"};
  for (int i = 0; i < 8; ++i) {
    EXPECT_STREQ(why_name(static_cast<why_e>(i)), whys[i]) << i;
  }
}

// ── 知道與否、觀察期 ─────────────────────────────────────────────────────

TEST(VrPresentCadenceKnown, NeedsTwentyFourRecordsThenTenSecondsOfProof) {
  for (const int64_t period : {kT120, kT90, kT60}) {
    SCOPED_TRACE(period);
    sim_t s(period);
    EXPECT_EQ(s.st(), state_e::unknown);
    s.run(23);
    EXPECT_EQ(s.st(), state_e::unknown);  // 第 23 筆
    EXPECT_EQ(static_cast<int>(s.d.info(s.now).divisor), 0);
    s.present();
    EXPECT_EQ(s.st(), state_e::unsure);  // 第 24 筆：有輸出了，但「鎖得住」要看滿 10 s 才說得出口
    EXPECT_EQ(s.why(), why_e::proving);
    EXPECT_EQ(static_cast<int>(s.d.info(s.now).divisor), 1);
    s.run_for(9800 * kMs);
    EXPECT_EQ(s.st(), state_e::unsure);
    EXPECT_EQ(s.why(), why_e::proving);
    s.run_for(400 * kMs);
    EXPECT_EQ(s.st(), state_e::locked);
    EXPECT_EQ(s.why(), why_e::none);
    EXPECT_EQ(s.d.divisor(), 1);
  }
}

TEST(VrPresentCadenceKnown, SlippingIsReportedWhileStillProving) {
  // 確定的證據不等觀察期：連線開起來不到 1 s 就有 6 個滑動
  sim_t s;
  s.run(30);
  for (int i = 0; i < 6; ++i) {
    s.present(2);
    s.run(10);
  }
  s.run_for(600 * kMs);
  EXPECT_EQ(s.st(), state_e::slipping);
}

// ── 臨界負載：10 s 內 4 個不屬於卡頓的滑動；12 s 沒有才清 ────────────────────

TEST(VrPresentCadenceMarginal, FourthSlipWithinTenSecondsSetsAndTwelveQuietSecondsClear) {
  sim_t s;
  s.settle();
  // 10 s 內 3 個單格：還是 locked
  for (int i = 0; i < 3; ++i) {
    s.present(2);
    s.run_for(2 * kSec);
  }
  EXPECT_EQ(s.st(), state_e::locked);
  EXPECT_EQ(static_cast<int>(s.d.info(s.now).slips_10s), 3);
  // 第 4 個落在第 1 個之後 10 s 內
  s.present(2);
  EXPECT_EQ(s.st(), state_e::locked);  // 要等下一筆 Present 說明那一段結束了才記
  s.present(1);
  EXPECT_EQ(s.st(), state_e::slipping);
  EXPECT_EQ(s.why(), why_e::marginal);
  // 最後一個滑動之後 12 s 才清
  s.run_for(11600 * kMs);
  EXPECT_EQ(s.st(), state_e::slipping);
  EXPECT_EQ(s.why(), why_e::marginal);
  s.run_for(600 * kMs);  // 12.2 s
  EXPECT_EQ(s.st(), state_e::locked);
}

TEST(VrPresentCadenceMarginal, OneSlipEvery3400MsNeverSets) {
  // 每 3.4 s 一個：任何 10 s 內最多 3 個
  sim_t s;
  s.settle();
  bool ever = false;
  for (int i = 0; i < 40; ++i) {
    s.present(2);
    s.run_for(3400 * kMs - 2 * kT120);
    ever = ever || s.st() == state_e::slipping;
  }
  EXPECT_FALSE(ever);
  EXPECT_EQ(s.st(), state_e::locked);
}

TEST(VrPresentCadenceMarginal, OneSlipEveryThreeSecondsSets) {
  // 每 3.0 s 一個：第 4 個落在第 1 個之後 9 s
  sim_t s;
  s.settle();
  for (int i = 0; i < 6; ++i) {
    s.present(2);
    s.run_for(3000 * kMs - 2 * kT120);
  }
  EXPECT_EQ(s.st(), state_e::slipping);
  EXPECT_EQ(s.why(), why_e::marginal);
}

// ── 重度：3 s 內的滑動達「3 s 的格數 ÷ 15」；結束後 3 s 沒有滑動就清 ─────────────

TEST(VrPresentCadenceHeavy, ThresholdIsOneFifteenthOfTheSlotsInThreeSeconds) {
  struct {
    int64_t period;
    int need;
  } cases[] = {{kT120, 24}, {kT90, 18}, {kT60, 12}};
  for (const auto &c : cases) {
    SCOPED_TRACE(c.period);
    sim_t s(c.period);
    s.settle();
    // 一個接一個的滑動（每隔一張）：need 個遠不到 3 s
    for (int i = 0; i < c.need - 1; ++i) {
      s.present(2);
      s.present(1);
    }
    EXPECT_EQ(s.st(), state_e::slipping);
    EXPECT_EQ(s.why(), why_e::marginal);  // 第 need − 1 個
    s.present(2);
    s.present(1);
    EXPECT_EQ(s.st(), state_e::slipping);
    EXPECT_EQ(s.why(), why_e::heavy);  // 第 need 個
  }
}

TEST(VrPresentCadenceHeavy, ThresholdIsClampedToEightAndThirtyTwo) {
  // 10 Hz：3 s 只有 30 格，÷ 15 是 2，夾到 8
  {
    sim_t s(100 * kMs);
    s.run(150);  // 15 s
    ASSERT_EQ(s.st(), state_e::locked);
    for (int i = 0; i < 7; ++i) {
      s.present(2);
      s.present(1);
    }
    EXPECT_EQ(s.why(), why_e::marginal);
    s.present(2);
    s.present(1);
    EXPECT_EQ(s.why(), why_e::heavy);
  }
  // 500 Hz：3 s 有 1500 格，÷ 15 是 100，夾到 32
  {
    sim_t s(2 * kMs);
    s.settle();
    for (int i = 0; i < 31; ++i) {
      s.present(2);
      s.present(1);
    }
    EXPECT_EQ(s.why(), why_e::marginal);
    s.present(2);
    s.present(1);
    EXPECT_EQ(s.why(), why_e::heavy);
  }
}

TEST(VrPresentCadenceHeavy, ClearsThreeSecondsAfterItEnds) {
  for (const int64_t period : {kT120, kT90, kT60}) {
    SCOPED_TRACE(period);
    sim_t s(period);
    s.settle();
    // 重度持續 8 s（每 3 張一個滑動），然後停
    const int64_t end = s.now + 8 * kSec;
    while (s.now < end) {
      s.present(2);
      s.present(1);
      s.present(1);
    }
    ASSERT_EQ(s.why(), why_e::heavy);
    const int64_t last = s.now - 2 * period;  // 最後一個滑動
    s.run_for(last + 2800 * kMs - s.now);
    EXPECT_EQ(s.st(), state_e::slipping);
    EXPECT_EQ(s.why(), why_e::recovering);
    s.run_for(last + 3300 * kMs - s.now);
    EXPECT_EQ(s.st(), state_e::locked);
  }
}

TEST(VrPresentCadenceHeavy, ShortBurstsDoNotStick) {
  // 3 s 的重度連續三次：每次結束後 3.6 s 都清得掉（重度期間的滑動不帶進臨界那一條）
  sim_t s;
  s.settle();
  for (int rep = 0; rep < 3; ++rep) {
    SCOPED_TRACE(rep);
    const int64_t end = s.now + 3 * kSec;
    while (s.now < end) {
      s.present(2);
      s.run(6);
    }
    EXPECT_EQ(s.st(), state_e::slipping);
    s.run_for(3600 * kMs);
    EXPECT_EQ(s.st(), state_e::locked);
    s.run_for(9 * kSec);
  }
}

TEST(VrPresentCadenceHeavy, DenseSlippingRightAfterAHitchIsHeavy) {
  // 負載改變的當下先卡一次、接著密集滑動：第一個 masked 滑動起是 unsure；3 s 內滿 24 個就是重度，即使每一個都是 masked
  sim_t s;
  s.settle();
  s.present(11);
  s.present(1);
  s.present(2);
  s.present(1);
  EXPECT_EQ(s.st(), state_e::unsure);
  EXPECT_EQ(s.why(), why_e::recent);
  int n = 1;
  while (s.st() != state_e::slipping && n < 100) {
    s.present(2);
    s.present(1);
    s.present(1);
    ++n;
  }
  EXPECT_EQ(n, 24);
  EXPECT_EQ(s.why(), why_e::heavy);
  const auto w = s.d.take_window();
  EXPECT_EQ(w.slips, 0u);
  EXPECT_GE(w.masked, 24u);
  EXPECT_EQ(w.hitches, 1u);
  EXPECT_EQ(w.skipped, 10u + w.masked);  // 卡頓多跨 10 格，每個 masked 滑動各 1 格
}

// ── 卡頓與它的餘波 ───────────────────────────────────────────────────────

TEST(VrPresentCadenceHitch, OneLongGapIsNotASlip) {
  sim_t s;
  s.settle();
  s.present(11);  // 一次 85 ms 的停頓
  EXPECT_EQ(s.st(), state_e::locked);
  s.run_for(5 * kSec);
  EXPECT_EQ(s.st(), state_e::locked);
  const auto w = s.d.take_window();
  EXPECT_EQ(w.hitches, 1u);
  EXPECT_EQ(w.slips, 0u);
  EXPECT_EQ(w.masked, 0u);
  EXPECT_EQ(w.skipped, 10u);
  EXPECT_EQ(static_cast<int>(s.d.info(s.now).slips_10s), 0);
}

TEST(VrPresentCadenceHitch, AftershocksAreMaskedAndKeepItUnsureForTwelveSeconds) {
  sim_t s;
  s.settle();
  s.present(11);
  EXPECT_EQ(s.st(), state_e::locked);
  s.run(60);
  // 卡頓後 0.5～1.3 s 的 9 個單格：masked，狀態是「說不準」
  for (int i = 0; i < 9; ++i) {
    s.present(2);
    s.run(8);
  }
  EXPECT_EQ(s.st(), state_e::unsure);
  EXPECT_EQ(s.why(), why_e::recent);
  const int64_t last = s.now - 8 * kT120;  // 最後一個餵進去的滑動（記下來的時刻晚一格）
  s.run_for(last + 11500 * kMs - s.now);
  EXPECT_EQ(s.st(), state_e::unsure);  // 到最後一個 masked 滑動之後 12 s 為止
  EXPECT_EQ(s.why(), why_e::recent);
  s.run_for(last + 12300 * kMs - s.now);
  EXPECT_EQ(s.st(), state_e::locked);
  const auto w = s.d.take_window();
  EXPECT_EQ(w.hitches, 1u);
  EXPECT_EQ(w.masked, 9u);
  EXPECT_EQ(w.slips, 0u);
}

TEST(VrPresentCadenceHitch, ASingleMaskedSlipAlsoKeepsItUnsure) {
  sim_t s;
  s.settle();
  s.present(11);
  s.run(60);
  s.present(2);
  const int64_t t = s.now;
  s.present(1);
  EXPECT_EQ(s.st(), state_e::unsure);
  EXPECT_EQ(s.why(), why_e::recent);
  s.run_for(t + 11800 * kMs - s.now);
  EXPECT_EQ(s.st(), state_e::unsure);
  s.run_for(t + 12300 * kMs - s.now);
  EXPECT_EQ(s.st(), state_e::locked);
}

TEST(VrPresentCadenceHitch, TheMaskLastsThreeSeconds) {
  // 卡頓後 2.6 s 的滑動還是 masked；3.1 s 的不是
  {
    sim_t s;
    s.settle();
    s.present(11);
    s.run_for(2600 * kMs);
    s.present(2);
    s.present(1);
    const auto w = s.d.take_window();
    EXPECT_EQ(w.masked, 1u);
    EXPECT_EQ(w.slips, 0u);
    EXPECT_EQ(s.st(), state_e::unsure);
  }
  {
    sim_t s;
    s.settle();
    s.present(11);
    s.run_for(3100 * kMs);
    s.present(2);
    s.present(1);
    const auto w = s.d.take_window();
    EXPECT_EQ(w.masked, 0u);
    EXPECT_EQ(w.slips, 1u);
    EXPECT_EQ(s.st(), state_e::locked);
  }
}

TEST(VrPresentCadenceHitch, SlipsLaterThanThreeSecondsCount) {
  sim_t s;
  s.settle();
  s.present(11);
  s.run_for(3100 * kMs);
  for (int i = 0; i < 4; ++i) {
    s.present(2);
    s.run(8);
  }
  EXPECT_EQ(s.st(), state_e::slipping);
  EXPECT_EQ(s.why(), why_e::marginal);
}

TEST(VrPresentCadenceHitch, SlipsBeforeAHitchAreOrdinary) {
  // 只有卡頓「之後」3 s 的滑動才 masked；之前的照算
  sim_t s;
  s.settle();
  s.present(2);
  s.run(6);
  s.present(2);
  s.run(6);
  s.present(11);
  s.run_for(kSec);
  const auto w = s.d.take_window();
  EXPECT_EQ(w.masked, 0u);
  EXPECT_EQ(w.slips, 2u);
  EXPECT_EQ(w.hitches, 1u);
  EXPECT_EQ(s.st(), state_e::locked);
}

TEST(VrPresentCadenceHitch, ClassificationByRefreshRate) {
  // 卡頓＝比節拍多跨至少 2 格，而且至少多 16 ms：120／90／60 Hz 是多 2 格（跨 3 格），144 Hz 是多 3 格（跨 4 格）
  struct {
    int64_t period;
    int64_t slip_slots;
    int64_t hitch_slots;
  } cases[] = {{kT120, 2, 3}, {kT90, 2, 3}, {kT60, 2, 3}, {kT144, 3, 4}};
  for (const auto &c : cases) {
    SCOPED_TRACE(c.period);
    sim_t s(c.period);
    s.settle();
    s.present(c.slip_slots);
    s.run(8);
    s.present(c.hitch_slots);
    s.run_for(kSec);
    const auto w = s.d.take_window();
    EXPECT_EQ(w.hitches, 1u);
    EXPECT_EQ(w.masked, 0u);
    EXPECT_EQ(w.slips, 1u);
  }
}

TEST(VrPresentCadenceHitch, AftershocksDoNotKeepTheFlagSet) {
  // 鎖死反例：每 5 s 一次卡頓、各帶 6 個 masked 的餘波，中間有「一次」8 個不屬於卡頓的滑動。
  // 旗標在那一次設起來，之後卡頓照舊，12 s 後還是清得掉（清除只看不屬於卡頓的滑動）
  sim_t s;
  s.settle();
  const auto hitch_cycle = [&s]() {
    const int64_t t0 = s.now;
    s.present(11);
    s.run_for(900 * kMs);
    for (int i = 0; i < 6; ++i) {
      s.present(2);
      s.run(6);
    }
    s.run_for(t0 + 5 * kSec - s.now);
  };
  for (int i = 0; i < 6; ++i) {
    hitch_cycle();
    EXPECT_NE(s.st(), state_e::slipping) << i;
  }
  {
    // 那一次：放在卡頓後 3.5 s，不會被遮罩
    const int64_t t0 = s.now;
    s.present(11);
    s.run_for(3500 * kMs);
    for (int i = 0; i < 8; ++i) {
      s.present(2);
      s.run(5);
    }
    s.run_for(t0 + 5 * kSec - s.now);
  }
  EXPECT_EQ(s.st(), state_e::slipping);
  const int64_t burst_end = s.now;
  bool cleared = false;
  int64_t cleared_after = 0;
  for (int i = 0; i < 20 && !cleared; ++i) {
    hitch_cycle();
    if (s.st() != state_e::slipping) {
      cleared = true;
      cleared_after = s.now - burst_end;
    }
  }
  EXPECT_TRUE(cleared);
  EXPECT_LE(cleared_after, 15 * kSec);
}

TEST(VrPresentCadenceHitch, HiddenSlippingIsNeverReportedLocked) {
  // 藏住反例：每秒 1.6 個滑動（約 118 fps）加上每 2.5 s 一次卡頓——每一個滑動都落在某次卡頓後 3 s 內。
  // 分不出是餘波還是真的滑動，就不能說「鎖得住」
  sim_t s;
  s.settle();
  std::mt19937_64 g(7);
  int64_t next_hitch = s.now;
  bool locked_seen = false;
  const int64_t end = s.now + 120 * kSec;
  const int64_t grace = s.now + 3 * kSec;
  while (s.now < end) {
    if (s.now >= next_hitch) {
      s.present(11);
      next_hitch += 2500 * kMs;
    } else {
      s.present(g() % 75 == 0 ? 2 : 1);
    }
    if (s.now > grace && s.st() == state_e::locked) {
      locked_seen = true;
    }
  }
  EXPECT_FALSE(locked_seen);
}

// ── 更新率的整數分之一 ───────────────────────────────────────────────────

TEST(VrPresentCadenceFraction, ThrottledHalfRateIsAdoptedAfterTwentyFourFrames) {
  // SteamVR 節流：每一張都跨 1 + nFramesToThrottle 格，而且紀錄裡的節流值這樣說
  sim_t s;
  s.settle();
  s.run(23, 2, 1);
  EXPECT_EQ(s.d.divisor(), 1);
  s.present(2, 1);
  EXPECT_EQ(s.d.divisor(), 2);  // 第 24 張
  s.run_for(5 * kSec, 2, 1);
  EXPECT_EQ(s.st(), state_e::locked);
  EXPECT_EQ(s.d.divisor(), 2);
  EXPECT_EQ(static_cast<int>(s.d.info(s.now).divisor), 2);
  auto w = s.d.take_window();
  EXPECT_EQ(w.slips, 0u);  // 節流開始的那 24 張一個事件都不產生
  EXPECT_EQ(w.masked, 0u);
  EXPECT_EQ(w.hitches, 0u);
  EXPECT_EQ(w.divisor_changes, 1u);
  EXPECT_EQ(w.throttle_max, 1);
  // 相對新節拍的偏離：跨 3 格是滑動，跨 1 格是 early（也算滑動）
  s.present(3, 1);
  s.run(10, 2, 1);
  s.present(1, 1);
  s.run_for(kSec, 2, 1);
  w = s.d.take_window();
  EXPECT_EQ(w.slips, 2u);
  EXPECT_EQ(w.early, 1u);
  EXPECT_EQ(w.skipped, 1u);
  // 回到全速
  s.run(24, 1, 0);
  EXPECT_EQ(s.d.divisor(), 1);
  s.run_for(kSec);
  w = s.d.take_window();
  EXPECT_EQ(w.slips, 0u);
  EXPECT_EQ(w.divisor_changes, 1u);
  EXPECT_EQ(s.st(), state_e::locked);
}

TEST(VrPresentCadenceFraction, UnexplainedPlateauIsUnsure) {
  // 穩定在 1/2、但節流值是 0（GPU 受限剛好在半速附近、或遊戲自己每兩格交一張）：說不準，不記任何滑動
  sim_t s;
  s.settle();
  s.run(23, 2, 0);
  EXPECT_EQ(s.st(), state_e::locked);
  s.present(2, 0);
  EXPECT_EQ(s.st(), state_e::unsure);
  EXPECT_EQ(s.why(), why_e::fraction);
  EXPECT_EQ(s.d.divisor(), 1);
  s.run_for(10 * kSec, 2, 0);
  EXPECT_EQ(s.st(), state_e::unsure);
  EXPECT_EQ(s.why(), why_e::fraction);
  auto w = s.d.take_window();
  EXPECT_EQ(w.slips, 0u);
  EXPECT_EQ(w.masked, 0u);
  EXPECT_EQ(w.divisor_changes, 0u);
  // 回到每格一張：連續 24 張在節拍上才清
  s.run(23);
  EXPECT_EQ(s.st(), state_e::unsure);
  s.present();
  EXPECT_EQ(s.st(), state_e::locked);
  w = s.d.take_window();
  EXPECT_EQ(w.slips, 0u);
}

TEST(VrPresentCadenceFraction, TwoTwoTwoTwoThreeIsSlipping) {
  // nFramesToThrottle ≥ 2 時 driver 的睡眠被 2T 截斷：2,2,2,2,3——不在格點上，鎖不住
  sim_t s;
  s.settle();
  const int64_t t0 = s.now;
  const int64_t pattern[] = {2, 2, 2, 2, 3};
  int64_t first = -1;
  for (int i = 0; i < 200; ++i) {
    s.present(pattern[i % 5], 2);
    if (first < 0 && s.st() == state_e::slipping) {
      first = s.now - t0;
    }
  }
  EXPECT_GE(first, 0);
  EXPECT_LE(first, 800 * kMs);
  EXPECT_EQ(s.d.divisor(), 1);
  EXPECT_EQ(s.st(), state_e::slipping);
}

TEST(VrPresentCadenceFraction, AShortRunIsASeriesOfSlips) {
  // 不滿 24 張就回到節拍：那 10 張是 10 個滑動
  sim_t s;
  s.settle();
  s.run(10, 2, 1);
  s.run_for(kSec);
  const auto w = s.d.take_window();
  EXPECT_EQ(w.slips, 10u);
  EXPECT_EQ(w.divisor_changes, 0u);
  EXPECT_EQ(s.st(), state_e::slipping);
}

TEST(VrPresentCadenceFraction, AnInterruptedRunIsNotAdopted) {
  // 23 張（2 格、節流 1）、一張在節拍上、再 23 張：沒有連續 24 張，不採用，46 個滑動
  sim_t s;
  s.settle();
  s.run(23, 2, 1);
  s.present(1, 0);
  s.run(23, 2, 1);
  s.present(1, 0);
  EXPECT_EQ(s.d.divisor(), 1);
  const auto w = s.d.take_window();
  EXPECT_EQ(w.slips, 46u);
  EXPECT_EQ(w.divisor_changes, 0u);
  EXPECT_EQ(s.st(), state_e::slipping);
  EXPECT_EQ(s.why(), why_e::heavy);
}

TEST(VrPresentCadenceFraction, APlateauThatBecomesExplainedIsAdopted) {
  // 先是解釋不了的平台，之後節流值開始對得上：要連續 24 張都對得上才採用
  sim_t s;
  s.settle();
  s.run(30, 2, 0);
  EXPECT_EQ(s.st(), state_e::unsure);
  EXPECT_EQ(s.why(), why_e::fraction);
  EXPECT_EQ(s.d.divisor(), 1);
  s.run(23, 2, 1);
  EXPECT_EQ(s.d.divisor(), 1);
  s.present(2, 1);
  EXPECT_EQ(s.d.divisor(), 2);
  EXPECT_EQ(s.st(), state_e::locked);
  const auto w = s.d.take_window();
  EXPECT_EQ(w.slips, 0u);
  EXPECT_EQ(w.masked, 0u);
  EXPECT_EQ(w.hitches, 0u);
  EXPECT_EQ(w.divisor_changes, 1u);
}

TEST(VrPresentCadenceFraction, OutOfRangeThrottleExplainsNothing) {
  sim_t s;
  s.settle();
  s.run(40, 6, 5);  // 1 + 5 = 6，但 driver 把節流值夾在 4 以內，6 格也超出可以採用的節拍
  EXPECT_EQ(s.d.divisor(), 1);
  s.run(40, 2, -1);
  EXPECT_EQ(s.d.divisor(), 1);
  s.run(40, 2, kI64Max);  // 1 + throttle 不可以溢位
  EXPECT_EQ(s.d.divisor(), 1);
  s.run(40, 2, kI64Min);
  EXPECT_EQ(s.d.divisor(), 1);
  EXPECT_EQ(s.d.take_window().divisor_changes, 0u);
}

TEST(VrPresentCadenceFraction, AdoptingWhileSetWindsDownInThreeSeconds) {
  // 旗標設著的時候開始節流：新的節拍是穩定的，比照重度結束（3 s 後清）
  sim_t s;
  s.settle();
  for (int i = 0; i < 40; ++i) {
    s.present(2);
    s.present(1);
  }
  EXPECT_EQ(s.st(), state_e::slipping);
  s.run(24, 2, 1);
  EXPECT_EQ(s.d.divisor(), 2);
  EXPECT_EQ(s.st(), state_e::slipping);
  EXPECT_EQ(s.why(), why_e::recovering);
  s.run_for(2800 * kMs, 2, 1);
  EXPECT_EQ(s.st(), state_e::slipping);
  s.run_for(400 * kMs, 2, 1);
  EXPECT_EQ(s.st(), state_e::locked);
}

TEST(VrPresentCadenceFraction, AdoptingWhileMarginalAlsoWindsDownInThreeSeconds) {
  // 同上，但旗標是臨界那一條設的（原本要 12 s 才清）：採用新節拍之後一樣是 3 s
  sim_t s;
  s.settle();
  for (int i = 0; i < 4; ++i) {
    s.present(2);
    s.run(8);
  }
  ASSERT_EQ(s.st(), state_e::slipping);
  ASSERT_EQ(s.why(), why_e::marginal);
  s.run(24, 2, 1);
  EXPECT_EQ(s.d.divisor(), 2);
  EXPECT_EQ(s.st(), state_e::slipping);
  EXPECT_EQ(s.why(), why_e::recovering);
  s.run_for(2800 * kMs, 2, 1);
  EXPECT_EQ(s.st(), state_e::slipping);
  s.run_for(400 * kMs, 2, 1);
  EXPECT_EQ(s.st(), state_e::locked);
}

TEST(VrPresentCadenceFraction, GpuBoundNearHalfRateIsNeverLocked) {
  // GPU 受限、剛好在半速附近自由跑（沒有節流）：30～230 張跨 2 格的平台，中間夾單獨的 3 或 1。
  // 不採用成節拍，也從不報 locked
  sim_t s;
  s.settle();
  std::mt19937_64 g(11);
  bool locked_seen = false;
  const int64_t grace = s.now + kSec;
  for (int rep = 0; rep < 60; ++rep) {
    s.run(30 + static_cast<int>(g() % 200), 2, 0);
    s.present(g() % 2 == 0 ? 3 : 1, 0);
    if (s.now > grace && s.st() == state_e::locked) {
      locked_seen = true;
    }
  }
  EXPECT_FALSE(locked_seen);
  EXPECT_EQ(s.d.divisor(), 1);
}

TEST(VrPresentCadenceFraction, FarTooLongToBeACadenceBecomesSlipping) {
  // 每一張都跨 17 格（7 fps）：長到不可能是節拍，逐筆分類；連續 12 張之後已經沒有節拍可回，長間隔也算滑動
  sim_t s;
  s.settle();
  const int64_t t0 = s.now;
  int64_t first = -1;
  bool locked_late = false;
  for (int i = 0; i < 400; ++i) {
    s.present(17);
    if (first < 0 && s.st() == state_e::slipping) {
      first = s.now - t0;
    }
    if (s.now - t0 > 8 * kSec && s.st() == state_e::locked) {
      locked_late = true;
    }
  }
  EXPECT_GE(first, 0);
  EXPECT_LE(first, 8 * kSec);
  EXPECT_FALSE(locked_late);
}

TEST(VrPresentCadenceFraction, FreeRunAtTwentyFiveFps) {
  // 120 Hz 下 25 fps 自由跑（5,5,5,5,4）
  sim_t s;
  s.settle();
  const int64_t t0 = s.now;
  const int64_t pattern[] = {5, 5, 5, 5, 4};
  int64_t first = -1;
  for (int i = 0; i < 300; ++i) {
    s.present(pattern[i % 5]);
    if (first < 0 && s.st() == state_e::slipping) {
      first = s.now - t0;
    }
  }
  EXPECT_GE(first, 0);
  EXPECT_LE(first, 5 * kSec);
}

// ── 有效性 ───────────────────────────────────────────────────────────────

TEST(VrPresentCadenceValidity, TwelveRecordsWithoutATargetStartOver) {
  sim_t s;
  s.settle();
  ASSERT_EQ(s.st(), state_e::locked);
  for (int i = 0; i < 11; ++i) {
    s.present(1, 0, kExitNoTarget);
  }
  EXPECT_EQ(s.st(), state_e::locked);  // 連續 11 筆沒有輸出目標：狀態還在
  s.present(1, 0, kExitNoTarget);
  EXPECT_EQ(s.st(), state_e::unknown);  // 第 12 筆：重來
  s.run(23);
  EXPECT_EQ(s.st(), state_e::unknown);
  s.present();
  EXPECT_EQ(s.st(), state_e::unsure);  // 重來之後同樣要 24 筆、再觀察 10 s
  EXPECT_EQ(s.why(), why_e::proving);
  s.run_for(9500 * kMs);
  EXPECT_EQ(s.st(), state_e::unsure);
  s.run_for(700 * kMs);
  EXPECT_EQ(s.st(), state_e::locked);
  const auto w = s.d.take_window();
  EXPECT_EQ(w.no_target, 12u);
}

TEST(VrPresentCadenceValidity, ARecordWithoutATargetEveryTwentyKeepsItUnknown) {
  // 沒有輸出目標的紀錄不算進「24 筆可用的紀錄」，而且讓那個計數歸零
  sim_t s;
  for (int i = 0; i < 100; ++i) {
    s.run(20);
    s.present(1, 0, kExitNoTarget);
    ASSERT_EQ(s.st(), state_e::unknown) << i;
  }
}

TEST(VrPresentCadenceValidity, RecordsStopping) {
  sim_t s;
  s.settle();
  s.advance(480 * kMs);
  EXPECT_EQ(s.st(), state_e::locked);  // 480 ms 沒有紀錄：還在
  s.advance(60 * kMs);
  EXPECT_EQ(s.st(), state_e::unknown);  // 540 ms
  EXPECT_EQ(static_cast<int>(s.d.info(s.now).divisor), 0);
  s.present(60);  // 2 s 內恢復：只是一次很長的卡頓，狀態還在
  EXPECT_EQ(s.st(), state_e::locked);
  s.advance(2100 * kMs);
  EXPECT_EQ(s.st(), state_e::unknown);  // 超過 2 s：全部重來
  s.present();
  s.run(23);
  EXPECT_EQ(s.st(), state_e::unsure);
  EXPECT_EQ(s.why(), why_e::proving);
}

TEST(VrPresentCadenceValidity, ALongGapStartsOverEvenWithoutTicks) {
  // 呼叫端沒有照節拍呼叫 on_tick() 的時候，on_present() 自己也看得出紀錄停過 2 s
  sim_t s;
  s.settle();
  for (int i = 0; i < 4; ++i) {
    s.present(2);
    s.run(8);
  }
  ASSERT_EQ(s.st(), state_e::slipping);
  s.now += 2500 * kMs;  // 不經過 advance()：這段時間沒有任何呼叫
  present_t p;
  p.now_ns = s.now;
  p.slots = 1;
  s.d.on_present(p);
  EXPECT_EQ(s.st(), state_e::unknown);
  s.run(22);
  EXPECT_EQ(s.st(), state_e::unknown);
  s.present();
  EXPECT_EQ(s.st(), state_e::unsure);
  EXPECT_EQ(s.why(), why_e::proving);
}

TEST(VrPresentCadenceValidity, ReAnchorAfterMoreThanTwoSeconds) {
  // driver 停頓超過 2 s 就重新起算格點：120 Hz 下跨 241 格還只是很長的卡頓，242 格就重來
  sim_t s;
  s.settle();
  present_t p;
  s.advance(kSec);
  p.now_ns = s.now;
  p.slots = 241;
  s.d.on_present(p);
  EXPECT_EQ(s.st(), state_e::locked);
  s.advance(kSec);
  p.now_ns = s.now;
  p.slots = 242;
  s.d.on_present(p);
  EXPECT_EQ(s.st(), state_e::unknown);
  const auto w = s.d.take_window();
  EXPECT_EQ(w.restarts, 1u);
  EXPECT_EQ(w.hitches, 1u);
  EXPECT_EQ(w.skipped, 240u);  // 重來的那一筆不是可用的紀錄，不計
}

TEST(VrPresentCadenceValidity, UnreasonablePeriodsFallBackToNinetyHz) {
  // 不在 2～100 ms 內的週期用 90 Hz（2 s 是 180 格）：跨 181 格是卡頓、182 格重來；多跨 2 格就是卡頓
  for (const int64_t period : {int64_t {0}, int64_t {-5}, int64_t {1}, 2 * kMs - 1, 100 * kMs + 1, kI64Max, kI64Min}) {
    SCOPED_TRACE(period);
    detector_t d;
    int64_t now = kStart;
    d.reset(period, now);
    present_t p;
    for (int i = 0; i < 40; ++i) {
      now += kT90;
      p.now_ns = now;
      p.slots = 1;
      d.on_present(p);
    }
    now += 3 * kT90;
    p.now_ns = now;
    p.slots = 3;
    d.on_present(p);
    now += kSec;
    p.now_ns = now;
    p.slots = 181;
    d.on_present(p);
    auto w = d.take_window();
    EXPECT_EQ(w.hitches, 2u);
    EXPECT_EQ(w.restarts, 0u);
    now += kSec;
    p.now_ns = now;
    p.slots = 182;
    d.on_present(p);
    w = d.take_window();
    EXPECT_EQ(w.restarts, 1u);
  }
  // 沒有 reset() 過的物件也是 90 Hz 的設定
  detector_t d;
  int64_t now = kStart;
  present_t p;
  for (int i = 0; i < 40; ++i) {
    now += kT90;
    p.now_ns = now;
    p.slots = 1;
    d.on_present(p);
  }
  now += 3 * kT90;
  p.now_ns = now;
  p.slots = 3;
  d.on_present(p);
  now += kSec;
  p.now_ns = now;
  p.slots = 181;
  d.on_present(p);
  auto w = d.take_window();
  EXPECT_EQ(w.hitches, 2u);
  EXPECT_EQ(w.restarts, 0u);
  now += kSec;
  p.now_ns = now;
  p.slots = 182;
  d.on_present(p);
  w = d.take_window();
  EXPECT_EQ(w.restarts, 1u);
}

TEST(VrPresentCadenceValidity, OnlyTicksIsUnknown) {
  sim_t s;
  s.advance(30 * kSec);
  EXPECT_EQ(s.st(), state_e::unknown);
  const info_t i = s.d.info(s.now);
  EXPECT_EQ(i.state, state_e::unknown);
  EXPECT_EQ(i.why, why_e::none);
  EXPECT_EQ(static_cast<int>(i.divisor), 0);
  EXPECT_EQ(static_cast<int>(i.slips_10s), 0);
  EXPECT_FALSE(fresh(i, i.stamp_ms));
}

TEST(VrPresentCadenceValidity, ClockGoingBackwardsChangesNothing) {
  sim_t s;
  s.settle();
  for (int i = 0; i < 4; ++i) {
    s.present(2);
    s.run(8);
  }
  ASSERT_EQ(s.st(), state_e::slipping);
  s.d.on_tick(s.now - 3600 * kSec);
  EXPECT_EQ(s.st(), state_e::slipping);
  present_t p;
  p.now_ns = s.now - 100 * kSec;
  p.slots = 1;
  s.d.on_present(p);
  EXPECT_EQ(s.st(), state_e::slipping);
  s.run_for(11 * kSec);
  EXPECT_EQ(s.st(), state_e::slipping);  // 12 s 從最後一個滑動起算，不是從那兩個亂掉的時刻
  s.run_for(2 * kSec);
  EXPECT_EQ(s.st(), state_e::locked);
}

TEST(VrPresentCadenceValidity, StaleKeepsTheFlag) {
  // 0.5～2 s 沒有紀錄：報 unknown，但旗標先留著——紀錄回來時還是 slipping
  sim_t s;
  s.settle();
  for (int i = 0; i < 4; ++i) {
    s.present(2);
    s.run(8);
  }
  ASSERT_EQ(s.st(), state_e::slipping);
  s.advance(1500 * kMs);
  EXPECT_EQ(s.st(), state_e::unknown);
  s.present(1);
  EXPECT_EQ(s.st(), state_e::slipping);
}

// ── 不可信的輸入 ─────────────────────────────────────────────────────────

TEST(VrPresentCadenceSuspect, ThirdImpossibleValueInTenSeconds) {
  // 10 s 內 2 筆不可能的值可以接受（Activate、一次重新起算），第 3 筆就不信了
  sim_t s;
  s.settle();
  s.present(0);
  s.run(50);
  s.present(-3);
  s.run(50);
  EXPECT_EQ(s.st(), state_e::locked);
  EXPECT_FALSE(s.d.take_suspect_alert());
  s.present(0);
  EXPECT_EQ(s.st(), state_e::unsure);
  EXPECT_EQ(s.why(), why_e::suspect);
  EXPECT_TRUE(s.d.take_suspect_alert());
  EXPECT_FALSE(s.d.take_suspect_alert());  // 只回一次
  s.run_for(9 * kSec);
  EXPECT_EQ(s.st(), state_e::unsure);
  EXPECT_FALSE(s.d.take_suspect_alert());
  s.run_for(2500 * kMs);
  EXPECT_EQ(s.st(), state_e::locked);
  const auto w = s.d.take_window();
  EXPECT_EQ(w.zeros, 2u);
  EXPECT_EQ(w.invalid, 1u);
  EXPECT_EQ(w.slips, 0u);
}

TEST(VrPresentCadenceSuspect, SuspectWinsOverSlipping) {
  // 紀錄本身不可信時，從它算出來的「鎖不住」也不可信
  sim_t s;
  s.settle();
  for (int i = 0; i < 40; ++i) {
    s.present(2);
    s.present(1);
  }
  EXPECT_EQ(s.st(), state_e::slipping);
  s.present(0);
  s.present(0);
  s.present(0);
  EXPECT_EQ(s.st(), state_e::unsure);
  EXPECT_EQ(s.why(), why_e::suspect);
}

TEST(VrPresentCadenceSuspect, ADriverCountingFromZeroIsNeverTrusted) {
  // driver 把 arg1 改成從 0 起算（準時＝0、晚一格＝1），約 106 fps：不可以被當成鎖住，也不可以下任何結論
  sim_t s;
  std::mt19937_64 g(3);
  bool concluded = false;
  for (int i = 0; i < 12000; ++i) {
    const int64_t slots = g() % 9 == 0 ? 2 : 1;
    s.advance(slots * s.period);
    present_t p;
    p.now_ns = s.now;
    p.slots = slots - 1;
    s.d.on_present(p);
    if (i > 400 && (s.st() == state_e::locked || s.st() == state_e::slipping)) {
      concluded = true;
    }
  }
  EXPECT_FALSE(concluded);
  EXPECT_EQ(s.st(), state_e::unsure);
  EXPECT_EQ(s.why(), why_e::suspect);
  EXPECT_TRUE(s.d.take_suspect_alert());
}

TEST(VrPresentCadenceSuspect, OnePercentGarbageIsNeverLocked) {
  // 約 106 fps 的滑動裡混進 1% 的 0xFFFFFFFF
  sim_t s;
  s.settle();
  std::mt19937_64 g(5);
  bool locked_seen = false;
  const int64_t grace = s.now + 3 * kSec;
  for (int i = 0; i < 12000; ++i) {
    const int64_t slots = g() % 9 == 0 ? 2 : 1;
    s.advance(slots * s.period);
    present_t p;
    p.now_ns = s.now;
    p.slots = g() % 100 == 0 ? int64_t {0xFFFFFFFF} : slots;
    s.d.on_present(p);
    if (s.now > grace && s.st() == state_e::locked) {
      locked_seen = true;
    }
  }
  EXPECT_FALSE(locked_seen);
}

// ── PostPresent 不等虛擬 vsync 的那一段（dev 的節拍模式；§VR-A-KICK 的實驗每次約 20 ms）───────

namespace {
  struct burst_t {
    int zeros = 0;  ///< 餵了幾筆 slots＝0
    int zeros_after = 0;  ///< 其中在寫回正式模式之後的
  };

  /**
   * @brief 一段「PostPresent 不等虛擬 vsync」：合成器每 round_ns 就再跑一輪，持續 dur_ns。照 driver 的算法
   *        （steamvr_driver/virtual_vsync.cpp 的 step()）：時間過了下一個格點的那一輪才算跨了幾格，同一格裡其餘的
   *        幾輪 slots 是 0。寫回正式模式之後 driver 要到下一次 PostPresent 才讀到，所以還會照舊再跑一輪才睡到格點。
   *        進來時 s.now 當成剛過的格點；出去時停在睡醒的那個格點，那一張（slots 1）已經餵了。
   * @param announce 呼叫端有沒有用 set_free_run() 先說（false＝偵測器不知道節拍模式被改過）
   */
  burst_t free_run_burst(sim_t &s, int64_t dur_ns, int64_t round_ns, bool announce = true) {
    burst_t b;
    int64_t next = s.now + s.period;  // 下一個格點
    bool written_back = false;
    const auto round = [&]() {
      s.advance(round_ns);
      present_t p;
      p.now_ns = s.now;
      if (s.now >= next) {
        p.slots = (s.now - next) / s.period + 1;
        next += p.slots * s.period;
      } else {
        ++b.zeros;  // p.slots 是 0
        b.zeros_after += written_back ? 1 : 0;
      }
      s.d.on_present(p);
    };
    if (announce) {
      s.d.set_free_run(true);
    }
    const int64_t end = s.now + dur_ns;
    while (s.now < end) {
      round();
    }
    if (announce) {
      s.d.set_free_run(false);
    }
    written_back = true;
    round();
    s.advance(next - s.now);  // 睡到格點
    present_t p;
    p.now_ns = s.now;
    p.slots = 1;
    s.d.on_present(p);
    return b;
  }
}  // namespace

TEST(VrPresentCadenceFreeRun, ZerosWhilePostPresentDoesNotWaitAreNotImpossibleValues) {
  // 一次 20 ms 的實驗、合成器 3 ms 一輪：8 輪裡有 6 筆 0。呼叫端先說了：不算不可能的值，節拍照常
  sim_t s;
  s.settle();
  s.d.take_window();
  const burst_t b = free_run_burst(s, 20 * kMs, 3 * kMs);
  ASSERT_EQ(b.zeros, 6);
  ASSERT_EQ(b.zeros_after, 1);  // 寫回之後 driver 還沒讀到的那一輪
  EXPECT_EQ(s.st(), state_e::locked);
  s.run(50);
  EXPECT_EQ(s.st(), state_e::locked);
  EXPECT_FALSE(s.d.take_suspect_alert());
  auto w = s.d.take_window();
  EXPECT_EQ(w.free_run, 6u);
  EXPECT_EQ(w.zeros, 0u);
  EXPECT_EQ(w.slips + w.masked + w.hitches + w.skipped, 0u);
  EXPECT_EQ(w.ok + w.free_run, w.records);  // 其餘每一筆都在節拍上：每個格點還是恰好一筆
  EXPECT_EQ(w.ok, 53u);  // 實驗跨過的 2 個格點＋睡醒的那一張＋之後的 50 張
  // 同樣的紀錄，但沒有人告訴偵測器節拍模式被改過：第 3 筆 0 起整個不信，一直到 10 s 之後
  sim_t u;
  u.settle();
  u.d.take_window();
  const burst_t ub = free_run_burst(u, 20 * kMs, 3 * kMs, false);
  EXPECT_EQ(ub.zeros, 6);
  EXPECT_EQ(u.st(), state_e::unsure);
  EXPECT_EQ(u.why(), why_e::suspect);
  EXPECT_TRUE(u.d.take_suspect_alert());
  u.run_for(9 * kSec);
  EXPECT_EQ(u.why(), why_e::suspect);
  w = u.d.take_window();
  EXPECT_EQ(w.zeros, 6u);
  EXPECT_EQ(w.free_run, 0u);
}

TEST(VrPresentCadenceFreeRun, AnExperimentEveryFiveSecondsNeverMakesItSuspect) {
  // 每 5 s 一次、連續 2 分鐘（vr_dev_kick_every_s 的下限）：整場都是 locked，一次警告都沒有
  for (const int64_t period : {kT120, kT60}) {
    SCOPED_TRACE(period);
    sim_t s(period);
    s.settle();
    s.d.take_window();
    int zeros = 0;
    for (int i = 0; i < 24; ++i) {
      s.run_for(5 * kSec);
      ASSERT_EQ(s.st(), state_e::locked) << i;
      zeros += free_run_burst(s, 20 * kMs, 3 * kMs).zeros;
      ASSERT_EQ(s.st(), state_e::locked) << i;
      ASSERT_FALSE(s.d.take_suspect_alert()) << i;
    }
    EXPECT_GE(zeros, 24 * 6);
    const auto w = s.d.take_window();
    EXPECT_EQ(w.free_run, static_cast<uint32_t>(zeros));
    EXPECT_EQ(w.zeros, 0u);
    EXPECT_EQ(w.slips + w.masked + w.hitches + w.skipped, 0u);
    EXPECT_EQ(static_cast<int>(s.d.info(s.now).slips_10s), 0);
  }
}

TEST(VrPresentCadenceFreeRun, TheGraceAfterWritingTheModeBackIsOneHundredMilliseconds) {
  detector_t d;
  int64_t now = kStart;
  d.reset(kT120, now);
  present_t p;
  // 每格一張，到 until 為止（不超過）
  const auto on_cadence_until = [&](int64_t until) {
    while (now + kT120 <= until) {
      now += kT120;
      p.now_ns = now;
      p.slots = 1;
      d.on_present(p);
    }
  };
  const auto zero_at = [&](int64_t at) {
    p.now_ns = at;
    p.slots = 0;
    d.on_present(p);
  };
  on_cadence_until(now + 11'700 * kMs);
  ASSERT_EQ(d.state(), state_e::locked);
  d.take_window();
  d.set_free_run(true);
  d.set_free_run(false);  // 寫回正式模式……
  d.on_tick(now);  // ……那一段結束的時刻＝之後偵測器第一次看到的時刻
  const int64_t end = now;
  on_cadence_until(end + 99 * kMs);
  // 100 ms 整還算那一段的
  zero_at(end + 100 * kMs);
  zero_at(end + 100 * kMs);
  zero_at(end + 100 * kMs);
  EXPECT_EQ(d.state(), state_e::locked);
  auto w = d.take_window();
  EXPECT_EQ(w.free_run, 3u);
  EXPECT_EQ(w.zeros, 0u);
  // 過了就又是不可能的值：第 3 筆起不信
  zero_at(end + 100 * kMs + 1);
  zero_at(end + 100 * kMs + 1);
  EXPECT_EQ(d.state(), state_e::locked);
  EXPECT_FALSE(d.take_suspect_alert());
  zero_at(end + 100 * kMs + 1);
  EXPECT_EQ(d.state(), state_e::unsure);
  EXPECT_EQ(d.why(), why_e::suspect);
  EXPECT_TRUE(d.take_suspect_alert());
  w = d.take_window();
  EXPECT_EQ(w.free_run, 0u);
  EXPECT_EQ(w.zeros, 3u);
}

TEST(VrPresentCadenceFreeRun, CallsThatDoNotChangeTheValueDoNothing) {
  // bridge 每次寫節拍都會說一次（絕大多數是「會等」之後又是「會等」）：值沒變的呼叫不可以憑空開出一段，
  // 也不可以讓剛結束的那一段重新起算 100 ms
  sim_t s;
  s.settle();
  s.d.take_window();
  s.d.set_free_run(false);  // 從來沒開過
  s.present(0);
  auto w = s.d.take_window();
  EXPECT_EQ(w.zeros, 1u);
  EXPECT_EQ(w.free_run, 0u);
  s.d.set_free_run(true);
  s.d.set_free_run(true);
  s.present(0);
  s.d.set_free_run(false);
  s.advance(0);  // 結束的時刻：s.now
  s.run_for(90 * kMs);
  s.d.set_free_run(false);  // 約 83 ms 後又寫了一次正式的節拍（LATCH 的修正）
  s.run_for(50 * kMs);  // 距結束約 133 ms（> 100 ms），距上一行約 50 ms
  s.present(0);
  w = s.d.take_window();
  EXPECT_EQ(w.free_run, 1u);  // 開著的那一筆
  EXPECT_EQ(w.zeros, 1u);  // 過了 100 ms 的這一筆
}

TEST(VrPresentCadenceFreeRun, AStalledCallerDoesNotTurnTheBacklogIntoImpossibleValues) {
  // bridge 執行緒被卡住 300 ms：這段時間 driver 照樣寫紀錄，偵測器什麼都沒看到（連 on_tick 都沒有）。恢復的那一輪
  // 先把節拍寫回正式模式、再排空積下來的紀錄——時間全是那一輪的時刻。那一段結束的時刻也是那一輪，不是卡住之前
  sim_t s;
  s.settle();
  s.d.take_window();
  s.d.set_free_run(true);
  const int64_t resumed = s.now + 300 * kMs;
  s.d.set_free_run(false);
  present_t p;
  p.now_ns = resumed;
  for (int i = 0; i < 90; ++i) {
    p.slots = i % 3 == 2 ? 1 : 0;
    s.d.on_present(p);
  }
  s.d.on_tick(resumed);
  EXPECT_EQ(s.st(), state_e::locked);
  EXPECT_FALSE(s.d.take_suspect_alert());
  auto w = s.d.take_window();
  EXPECT_EQ(w.free_run, 60u);
  EXPECT_EQ(w.zeros, 0u);
  EXPECT_EQ(w.ok, 30u);
  // 那一輪之後 100 ms 起，0 又是不可能的值
  s.now = resumed;
  s.run_for(108 * kMs);
  s.present(0);
  w = s.d.take_window();
  EXPECT_EQ(w.free_run, 0u);
  EXPECT_EQ(w.zeros, 1u);
}

TEST(VrPresentCadenceFreeRun, OtherImpossibleValuesStillCount) {
  // 那一段裡只有「有輸出目標而 slots 是 0」不算：負值、一次跨超過 2 s 照樣是不可能的值，沒有輸出目標的照樣另外算
  sim_t s;
  s.settle();
  s.d.take_window();
  s.d.set_free_run(true);
  s.present(0, 0, kExitNoTarget);
  s.present(-1);
  s.present(242);
  EXPECT_EQ(s.st(), state_e::unknown);  // 跨 242 格：重來
  EXPECT_FALSE(s.d.take_suspect_alert());
  s.present(-1);
  EXPECT_EQ(s.st(), state_e::unsure);
  EXPECT_EQ(s.why(), why_e::suspect);
  EXPECT_TRUE(s.d.take_suspect_alert());
  const auto w = s.d.take_window();
  EXPECT_EQ(w.no_target, 1u);
  EXPECT_EQ(w.invalid, 2u);
  EXPECT_EQ(w.restarts, 1u);
  EXPECT_EQ(w.zeros, 0u);
  EXPECT_EQ(w.free_run, 0u);
}

TEST(VrPresentCadenceFreeRun, StartingOverKeepsItAndResetForgetsIt) {
  // 節拍模式是呼叫端自己寫的，不是從紀錄推出來的：紀錄重來（沒有輸出目標、停了超過 2 s）不影響它；新連線（reset）才清
  sim_t s;
  s.settle();
  s.d.set_free_run(true);
  for (int i = 0; i < 12; ++i) {
    s.present(1, 0, kExitNoTarget);
  }
  ASSERT_EQ(s.st(), state_e::unknown);
  s.d.take_window();
  s.run(30, 0);
  auto w = s.d.take_window();
  EXPECT_EQ(w.free_run, 30u);
  EXPECT_EQ(w.zeros, 0u);
  // 那些 0 不是可用的紀錄：重來之後照樣要 24 筆跨了格的才有輸出
  EXPECT_EQ(s.st(), state_e::unknown);
  s.run(23);
  EXPECT_EQ(s.st(), state_e::unknown);
  s.present();
  EXPECT_EQ(s.st(), state_e::unsure);
  EXPECT_EQ(s.why(), why_e::proving);
  s.d.take_window();
  s.advance(2100 * kMs);
  s.run(3, 0);
  w = s.d.take_window();
  EXPECT_EQ(w.free_run, 3u);
  EXPECT_EQ(w.zeros, 0u);
  EXPECT_FALSE(s.d.take_suspect_alert());
  s.d.reset(s.period, s.now);
  s.run(3, 0);
  w = s.d.take_window();
  EXPECT_EQ(w.free_run, 0u);
  EXPECT_EQ(w.zeros, 3u);
  EXPECT_EQ(s.st(), state_e::unsure);
  EXPECT_EQ(s.why(), why_e::suspect);
}

// ── 組幀：把同一幀的紀錄配起來 ────────────────────────────────────────────

TEST(VrPresentCadenceAssembler, PairsTheRecordsOfOneFrame) {
  const auto ticks = [](int64_t ns) {
    return ns / 100;  // 紀錄的時戳單位是 100 ns（QPC 頻率 10 MHz）
  };
  detector_t d;
  int64_t now = kStart;
  d.reset(kT120, now, 10'000'000);
  uint32_t fid = 100;
  int64_t exit_t = now;  // 上一筆 POSTPRESENT_EXIT 的時刻（＝格點）
  // 一幀的正常序列：PRESENT_ENTER、PRESENT_EXIT、POSTPRESENT_ENTER、VSYNC_VIRTUAL、POSTPRESENT_EXIT
  const auto frame = [&](int64_t slots, int64_t phase_ns, bool drop_exit = false, bool drop_enter = false, uint32_t exit_fid_off = 0) {
    const int64_t present = exit_t + phase_ns;
    d.on_record(now, kEvPresentEnter, fid, 0, 0, ticks(present));
    ++fid;
    if (!drop_exit) {
      d.on_record(now, kEvPresentExit, fid + exit_fid_off, 0, 2, ticks(present + 200'000));
    }
    if (!drop_enter) {
      d.on_record(now, kEvPostPresentEnter, fid, 0, 0, ticks(present + 300'000));
    }
    d.on_record(now, kEvVsyncVirtual, fid, kT120, slots, ticks(present + 310'000));
    exit_t += slots * kT120;
    d.on_record(now, kEvPostPresentExit, fid, 4000, 0, ticks(exit_t));
    now = exit_t + kMs;
    d.on_tick(now);
  };
  for (int i = 0; i < 100; ++i) {
    frame(1, 3'500'000);
  }
  auto w = d.take_window();
  EXPECT_EQ(w.records, 100u);
  EXPECT_EQ(w.ok, 100u);
  EXPECT_EQ(w.orphans, 0u);
  EXPECT_EQ(w.edge, 0u);
  EXPECT_EQ(w.no_target, 0u);
  EXPECT_EQ(d.state(), state_e::unsure);  // 100 筆、不到 1 s：還在觀察期
  EXPECT_EQ(d.why(), why_e::proving);
  // Present 落在格內最後 14%（120 Hz 的界線是 7.17 ms）
  frame(1, 7'400'000);
  w = d.take_window();
  EXPECT_EQ(w.edge, 1u);
  frame(1, 7'100'000);
  w = d.take_window();
  EXPECT_EQ(w.edge, 0u);
  // 少了 PRESENT_EXIT：這筆 VSYNC_VIRTUAL 配不到同伴，不用
  frame(2, 3'500'000, true);
  w = d.take_window();
  EXPECT_EQ(w.orphans, 1u);
  EXPECT_EQ(w.records, 0u);
  EXPECT_EQ(w.slips, 0u);
  // 少了 POSTPRESENT_ENTER
  frame(2, 3'500'000, false, true);
  w = d.take_window();
  EXPECT_EQ(w.orphans, 1u);
  EXPECT_EQ(w.records, 0u);
  // PRESENT_EXIT 是別幀的（中間掉了紀錄）
  frame(2, 3'500'000, false, false, 7);
  w = d.take_window();
  EXPECT_EQ(w.orphans, 1u);
  EXPECT_EQ(w.records, 0u);
  // 暫存不外漏：孤兒之後的完整一幀正常使用
  frame(1, 3'500'000);
  w = d.take_window();
  EXPECT_EQ(w.records, 1u);
  EXPECT_EQ(w.ok, 1u);
  EXPECT_EQ(w.orphans, 0u);
  // 連續兩筆單獨的 VSYNC_VIRTUAL（frame_id 還和上一幀相同）：上一幀的暫存已經清掉，兩筆都是孤兒
  d.on_record(now, kEvVsyncVirtual, fid, kT120, 1, 0);
  d.on_record(now, kEvVsyncVirtual, fid, kT120, 1, 0);
  w = d.take_window();
  EXPECT_EQ(w.orphans, 2u);
  EXPECT_EQ(w.records, 0u);
  // 不認得的 event 與荒謬的時戳都忽略
  d.on_record(now, 9, fid, 1, 2, kI64Min);
  d.on_record(now, 77, fid, 1, 2, kI64Max);
  d.on_record(now, 0, fid, 1, 2, 0);
  d.on_record(now, 0xFFFFFFFFu, fid, 1, 2, 0);
  d.on_record(now, kEvPostPresentExit, fid, 0, 0, kI64Min);
  d.on_record(now, kEvPresentEnter, fid, 0, 0, kI64Max);
  frame(1, 3'500'000);
  w = d.take_window();
  EXPECT_EQ(w.records, 1u);
  EXPECT_EQ(w.ok, 1u);
  EXPECT_EQ(w.edge, 0u);
  EXPECT_EQ(w.orphans, 0u);
}

TEST(VrPresentCadenceAssembler, CarriesExitCodeThrottleAndSlots) {
  // 組出來的 present_t 帶的是同一幀的值：沒有輸出目標（PRESENT_EXIT.arg0 = 4）、節流值（POSTPRESENT_ENTER.arg0）、
  // 跨的格數（VSYNC_VIRTUAL.arg1；arg0 是週期，不用）
  detector_t d;
  int64_t now = kStart;
  d.reset(kT120, now);  // 不給時戳的單位：不算 Present 相位
  uint32_t fid = 7;
  const auto frame = [&](int64_t slots, int64_t exit_code, int64_t throttle) {
    now += slots * kT120;
    ++fid;
    d.on_record(now, kEvPresentEnter, fid, 0, 0, 0);
    d.on_record(now, kEvPresentExit, fid, exit_code, 0, 0);
    d.on_record(now, kEvPostPresentEnter, fid, throttle, 0, 0);
    d.on_record(now, kEvVsyncVirtual, fid, kT120, slots, 0);
    d.on_record(now, kEvPostPresentExit, fid, 0, 0, 0);
    d.on_tick(now);
  };
  for (int i = 0; i < 30; ++i) {
    frame(1, 0, 0);
  }
  for (int i = 0; i < 5; ++i) {
    frame(1, kExitNoTarget, 0);
  }
  frame(2, 0, 0);
  frame(1, 0, 0);
  auto w = d.take_window();
  EXPECT_EQ(w.records, 37u);
  EXPECT_EQ(w.no_target, 5u);
  EXPECT_EQ(w.ok, 31u);
  EXPECT_EQ(w.slips, 1u);
  EXPECT_EQ(w.skipped, 1u);
  EXPECT_EQ(w.edge, 0u);
  EXPECT_EQ(w.throttle_max, 0);
  // 節流：24 幀都是（2 格、節流 1）→ 採用
  for (int i = 0; i < 24; ++i) {
    frame(2, 0, 1);
  }
  EXPECT_EQ(d.divisor(), 2);
  w = d.take_window();
  EXPECT_EQ(w.divisor_changes, 1u);
  EXPECT_EQ(w.throttle_max, 1);
  // 節流值夾在 0～255 才記進 thrMax
  frame(2, 0, 1'000'000);
  frame(2, 0, -7);
  w = d.take_window();
  EXPECT_EQ(w.throttle_max, 255);
}

// ── 10 s log 的計數 ──────────────────────────────────────────────────────

TEST(VrPresentCadenceWindow, CountsAndTakeWindow) {
  sim_t s;
  s.settle();
  s.d.take_window();
  // 同一整秒 3 個滑動，4.2 s 後再 1 個
  const int64_t base = (s.now / kSec + 2) * kSec;  // 某一整秒的開頭
  s.run_for(base - s.now);
  s.present(2);
  s.run(10);
  s.present(2);
  s.run(10);
  s.present(2);
  s.run_for(4200 * kMs);
  s.present(2);
  s.present(1);
  const auto w = s.d.take_window();
  EXPECT_EQ(w.slips, 4u);
  EXPECT_EQ(w.masked, 0u);
  EXPECT_EQ(w.hitches, 0u);
  EXPECT_EQ(w.skipped, 4u);
  EXPECT_EQ(w.slip_secs, 2u);
  EXPECT_EQ(w.burst_max, 3u);
  EXPECT_GE(w.slip_gap_max_ns, 4200 * kMs);
  EXPECT_LE(w.slip_gap_max_ns, 4300 * kMs);
  EXPECT_GE(w.records, 500u);
  EXPECT_EQ(w.ok, w.records - 4);
  // 取走之後全部歸零
  const auto w2 = s.d.take_window();
  EXPECT_EQ(w2.records, 0u);
  EXPECT_EQ(w2.ok, 0u);
  EXPECT_EQ(w2.slips, 0u);
  EXPECT_EQ(w2.skipped, 0u);
  EXPECT_EQ(w2.slip_secs, 0u);
  EXPECT_EQ(w2.burst_max, 0u);
  EXPECT_EQ(w2.slip_gap_max_ns, 0);
  EXPECT_EQ(w2.throttle_max, 0);
  // 摘要裡的是「最近 10 s」，和 10 s log 的視窗無關
  const info_t i = s.d.info(s.now);
  EXPECT_EQ(static_cast<int>(i.slips_10s), 4);
  EXPECT_EQ(i.state, state_e::slipping);
  EXPECT_EQ(static_cast<int>(i.divisor), 1);
  EXPECT_EQ(i.stamp_ms, static_cast<uint32_t>(s.now / kMs));
  // 下一個視窗的間隔從頭算：上一個視窗最後一個滑動不帶過來
  s.present(2);
  s.present(1);
  EXPECT_EQ(s.d.take_window().slip_gap_max_ns, 0);
}

TEST(VrPresentCadenceWindow, SlipsInTheSummaryAreCappedAtThirtyTwo) {
  sim_t s;
  s.settle();
  for (int i = 0; i < 60; ++i) {
    s.present(2);
    s.present(1);
  }
  const info_t i = s.d.info(s.now);
  EXPECT_EQ(static_cast<int>(i.slips_10s), 32);
  EXPECT_EQ(unpack(pack(i)), i);
  EXPECT_EQ(s.d.take_window().slips, 60u);
}

// ── 亂數序列：不論怎麼餵都要成立的性質 ─────────────────────────────────────

namespace {
  /// 不合理的值（slots、exit_code、throttle、arg0／arg1 都從這裡抽）
  constexpr int64_t kEdgeVals[] = {0, 1, -1, 2, 3, 4, 5, 6, 16, 17, 240, 241, 242, int64_t {0xFFFFFFFF}, kI64Max, kI64Min, 4, 4};

  /**
   * @brief 每一步都要成立的性質：
   *   打包來回相等；info() 與 state()／why() 一致；slips_10s ≤ 32；1 ≤ divisor ≤ 5；
   *   unknown：why 是 none、摘要的 divisor 是 0；locked：why 是 none、摘要的 divisor 就是目前的節拍；
   *   slipping／unsure 的 why 屬於各自的集合
   */
  ::testing::AssertionResult invariants_hold(const detector_t &d, int64_t now) {
    const info_t in = d.info(now);
    bool ok = unpack(pack(in)) == in && in.state == d.state() && in.why == d.why() && in.slips_10s <= 32 && d.divisor() >= 1 && d.divisor() <= 5;
    switch (in.state) {
      case state_e::unknown:
        ok = ok && in.why == why_e::none && in.divisor == 0;
        break;
      case state_e::locked:
        ok = ok && in.why == why_e::none && in.divisor == d.divisor();
        break;
      case state_e::slipping:
        ok = ok && (in.why == why_e::marginal || in.why == why_e::heavy || in.why == why_e::recovering) && in.divisor == d.divisor();
        break;
      case state_e::unsure:
        ok = ok && (in.why == why_e::recent || in.why == why_e::fraction || in.why == why_e::suspect || in.why == why_e::proving) && in.divisor == d.divisor();
        break;
    }
    if (ok) {
      return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << "state " << static_cast<int>(in.state) << " why " << static_cast<int>(in.why) << " divisor "
                                         << static_cast<int>(in.divisor) << "/" << d.divisor() << " slips10 " << static_cast<int>(in.slips_10s);
  }

  /// 10 s 計數之間的關係
  ::testing::AssertionResult window_is_consistent(const window_t &w) {
    if (w.slip_secs <= w.slips && w.burst_max <= w.slips && w.slip_gap_max_ns >= 0 && w.throttle_max >= 0 && w.throttle_max <= 255 &&
        w.ok <= w.records && w.no_target <= w.records && w.free_run <= w.records && (w.slips == 0) == (w.slip_secs == 0) &&
        (w.slips == 0) == (w.burst_max == 0)) {
      return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << "records " << w.records << " ok " << w.ok << " noTgt " << w.no_target << " slips " << w.slips
                                         << " slipSecs " << w.slip_secs << " burstMax " << w.burst_max << " slipGapMax " << w.slip_gap_max_ns
                                         << " thrMax " << w.throttle_max << " freeRun " << w.free_run;
  }
}  // namespace

TEST(VrPresentCadenceFuzz, InvariantsHoldUnderHostileInput) {
  // 固定種子。含極值、亂跳（包括倒退）的時鐘、不合理的週期與時戳單位。這種輸入下狀態幾乎都是 unknown／unsure
  // （不可能的值一直有）；locked／slipping 的覆蓋在下一個測試
  std::mt19937_64 g(20261010);
  const int64_t periods[] = {kT120, kT90, kT60, 0, -5, 1, kI64Max, 13'888'888};
  uint64_t seen[4] = {0, 0, 0, 0};
  for (int r = 0; r < 200; ++r) {
    detector_t d;
    int64_t now = static_cast<int64_t>(g() % 3 == 0 ? g() : (g() % (1ull << 40)));
    d.reset(periods[g() % 8], now, g() % 4 == 0 ? static_cast<int64_t>(g()) : 10'000'000);
    for (int i = 0; i < 20000; ++i) {
      const uint64_t k = g() % 100;
      if (k < 3) {
        now = static_cast<int64_t>(g());  // 任何值，包括倒退
      } else if (k < 10) {
        now += static_cast<int64_t>(g() % 3'000'000'000ull);
      } else if (now < kI64Max - 20'000'000) {
        now += static_cast<int64_t>(g() % 20'000'000ull);
      }
      const uint64_t what = g() % 10;
      if (what < 5) {
        present_t p;
        p.now_ns = now;
        p.slots = g() % 4 == 0 ? kEdgeVals[g() % 18] : static_cast<int64_t>(1 + g() % 3);
        p.exit_code = g() % 6 == 0 ? kEdgeVals[g() % 18] : 0;
        p.throttle = g() % 3 == 0 ? kEdgeVals[g() % 18] : 0;
        p.have_phase = g() % 2 == 0;
        p.phase_ns = static_cast<int64_t>(g());
        d.on_present(p);
      } else if (what < 8) {
        const uint32_t event = static_cast<uint32_t>(g() % 9);
        const uint32_t frame_id = static_cast<uint32_t>(g() % 4);
        const int64_t arg0 = kEdgeVals[g() % 18];
        const int64_t arg1 = kEdgeVals[g() % 18];
        d.on_record(now, event, frame_id, arg0, arg1, static_cast<int64_t>(g()));
      } else {
        d.on_tick(now);
      }
      ASSERT_TRUE(invariants_hold(d, now)) << "round " << r << " step " << i;
      ++seen[static_cast<uint8_t>(d.state()) & 3u];
      if (g() % 500 == 0) {
        ASSERT_TRUE(window_is_consistent(d.take_window())) << "round " << r << " step " << i;
      }
    }
  }
  // 序列真的有走到這兩種狀態（數字只是下限）
  EXPECT_GT(seen[static_cast<uint8_t>(state_e::unknown)], 20'000u);
  EXPECT_GT(seen[static_cast<uint8_t>(state_e::unsure)], 1'000'000u);
}

TEST(VrPresentCadenceFuzz, InvariantsHoldUnderPlausibleInput) {
  // 接近真實的序列：時鐘照節拍走，負載在鎖住、臨界、重度、半速節流、卡頓、輸出目標時有時無之間換，
  // 偶爾混進不可能的值與紀錄停住的空檔。四種狀態都要走到
  std::mt19937_64 g(20261011);
  const int64_t periods[] = {kT120, kT90, kT60};
  uint64_t seen[4] = {0, 0, 0, 0};
  uint64_t adopted = 0;
  for (int r = 0; r < 40; ++r) {
    const int64_t period = periods[g() % 3];
    detector_t d;
    int64_t now = 1'000 * kSec + static_cast<int64_t>(g() % 1'000'000'000ull);
    d.reset(period, now, 10'000'000);
    int regime = 0;
    int regime_left = 0;
    for (int i = 0; i < 20000; ++i) {
      if (regime_left <= 0) {
        regime = static_cast<int>(g() % 6);
        regime_left = 500 + static_cast<int>(g() % 4500);
      }
      --regime_left;
      present_t p;
      p.slots = 1;
      switch (regime) {
        case 0:  // 鎖住
          break;
        case 1:  // 臨界：偶爾少一格
          p.slots = g() % 200 == 0 ? 2 : 1;
          break;
        case 2:  // 重度
          p.slots = g() % 8 == 0 ? 2 : 1;
          break;
        case 3:  // SteamVR 把遊戲固定在半速
          p.slots = 2;
          p.throttle = 1;
          break;
        case 4:  // 卡頓與零星的滑動
          p.slots = g() % 300 == 0 ? 10 : (g() % 40 == 0 ? 2 : 1);
          break;
        default:  // 輸出目標時有時無
          p.exit_code = g() % 3 == 0 ? kExitNoTarget : 0;
          break;
      }
      if (g() % 5000 == 0) {
        p.slots = kEdgeVals[g() % 18];
      }
      now += (p.slots > 0 && p.slots < 1000 ? p.slots : 1) * period;
      if (g() % 2000 == 0) {
        now += static_cast<int64_t>(g() % 3'000'000'000ull);  // 紀錄停一陣子
        d.on_tick(now);
      }
      p.now_ns = now;
      d.on_present(p);
      if (g() % 4 == 0) {
        d.on_tick(now);
      }
      ASSERT_TRUE(invariants_hold(d, now)) << "round " << r << " step " << i;
      ++seen[static_cast<uint8_t>(d.state()) & 3u];
      if (g() % 500 == 0) {
        const auto w = d.take_window();
        ASSERT_TRUE(window_is_consistent(w)) << "round " << r << " step " << i;
        adopted += w.divisor_changes;
      }
    }
  }
  // 四種狀態、還有節拍的採用，都真的走到了（數字只是下限，確認序列有覆蓋到）
  EXPECT_GT(seen[static_cast<uint8_t>(state_e::unknown)], 10'000u);
  EXPECT_GT(seen[static_cast<uint8_t>(state_e::locked)], 100'000u);
  EXPECT_GT(seen[static_cast<uint8_t>(state_e::slipping)], 100'000u);
  EXPECT_GT(seen[static_cast<uint8_t>(state_e::unsure)], 30'000u);
  EXPECT_GT(adopted, 30u);
}

TEST(VrPresentCadenceFuzz, AFreeRunExemptsExactlyTheZerosItCovers) {
  // 亂數時機的 set_free_run()（開、關、值沒變的重複呼叫）加上混在紀錄裡的 0。每一步核對：性質照樣成立；
  // 有輸出目標的 0 落在那一段裡（含寫回之後的 100 ms）就記在 free_run，否則記在 zeros——不會兩邊都算，也不會漏
  std::mt19937_64 g(20261012);
  uint64_t exempt = 0;
  uint64_t counted = 0;
  for (int r = 0; r < 40; ++r) {
    detector_t d;
    int64_t now = 2'000 * kSec + static_cast<int64_t>(g() % 1'000'000'000ull);
    d.reset(kT120, now);
    bool open = false;
    bool closing = false;  // 剛寫回正式模式：那一段結束的時刻是偵測器下一次看到的時刻
    bool closed_once = false;
    int64_t closed_at = 0;
    for (int i = 0; i < 10000; ++i) {
      if (g() % 300 == 0) {
        const bool on = g() % 2 == 0;
        if (on != open) {
          closing = !on;
        }
        open = on;
        d.set_free_run(on);
      }
      now += static_cast<int64_t>(1 + g() % 12) * kMs;
      present_t p;
      p.now_ns = now;
      const uint64_t k = g() % 100;
      p.slots = k < 30 ? 0 : (k < 33 ? 2 : (k < 34 ? -1 : 1));
      p.exit_code = g() % 40 == 0 ? kExitNoTarget : 0;
      (void) d.take_window();
      d.on_present(p);
      if (closing) {
        closing = false;
        closed_once = true;
        closed_at = now;
      }
      ASSERT_TRUE(invariants_hold(d, now)) << "round " << r << " step " << i;
      const auto w = d.take_window();
      ASSERT_TRUE(window_is_consistent(w)) << "round " << r << " step " << i;
      const bool zero = p.slots == 0 && p.exit_code != kExitNoTarget;
      const bool covered = open || (closed_once && now - closed_at <= 100 * kMs);
      ASSERT_EQ(w.free_run, zero && covered ? 1u : 0u) << "round " << r << " step " << i;
      ASSERT_EQ(w.zeros, zero && !covered ? 1u : 0u) << "round " << r << " step " << i;
      exempt += w.free_run;
      counted += w.zeros;
    }
  }
  // 兩種都真的走到了（數字只是下限）
  EXPECT_GT(exempt, 10'000u);
  EXPECT_GT(counted, 10'000u);
}
