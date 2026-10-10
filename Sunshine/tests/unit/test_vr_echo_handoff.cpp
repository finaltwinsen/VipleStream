/**
 * @file tests/unit/test_vr_echo_handoff.cpp
 * @brief VipleStream 2.0 §VR-DIRECT-KICK：回聲年齡從擷取執行緒交到 bridge 執行緒的緩衝（src/vr/vr_echo_handoff.h）。
 *
 * 和 test_vr_direct_kick.cpp 一樣不 include ../tests_common.h：受測的標頭只用標準函式庫，測試只需要 googletest，
 * 可以脫離整個 server 單獨編譯執行（要 -pthread）。放進 test_sunshine 一起建也一樣能跑。
 *
 * 前半是單一執行緒看得出來的語意（閘門、generation、順序、滿了丟新的、重開會清掉）。後半用真的執行緒對打：
 * 次數每次跑都不一樣，斷言的只有不變式——收下的每一筆恰好交出去一次、內容完整、同一個寫入方的順序不變、
 * 不會交給別的 generation。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <src/vr/vr_echo_handoff.h>
#include <thread>
#include <vector>

using namespace vr::kick;

namespace {
  constexpr int64_t kI64Max = std::numeric_limits<int64_t>::max();
  constexpr int64_t kI64Min = std::numeric_limits<int64_t>::min();

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
