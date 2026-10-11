/**
 * @file tests/unit/test_vr_echo_handoff.cpp
 * @brief VipleStream 2.0 §VR-DIRECT-KICK：回聲年齡從擷取執行緒交到 bridge 執行緒的緩衝（src/vr/vr_echo_handoff.h）。
 *
 * 和 test_vr_direct_kick.cpp 一樣不 include ../tests_common.h：受測的標頭只用標準函式庫，測試只需要 googletest，
 * 可以脫離整個 server 單獨編譯執行（要 -pthread）。放進 test_sunshine 一起建也一樣能跑。
 *
 * 前半是單一執行緒看得出來的語意（閘門、generation、順序、滿了丟新的、重開會清掉）。中間用真的執行緒對打：
 * 次數每次跑都不一樣，斷言的只有不變式——收下的每一筆恰好交出去一次、內容完整、同一個寫入方的順序不變、
 * 不會交給別的 generation。最後把它和偵測器（src/vr/vr_direct_kick.h）照 bridge 執行緒的走法接起來，
 * 確認緩衝的大小夠：bridge 卡住到會丟樣本的時候，判定不會因此出錯。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <src/vr/vr_direct_kick.h>
#include <src/vr/vr_echo_handoff.h>
#include <thread>
#include <vector>

using namespace vr::kick;

namespace {
  constexpr int64_t kI64Max = std::numeric_limits<int64_t>::max();
  constexpr int64_t kI64Min = std::numeric_limits<int64_t>::min();
  constexpr int64_t kMs = 1'000'000;
  constexpr int64_t kSec = 1'000'000'000;
  constexpr int64_t kT60 = 16'666'666;  // 60 Hz
  constexpr int64_t kT72 = 13'888'888;  // 72 Hz
  constexpr int64_t kT90 = 11'111'111;  // 90 Hz
  constexpr int64_t kT120 = 8'333'333;  // 120 Hz：偵測器不動作

  struct got_t {
    int64_t t;
    int64_t age;
  };

  /// 把緩衝倒空，回傳交出來的每一筆（照順序）
  std::vector<got_t> drain_all(echo_handoff_t &h, uint64_t generation) {
    std::vector<got_t> out;
    const std::size_t n = h.drain(generation, [&out](int64_t t, int64_t age) {
      out.push_back(got_t {t, age});
    });
    EXPECT_EQ(n, out.size());
    return out;
  }
}  // namespace

// ── 閘門 ─────────────────────────────────────────────────────────────────────

TEST(VrEchoHandoff, ClosedUntilOpened) {
  // 開關關著、偵測器不動作、沒有連線：沒有人呼叫 open()，什麼都不收
  echo_handoff_t h;
  EXPECT_EQ(h.gate(), 0u);
  EXPECT_FALSE(h.push(1, 100, 5));
  EXPECT_FALSE(h.push(0, 100, 5));
  EXPECT_TRUE(drain_all(h, 1).empty());
  EXPECT_TRUE(drain_all(h, 0).empty());
}

TEST(VrEchoHandoff, TakesOnlyTheOpenGeneration) {
  echo_handoff_t h;
  h.open(7);
  EXPECT_EQ(h.gate(), 7u);
  EXPECT_TRUE(h.push(7, 100, 5));
  EXPECT_FALSE(h.push(6, 101, 6));  // 上一個 generation 的影像
  EXPECT_FALSE(h.push(8, 102, 7));
  EXPECT_FALSE(h.push(0, 103, 8));
  EXPECT_TRUE(h.push(7, 104, 9));
  const auto got = drain_all(h, 7);
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].t, 100);
  EXPECT_EQ(got[0].age, 5);
  EXPECT_EQ(got[1].t, 104);
  EXPECT_EQ(got[1].age, 9);
  EXPECT_TRUE(drain_all(h, 7).empty());  // 倒過就空了

  // 0 不是合法的 generation（generation 從 1 起）：閘門開成 0 等於關著
  h.open(0);
  EXPECT_EQ(h.gate(), 0u);
  EXPECT_FALSE(h.push(0, 1, 1));
  EXPECT_FALSE(h.push(7, 1, 1));
  EXPECT_TRUE(drain_all(h, 0).empty());
}

TEST(VrEchoHandoff, CloseStopsNewOnesButKeepsWhatIsThere) {
  echo_handoff_t h;
  h.open(3);
  EXPECT_TRUE(h.push(3, 10, 1));
  EXPECT_TRUE(h.push(3, 11, 2));
  h.close();
  EXPECT_EQ(h.gate(), 0u);
  EXPECT_FALSE(h.push(3, 12, 3));
  // 關之前放進去的還在，讀取方照樣倒得到
  const auto got = drain_all(h, 3);
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].t, 10);
  EXPECT_EQ(got[1].t, 11);
}

TEST(VrEchoHandoff, OpenClearsLeftovers) {
  // 拆線時只關閘門；下一次握手的 open() 把上一條連線留下來的清掉
  echo_handoff_t h;
  h.open(3);
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(h.push(3, i, i));
  }
  h.close();
  h.open(4);
  EXPECT_TRUE(drain_all(h, 4).empty());
  EXPECT_TRUE(drain_all(h, 3).empty());
  EXPECT_TRUE(h.push(4, 20, 2));
  EXPECT_FALSE(h.push(3, 21, 2));
  const auto got = drain_all(h, 4);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].t, 20);

  // 同一個 generation 重開一次也一樣清
  EXPECT_TRUE(h.push(4, 22, 2));
  h.open(4);
  EXPECT_TRUE(drain_all(h, 4).empty());
}

TEST(VrEchoHandoff, DrainDropsOtherGenerations) {
  // push() 通過閘門之後、取到鎖之前換了 generation 的那一筆會留在緩衝裡。倒的時候照 generation 過濾：
  // 這裡直接拿別的 generation 去倒，效果相同
  echo_handoff_t h;
  h.open(3);
  EXPECT_TRUE(h.push(3, 10, 1));
  EXPECT_TRUE(h.push(3, 11, 2));
  EXPECT_TRUE(drain_all(h, 4).empty());  // 不是它的：丟掉
  EXPECT_TRUE(drain_all(h, 3).empty());  // 而且已經倒空，不會留到之後
  EXPECT_TRUE(h.push(3, 12, 3));
  ASSERT_EQ(drain_all(h, 3).size(), 1u);
}

// ── 內容與容量 ───────────────────────────────────────────────────────────────

TEST(VrEchoHandoff, KeepsOrderAndPassesValuesThrough) {
  // 數值照原樣交過去（要不要、合不合理是偵測器的事）；順序＝放進去的順序
  echo_handoff_t h;
  h.open(1);
  const got_t in[] = {{5, 19'000'000}, {4, 0}, {kI64Max, kI64Max}, {kI64Min, kI64Min}, {-7, -1}, {5, 19'000'000}};
  for (const got_t &x : in) {
    EXPECT_TRUE(h.push(1, x.t, x.age));
  }
  const auto got = drain_all(h, 1);
  ASSERT_EQ(got.size(), std::size(in));
  for (std::size_t i = 0; i < got.size(); ++i) {
    EXPECT_EQ(got[i].t, in[i].t) << i;
    EXPECT_EQ(got[i].age, in[i].age) << i;
  }
}

TEST(VrEchoHandoff, DropsNewOnesWhenFull) {
  // 讀取方卡住：放滿 64 筆之後新的不收（留著舊的），倒空之後又收
  echo_handoff_t h;
  h.open(3);
  for (int i = 0; i < 64; ++i) {
    EXPECT_TRUE(h.push(3, i, 2 * i)) << i;
  }
  EXPECT_FALSE(h.push(3, 64, 128));
  EXPECT_FALSE(h.push(3, 65, 130));
  const auto got = drain_all(h, 3);
  ASSERT_EQ(got.size(), 64u);
  for (int i = 0; i < 64; ++i) {
    EXPECT_EQ(got[static_cast<std::size_t>(i)].t, i);
    EXPECT_EQ(got[static_cast<std::size_t>(i)].age, 2 * i);
  }
  EXPECT_TRUE(h.push(3, 66, 132));
  const auto again = drain_all(h, 3);
  ASSERT_EQ(again.size(), 1u);
  EXPECT_EQ(again[0].t, 66);
  // 連續很多輪都一樣（筆數歸零、沒有殘留）
  for (int round = 0; round < 1000; ++round) {
    EXPECT_TRUE(h.push(3, round, 1));
    EXPECT_TRUE(h.push(3, round, 2));
    ASSERT_EQ(drain_all(h, 3).size(), 2u) << round;
  }
}

// ── 真的執行緒 ───────────────────────────────────────────────────────────────

TEST(VrEchoHandoffThreads, EveryAcceptedSampleArrivesOnceAndIntact) {
  // 四條寫入執行緒對一條讀取執行緒（實際只有一條擷取執行緒；多幾條是為了把鎖逼出來）。
  // t＝寫入者 × 1e6＋序號，age＝t × 3＋1：內容被撕裂、重複、順序顛倒都看得出來
  constexpr int kWriters = 4;
  constexpr int kPerWriter = 50'000;
  echo_handoff_t h;
  h.open(5);
  std::atomic<int> running {kWriters};
  std::vector<uint64_t> accepted(kWriters, 0);
  std::vector<std::thread> writers;
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&h, &running, &accepted, w]() {
      uint64_t ok = 0;
      for (int seq = 0; seq < kPerWriter; ++seq) {
        const int64_t t = static_cast<int64_t>(w) * 1'000'000 + seq;
        ok += h.push(5, t, t * 3 + 1) ? 1u : 0u;
        if ((seq & 63) == 63) {
          std::this_thread::yield();  // 讓讀取方有機會倒，不然幾乎每一筆都因為滿了被丟
        }
      }
      accepted[static_cast<std::size_t>(w)] = ok;
      running.fetch_sub(1);
    });
  }

  std::vector<std::vector<uint8_t>> seen(kWriters, std::vector<uint8_t>(kPerWriter, 0));
  std::vector<int64_t> last_seq(kWriters, -1);
  std::vector<uint64_t> delivered(kWriters, 0);
  uint64_t torn = 0, duplicated = 0, reordered = 0, garbage = 0;
  const auto take = [&](int64_t t, int64_t age) {
    const int64_t w = t / 1'000'000;
    const int64_t seq = t % 1'000'000;
    if (t < 0 || w >= kWriters || seq >= kPerWriter) {
      ++garbage;
      return;
    }
    torn += age != t * 3 + 1 ? 1u : 0u;
    const auto wi = static_cast<std::size_t>(w);
    const auto si = static_cast<std::size_t>(seq);
    duplicated += seen[wi][si] ? 1u : 0u;
    seen[wi][si] = 1;
    reordered += seq <= last_seq[wi] ? 1u : 0u;
    last_seq[wi] = seq;
    ++delivered[wi];
  };
  while (running.load() > 0) {
    h.drain(5, take);
  }
  for (std::thread &th : writers) {
    th.join();
  }
  h.drain(5, take);  // 最後一輪：寫入方都結束之後還留在緩衝裡的
  EXPECT_EQ(h.drain(5, take), 0u);

  EXPECT_EQ(garbage, 0u);
  EXPECT_EQ(torn, 0u);
  EXPECT_EQ(duplicated, 0u);
  EXPECT_EQ(reordered, 0u);
  uint64_t total = 0;
  for (int w = 0; w < kWriters; ++w) {
    const auto wi = static_cast<std::size_t>(w);
    EXPECT_EQ(delivered[wi], accepted[wi]) << "writer " << w;  // 收下的每一筆恰好交出去一次
    total += delivered[wi];
  }
  EXPECT_GT(total, 0u);
  RecordProperty("delivered", static_cast<int>(total));
  RecordProperty("dropped_when_full", static_cast<int>(static_cast<uint64_t>(kWriters) * kPerWriter - total));
}

TEST(VrEchoHandoffThreads, NothingCrossesIntoAnotherGeneration) {
  // 讀取方不斷換 generation（關閘門 → 清緩衝、開給下一個），寫入方照它當下讀到的閘門交。每一筆的 t 裡編著它的
  // generation：倒給 generation g 的一定都是 g 的——包括「通過閘門之後才換 generation」的那幾筆
  constexpr int kWriters = 3;
  echo_handoff_t h;
  std::atomic<bool> stop {false};
  std::vector<std::thread> writers;
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&h, &stop]() {
      int64_t seq = 0;
      while (!stop.load()) {
        const uint64_t g = h.gate();
        if (g != 0) {
          h.push(g, static_cast<int64_t>(g) * 1'000'000 + (seq++ % 1'000'000), 7);
        }
      }
    });
  }

  uint64_t delivered = 0, wrong_generation = 0, torn = 0;
  uint64_t generations = 0;
  for (uint64_t g = 1; g <= 20'000 && (g <= 300 || delivered < 1'000); ++g) {
    h.open(g);
    for (int k = 0; k < 20; ++k) {
      h.drain(g, [&](int64_t t, int64_t age) {
        wrong_generation += t / 1'000'000 != static_cast<int64_t>(g) ? 1u : 0u;
        torn += age != 7 ? 1u : 0u;
        ++delivered;
      });
      std::this_thread::yield();
    }
    h.close();
    generations = g;
  }
  stop.store(true);
  for (std::thread &th : writers) {
    th.join();
  }
  EXPECT_EQ(wrong_generation, 0u);
  EXPECT_EQ(torn, 0u);
  EXPECT_GE(delivered, 1'000u) << "after " << generations << " generations";
  RecordProperty("delivered", static_cast<int>(delivered));
  RecordProperty("generations", static_cast<int>(generations));
}

// ── 和偵測器接起來：bridge 執行緒卡住的時候緩衝夠不夠 ─────────────────────────

namespace {
  struct stall_run_t {
    int64_t kick_at = -1;  ///< 第一次 kick 在起點之後多久（-1＝沒有）
    int pushed = 0;  ///< 擷取端交了幾筆
    int accepted = 0;  ///< 緩衝收下幾筆（其餘是滿了被丟掉的）
  };

  /**
   * @brief 照 bridge 執行緒的走法把偵測器與交接的緩衝接起來跑 dur：每 2 ms 一輪，先 poll()（periodic() 的前段），
   *        再把 timing ring 裡的 presentPhase 與緩衝裡的回聲年齡倒進偵測器（drain_rings()）。driver 每個週期寫一筆
   *        timing、擷取端每一張影像交一筆回聲年齡，兩邊都不等 bridge；一路都是多一格的樣子。
   *        起點之後 [stall_begin, stall_begin + stall_len) 這一段 bridge 執行緒不跑：presentPhase 留在 timing ring
   *        （夠大，一筆不少），回聲年齡留在交接的緩衝（64 格，滿了丟新的）。
   */
  stall_run_t run_with_stall(int64_t period, int64_t stall_begin, int64_t stall_len, int64_t dur) {
    const uint64_t generation = 3;
    const int64_t t0 = 1'000 * kSec;
    detector_t det;
    echo_handoff_t h;
    det.reset(period, t0);
    h.open(det.active() ? generation : 0);  // 握手時：偵測器會動作才開閘門
    std::vector<int64_t> timing;  // 還沒倒進偵測器的 presentPhase（它的時刻）
    stall_run_t r;
    int64_t next_frame = t0 + period;
    for (int64_t now = t0 + 2 * kMs; now <= t0 + dur; now += 2 * kMs) {
      for (; next_frame <= now; next_frame += period) {
        timing.push_back(next_frame);
        ++r.pushed;
        r.accepted += h.push(generation, next_frame, period + 2 * kMs) ? 1 : 0;
      }
      const int64_t rel = now - t0;
      if (rel >= stall_begin && rel < stall_begin + stall_len) {
        continue;  // bridge 執行緒卡住
      }
      const decision_t d = det.poll(now);
      if (d.event == event_e::kick && r.kick_at < 0) {
        r.kick_at = rel;
      }
      for (const int64_t t : timing) {
        det.on_present_phase(t, 800'000);
      }
      timing.clear();
      h.drain(generation, [&det](int64_t t, int64_t age) {
        det.on_echo_age(t, age);
      });
    }
    return r;
  }

  ::testing::AssertionResult between(int64_t v, int64_t lo, int64_t hi) {
    if (v >= lo && v <= hi) {
      return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << v << " is not in [" << lo << ", " << hi << "]";
  }
}  // namespace

TEST(VrEchoHandoffWithDetector, DeliversWhatTheDetectorNeedsToKick) {
  // 沒有卡住：擷取端交的每一筆都到了偵測器，暖機 5 秒＋連續成立 3 秒就推（每 100 ms 評估一次，最多晚 100 ms）
  for (const int64_t period : {kT60, kT72, kT90}) {
    const stall_run_t r = run_with_stall(period, 0, 0, 9 * kSec);
    EXPECT_EQ(r.accepted, r.pushed) << "period=" << period;
    EXPECT_TRUE(between(r.kick_at, 8 * kSec, 8 * kSec + 100 * kMs)) << "period=" << period;
  }
  // 不動作的 generation（120 Hz）：閘門沒有開，擷取端交的一筆都不收，也不會推
  const stall_run_t idle = run_with_stall(kT120, 0, 0, 9 * kSec);
  EXPECT_GT(idle.pushed, 1'000);
  EXPECT_EQ(idle.accepted, 0);
  EXPECT_EQ(idle.kick_at, -1);
}

TEST(VrEchoHandoffWithDetector, AShortStallLosesNothing) {
  // 第 6 秒起卡 0.5 秒：緩衝放得下（60 Hz 30 筆、90 Hz 45 筆）；恢復後第一次評估的 1 秒視窗裡還有卡住之前的
  // 半秒樣本，連續成立的計時不中斷，照原來的時刻推
  for (const int64_t period : {kT60, kT72, kT90}) {
    const stall_run_t r = run_with_stall(period, 6 * kSec, 500 * kMs, 9 * kSec);
    EXPECT_EQ(r.accepted, r.pushed) << "period=" << period;
    EXPECT_TRUE(between(r.kick_at, 8 * kSec, 8 * kSec + 100 * kMs)) << "period=" << period;
  }
}

TEST(VrEchoHandoffWithDetector, WhenSamplesAreDroppedTheDwellHasRestartedAnyway) {
  // 卡到緩衝會放滿（60 Hz 1.07 秒、72 Hz 0.89 秒、90 Hz 0.71 秒）的時候，恢復後的第一次評估——poll() 在倒樣本
  // 之前——看到的 1 秒視窗裡 presentPhase 不到 24 筆，或兩次評估已經隔超過 1 秒：連續成立的計時不管有沒有丟樣本
  // 都從恢復後重新開始，被丟掉的那幾筆回聲年齡不影響任何判定。kick 落在「恢復後第一次樣本齊全的評估」
  // （恢復後 100 ms）＋3 秒
  struct case_t {
    int64_t period;
    int64_t stall_ms;
    int dropped;
  };
  const case_t cases[] = {
    {kT60, 1'000, 0},  // 61 筆還放得下，但計時已經重來（兩次評估隔了 1.1 秒）
    {kT60, 1'200, 9},  // 73 筆：丟 9 筆
    {kT72, 950, 5},  // 69 筆：丟 5 筆
    {kT90, 900, 18},  // 82 筆：丟 18 筆；兩次評估剛好隔 1 秒（不算超過），擋下來的是視窗裡只剩 8 筆
  };
  for (const case_t &c : cases) {
    const stall_run_t r = run_with_stall(c.period, 6 * kSec, c.stall_ms * kMs, 12 * kSec);
    EXPECT_EQ(r.pushed - r.accepted, c.dropped) << "period=" << c.period << " stall=" << c.stall_ms;
    const int64_t resumed = 6 * kSec + c.stall_ms * kMs;
    EXPECT_TRUE(between(r.kick_at, resumed + 3'100 * kMs, resumed + 3'200 * kMs)) << "period=" << c.period << " stall=" << c.stall_ms;
  }
}

TEST(VrEchoHandoffWithDetector, TheOneDropWithoutARestartDoesNotMatter) {
  // 90 Hz、卡 0.72 秒：65 筆裡丟掉最新的 1 筆，而恢復後第一次評估的視窗裡還有卡住之前的 25 筆（夠 24 筆），
  // 計時沒有重來。倒出來之後視窗裡有八十幾筆回聲年齡：照原來的時刻推
  const stall_run_t r = run_with_stall(kT90, 6 * kSec, 720 * kMs, 9 * kSec);
  EXPECT_EQ(r.pushed - r.accepted, 1);
  EXPECT_TRUE(between(r.kick_at, 8 * kSec, 8 * kSec + 100 * kMs));
}
