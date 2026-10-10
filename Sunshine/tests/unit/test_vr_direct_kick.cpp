/**
 * @file tests/unit/test_vr_direct_kick.cpp
 * @brief VipleStream 2.0 §VR-DIRECT-KICK：host 直通落在「多一格」的時序時的判定、kick、確認、退避與各種停手條件
 *        （src/vr/vr_direct_kick.h；全部用假的時間與假的 presentPhase 序列驅動）。
 *
 * 這個檔刻意不 include ../tests_common.h：受測的標頭只用標準函式庫，測試也只需要 googletest，
 * 所以可以脫離整個 server 單獨編譯執行（-Wall -Wextra -Wshadow -Wconversion -Werror 也過得了；
 * tests_common.h 會拉進 logging／platform 的標頭，這組旗標過不了）。放進 test_sunshine 一起建也一樣能跑。
 *
 * 寫法上的三個約定：
 *   - 規格的數字（1.15／2.2／2.0 ms、24 筆、3 s、500 ms＋1.5 s、5 s、10／30／90／300 s、11 ms、80%、20 次……）在這裡
 *     寫死，不引用標頭的常數：常數被改掉時測試要會失敗。
 *   - sim_t 照 bridge 執行緒的走法：每 2 ms 一輪，先 poll()（periodic() 前段）、再把這 2 ms 內的樣本餵進去
 *     （periodic() 尾端的 drain_rings()）。樣本的時刻是它自己的，不是餵的當下。
 *   - 要對到 1 ns 的邊界用 hand_t（每一次 poll() 的時刻都由測試指定）。偵測器每 100 ms 才評估一次，
 *     所以「差 1 ns 不成立」與「剛好成立」要分兩趟跑，不能在同一趟裡連著問。
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <ostream>
#include <src/vr/vr_direct_kick.h>
#include <string>
#include <vector>

using namespace vr::kick;

namespace vr::kick {
  // 斷言失敗時印名字（不然 googletest 只會印「4-byte object <01-00 00-00>」）
  void PrintTo(event_e e, std::ostream *os) {
    *os << event_name(e);
  }
}  // namespace vr::kick

namespace {
  constexpr int64_t kUs = 1'000;
  constexpr int64_t kMs = 1'000'000;
  constexpr int64_t kSec = 1'000'000'000;
  constexpr int64_t kT60 = 16'666'666;  // 60 Hz（半速串流時 host 的週期）
  constexpr int64_t kT72 = 13'888'888;  // 72 Hz
  constexpr int64_t kT90 = 11'111'111;  // 90 Hz
  constexpr int64_t kT120 = 8'333'333;  // 120 Hz
  constexpr int64_t kStart = 7'000 * kSec + 123'456'789;  // 任意的起點（不是 0、也不在任何整數格上）
  constexpr int64_t kI64Max = std::numeric_limits<int64_t>::max();
  constexpr int64_t kI64Min = std::numeric_limits<int64_t>::min();

  /// 可重現的亂數（xorshift64*）。不用 <random> 的分佈：各家標準函式庫的結果不同
  struct rng_t {
    uint64_t s;

    explicit rng_t(uint64_t seed):
        s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {
    }

    uint64_t next() {
      s ^= s >> 12;
      s ^= s << 25;
      s ^= s >> 27;
      return s * 0x2545F4914F6CDD1Dull;
    }

    /// [lo, hi] 的整數（lo ≤ hi）
    int64_t range(int64_t lo, int64_t hi) {
      return lo + static_cast<int64_t>(next() % static_cast<uint64_t>(hi - lo + 1));
    }
  };

  ::testing::AssertionResult between(int64_t v, int64_t lo, int64_t hi) {
    if (v >= lo && v <= hi) {
      return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << v << " is not in [" << lo << ", " << hi << "]";
  }

  /// presentPhase 的樣子。數字照 2026-10-10 量到的每 10 秒視窗（規格第 1 節的表）
  enum class mode_e {
    silent,  ///< 沒有 Present
    a,  ///< 等遊戲這一格：3.3 ms 上下，少數幾張到 7 ms（p50 3.27～4.26、p95 4.44～6.96）
    b,  ///< 多一格：0.8 ms 上下（p50 0.72～1.11、p95 0.98～1.77）
    s,  ///< host 的儀表板開著：1.2～2.1 ms（p95 2.08～3.04）
    heavy,  ///< A，但遊戲的幀佔掉週期的 82～93%
    fixed,  ///< 每一筆都是 fixed_ns
    band,  ///< [band_lo, band_hi] 均勻
    mix,  ///< 每 mix_every 筆有一筆是 mix_hi，其餘是 mix_lo
  };

  struct ev_t {
    int64_t t = 0;  ///< poll() 回這個事件的時刻
    decision_t d;
    bool declined = false;  ///< kick：呼叫端回報沒做成
  };

  /**
   * @brief 假的 bridge 執行緒：每 2 ms 一輪，先 poll() 再餵這 2 ms 內的樣本。kick 做了之後可以換成另一種狀態
   *        （kick 有用／沒用），也可以回報沒做成。
   */
  struct sim_t {
    detector_t det;
    rng_t rng;
    int64_t period;
    int64_t sample_step;  ///< 樣本間隔（預設＝週期：合成器每個 vsync Present 一次）
    int64_t t0;
    int64_t now;
    int64_t next_sample;
    mode_e mode;
    int64_t fixed_ns = 0;
    int64_t band_lo = 0;
    int64_t band_hi = 0;
    int64_t mix_lo = 0;
    int64_t mix_hi = 0;
    int mix_every = 1;
    int mix_count = 0;
    bool switch_on_kick = false;  ///< kick 做了之後 mode 換成 after_kick
    mode_e after_kick = mode_e::a;
    bool decline = false;  ///< 之後的 kick 都回報沒做成
    std::vector<ev_t> ev;

    explicit sim_t(int64_t period_ns, mode_e m, int64_t start_ns = kStart, uint64_t seed = 1):
        rng(seed),
        period(period_ns),
        sample_step(period_ns > 0 ? period_ns : kT60),  // 週期不合理的 generation 也照 60 Hz 餵樣本
        t0(start_ns),
        now(start_ns),
        next_sample(start_ns + sample_step),
        mode(m) {
      det.reset(period_ns, start_ns);
    }

    void kick_to(mode_e m) {
      switch_on_kick = true;
      after_kick = m;
    }

    int64_t phase() {
      const bool tail = rng.range(0, 99) < 8;
      switch (mode) {
        case mode_e::a:
          return tail ? rng.range(4'500'000, 7'000'000) : rng.range(2'900'000, 3'900'000);
        case mode_e::b:
          return tail ? rng.range(1'000'000, 1'800'000) : rng.range(650'000, 950'000);
        case mode_e::s:
          return tail ? rng.range(2'100'000, 3'000'000) : rng.range(1'200'000, 2'100'000);
        case mode_e::heavy:
          return rng.range(period / 100 * 82, period / 100 * 93);
        case mode_e::fixed:
          return fixed_ns;
        case mode_e::band:
          return rng.range(band_lo, band_hi);
        case mode_e::mix:
          return (++mix_count % mix_every) == 0 ? mix_hi : mix_lo;
        case mode_e::silent:
          break;
      }
      return 0;
    }

    void tick() {
      now += 2 * kMs;
      const decision_t d = det.poll(now);
      if (d.event != event_e::none) {
        ev_t e;
        e.t = now;
        e.d = d;
        if (d.event == event_e::kick) {
          if (decline) {
            det.kick_not_done();
            e.declined = true;
          } else if (switch_on_kick) {
            mode = after_kick;
          }
        }
        ev.push_back(e);
      }
      for (; next_sample <= now; next_sample += sample_step) {
        if (mode != mode_e::silent) {
          det.on_present_phase(next_sample, phase());
        }
      }
    }

    void run(int64_t dur_ns) {
      const int64_t end = now + dur_ns;
      while (now < end) {
        tick();
      }
    }

    /// 跑到出現這種事件為止（limit_ns 內沒有出現回 false）；出現時它是 ev.back()
    bool run_until(event_e e, int64_t limit_ns) {
      const int64_t end = now + limit_ns;
      while (now < end) {
        const std::size_t before = ev.size();
        tick();
        if (ev.size() != before && ev.back().d.event == e) {
          return true;
        }
      }
      return false;
    }

    int count(event_e e) const {
      return static_cast<int>(std::count_if(ev.begin(), ev.end(), [e](const ev_t &x) {
        return x.d.event == e;
      }));
    }

    /// 斷言失敗時印出整串事件（時刻是距離起點幾 ms）
    std::string dump() const {
      std::string out = "events:";
      for (const ev_t &e : ev) {
        out += " [+" + std::to_string((e.t - t0) / kMs) + "ms " + event_name(e.d.event) + (e.declined ? " declined" : "") + "]";
      }
      return out;
    }
  };

  /**
   * @brief 手動驅動：每一次 poll() 的時刻都由測試指定（要對到 1 ns 的邊界時用）。樣本每 10 ms 一筆，預設是 B。
   */
  struct hand_t {
    detector_t det;
    int64_t t0;
    int64_t step = 10 * kMs;  ///< 樣本間隔
    int64_t phase = 800 * kUs;  ///< 餵的數值
    bool feeding = true;
    int64_t fed;  ///< 下一筆樣本的時刻
    int64_t last;  ///< 上一次 poll() 的時刻

    explicit hand_t(int64_t period_ns = kT60, int64_t start_ns = kStart):
        t0(start_ns),
        fed(start_ns + 10 * kMs),
        last(start_ns) {
      det.reset(period_ns, start_ns);
    }

    /// 樣本餵到 t 為止（含），再 poll(t)
    decision_t at(int64_t t) {
      for (; feeding && fed <= t; fed += step) {
        det.on_present_phase(fed, phase);
      }
      last = t;
      return det.poll(t);
    }

    /// 每 500 ms poll() 一次走到 t 之前（最後一次離 t 還有 500 ms 以上、不到 1 s），途中不應該有任何事件
    void quiet_until(int64_t t) {
      while (last + 500 * kMs <= t - 500 * kMs) {
        const int64_t x = last + 500 * kMs;
        EXPECT_EQ(at(x).event, event_e::none) << "at +" << (x - t0) / kMs << " ms";
      }
    }

    /// 走到第一次 kick（一路都是 B：暖機 5 s＋連續成立 3 s），回傳它的時刻
    int64_t first_kick() {
      quiet_until(t0 + 5 * kSec);
      EXPECT_EQ(at(t0 + 5 * kSec).event, event_e::none);
      quiet_until(t0 + 8 * kSec);
      EXPECT_EQ(at(t0 + 8 * kSec).event, event_e::kick);
      return t0 + 8 * kSec;
    }
  };
}  // namespace

// ── 視窗與統計量 ─────────────────────────────────────────────────────────

TEST(VrDirectKickWindow, MedianAndP90UseTheSameRankAsTheTimingSummary) {
  // 排好序的 n 筆裡取第 round(q × (n − 1)) 筆（從 0 數；bridge 10 秒摘要的 presentPhase 也是這樣取）
  struct case_t {
    int n;
    int median_idx;
    int p90_idx;
  };
  for (const case_t &c : {case_t {1, 0, 0}, case_t {2, 1, 1}, case_t {24, 12, 21}, case_t {25, 12, 22}, case_t {60, 30, 53}, case_t {91, 45, 81}, case_t {100, 50, 89}}) {
    detector_t d;
    d.reset(kT60, kStart);
    const int64_t now = kStart + 10 * kSec;
    // 數值是 1、2、…、n µs，照打亂的順序餵（11 與這幾個 n 都互質，所以是一個排列）；時刻也是倒著給
    for (int i = 0; i < c.n; ++i) {
      d.on_present_phase(now - i * kMs, ((i * 11) % c.n + 1) * kUs);
    }
    const stats_t s = d.recent(now);
    EXPECT_EQ(s.n, c.n);
    EXPECT_EQ(s.median_ns, (c.median_idx + 1) * kUs) << "n=" << c.n;
    EXPECT_EQ(s.p90_ns, (c.p90_idx + 1) * kUs) << "n=" << c.n;
  }
  // 一筆都沒有
  detector_t empty;
  empty.reset(kT60, kStart);
  EXPECT_EQ(empty.recent(kStart + kSec).n, 0);
  EXPECT_EQ(empty.recent(kStart + kSec).median_ns, 0);
  EXPECT_EQ(empty.recent(kStart + kSec).p90_ns, 0);
}

TEST(VrDirectKickWindow, CoversExactlyTheLastSecond) {
  detector_t d;
  d.reset(kT60, kStart);
  const int64_t now = kStart + 20 * kSec;
  d.on_present_phase(now - kSec, 1 * kMs);  // 剛好 1 秒前：不算
  d.on_present_phase(now - kSec + 1, 2 * kMs);  // 少 1 ns：算
  d.on_present_phase(now, 3 * kMs);  // 現在：算
  d.on_present_phase(now + 1, 4 * kMs);  // 時刻在未來：等「現在」追上它才算
  EXPECT_EQ(d.recent(now).n, 2);
  EXPECT_EQ(d.recent(now).median_ns, 3 * kMs);
  EXPECT_EQ(d.recent(now + 1).n, 2);  // 2 ms 那一筆滿 1 秒了，4 ms 那一筆進來
  EXPECT_EQ(d.recent(now + 1).median_ns, 4 * kMs);
  EXPECT_EQ(d.recent(now + kSec).n, 1);
  EXPECT_EQ(d.recent(now + kSec + 1).n, 0);
}

TEST(VrDirectKickWindow, KeepsTheNewestSamplesWhenFedTooFast) {
  // 寫入方每 1 ms 一筆（比任何會動作的更新率都密）：環只留最新的 256 筆，統計量用它們算
  detector_t d;
  d.reset(kT60, kStart);
  const int64_t now = kStart + 30 * kSec;
  for (int i = 0; i < 600; ++i) {
    d.on_present_phase(now - (599 - i) * kMs, (i + 1) * kUs);  // 第 i 筆的數值是 i+1 µs，最後一筆在「現在」
  }
  const stats_t s = d.recent(now);
  EXPECT_EQ(s.n, 256);
  EXPECT_EQ(s.median_ns, (344 + 128 + 1) * kUs);  // 留下的是第 344～599 筆
  EXPECT_EQ(s.p90_ns, (344 + 230 + 1) * kUs);
}

TEST(VrDirectKickWindow, DropsNegativePhases) {
  detector_t d;
  d.reset(kT60, kStart);
  const int64_t now = kStart + 30 * kSec;
  for (int i = 0; i < 10; ++i) {
    d.on_present_phase(now - i * kMs, 800 * kUs);
    d.on_present_phase(now - i * kMs, -1);
    d.on_present_phase(now - i * kMs, kI64Min);
  }
  EXPECT_EQ(d.recent(now).n, 10);
  EXPECT_EQ(d.recent(now).median_ns, 800 * kUs);
  // 0 是合理的數值（兩筆紀錄落在同一個 tick），要算
  for (int i = 0; i < 11; ++i) {
    d.on_present_phase(now - i * kMs, 0);
  }
  EXPECT_EQ(d.recent(now).n, 21);
  EXPECT_EQ(d.recent(now).median_ns, 0);
  EXPECT_EQ(d.recent(now).p90_ns, 800 * kUs);
}

// ── 1、2：穩定的 A 與 S 不 kick；中位數與第 90 百分位的邊界 ───────────────────

TEST(VrDirectKickDetect, StableANeverKicks) {
  for (const int64_t period : {kT60, kT72, kT90}) {
    sim_t s(period, mode_e::a);
    s.run(600 * kSec);
    EXPECT_TRUE(s.ev.empty()) << s.dump();
    EXPECT_EQ(s.det.kicks(), 0);
    EXPECT_EQ(s.det.dwell_ns(s.now), 0);
    const stats_t w = s.det.recent(s.now);
    EXPECT_TRUE(between(w.median_ns, 2'900'000, 3'900'000));  // 3.3 ms 上下
  }
}

TEST(VrDirectKickDetect, StableSNeverKicks) {
  for (const int64_t period : {kT60, kT90}) {
    sim_t s(period, mode_e::s);
    s.run(600 * kSec);
    EXPECT_TRUE(s.ev.empty()) << s.dump();
    EXPECT_TRUE(between(s.det.recent(s.now).median_ns, 1'200'000, 2'100'000));
  }
}

TEST(VrDirectKickDetect, MedianBoundaryIsOnePointOneFive) {
  // 每一筆都剛好 1.15 ms：中位數沒有「小於」門檻，不算 B
  sim_t on(kT60, mode_e::fixed);
  on.fixed_ns = 1'150'000;
  on.run(60 * kSec);
  EXPECT_TRUE(on.ev.empty()) << on.dump();

  // 少 1 ns 就算
  sim_t below(kT60, mode_e::fixed);
  below.fixed_ns = 1'149'999;
  below.run(9 * kSec);
  ASSERT_EQ(below.ev.size(), 1u) << below.dump();
  EXPECT_EQ(below.ev[0].d.event, event_e::kick);
  EXPECT_EQ(below.ev[0].d.median_before_ns, 1'149'999);

  // 壓在門檻兩側的分佈：S 的下緣（1.15～1.30 ms）不 kick、B 的上緣（1.00～1.149 ms）會
  sim_t s_low(kT60, mode_e::band);
  s_low.band_lo = 1'150'000;
  s_low.band_hi = 1'300'000;
  s_low.run(600 * kSec);
  EXPECT_TRUE(s_low.ev.empty()) << s_low.dump();

  sim_t b_high(kT60, mode_e::band);
  b_high.band_lo = 1'000'000;
  b_high.band_hi = 1'149'999;
  b_high.kick_to(mode_e::a);
  b_high.run(60 * kSec);
  EXPECT_EQ(b_high.count(event_e::kick), 1) << b_high.dump();
}

TEST(VrDirectKickDetect, P90BoundaryIsTwoPointTwo) {
  // 五筆裡有一筆慢的（20%）：第 90 百分位落在慢的那一群上。剛好 2.2 ms 不算 B，少 1 ns 就算
  sim_t on(kT60, mode_e::mix);
  on.mix_lo = 800'000;
  on.mix_hi = 2'200'000;
  on.mix_every = 5;
  on.run(60 * kSec);
  EXPECT_TRUE(on.ev.empty()) << on.dump();

  sim_t below(kT60, mode_e::mix);
  below.mix_lo = 800'000;
  below.mix_hi = 2'199'999;
  below.mix_every = 5;
  below.run(9 * kSec);
  ASSERT_EQ(below.ev.size(), 1u) << below.dump();
  EXPECT_EQ(below.ev[0].d.event, event_e::kick);
  EXPECT_EQ(below.ev[0].d.median_before_ns, 800'000);
  EXPECT_EQ(below.ev[0].d.p90_before_ns, 2'199'999);
}

TEST(VrDirectKickDetect, AFewSlowFramesDoNotHideB) {
  // 十二筆裡一筆 3.3 ms（8%）：第 90 百分位還在快的那一群，照樣算 B
  sim_t few(kT60, mode_e::mix);
  few.mix_lo = 800'000;
  few.mix_hi = 3'300'000;
  few.mix_every = 12;
  few.kick_to(mode_e::a);
  few.run(60 * kSec);
  EXPECT_EQ(few.count(event_e::kick), 1) << few.dump();

  // 八筆裡一筆（12.5%）就不算了
  sim_t many(kT60, mode_e::mix);
  many.mix_lo = 800'000;
  many.mix_hi = 3'300'000;
  many.mix_every = 8;
  many.run(600 * kSec);
  EXPECT_TRUE(many.ev.empty()) << many.dump();
}

TEST(VrDirectKickDetect, NeedsTwentyFourSamplesInTheWindow) {
  // 每一次都在整秒上 poll()，樣本從起點起等間隔：間隔比 1/24 秒短 0.67 ns 時，每一個 1 秒的視窗裡剛好 24 筆
  {
    hand_t h;
    h.step = 41'666'666;
    h.fed = h.t0 + h.step;
    for (int sec = 1; sec <= 7; ++sec) {
      EXPECT_EQ(h.at(h.t0 + sec * kSec).event, event_e::none) << sec;
      EXPECT_EQ(h.det.recent(h.t0 + sec * kSec).n, 24) << sec;
    }
    const decision_t d = h.at(h.t0 + 8 * kSec);
    EXPECT_EQ(d.event, event_e::kick);  // 24 筆夠了
    EXPECT_EQ(d.samples, 24);
  }
  // 間隔比 1/23 秒長 0.13 ns：每一個視窗裡剛好 23 筆，永遠不成立
  {
    hand_t h;
    h.step = 43'478'261;
    h.fed = h.t0 + h.step;
    for (int sec = 1; sec <= 30; ++sec) {
      EXPECT_EQ(h.at(h.t0 + sec * kSec).event, event_e::none) << sec;
      if (sec >= 2) {
        EXPECT_EQ(h.det.recent(h.t0 + sec * kSec).n, 23) << sec;
      }
      EXPECT_EQ(h.det.dwell_ns(h.t0 + sec * kSec + 1), 0) << sec;
    }
  }
}

// ── 3：B 滿 3 秒 → 剛好一次 kick；長度照週期算 ────────────────────────────────

TEST(VrDirectKickDetect, KicksOnceAfterThreeSecondsOfB) {
  struct case_t {
    int64_t period;
    int64_t burst;
    int per_second;
  };
  // 長度＝max(20 ms, 週期 × 1.25)：60 Hz 是 20.8 ms，72 Hz 與 90 Hz（11.1 ms）都是 20 ms
  for (const case_t &c : {case_t {kT60, 20'833'332, 60}, case_t {kT72, 20'000'000, 72}, case_t {kT90, 20'000'000, 90}}) {
    sim_t s(c.period, mode_e::b);
    s.kick_to(mode_e::a);  // kick 有用
    s.run(600 * kSec);
    ASSERT_EQ(s.count(event_e::kick), 1) << s.dump();
    ASSERT_EQ(s.ev.size(), 2u) << s.dump();
    const ev_t &k = s.ev[0];
    EXPECT_EQ(k.d.event, event_e::kick);
    // 暖機 5 秒＋連續成立 3 秒（每 100 ms 評估一次，所以最多晚 100 ms）
    EXPECT_TRUE(between(k.t - kStart, 8 * kSec, 8 * kSec + 100 * kMs));
    EXPECT_EQ(k.d.burst_ns, c.burst);
    EXPECT_EQ(k.d.kick_no, 1);
    EXPECT_TRUE(between(k.d.samples, c.per_second - 1, c.per_second + 1));
    EXPECT_TRUE(between(k.d.median_before_ns, 650'000, 950'000));
    EXPECT_LT(k.d.p90_before_ns, 1'800'001);
    EXPECT_EQ(s.ev[1].d.event, event_e::result_ok);
    // 確認用的是 1.5 秒的樣本（90 Hz 是 135 筆：環要裝得下）
    EXPECT_TRUE(between(s.ev[1].d.samples, c.per_second * 3 / 2 - 2, c.per_second * 3 / 2 + 2));
    EXPECT_EQ(s.det.kicks(), 1);
  }
}

TEST(VrDirectKickDetect, KicksAboutFourSecondsAfterBBegins) {
  // 先是 A；掉進 B 之後，視窗要先換成 B 的樣本（慢的那幾張降到一成以下，約 0.9 秒），再連續成立 3 秒
  sim_t s(kT60, mode_e::a);
  s.kick_to(mode_e::a);
  s.run(30 * kSec);
  s.mode = mode_e::b;
  const int64_t tb = s.now;
  ASSERT_TRUE(s.run_until(event_e::kick, 20 * kSec)) << s.dump();
  EXPECT_TRUE(between(s.ev.back().t - tb, 3'800 * kMs, 4'200 * kMs));
  EXPECT_EQ(s.ev.size(), 1u);
}

TEST(VrDirectKickDetect, BurstLengthFollowsThePeriod) {
  struct case_t {
    int64_t period;
    int64_t burst;
  };
  const case_t cases[] = {
    {11'000'000, 20'000'000},  // 13.75 ms → 下限 20 ms
    {15'999'999, 20'000'000},
    {16'000'000, 20'000'000},  // 剛好 20 ms
    {16'000'004, 20'000'005},
    {kT60, 20'833'332},
    {20'000'000, 25'000'000},
    {31'999'999, 39'999'998},
    {32'000'000, 40'000'000},  // 剛好 40 ms
    {33'333'333, 40'000'000},  // 41.7 ms → 上限 40 ms
    {40'000'000, 40'000'000},
    {kSec, 40'000'000},
    {kI64Max, 40'000'000},  // 不合理的週期也不會溢位
  };
  for (const case_t &c : cases) {
    detector_t d;
    d.reset(c.period, kStart);
    EXPECT_EQ(d.burst_ns(), c.burst) << "period=" << c.period;
    EXPECT_EQ(d.period_ns(), c.period);
  }
}

TEST(VrDirectKickDetect, DwellIsExactlyThreeSeconds) {
  for (const bool early : {true, false}) {
    hand_t h;
    h.quiet_until(h.t0 + 5 * kSec);
    EXPECT_EQ(h.at(h.t0 + 5 * kSec).event, event_e::none);  // 第一次評估：B 成立，開始計時
    h.quiet_until(h.t0 + 8 * kSec);
    if (early) {
      EXPECT_EQ(h.at(h.t0 + 8 * kSec - 1).event, event_e::none);  // 差 1 ns
      EXPECT_EQ(h.det.dwell_ns(h.t0 + 8 * kSec - 1), 3 * kSec - 1);
    } else {
      const decision_t d = h.at(h.t0 + 8 * kSec);
      EXPECT_EQ(d.event, event_e::kick);
      EXPECT_EQ(d.samples, 100);  // 每 10 ms 一筆，1 秒 100 筆
      EXPECT_EQ(d.median_before_ns, 800'000);
      EXPECT_EQ(d.p90_before_ns, 800'000);
    }
  }
}

TEST(VrDirectKickDetect, EvaluatesEveryHundredMilliseconds) {
  for (const bool early : {true, false}) {
    hand_t h;
    h.feeding = false;
    h.quiet_until(h.t0 + 5 * kSec);
    EXPECT_EQ(h.at(h.t0 + 5 * kSec).event, event_e::none);  // 第一次評估：一筆樣本都沒有
    // 接下來的 100 ms 裡來了 33 筆 B
    for (int64_t t = h.t0 + 5 * kSec + 3 * kMs; t < h.t0 + 5'100 * kMs; t += 3 * kMs) {
      h.det.on_present_phase(t, 800 * kUs);
    }
    const int64_t next = h.t0 + 5'100 * kMs;
    if (early) {
      EXPECT_EQ(h.at(next - 1).event, event_e::none);  // 離上一次評估還差 1 ns：不評估
      EXPECT_EQ(h.det.dwell_ns(next + kSec), 0);
    } else {
      EXPECT_EQ(h.at(next).event, event_e::none);  // 滿 100 ms：評估，B 成立，開始計時
      EXPECT_EQ(h.det.dwell_ns(next + kSec), kSec);
    }
  }
}

TEST(VrDirectKickDetect, AGapBetweenEvaluationsRestartsTheDwell) {
  // 兩次評估剛好隔 1 秒：每一筆樣本都還被某一次評估看到，算連續
  {
    hand_t h;
    h.quiet_until(h.t0 + 5 * kSec);
    EXPECT_EQ(h.at(h.t0 + 5 * kSec).event, event_e::none);
    EXPECT_EQ(h.at(h.t0 + 6 * kSec).event, event_e::none);
    EXPECT_EQ(h.at(h.t0 + 7 * kSec).event, event_e::none);
    EXPECT_EQ(h.at(h.t0 + 8 * kSec).event, event_e::kick);
  }
  // 隔了 1 秒又 1 ns（bridge 執行緒被卡住）：中間有樣本沒被看到，從這一次重新計時
  {
    hand_t h;
    h.quiet_until(h.t0 + 5 * kSec);
    EXPECT_EQ(h.at(h.t0 + 5 * kSec).event, event_e::none);
    EXPECT_EQ(h.at(h.t0 + 6 * kSec).event, event_e::none);
    EXPECT_EQ(h.at(h.t0 + 7 * kSec + 1).event, event_e::none);
    EXPECT_EQ(h.det.dwell_ns(h.t0 + 7 * kSec + 2), 1);
    EXPECT_EQ(h.at(h.t0 + 8 * kSec + 1).event, event_e::none);
    EXPECT_EQ(h.at(h.t0 + 9 * kSec + 1).event, event_e::none);
    EXPECT_EQ(h.at(h.t0 + 10 * kSec + 1).event, event_e::kick);
  }
}

// ── 11：B 與 A 交錯，連續成立不到 3 秒 ─────────────────────────────────────────

TEST(VrDirectKickDetect, AlternatingBAndANeverKicks) {
  sim_t s(kT60, mode_e::b);
  for (int i = 0; i < 300; ++i) {  // 10 分鐘，每 2 秒換一次
    s.mode = (i % 2) == 0 ? mode_e::b : mode_e::a;
    s.run(2 * kSec);
    EXPECT_LT(s.det.dwell_ns(s.now), 3 * kSec);
  }
  EXPECT_TRUE(s.ev.empty()) << s.dump();
  EXPECT_EQ(s.det.kicks(), 0);
}

// ── 9：樣本斷掉、前 5 秒不評估、generation 重設 ───────────────────────────────

TEST(VrDirectKickDetect, AGapInTheSamplesRestartsTheDwell) {
  sim_t s(kT60, mode_e::b);
  s.kick_to(mode_e::a);
  s.run(7 * kSec);  // 暖機 5 秒＋連續成立 2 秒
  EXPECT_TRUE(between(s.det.dwell_ns(s.now), 1'900 * kMs, 2 * kSec));
  s.mode = mode_e::silent;  // 沒有 Present
  s.run(1'200 * kMs);
  EXPECT_EQ(s.det.recent(s.now).n, 0);  // 超過 1 秒：視窗空了
  EXPECT_EQ(s.det.dwell_ns(s.now), 0);  // 連續成立的計時也歸零
  EXPECT_TRUE(s.ev.empty()) << s.dump();
  s.mode = mode_e::b;
  const int64_t back = s.now;
  ASSERT_TRUE(s.run_until(event_e::kick, 20 * kSec)) << s.dump();
  // 先收滿 24 筆（60 Hz 是 0.4 秒），再連續成立 3 秒——不是接著之前的 2 秒算
  EXPECT_TRUE(between(s.ev.back().t - back, 3'400 * kMs, 3'600 * kMs));
}

TEST(VrDirectKickDetect, NothingIsEvaluatedInTheFirstFiveSeconds) {
  hand_t h;
  h.quiet_until(h.t0 + 5 * kSec);
  EXPECT_EQ(h.at(h.t0 + 5 * kSec - 1).event, event_e::none);  // 還差 1 ns：不評估
  EXPECT_EQ(h.det.dwell_ns(h.t0 + 6 * kSec), 0);
  EXPECT_EQ(h.at(h.t0 + 5 * kSec).event, event_e::none);  // 第一次評估：B 成立，開始計時
  EXPECT_EQ(h.det.dwell_ns(h.t0 + 6 * kSec), 1 * kSec);
  h.quiet_until(h.t0 + 8 * kSec);
  EXPECT_EQ(h.at(h.t0 + 8 * kSec).event, event_e::kick);  // 5 + 3
}

TEST(VrDirectKickDetect, ResetStartsEverythingOver) {
  sim_t s(kT60, mode_e::b);
  s.run(7'900 * kMs);  // 差 100 ms 就要 kick 了
  EXPECT_TRUE(s.ev.empty());
  s.det.reset(kT60, s.now);  // driver 重連：新的 generation
  const int64_t r1 = s.now;
  EXPECT_EQ(s.det.dwell_ns(s.now), 0);
  EXPECT_EQ(s.det.recent(s.now).n, 0);  // 之前的樣本也不要了
  ASSERT_TRUE(s.run_until(event_e::kick, 20 * kSec)) << s.dump();
  EXPECT_TRUE(between(s.ev.back().t - r1, 8 * kSec, 8 * kSec + 100 * kMs));  // 又是暖機 5 秒＋3 秒
  ASSERT_TRUE(s.run_until(event_e::result_no_change, 5 * kSec));
  EXPECT_EQ(s.det.kicks(), 1);
  EXPECT_EQ(s.det.backoff_step(), 1);

  // 再重設一次：次數、退避都歸零，不必等上一個 generation 的退避
  s.det.reset(kT60, s.now);
  const int64_t r2 = s.now;
  EXPECT_EQ(s.det.kicks(), 0);
  EXPECT_EQ(s.det.backoff_step(), 0);
  ASSERT_TRUE(s.run_until(event_e::kick, 20 * kSec)) << s.dump();
  EXPECT_TRUE(between(s.ev.back().t - r2, 8 * kSec, 8 * kSec + 100 * kMs));
  EXPECT_EQ(s.ev.back().d.kick_no, 1);

  // 確認到一半重設：那一次不會再有結果
  s.run(1 * kSec);
  EXPECT_TRUE(s.det.verifying());
  const std::size_t before = s.ev.size();
  s.det.reset(kT60, s.now);
  EXPECT_FALSE(s.det.verifying());
  s.run(7 * kSec);
  EXPECT_EQ(s.ev.size(), before) << s.dump();

  // 更新率改成 120 Hz 的 generation：不動作
  s.det.reset(kT120, s.now);
  s.run(60 * kSec);
  EXPECT_EQ(s.ev.size(), before) << s.dump();
}

TEST(VrDirectKickDetect, WorksWithAnyTimeOrigin) {
  // 時間是 QPC 換算來的，起點是任意的：負的、0、很大都一樣
  for (const int64_t origin : {-100 * kSec, int64_t {0}, kI64Max / 2}) {
    sim_t s(kT60, mode_e::b, origin);
    s.kick_to(mode_e::a);
    s.run(20 * kSec);
    ASSERT_EQ(s.ev.size(), 2u) << s.dump();
    EXPECT_EQ(s.ev[0].d.event, event_e::kick);
    EXPECT_TRUE(between(s.ev[0].t - origin, 8 * kSec, 8 * kSec + 100 * kMs));
    EXPECT_EQ(s.ev[1].d.event, event_e::result_ok);
    EXPECT_EQ(s.ev[1].t - s.ev[0].t, 2 * kSec);
  }
}

TEST(VrDirectKickDetect, AClockThatGoesBackIsTreatedAsStandingStill) {
  sim_t s(kT60, mode_e::b);
  s.kick_to(mode_e::a);
  s.run(7 * kSec);
  EXPECT_EQ(s.det.poll(kStart).event, event_e::none);  // 回到起點的時刻：當成沒動
  EXPECT_EQ(s.det.poll(kI64Min).event, event_e::none);
  EXPECT_TRUE(between(s.det.dwell_ns(s.now), 1'900 * kMs, 2 * kSec));  // 計時沒有被打亂
  ASSERT_TRUE(s.run_until(event_e::kick, 5 * kSec)) << s.dump();
  EXPECT_TRUE(between(s.ev.back().t - kStart, 8 * kSec, 8 * kSec + 100 * kMs));

  // 外部 kick 報的時刻比現在早：也當成現在（5 秒的間隔從現在起算，不是從那個比較早的時刻）
  sim_t t(kT60, mode_e::b);
  t.run(7'900 * kMs);
  const int64_t x = t.now;
  t.det.on_external_kick(x - 10 * kSec, 20 * kMs);
  ASSERT_TRUE(t.run_until(event_e::kick, 20 * kSec)) << t.dump();
  EXPECT_TRUE(between(t.ev.back().t - x, 5 * kSec, 5 * kSec + 100 * kMs));
}

TEST(VrDirectKickDetect, TimesNearTheEndOfInt64Saturate) {
  // 時間加到 int64 的盡頭時飽和，不溢位（用 -ftrapv 建這個檔時，有號溢位會直接中止）
  using namespace vr::kick::detail;
  EXPECT_EQ(elapsed(5, 3), 2);
  EXPECT_EQ(elapsed(3, 5), 0);
  EXPECT_EQ(elapsed(kI64Max, kI64Min), kI64Max);
  EXPECT_EQ(elapsed(0, kI64Min), kI64Max);
  EXPECT_EQ(elapsed(kI64Min, kI64Max), 0);
  EXPECT_EQ(add_sat(kI64Max - 1, 5), kI64Max);
  EXPECT_EQ(add_sat(kI64Min, 5), kI64Min + 5);
  EXPECT_EQ(sub_sat(kI64Min + 1, 5), kI64Min);
  EXPECT_EQ(sub_sat(kI64Max, 5), kI64Max - 5);
  EXPECT_EQ(mul_div(kT60, 5, 4), 20'833'332);
  EXPECT_EQ(mul_div(kT60, 4, 5), 13'333'332);
  EXPECT_EQ(mul_div(kI64Max, 4, 5), 7'378'697'629'483'820'645);

  // 起點離盡頭 9 秒：暖機 5 秒＋連續成立 3 秒，kick 落在盡頭前 1 秒；確認的結束時刻飽和在盡頭
  detector_t d;
  const int64_t t0 = kI64Max - 9 * kSec;
  d.reset(kT60, t0);
  int64_t kick_at = 0;
  for (int64_t t = t0 + 2 * kMs; t <= kI64Max - 2 * kMs; t += 2 * kMs) {
    if ((t - t0) % (16 * kMs) == 0) {
      d.on_present_phase(t, 800 * kUs);
    }
    const decision_t r = d.poll(t);
    if (r.event == event_e::kick) {
      EXPECT_EQ(kick_at, 0);
      kick_at = t;
    } else {
      EXPECT_EQ(r.event, event_e::none);
    }
  }
  EXPECT_TRUE(between(kick_at - t0, 8 * kSec, 8 * kSec + 100 * kMs));
  EXPECT_TRUE(d.verifying());
  const decision_t r = d.poll(kI64Max);
  EXPECT_EQ(r.event, event_e::result_no_change);  // 最後不到 0.5 秒的樣本還是 B
  EXPECT_EQ(r.next_try_ns, 10 * kSec);
  d.on_external_kick(kI64Max, kI64Max);
  EXPECT_EQ(d.poll(kI64Max).event, event_e::none);
  EXPECT_EQ(d.recent(kI64Max).n, 0);
}

// ── 7：只在週期夠長時動作 ─────────────────────────────────────────────────────

TEST(VrDirectKickPeriod, OnlyActsAtElevenMillisecondsOrLonger) {
  // 120 Hz：B 自己會消失，永遠不 kick
  sim_t hz120(kT120, mode_e::b);
  hz120.run(600 * kSec);
  EXPECT_TRUE(hz120.ev.empty()) << hz120.dump();
  EXPECT_FALSE(hz120.det.active());
  EXPECT_EQ(hz120.det.recent(hz120.now).n, 0);  // 不動作時連樣本都不收

  // 邊界：剛好 11 ms 會動作，少 1 ns 不會
  sim_t on(11'000'000, mode_e::b);
  on.run(9 * kSec);
  EXPECT_TRUE(on.det.active());
  ASSERT_EQ(on.ev.size(), 1u) << on.dump();
  EXPECT_EQ(on.ev[0].d.event, event_e::kick);
  EXPECT_EQ(on.ev[0].d.burst_ns, 20 * kMs);

  sim_t below(10'999'999, mode_e::b);
  below.run(600 * kSec);
  EXPECT_FALSE(below.det.active());
  EXPECT_TRUE(below.ev.empty()) << below.dump();

  // 沒有 config 的 generation（週期 0）、不合理的值、還沒 reset 過：都不動作
  for (const int64_t period : {int64_t {0}, int64_t {-1}, kI64Min}) {
    sim_t idle(period, mode_e::b);
    idle.run(60 * kSec);
    EXPECT_FALSE(idle.det.active());
    EXPECT_EQ(idle.det.period_ns(), 0);
    EXPECT_TRUE(idle.ev.empty()) << idle.dump();
  }
  detector_t fresh;
  EXPECT_FALSE(fresh.active());
  for (int i = 0; i < 1000; ++i) {
    fresh.on_present_phase(kStart + i * kT60, 800 * kUs);
    EXPECT_EQ(fresh.poll(kStart + i * kT60).event, event_e::none);
  }
  fresh.on_external_kick(kStart + 20 * kSec, 20 * kMs);
  fresh.kick_not_done();
  EXPECT_EQ(fresh.poll(kStart + 30 * kSec).event, event_e::none);
}

// ── 4：kick 之後變成 A ────────────────────────────────────────────────────────

TEST(VrDirectKickResult, BackToAIsReportedOnceAndLeavesNoBackoff) {
  sim_t s(kT60, mode_e::b);
  s.kick_to(mode_e::a);
  ASSERT_TRUE(s.run_until(event_e::kick, 20 * kSec)) << s.dump();
  const int64_t k1 = s.ev.back().t;
  EXPECT_TRUE(s.det.verifying());
  ASSERT_TRUE(s.run_until(event_e::result_ok, 5 * kSec)) << s.dump();
  const ev_t r = s.ev.back();
  EXPECT_EQ(r.t - k1, 2 * kSec);  // 前 500 ms 不看＋收 1.5 秒
  EXPECT_EQ(r.d.kick_no, 1);
  EXPECT_TRUE(between(r.d.samples, 89, 91));  // 1.5 秒 × 60 Hz
  EXPECT_TRUE(between(r.d.median_before_ns, 650'000, 950'000));
  EXPECT_TRUE(between(r.d.median_after_ns, 2'900'000, 3'900'000));
  EXPECT_EQ(r.d.next_try_ns, 0);
  EXPECT_FALSE(s.det.verifying());
  EXPECT_FALSE(s.det.stopped());
  EXPECT_EQ(s.det.backoff_step(), 0);

  // 留在 A：之後不再有任何事件
  s.run(120 * kSec);
  EXPECT_EQ(s.ev.size(), 2u) << s.dump();

  // 又掉回 B（又開關了一次儀表板、或長停頓）：照樣再推一次
  s.mode = mode_e::b;
  const int64_t tb = s.now;
  ASSERT_TRUE(s.run_until(event_e::kick, 20 * kSec)) << s.dump();
  EXPECT_TRUE(between(s.ev.back().t - tb, 3'800 * kMs, 4'200 * kMs));
  EXPECT_EQ(s.ev.back().d.kick_no, 2);
  ASSERT_TRUE(s.run_until(event_e::result_ok, 5 * kSec)) << s.dump();
  EXPECT_EQ(s.ev.size(), 4u);
}

TEST(VrDirectKickResult, SecondKickIsAtLeastFiveSecondsAfterTheFirst) {
  // kick 有用，但 1.4 秒後又掉回 B（確認的那 1.5 秒裡 A 佔 0.9 秒，中位數還是 A → 算成功）。
  // 下一次最快也要離上一次 5 秒：0.5＋1.5 秒確認，再連續成立 3 秒
  sim_t s(kT60, mode_e::b);
  s.kick_to(mode_e::a);
  ASSERT_TRUE(s.run_until(event_e::kick, 20 * kSec)) << s.dump();
  const int64_t k1 = s.ev.back().t;
  s.run(1'400 * kMs);
  s.mode = mode_e::b;
  ASSERT_TRUE(s.run_until(event_e::result_ok, 5 * kSec)) << s.dump();
  ASSERT_TRUE(s.run_until(event_e::kick, 20 * kSec)) << s.dump();
  EXPECT_TRUE(between(s.ev.back().t - k1, 5 * kSec, 5'500 * kMs));
  EXPECT_EQ(s.ev.size(), 3u) << s.dump();
}

// ── 5：kick 之後還是 B ───────────────────────────────────────────────────────

TEST(VrDirectKickResult, NoChangeBacksOffTenThirtyNinetyThreeHundred) {
  sim_t s(kT60, mode_e::b);  // kick 沒有用：一直是 B
  const int64_t waits[] = {10, 30, 90, 300, 300, 300};
  int64_t last_result = 0;
  for (int i = 0; i < 6; ++i) {
    ASSERT_TRUE(s.run_until(event_e::kick, 400 * kSec)) << i << " " << s.dump();
    const int64_t k = s.ev.back().t;
    EXPECT_EQ(s.ev.back().d.kick_no, i + 1);
    if (i > 0) {
      // 退避從上一次的結果起算；B 一直成立，所以時間一到的那一次評估就做（不必再等 3 秒）
      EXPECT_TRUE(between(k - last_result, waits[i - 1] * kSec, waits[i - 1] * kSec + 100 * kMs)) << i;
    }
    ASSERT_TRUE(s.run_until(event_e::result_no_change, 5 * kSec)) << i << " " << s.dump();
    last_result = s.ev.back().t;
    EXPECT_EQ(last_result - k, 2 * kSec);
    EXPECT_EQ(s.ev.back().d.next_try_ns, waits[i] * kSec) << i;
    EXPECT_EQ(s.ev.back().d.kick_no, i + 1);
    EXPECT_TRUE(between(s.ev.back().d.median_after_ns, 650'000, 950'000));
    EXPECT_TRUE(between(s.ev.back().d.samples, 89, 91));
    EXPECT_EQ(s.det.backoff_step(), std::min(i + 1, 4));
    EXPECT_FALSE(s.det.stopped());
  }
  EXPECT_EQ(s.ev.size(), 12u) << s.dump();
}

TEST(VrDirectKickResult, BackoffIsCountedFromTheResult) {
  for (const bool early : {true, false}) {
    hand_t h;
    const int64_t k = h.first_kick();
    h.quiet_until(k + 2 * kSec);
    EXPECT_EQ(h.at(k + 2 * kSec - 1).event, event_e::none);  // 確認的 1.5 秒還差 1 ns
    const decision_t r = h.at(k + 2 * kSec);
    ASSERT_EQ(r.event, event_e::result_no_change);
    EXPECT_EQ(r.samples, 150);  // [k + 0.5 s, k + 2 s) 每 10 ms 一筆
    EXPECT_EQ(r.median_after_ns, 800'000);
    EXPECT_EQ(r.next_try_ns, 10 * kSec);
    const int64_t due = k + 12 * kSec;
    h.quiet_until(due);
    if (early) {
      EXPECT_EQ(h.at(due - 1).event, event_e::none);
    } else {
      const decision_t d = h.at(due);
      EXPECT_EQ(d.event, event_e::kick);
      EXPECT_EQ(d.kick_no, 2);
    }
  }
}

TEST(VrDirectKickResult, ASuccessInBetweenClearsTheBackoff) {
  sim_t s(kT60, mode_e::b);
  for (int i = 0; i < 2; ++i) {  // 兩次沒有用
    ASSERT_TRUE(s.run_until(event_e::kick, 400 * kSec)) << s.dump();
    ASSERT_TRUE(s.run_until(event_e::result_no_change, 5 * kSec)) << s.dump();
  }
  EXPECT_EQ(s.det.backoff_step(), 2);
  EXPECT_EQ(s.ev.back().d.next_try_ns, 30 * kSec);

  s.kick_to(mode_e::a);  // 第三次有用
  ASSERT_TRUE(s.run_until(event_e::kick, 400 * kSec)) << s.dump();
  ASSERT_TRUE(s.run_until(event_e::result_ok, 5 * kSec)) << s.dump();
  EXPECT_EQ(s.det.backoff_step(), 0);

  // 又掉回 B，而且這次又沒有用：退避從 10 秒重新開始；而且不必等之前那 30 秒
  s.switch_on_kick = false;
  s.mode = mode_e::b;
  const int64_t tb = s.now;
  ASSERT_TRUE(s.run_until(event_e::kick, 400 * kSec)) << s.dump();
  EXPECT_TRUE(between(s.ev.back().t - tb, 3'800 * kMs, 4'200 * kMs));
  ASSERT_TRUE(s.run_until(event_e::result_no_change, 5 * kSec)) << s.dump();
  EXPECT_EQ(s.ev.back().d.next_try_ns, 10 * kSec);
  EXPECT_EQ(s.det.backoff_step(), 1);
}

// ── 6：kick 之後變成 S ───────────────────────────────────────────────────────

TEST(VrDirectKickResult, EndingUpInSIsNoChangeAndThenQuiet) {
  sim_t s(kT60, mode_e::b);
  s.kick_to(mode_e::s);  // 推的那一刻儀表板剛好打開
  ASSERT_TRUE(s.run_until(event_e::kick, 20 * kSec)) << s.dump();
  ASSERT_TRUE(s.run_until(event_e::result_no_change, 5 * kSec)) << s.dump();
  EXPECT_TRUE(between(s.ev.back().d.median_after_ns, 1'200'000, 2'100'000));
  EXPECT_EQ(s.ev.back().d.next_try_ns, 10 * kSec);
  // 之後不是 B，所以退避過了也不會再推
  s.run(600 * kSec);
  EXPECT_EQ(s.ev.size(), 2u) << s.dump();
  EXPECT_EQ(s.det.kicks(), 1);
}

TEST(VrDirectKickResult, SuccessBoundaryIsTwoMilliseconds) {
  // 確認的那 1.5 秒每一筆都剛好 2.0 ms：中位數「大於等於」門檻，算回到 A
  sim_t on(kT60, mode_e::b);
  on.fixed_ns = 2'000'000;
  on.kick_to(mode_e::fixed);
  on.run(11 * kSec);
  ASSERT_EQ(on.ev.size(), 2u) << on.dump();
  EXPECT_EQ(on.ev[1].d.event, event_e::result_ok);
  EXPECT_EQ(on.ev[1].d.median_after_ns, 2'000'000);

  // 少 1 ns：沒有變化
  sim_t below(kT60, mode_e::b);
  below.fixed_ns = 1'999'999;
  below.kick_to(mode_e::fixed);
  below.run(11 * kSec);
  ASSERT_EQ(below.ev.size(), 2u) << below.dump();
  EXPECT_EQ(below.ev[1].d.event, event_e::result_no_change);
  EXPECT_EQ(below.ev[1].d.median_after_ns, 1'999'999);
}

TEST(VrDirectKickResult, IgnoresTheFirstHalfSecondThenChecksOneAndAHalf) {
  struct case_t {
    int64_t extra_at;  ///< 第 24 筆 A 的樣本放在 kick 之後多久
    bool counted;
  };
  const case_t cases[] = {
    {500 * kMs, true},  // 剛好滿 500 ms：算
    {500 * kMs - 1, false},  // 還在不看的那一段裡
    {2'000 * kMs - 1, true},  // 確認的 1.5 秒的最後 1 ns
    {2'000 * kMs, false},  // 已經超過
  };
  for (const case_t &c : cases) {
    hand_t h;
    const int64_t k = h.first_kick();
    h.feeding = false;
    // kick 本身那幾張與之後的過渡（PostPresent 立刻返回時間隔很短）：全部不看
    for (int64_t t = k + 1 * kMs; t < k + 500 * kMs; t += 2 * kMs) {
      h.det.on_present_phase(t, 300 * kUs);
    }
    // 確認的那一段裡 23 筆 A，第 24 筆放在邊界上
    for (int i = 0; i < 23; ++i) {
      h.det.on_present_phase(k + 600 * kMs + i * 10 * kMs, 3'300 * kUs);
    }
    h.det.on_present_phase(k + c.extra_at, 3'300 * kUs);
    EXPECT_EQ(h.det.poll(k + 2 * kSec - 1).event, event_e::none);
    const decision_t d = h.det.poll(k + 2 * kSec);
    // 23 筆不夠（至少要 24 筆）：量不出結果，照沒有變化處理
    EXPECT_EQ(d.event, c.counted ? event_e::result_ok : event_e::result_no_change) << "extra at +" << c.extra_at;
    EXPECT_EQ(d.samples, c.counted ? 24 : 23) << "extra at +" << c.extra_at;
    EXPECT_EQ(d.median_after_ns, 3'300 * kUs);
    EXPECT_EQ(d.median_before_ns, 800 * kUs);
  }
}

TEST(VrDirectKickResult, NoFramesToCheckCountsAsNoChange) {
  // kick 之後就沒有 Present 了（HMD 進 standby、SteamVR 關了）：量不出結果，照沒有變化處理、照樣退避
  sim_t s(kT60, mode_e::b);
  s.kick_to(mode_e::silent);
  ASSERT_TRUE(s.run_until(event_e::kick, 20 * kSec)) << s.dump();
  const int64_t k = s.ev.back().t;
  ASSERT_TRUE(s.run_until(event_e::result_no_change, 5 * kSec)) << s.dump();
  EXPECT_EQ(s.ev.back().t - k, 2 * kSec);
  EXPECT_EQ(s.ev.back().d.samples, 0);
  EXPECT_EQ(s.ev.back().d.next_try_ns, 10 * kSec);
  const int64_t r = s.ev.back().t;
  s.switch_on_kick = false;
  s.mode = mode_e::b;  // Present 回來了，還是 B：等退避的 10 秒
  ASSERT_TRUE(s.run_until(event_e::kick, 60 * kSec)) << s.dump();
  EXPECT_TRUE(between(s.ev.back().t - r, 10 * kSec, 10 * kSec + 100 * kMs));
}

// ── 8：重負載 ─────────────────────────────────────────────────────────────────

TEST(VrDirectKickResult, AHeavyGameStopsFurtherKicks) {
  sim_t s(kT60, mode_e::b);
  s.kick_to(mode_e::heavy);  // 回到 A 了，但遊戲一幀要 13.7～15.5 ms
  ASSERT_TRUE(s.run_until(event_e::kick, 20 * kSec)) << s.dump();
  ASSERT_TRUE(s.run_until(event_e::result_ok, 5 * kSec)) << s.dump();
  const int64_t r = s.ev.back().t;
  EXPECT_TRUE(s.det.stopped());
  // 通知排在結果後面的那一次 poll()
  ASSERT_TRUE(s.run_until(event_e::heavy_stop, kSec)) << s.dump();
  EXPECT_EQ(s.ev.back().t - r, 2 * kMs);
  EXPECT_GT(s.ev.back().d.p90_after_ns, 13'333'333);  // 週期 16.67 ms 的 80%
  EXPECT_EQ(s.ev.back().d.kick_no, 1);
  EXPECT_EQ(s.ev.size(), 3u) << s.dump();

  // 之後即使又掉回 B 也不再推（留在 B 不一定比較差）
  s.mode = mode_e::b;
  s.run(1'800 * kSec);
  EXPECT_EQ(s.ev.size(), 3u) << s.dump();
  EXPECT_EQ(s.det.kicks(), 1);
  EXPECT_TRUE(s.det.stopped());
  // 樣本照收（觀察點還看得到現在是 B），只是不再評估
  EXPECT_TRUE(between(s.det.recent(s.now).n, 59, 61));
  EXPECT_TRUE(between(s.det.recent(s.now).median_ns, 650'000, 950'000));
  EXPECT_EQ(s.det.dwell_ns(s.now), 0);
}

TEST(VrDirectKickResult, HeavyLooksAtTheNinetiethPercentile) {
  // 回到 A 了：多數的幀 10 ms（週期的 60%），五張裡有一張 15 ms → 第 90 百分位超過週期的 80%，算重負載
  sim_t s(kT60, mode_e::b);
  s.mix_lo = 10'000'000;
  s.mix_hi = 15'000'000;
  s.mix_every = 5;
  s.kick_to(mode_e::mix);
  s.run(11 * kSec);
  ASSERT_EQ(s.ev.size(), 3u) << s.dump();
  EXPECT_EQ(s.ev[1].d.event, event_e::result_ok);
  EXPECT_EQ(s.ev[1].d.median_after_ns, 10'000'000);
  EXPECT_EQ(s.ev[2].d.event, event_e::heavy_stop);
  EXPECT_EQ(s.ev[2].d.p90_after_ns, 15'000'000);

  // 十二張裡才一張 15 ms：第 90 百分位還是 10 ms，不算
  sim_t few(kT60, mode_e::b);
  few.mix_lo = 10'000'000;
  few.mix_hi = 15'000'000;
  few.mix_every = 12;
  few.kick_to(mode_e::mix);
  few.run(11 * kSec);
  ASSERT_EQ(few.ev.size(), 2u) << few.dump();
  EXPECT_EQ(few.ev[1].d.event, event_e::result_ok);
  EXPECT_EQ(few.ev[1].d.p90_after_ns, 10'000'000);
  EXPECT_FALSE(few.det.stopped());
}

TEST(VrDirectKickResult, HeavyBoundaryIsEightyPercentOfThePeriod) {
  struct case_t {
    int64_t period;
    int64_t phase;
    bool heavy;
  };
  const case_t cases[] = {
    {kT60, 13'333'332, false},  // 16'666'666 ns 的 80% 是 13'333'332.8：沒有超過
    {kT60, 13'333'333, true},
    {11'000'000, 8'800'000, false},  // 剛好 80%：不算
    {11'000'000, 8'800'001, true},
  };
  for (const case_t &c : cases) {
    sim_t s(c.period, mode_e::b);
    s.fixed_ns = c.phase;
    s.kick_to(mode_e::fixed);
    s.run(11 * kSec);
    ASSERT_GE(s.ev.size(), 2u) << s.dump();
    EXPECT_EQ(s.ev[1].d.event, event_e::result_ok);
    EXPECT_EQ(s.ev[1].d.p90_after_ns, c.phase);
    EXPECT_EQ(s.count(event_e::heavy_stop), c.heavy ? 1 : 0) << "period=" << c.period << " phase=" << c.phase;
    EXPECT_EQ(s.det.stopped(), c.heavy);
    EXPECT_EQ(s.ev.size(), c.heavy ? 3u : 2u) << s.dump();
  }
}

TEST(VrDirectKickResult, HeavyOnlyCountsAfterASuccess) {
  // kick 沒有用（中位數 1.5 ms），但五筆裡有一筆 15 ms：不是「回到 A 而且很重」，照一般的退避，不停手
  sim_t s(kT60, mode_e::b);
  s.mix_lo = 1'500'000;
  s.mix_hi = 15'000'000;
  s.mix_every = 5;
  s.kick_to(mode_e::mix);
  s.run(11 * kSec);
  ASSERT_EQ(s.ev.size(), 2u) << s.dump();
  EXPECT_EQ(s.ev[1].d.event, event_e::result_no_change);
  EXPECT_EQ(s.ev[1].d.p90_after_ns, 15'000'000);
  EXPECT_FALSE(s.det.stopped());
}

// ── 9：一個 generation 最多 20 次 ─────────────────────────────────────────────

TEST(VrDirectKickLimit, TwentyKicksPerGeneration) {
  sim_t s(kT60, mode_e::b);
  s.kick_to(mode_e::a);  // 每次都有用，但每次之後又掉回 B
  for (int i = 1; i <= 20; ++i) {
    ASSERT_TRUE(s.run_until(event_e::kick, 30 * kSec)) << i << " " << s.dump();
    EXPECT_EQ(s.ev.back().d.kick_no, i);
    ASSERT_TRUE(s.run_until(event_e::result_ok, 5 * kSec)) << i << " " << s.dump();
    EXPECT_EQ(s.det.stopped(), i == 20);
    s.mode = mode_e::b;
  }
  // 第 20 次的結果後面：通知一次
  ASSERT_TRUE(s.run_until(event_e::limit_stop, kSec)) << s.dump();
  EXPECT_EQ(s.ev.back().t - s.ev[s.ev.size() - 2].t, 2 * kMs);
  EXPECT_EQ(s.ev.back().d.kick_no, 20);
  // 之後一直是 B 也不再推
  s.run(1'800 * kSec);
  EXPECT_EQ(s.count(event_e::kick), 20);
  EXPECT_EQ(s.count(event_e::limit_stop), 1);
  EXPECT_EQ(s.ev.size(), 41u) << s.dump();
  EXPECT_EQ(s.det.kicks(), 20);

  // 新的 generation 重新算
  s.det.reset(kT60, s.now);
  EXPECT_FALSE(s.det.stopped());
  ASSERT_TRUE(s.run_until(event_e::kick, 30 * kSec)) << s.dump();
  EXPECT_EQ(s.ev.back().d.kick_no, 1);
}

TEST(VrDirectKickLimit, AHeavyTwentiethKickReportsTheLoad) {
  // 第 20 次剛好也是重負載：兩個停手的理由只通知一個（說得出原因的那個）
  sim_t s(kT60, mode_e::b);
  s.kick_to(mode_e::a);
  for (int i = 1; i <= 19; ++i) {
    ASSERT_TRUE(s.run_until(event_e::kick, 30 * kSec)) << i << " " << s.dump();
    ASSERT_TRUE(s.run_until(event_e::result_ok, 5 * kSec)) << i << " " << s.dump();
    s.mode = mode_e::b;
  }
  s.kick_to(mode_e::heavy);
  ASSERT_TRUE(s.run_until(event_e::kick, 30 * kSec)) << s.dump();
  EXPECT_EQ(s.ev.back().d.kick_no, 20);
  ASSERT_TRUE(s.run_until(event_e::result_ok, 5 * kSec)) << s.dump();
  s.mode = mode_e::b;
  s.run(60 * kSec);
  EXPECT_EQ(s.ev.back().d.event, event_e::heavy_stop);
  EXPECT_EQ(s.count(event_e::heavy_stop), 1);
  EXPECT_EQ(s.count(event_e::limit_stop), 0);
  EXPECT_EQ(s.count(event_e::kick), 20);
  EXPECT_EQ(s.ev.size(), 41u) << s.dump();
}

TEST(VrDirectKickLimit, FailedKicksCountToo) {
  sim_t s(kT60, mode_e::b);  // 每次都沒有用
  s.run(6'000 * kSec);  // 10＋30＋90＋300×17 秒的退避，再加每次 2 秒
  EXPECT_EQ(s.count(event_e::kick), 20) << s.dump();
  EXPECT_EQ(s.count(event_e::result_no_change), 20);
  EXPECT_EQ(s.count(event_e::limit_stop), 1);
  EXPECT_EQ(s.ev.size(), 41u);
  EXPECT_EQ(s.ev.back().d.event, event_e::limit_stop);
  EXPECT_TRUE(s.det.stopped());
  s.run(3'000 * kSec);
  EXPECT_EQ(s.ev.size(), 41u);
}

// ── 10：dev 的外部 kick ───────────────────────────────────────────────────────

TEST(VrDirectKickExternal, HidesItsSamplesAndRestartsTheDwell) {
  sim_t s(kT60, mode_e::b);
  s.run(7'900 * kMs);  // 暖機 5 秒＋連續成立 2.9 秒
  EXPECT_TRUE(between(s.det.dwell_ns(s.now), 2'800 * kMs, 2'900 * kMs));
  const int64_t x = s.now;
  s.det.on_external_kick(x, 20 * kMs);  // dev 的定時 kick（這一次沒有用：還是 B）
  EXPECT_EQ(s.det.dwell_ns(x + 1), 0);
  EXPECT_EQ(s.det.recent(x).n, 0);  // 之前的樣本不再拿來判定
  s.run(498 * kMs);
  EXPECT_EQ(s.det.recent(s.now).n, 0);  // 前 500 ms 的樣本不看
  EXPECT_EQ(s.det.dwell_ns(s.now), 0);
  s.run(502 * kMs);
  EXPECT_TRUE(between(s.det.recent(s.now).n, 29, 31));  // 只有 x + 0.5 s 之後的
  // 不算次數、沒有結果；自己的 kick 要等離那一次滿 5 秒（連續成立的 3 秒在 x + 3.9 s 就滿了）
  EXPECT_EQ(s.det.kicks(), 0);
  EXPECT_FALSE(s.det.verifying());
  ASSERT_TRUE(s.run_until(event_e::kick, 20 * kSec)) << s.dump();
  EXPECT_EQ(s.ev.size(), 1u) << s.dump();
  EXPECT_TRUE(between(s.ev[0].t - x, 5 * kSec, 5 * kSec + 100 * kMs));
  EXPECT_EQ(s.ev[0].d.kick_no, 1);
}

TEST(VrDirectKickExternal, OwnKickWaitsFiveSecondsAfterIt) {
  for (const bool early : {true, false}) {
    hand_t h;
    h.quiet_until(h.t0 + 6 * kSec);  // 5.0 s 的那一次開始計時
    const int64_t x = h.t0 + 6 * kSec;
    h.det.on_external_kick(x, 20 * kMs);
    h.last = x;
    // x + 0.5 s 恢復評估（只有 1 筆）、x + 1.0 s 成立、x + 4.0 s 滿 3 秒，但離外面那一次還不到 5 秒
    h.quiet_until(x + 5 * kSec);
    EXPECT_EQ(h.det.dwell_ns(x + 4'500 * kMs), 3'500 * kMs);
    if (early) {
      EXPECT_EQ(h.at(x + 5 * kSec - 1).event, event_e::none);
    } else {
      EXPECT_EQ(h.at(x + 5 * kSec).event, event_e::kick);
    }
  }
}

TEST(VrDirectKickExternal, DoesNotTouchTheBackoff) {
  sim_t s(kT60, mode_e::b);
  for (int i = 0; i < 2; ++i) {  // 兩次沒有用：退避 30 秒
    ASSERT_TRUE(s.run_until(event_e::kick, 400 * kSec)) << s.dump();
    ASSERT_TRUE(s.run_until(event_e::result_no_change, 5 * kSec)) << s.dump();
  }
  const int64_t r2 = s.ev.back().t;
  EXPECT_EQ(s.ev.back().d.next_try_ns, 30 * kSec);
  s.run(10 * kSec);
  s.det.on_external_kick(s.now, 20 * kMs);  // 也沒有用
  EXPECT_EQ(s.det.backoff_step(), 2);
  EXPECT_EQ(s.det.kicks(), 2);
  EXPECT_FALSE(s.det.verifying());
  // 下一次還是那 30 秒滿的時候：沒有提前、沒有延後，中間也沒有多出結果
  ASSERT_TRUE(s.run_until(event_e::kick, 60 * kSec)) << s.dump();
  EXPECT_TRUE(between(s.ev.back().t - r2, 30 * kSec, 30 * kSec + 100 * kMs));
  EXPECT_EQ(s.ev.back().d.kick_no, 3);
  EXPECT_EQ(s.ev.size(), 5u) << s.dump();
  // 第三次照舊算：沒有用 → 90 秒
  ASSERT_TRUE(s.run_until(event_e::result_no_change, 5 * kSec)) << s.dump();
  EXPECT_EQ(s.ev.back().d.next_try_ns, 90 * kSec);
}

TEST(VrDirectKickExternal, DuringACheckTheResultIsMeasuredAfterBoth) {
  sim_t s(kT60, mode_e::b);  // 自己的 kick 沒有用
  ASSERT_TRUE(s.run_until(event_e::kick, 20 * kSec)) << s.dump();
  s.run(1 * kSec);
  const int64_t x = s.now;
  s.det.on_external_kick(x, 20 * kMs);  // 1 秒後 dev 的 kick 有用
  s.mode = mode_e::a;
  EXPECT_TRUE(s.det.verifying());
  ASSERT_TRUE(s.run_until(event_e::result_ok, 5 * kSec)) << s.dump();
  EXPECT_EQ(s.ev.back().t - x, 2 * kSec);  // 從外面那一次重新等 500 ms、再收 1.5 秒
  EXPECT_TRUE(between(s.ev.back().d.samples, 89, 91));
  EXPECT_TRUE(between(s.ev.back().d.median_after_ns, 2'900'000, 3'900'000));
  EXPECT_EQ(s.ev.size(), 2u) << s.dump();  // 只有一個結果
  EXPECT_EQ(s.det.kicks(), 1);
  EXPECT_EQ(s.det.backoff_step(), 0);
}

TEST(VrDirectKickExternal, ALongerBurstHidesMore) {
  struct case_t {
    int64_t burst;
    int64_t quiet;  ///< 之後多久的樣本不看
  };
  // 不看的 500 ms 是照「kick 最長 40 ms」定的；更長的往後延（vr_dev_kick_ms 最大 200）
  const case_t cases[] = {
    {1 * kMs, 500 * kMs},
    {40 * kMs, 500 * kMs},
    {200 * kMs, 660 * kMs},
    {0, 500 * kMs},
    {-5, 500 * kMs},
    {kI64Min, 500 * kMs},
    {kI64Max, 10'500 * kMs},  // 不合理的值：延到上限為止，不會溢位
  };
  for (const case_t &c : cases) {
    detector_t d;
    d.reset(kT60, kStart);
    const int64_t x = kStart + 20 * kSec;
    d.on_external_kick(x, c.burst);
    d.on_present_phase(x + c.quiet - 1, 800 * kUs);  // 還在不看的那一段
    d.on_present_phase(x + c.quiet, 900 * kUs);  // 剛好出來
    const stats_t s = d.recent(x + c.quiet + 10 * kMs);
    EXPECT_EQ(s.n, 1) << "burst=" << c.burst;
    EXPECT_EQ(s.median_ns, 900 * kUs) << "burst=" << c.burst;
  }
}

// ── 12：kick 沒做成 ───────────────────────────────────────────────────────────

TEST(VrDirectKickDeclined, IsForgottenAndTriedAgainAfterAnotherDwell) {
  sim_t s(kT60, mode_e::b);
  s.kick_to(mode_e::a);
  s.decline = true;  // 另一段節拍調整正在進行：做不了
  ASSERT_TRUE(s.run_until(event_e::kick, 20 * kSec)) << s.dump();
  const int64_t k1 = s.ev.back().t;
  EXPECT_TRUE(s.ev.back().declined);
  EXPECT_EQ(s.ev.back().d.kick_no, 1);
  EXPECT_EQ(s.det.kicks(), 0);  // 不算次數
  EXPECT_FALSE(s.det.verifying());  // 不等結果
  EXPECT_EQ(s.det.backoff_step(), 0);
  EXPECT_TRUE(between(s.det.recent(s.now).n, 59, 61));  // 樣本照常看（沒有進入「不看」的那一段）

  // B 還在：再連續成立 3 秒就再回一次。不受 5 秒的間隔限制——上一次根本沒做
  s.decline = false;
  ASSERT_TRUE(s.run_until(event_e::kick, 20 * kSec)) << s.dump();
  EXPECT_EQ(s.ev.size(), 2u) << s.dump();  // 中間沒有結果
  EXPECT_TRUE(between(s.ev.back().t - k1, 3 * kSec, 3 * kSec + 200 * kMs));
  EXPECT_FALSE(s.ev.back().declined);
  EXPECT_EQ(s.ev.back().d.kick_no, 1);  // 這才是第一次
  ASSERT_TRUE(s.run_until(event_e::result_ok, 5 * kSec)) << s.dump();
  EXPECT_EQ(s.ev.size(), 3u) << s.dump();
  EXPECT_EQ(s.det.kicks(), 1);
}

TEST(VrDirectKickDeclined, NeverGetsStuckAndDoesNotUseUpTheLimit) {
  sim_t s(kT60, mode_e::b);
  s.kick_to(mode_e::a);
  s.decline = true;
  s.run(120 * kSec);  // 兩分鐘都做不了：每 3.1 秒問一次
  EXPECT_TRUE(between(s.count(event_e::kick), 34, 38)) << s.dump();  // 超過 20 次也照問
  EXPECT_EQ(static_cast<int>(s.ev.size()), s.count(event_e::kick)) << s.dump();  // 沒有結果、沒有停手
  for (const ev_t &e : s.ev) {
    EXPECT_TRUE(e.declined);
    EXPECT_EQ(e.d.kick_no, 1);
  }
  EXPECT_EQ(s.det.kicks(), 0);
  EXPECT_FALSE(s.det.stopped());
  for (std::size_t i = 1; i < s.ev.size(); ++i) {
    EXPECT_TRUE(between(s.ev[i].t - s.ev[i - 1].t, 3 * kSec, 3 * kSec + 200 * kMs));
  }

  // 可以做了：照常
  s.decline = false;
  const std::size_t before = s.ev.size();
  ASSERT_TRUE(s.run_until(event_e::kick, 5 * kSec)) << s.dump();
  EXPECT_EQ(s.ev.back().d.kick_no, 1);
  ASSERT_TRUE(s.run_until(event_e::result_ok, 5 * kSec)) << s.dump();
  EXPECT_EQ(s.ev.size(), before + 2);
}

TEST(VrDirectKickDeclined, OnlyAppliesRightAfterTheKick) {
  sim_t s(kT60, mode_e::b);
  ASSERT_TRUE(s.run_until(event_e::kick, 20 * kSec)) << s.dump();
  s.tick();  // 又過了一次 poll()：那個 kick 已經當成做了
  s.det.kick_not_done();
  EXPECT_TRUE(s.det.verifying());
  EXPECT_EQ(s.det.kicks(), 1);
  ASSERT_TRUE(s.run_until(event_e::result_no_change, 5 * kSec)) << s.dump();
  s.det.kick_not_done();  // 結果之後呼叫也沒有作用
  EXPECT_EQ(s.det.kicks(), 1);
  EXPECT_EQ(s.det.backoff_step(), 1);

  // 緊接在 kick 後面、但中間插了一次外部 kick：那個 kick 也當成做了
  sim_t t(kT60, mode_e::b);
  ASSERT_TRUE(t.run_until(event_e::kick, 20 * kSec)) << t.dump();
  t.det.on_external_kick(t.now, 20 * kMs);
  t.det.kick_not_done();
  EXPECT_TRUE(t.det.verifying());
  EXPECT_EQ(t.det.kicks(), 1);
}

// ── 一整段像實驗那樣的場次 ────────────────────────────────────────────────────

TEST(VrDirectKickScenario, DashboardThenALongPause) {
  // 照 2026-10-10 的實驗：開場 host 的儀表板開著（S）30 秒 → 關掉之後落在 B → 自動推回 A；
  // 一分鐘後遊戲停頓 1.5 秒（合成器照樣 Present，數值像 S）→ 又落在 B → 再推一次
  sim_t s(kT60, mode_e::s);
  s.kick_to(mode_e::a);
  s.run(30 * kSec);
  s.mode = mode_e::b;
  const int64_t b1 = s.now;
  s.run(60 * kSec);
  s.mode = mode_e::s;
  s.run(1'500 * kMs);
  s.mode = mode_e::b;
  const int64_t b2 = s.now;
  s.run(120 * kSec);
  ASSERT_EQ(s.ev.size(), 4u) << s.dump();
  // 從 S 進 B 比從 A 進 B 快一點成立：S 的樣本只要降到一半以下（約 0.6 秒）中位數就過了門檻，再連續成立 3 秒
  EXPECT_EQ(s.ev[0].d.event, event_e::kick);
  EXPECT_TRUE(between(s.ev[0].t - b1, 3'500 * kMs, 3'800 * kMs));
  EXPECT_EQ(s.ev[1].d.event, event_e::result_ok);
  EXPECT_EQ(s.ev[2].d.event, event_e::kick);
  EXPECT_TRUE(between(s.ev[2].t - b2, 3'500 * kMs, 3'800 * kMs));
  EXPECT_EQ(s.ev[3].d.event, event_e::result_ok);
  EXPECT_EQ(s.det.kicks(), 2);
}

// ── 13：任意輸入下的不變式 ────────────────────────────────────────────────────

TEST(VrDirectKickFuzz, InvariantsHoldForArbitraryInput) {
  const int64_t periods[] = {kT120, 10'999'999, 11'000'000, kT90, kT72, kT60, kT60, kT60, 33'333'333, 0, -5};
  const int64_t origins[] = {kStart, 0, -3'600 * kSec, kI64Max / 4};
  const int64_t backoff[] = {10 * kSec, 30 * kSec, 90 * kSec, 300 * kSec};
  const int64_t bursts[] = {1 * kMs, 20 * kMs, 200 * kMs, 0, -1, kI64Max};
  const int64_t junk[] = {-1, kI64Min, kI64Max, 0, 100 * kMs};

  enum class stop_e {
    none,
    heavy,
    limit,
  };
  // 檢查端自己記的狀態（一個 generation）
  struct gen_t {
    int64_t period = 0;
    int64_t start = 0;
    int accepted = 0;  ///< 真的做了的 kick
    bool have_kick = false;
    int64_t last_kick = 0;  ///< 上一次 kick（含外部的）
    bool awaiting = false;  ///< 做了、結果還沒回來
    int64_t verify_begin = 0;
    int64_t verify_end = 0;
    int fails = 0;  ///< 連續幾次沒有變化
    bool have_not_before = false;
    int64_t not_before = 0;
    stop_e due_stop = stop_e::none;  ///< 下一次 poll() 該回的停手通知
    bool stopped = false;
  };

  int total_kicks = 0;
  int total_declined = 0;
  int total_ok = 0;
  int total_no_change = 0;
  int total_heavy = 0;
  int total_limit = 0;
  int total_external = 0;
  int total_resets = 0;

  for (uint64_t seed = 1; seed <= 160; ++seed) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    rng_t rng(seed);
    detector_t det;
    // 每四個種子有一個是「一直掉回 B、不重設」的走法，才走得到 20 次的上限
    const bool grind = (seed % 4) == 0;
    int64_t now = origins[seed % 4];
    int64_t eff = now;  // 偵測器看到的時間（只往前）
    gen_t g;
    const auto begin_gen = [&]() {
      g = gen_t {};
      g.period = grind ? kT60 : periods[rng.range(0, 10)];
      g.start = now;
      eff = now;
      det.reset(g.period, now);
    };
    begin_gen();
    int mode = 1;  // 0 沒有 Present、1 B、2 A、3 S、4 重負載的 A、5 垃圾
    int64_t seg_end = now + 20 * kSec;
    int64_t next_sample = now;
    const auto phase = [&]() -> int64_t {
      switch (mode) {
        case 1:
          return rng.range(650'000, 1'100'000);
        case 2:
          return rng.range(2'900'000, 6'000'000);
        case 3:
          return rng.range(1'200'000, 2'600'000);
        case 4:
          return rng.range(13'600'000, 16'000'000);
        default:
          return rng.range(-1'000'000, 30'000'000);
      }
    };

    for (int step = 0; step < 200'000; ++step) {
      // 時間：通常 2 ms 一輪；偶爾抖動、偶爾整條執行緒卡住、偶爾時鐘倒退
      const int64_t roll = rng.range(0, 9999);
      int64_t dt = 2 * kMs;
      if (roll < 300) {
        dt = rng.range(0, 50) * kMs;
      } else if (roll < 306) {
        dt = rng.range(200, 3'000) * kMs;
      }
      now += dt;
      const int64_t poll_now = (roll >= 306 && roll < 316) ? now - rng.range(1, 5'000) * kMs : now;
      eff = std::max(eff, poll_now);

      const stop_e due_stop = g.due_stop;
      const bool due_result = g.awaiting && eff >= g.verify_end;
      const decision_t d = det.poll(poll_now);

      if (g.period < 11'000'000) {
        ASSERT_EQ(d.event, event_e::none);  // 週期太短：什麼都不做
      }
      if (due_stop != stop_e::none) {
        ASSERT_EQ(d.event, due_stop == stop_e::heavy ? event_e::heavy_stop : event_e::limit_stop);
      } else if (due_result) {
        ASSERT_TRUE(d.event == event_e::result_ok || d.event == event_e::result_no_change) << event_name(d.event);  // 不會卡在等結果
      }
      switch (d.event) {
        case event_e::none:
          break;
        case event_e::kick:
          {
            ASSERT_FALSE(g.awaiting);  // 兩個 kick 中間一定有結果或「沒做成」
            ASSERT_FALSE(g.stopped);
            ASSERT_LT(g.accepted, 20);
            ASSERT_EQ(d.kick_no, g.accepted + 1);
            ASSERT_GE(eff - g.start, 8 * kSec);  // 暖機 5 秒＋連續成立 3 秒
            if (g.have_kick) {
              ASSERT_GE(eff - g.last_kick, 5 * kSec);
            }
            if (g.have_not_before) {
              ASSERT_GE(eff, g.not_before);
            }
            ASSERT_GE(d.samples, 24);
            ASSERT_LT(d.median_before_ns, 1'150'000);
            ASSERT_LT(d.p90_before_ns, 2'200'000);
            ASSERT_GE(d.burst_ns, 20 * kMs);
            ASSERT_LE(d.burst_ns, 40 * kMs);
            if (rng.range(0, 99) < 30) {
              det.kick_not_done();
              ++total_declined;
              ASSERT_EQ(det.kicks(), g.accepted);
              ASSERT_FALSE(det.verifying());
            } else {
              ++total_kicks;
              ++g.accepted;
              g.have_kick = true;
              g.last_kick = eff;
              g.awaiting = true;
              g.verify_begin = eff + 500 * kMs;
              g.verify_end = eff + 2 * kSec;
              ASSERT_EQ(det.kicks(), g.accepted);
              // kick 之後的樣子：有用、沒用、很重、儀表板、沒有 Present
              const int64_t after = rng.range(0, 99);
              mode = grind ? 2 : (after < 50 ? 2 : (after < 75 ? 1 : (after < 85 ? 4 : (after < 95 ? 3 : 0))));
              seg_end = now + (grind ? 2'100 * kMs : rng.range(3, 20) * kSec);
            }
            break;
          }
        case event_e::result_ok:
        case event_e::result_no_change:
          {
            ASSERT_TRUE(g.awaiting);  // 每個做了的 kick 剛好一個結果
            ASSERT_GE(eff, g.verify_end);
            ASSERT_EQ(d.kick_no, g.accepted);
            g.awaiting = false;
            if (d.event == event_e::result_ok) {
              ++total_ok;
              ASSERT_GE(d.samples, 24);
              ASSERT_GE(d.median_after_ns, 2'000'000);
              g.fails = 0;
              g.have_not_before = false;
              if (d.p90_after_ns > g.period / 5 * 4 + (g.period % 5) * 4 / 5) {
                g.due_stop = stop_e::heavy;
              }
            } else {
              ++total_no_change;
              ASSERT_TRUE(d.samples < 24 || d.median_after_ns < 2'000'000);
              const int64_t wait = backoff[std::min(g.fails, 3)];
              ASSERT_EQ(d.next_try_ns, wait);
              ++g.fails;
              g.have_not_before = true;
              g.not_before = eff + wait;
            }
            if (g.due_stop == stop_e::none && g.accepted == 20) {
              g.due_stop = stop_e::limit;
            }
            ASSERT_EQ(det.stopped(), g.due_stop != stop_e::none);
            break;
          }
        case event_e::heavy_stop:
        case event_e::limit_stop:
          ASSERT_NE(due_stop, stop_e::none);  // 只會跟在造成它的結果後面
          (d.event == event_e::heavy_stop ? total_heavy : total_limit) += 1;
          g.due_stop = stop_e::none;
          g.stopped = true;
          break;
      }
      if (g.stopped) {
        ASSERT_TRUE(det.stopped());
      }

      // 樣本（寫入方不可信：偶爾時刻亂跳、數值是垃圾）
      for (; next_sample <= now; next_sample += kT60) {
        if (mode == 0) {
          continue;
        }
        int64_t t = next_sample;
        int64_t ph = phase();
        const int64_t noise = rng.range(0, 999);
        if (noise < 5) {
          t += rng.range(-3'000, 3'000) * kMs;
        } else if (noise == 5) {
          t = kI64Max;
        } else if (noise == 6) {
          t = kI64Min;
        } else if (noise >= 995) {
          ph = junk[noise - 995];
        }
        det.on_present_phase(t, ph);
      }

      // 換一種樣子
      if (now >= seg_end) {
        const int64_t pick = rng.range(0, 99);
        mode = grind ? 1 : (pick < 45 ? 1 : (pick < 65 ? 2 : (pick < 80 ? 3 : (pick < 85 ? 4 : (pick < 90 ? 0 : 5)))));
        seg_end = now + rng.range(1, 25) * kSec;
      }
      // dev 的定時 kick
      if (!grind && rng.range(0, 9999) < 4) {
        const int64_t burst = bursts[rng.range(0, 5)];
        det.on_external_kick(now, burst);
        eff = std::max(eff, now);
        ++total_external;
        if (g.period >= 11'000'000) {
          g.have_kick = true;
          g.last_kick = eff;
          if (g.awaiting) {
            const int64_t extra = std::clamp<int64_t>(burst > 40 * kMs ? burst - 40 * kMs : 0, 0, 10 * kSec);
            g.verify_begin = std::max(g.verify_begin, eff + 500 * kMs + extra);
            g.verify_end = g.verify_begin + 1'500 * kMs;
          }
        }
      }
      // driver 重連
      if (!grind && rng.range(0, 99'999) < 2) {
        begin_gen();
        ++total_resets;
      }
    }
  }
  // 確定每一種事件都真的走到過（不然上面的斷言等於沒測）；實際的數字記在 --gtest_output 的報告裡
  RecordProperty("kicks", total_kicks);
  RecordProperty("declined", total_declined);
  RecordProperty("result_ok", total_ok);
  RecordProperty("result_no_change", total_no_change);
  RecordProperty("heavy_stop", total_heavy);
  RecordProperty("limit_stop", total_limit);
  RecordProperty("external_kicks", total_external);
  RecordProperty("resets", total_resets);
  EXPECT_GT(total_kicks, 1'000);
  EXPECT_GT(total_declined, 300);
  EXPECT_GT(total_ok, 500);
  EXPECT_GT(total_no_change, 200);
  EXPECT_GT(total_heavy, 10);
  EXPECT_GT(total_limit, 10);
  EXPECT_GT(total_external, 100);
  EXPECT_GT(total_resets, 100);
}
