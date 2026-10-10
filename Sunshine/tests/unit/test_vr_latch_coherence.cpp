/**
 * @file tests/unit/test_vr_latch_coherence.cpp
 * @brief VipleStream 2.0 §VR-CADENCE：LATCH 樣本的相位一致性（log 的 lockR；src/vr/vr_latch_coherence.h）。
 *
 * 和 test_vr_capture_health.cpp 同樣的約定：不 include ../tests_common.h（受測的標頭只用標準函式庫，可以脫離整個
 * server 單獨編譯執行，-Wall -Wextra -Wshadow -Wconversion -Werror 也過得了）；規格的數字（20 筆、40 筆、8 s、
 * 1.5 個週期、2%）在這裡寫死。亂數只用 std::mt19937_64 的原始輸出自己換算（標準函式庫的分佈類別各家實作不同，
 * 同一個種子在別的平台會是另一串數字）。
 */
#include <gtest/gtest.h>

#ifdef _WIN32
  // 放在受測標頭之前：受測標頭的識別字不可以撞到 Windows 標頭的巨集（它會經 vr_session.h 被 stream.cpp include）
  #include <windows.h>
#endif

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <src/vr/vr_latch_coherence.h>

using vr::latch::coherence_t;

namespace {
  constexpr double kT120Us = 8333.333;  // 120 Hz 的週期（µs）
  constexpr double kT90Us = 11111.111;  // 90 Hz
  constexpr int64_t kStepNs = 100'000'000;  // LATCH 每 100 ms 一則
  constexpr int64_t kSec = 1'000'000'000;
  constexpr double kTwoPi = 6.283185307179586;

  /// [0, 1)
  double uniform01(std::mt19937_64 &g) {
    return static_cast<double>(g() >> 11) / 9007199254740992.0;
  }

  /// 標準常態（Box–Muller）
  double normal(std::mt19937_64 &g) {
    const double u1 = (static_cast<double>(g() >> 11) + 1.0) / 9007199254740993.0;  // (0, 1)
    const double u2 = uniform01(g);
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(kTwoPi * u2);
  }

  /// 連續餵 n 則新幀、同一個 slack
  void feed(coherence_t &c, int n, int32_t slack_us, uint32_t &frame_id, int64_t &now, double modulus_us = kT120Us) {
    for (int i = 0; i < n; ++i) {
      now += kStepNs;
      c.on_latch(slack_us, ++frame_id, modulus_us, now);
    }
  }
}  // namespace

TEST(VrLatchCoherence, NeedsTwentySamples) {
  coherence_t c;
  int64_t now = kSec;
  uint32_t fid = 0;
  EXPECT_EQ(c.r_permille(now), -1);
  for (int i = 0; i < 19; ++i) {
    now += kStepNs;
    c.on_latch(4000, ++fid, kT120Us, now);
    ASSERT_EQ(c.r_permille(now), -1) << i;
  }
  now += kStepNs;
  c.on_latch(4000, ++fid, kT120Us, now);
  EXPECT_EQ(c.r_permille(now), 1000);  // 第 20 筆：全部同相
  const auto w = c.take_window();
  EXPECT_EQ(w.samples, 20u);
  EXPECT_EQ(w.r_last, 1000);
  EXPECT_EQ(w.r_min, 1000);
  EXPECT_EQ(w.r_max, 1000);
  // 取走之後視窗的統計歸零，樣本留著
  const auto w2 = c.take_window();
  EXPECT_EQ(w2.samples, 0u);
  EXPECT_EQ(w2.r_last, -1);
  EXPECT_EQ(w2.r_min, -1);
  EXPECT_EQ(w2.r_max, -1);
  EXPECT_EQ(c.r_permille(now), 1000);
}

TEST(VrLatchCoherence, RepeatedFramesAndOutliersAreNotSamples) {
  coherence_t c;
  int64_t now = kSec;
  uint32_t fid = 0;
  feed(c, 20, 4000, fid, now);
  ASSERT_EQ(c.r_permille(now), 1000);
  c.take_window();
  // frameId 和上一則相同＝client 沒有新幀、重送舊值：不是樣本（即使 slack 不同）
  for (int i = 0; i < 50; ++i) {
    now += kStepNs;
    c.on_latch(100 + i * 37, fid, kT120Us, now);
  }
  // |slack| 超過 1.5 個週期（120 Hz 是 12500 µs）：丟掉
  for (const int32_t slack : {20000, -20000, 12501, -12501, 1000000}) {
    now += kStepNs;
    c.on_latch(slack, ++fid, kT120Us, now);
  }
  auto w = c.take_window();
  EXPECT_EQ(w.samples, 0u);
  EXPECT_EQ(w.r_last, -1);
  // 剛好在界線內的照收
  now += kStepNs;
  c.on_latch(12499, ++fid, kT120Us, now);
  now += kStepNs;
  c.on_latch(-12499, ++fid, kT120Us, now);
  w = c.take_window();
  EXPECT_EQ(w.samples, 2u);
}

TEST(VrLatchCoherence, SamplesOlderThanEightSecondsAreNotUsed) {
  coherence_t c;
  int64_t now = kSec;
  uint32_t fid = 0;
  feed(c, 20, 4000, fid, now);  // 最舊的一筆在 now − 1.9 s
  EXPECT_EQ(c.r_permille(now + 6000 * (kSec / 1000)), 1000);  // 最舊的 7.9 s：還在
  EXPECT_EQ(c.r_permille(now + 6100 * (kSec / 1000)), 1000);  // 剛好 8.0 s：還在
  EXPECT_EQ(c.r_permille(now + 6200 * (kSec / 1000)), -1);  // 8.1 s：剩 19 筆
  EXPECT_EQ(c.r_permille(now + 9 * kSec), -1);
  // LATCH 停了 9 s 之後再來：舊的 20 筆不算，要重新收滿 20 筆
  now += 9 * kSec;
  for (int i = 0; i < 19; ++i) {
    now += kStepNs;
    c.on_latch(1000, ++fid, kT120Us, now);
    ASSERT_EQ(c.r_permille(now), -1) << i;
  }
  now += kStepNs;
  c.on_latch(1000, ++fid, kT120Us, now);
  EXPECT_EQ(c.r_permille(now), 1000);
  // 只留最新 40 筆：再餵 40 筆別的相位，之前的全部被擠掉
  feed(c, 40, 5000, fid, now);
  EXPECT_EQ(c.r_permille(now), 1000);
}

TEST(VrLatchCoherence, SpreadPhasesReadLowAndConcentratedPhasesHigh) {
  std::mt19937_64 g(1);
  // 均勻散開：40 筆的平均 R 約 sqrt(π) ÷ 2 ÷ sqrt(40)＝0.14
  {
    coherence_t c;
    int64_t now = kSec;
    uint32_t fid = 0;
    double sum = 0;
    int n = 0;
    for (int i = 0; i < 4000; ++i) {
      now += kStepNs;
      c.on_latch(static_cast<int32_t>(uniform01(g) * kT120Us), ++fid, kT120Us, now);
      if (i >= 40) {
        sum += c.r_permille(now);
        ++n;
      }
    }
    const double mean = sum / n;
    EXPECT_GT(mean, 80.0);
    EXPECT_LT(mean, 200.0);
  }
  // 標準差 1 ms（120 Hz）：約 exp(−(2π × 1 ÷ 8.333)² ÷ 2)＝0.75
  {
    coherence_t c;
    int64_t now = kSec;
    uint32_t fid = 0;
    double sum = 0;
    int n = 0;
    for (int i = 0; i < 4000; ++i) {
      now += kStepNs;
      c.on_latch(static_cast<int32_t>(std::lround(4000.0 + 1000.0 * normal(g))), ++fid, kT120Us, now);
      if (i >= 40) {
        sum += c.r_permille(now);
        ++n;
      }
    }
    const double mean = sum / n;
    EXPECT_GT(mean, 700.0);
    EXPECT_LT(mean, 800.0);
  }
  // 繞著 0 的一群（±300 µs，一半是負值）：取模不會把它拆成 0 與 T 兩頭，仍然是同相
  {
    coherence_t c;
    int64_t now = kSec;
    uint32_t fid = 0;
    for (int i = 0; i < 40; ++i) {
      now += kStepNs;
      c.on_latch(static_cast<int32_t>((i % 7) * 100 - 300), ++fid, kT120Us, now);
    }
    EXPECT_GT(c.r_permille(now), 950);
  }
  // 正值的 slack 與「少一個週期」的同一個相位（晚到的幀被下一個 latch 接走）：算同相
  {
    coherence_t c;
    int64_t now = kSec;
    uint32_t fid = 0;
    for (int i = 0; i < 40; ++i) {
      now += kStepNs;
      c.on_latch(i % 2 ? 3000 : 3000 - 8333, ++fid, kT120Us, now);
    }
    EXPECT_GT(c.r_permille(now), 990);
  }
  // 相差半個週期的兩群各一半：互相抵銷
  {
    coherence_t c;
    int64_t now = kSec;
    uint32_t fid = 0;
    for (int i = 0; i < 40; ++i) {
      now += kStepNs;
      c.on_latch(i % 2 ? 1000 : 1000 + 4167, ++fid, kT120Us, now);
    }
    EXPECT_LT(c.r_permille(now), 10);
  }
}

TEST(VrLatchCoherence, ModulusChangeStartsOver) {
  coherence_t c;
  int64_t now = kSec;
  uint32_t fid = 0;
  feed(c, 30, 4000, fid, now);
  ASSERT_EQ(c.r_permille(now), 1000);
  // 2% 以內的變化（顯示週期的量測抖動）不重來
  feed(c, 1, 4000, fid, now, kT120Us * 1.015);
  EXPECT_NE(c.r_permille(now), -1);
  feed(c, 1, 4000, fid, now, kT120Us);
  EXPECT_NE(c.r_permille(now), -1);
  // 換更新率（120 → 90 Hz）：之前的樣本是對另一個週期取模的，全部作廢
  feed(c, 1, 4000, fid, now, kT90Us);
  EXPECT_EQ(c.r_permille(now), -1);
  feed(c, 18, 4000, fid, now, kT90Us);
  EXPECT_EQ(c.r_permille(now), -1);
  feed(c, 1, 4000, fid, now, kT90Us);
  EXPECT_EQ(c.r_permille(now), 1000);
  // 這個視窗的樣本數照常累計
  EXPECT_EQ(c.take_window().samples, 52u);
}

TEST(VrLatchCoherence, ResetForgetsEverything) {
  coherence_t c;
  int64_t now = kSec;
  uint32_t fid = 0;
  feed(c, 30, 4000, fid, now);
  ASSERT_EQ(c.r_permille(now), 1000);
  c.reset();
  EXPECT_EQ(c.r_permille(now), -1);
  const auto w = c.take_window();
  EXPECT_EQ(w.samples, 0u);
  EXPECT_EQ(w.r_last, -1);
  // 連「上一則的 frameId」也忘了：同一個 frameId 再來算新樣本
  now += kStepNs;
  c.on_latch(4000, fid, kT120Us, now);
  EXPECT_EQ(c.take_window().samples, 1u);
}

TEST(VrLatchCoherence, HostileInputIsHarmless) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  const int32_t i32_min = std::numeric_limits<int32_t>::min();
  const int32_t i32_max = std::numeric_limits<int32_t>::max();
  const int64_t i64_min = std::numeric_limits<int64_t>::min();
  const int64_t i64_max = std::numeric_limits<int64_t>::max();
  {
    // 不合理的模數（0、負值、太小、太大、NaN、無限大）整則忽略：不是樣本，也不會把已經收的樣本清掉
    coherence_t c;
    int64_t now = kSec;
    uint32_t fid = 0;
    feed(c, 25, 4000, fid, now);
    c.take_window();
    for (const double mod : {0.0, -8333.0, 1999.9, 100000.1, 1e300, nan, inf, -inf}) {
      now += kStepNs;
      c.on_latch(4000, ++fid, mod, now);
    }
    EXPECT_EQ(c.take_window().samples, 0u);
    EXPECT_EQ(c.r_permille(now), 1000);
    // 界線上的模數照收（2 ms、100 ms）
    coherence_t lo;
    uint32_t f2 = 0;
    int64_t t2 = kSec;
    feed(lo, 20, 500, f2, t2, 2000.0);
    EXPECT_EQ(lo.r_permille(t2), 1000);
    coherence_t hi;
    feed(hi, 20, 500, f2, t2, 100000.0);
    EXPECT_EQ(hi.r_permille(t2), 1000);
  }
  {
    // 極端的 slack 與時刻
    coherence_t c;
    uint32_t fid = 0;
    c.on_latch(i32_min, ++fid, kT120Us, i64_min);
    c.on_latch(i32_max, ++fid, kT120Us, i64_max);
    c.on_latch(1, ++fid, kT120Us, 5);
    c.on_latch(1, ++fid, kT120Us, i64_min);
    for (const int64_t t : {i64_min, int64_t {-1}, int64_t {0}, i64_max}) {
      const int r = c.r_permille(t);
      EXPECT_TRUE(r == -1 || (r >= 0 && r <= 1000)) << t;
    }
  }
  // 亂數：任意的 slack、frameId、模數、時刻
  std::mt19937_64 g(20261010);
  const double mods[] = {kT120Us, kT120Us, kT120Us, kT90Us, 16666.667, 0.0, -1.0, 1999.9, 2000.0, 100000.0, nan, inf};
  const int32_t slacks[] = {0, 1, -1, i32_min, i32_max, 8333, -8333, 12500, -12500, 4166};
  coherence_t c;
  int64_t now = kSec;
  uint64_t with_r = 0;
  for (int i = 0; i < 200000; ++i) {
    const uint64_t k = g() % 64;
    if (k == 0) {
      now = static_cast<int64_t>(g());  // 任何值，包括倒退
    } else if (k >= 8 && now < i64_max - 200'000'000) {
      now += static_cast<int64_t>(g() % 200'000'000ull);
    }
    const int32_t slack = g() % 8 == 0 ? slacks[g() % 10] : static_cast<int32_t>(g() % 30000) - 15000;
    const uint32_t fid = g() % 6 == 0 ? static_cast<uint32_t>(g() % 3) : static_cast<uint32_t>(i);
    const double mod = g() % 20 == 0 ? mods[g() % 12] : kT120Us;  // 多半是同一個更新率，偶爾換或是不合理的值
    c.on_latch(slack, fid, mod, now);
    const int r = c.r_permille(now);
    ASSERT_TRUE(r == -1 || (r >= 0 && r <= 1000)) << i;
    with_r += r >= 0 ? 1 : 0;
    if (g() % 500 == 0) {
      const auto w = c.take_window();
      ASSERT_TRUE(w.r_last == -1 || (w.r_last >= 0 && w.r_last <= 1000)) << i;
      ASSERT_EQ(w.r_min < 0, w.r_max < 0) << i;
      ASSERT_LE(w.r_min, w.r_max) << i;
      if (w.r_last >= 0) {
        ASSERT_LE(w.r_min, w.r_last) << i;
        ASSERT_GE(w.r_max, w.r_last) << i;
      }
      if (w.samples == 0) {
        ASSERT_EQ(w.r_last, -1) << i;
        ASSERT_EQ(w.r_max, -1) << i;
      }
    }
  }
  EXPECT_GT(with_r, 1000u);  // 序列裡真的有算出 R 的時候
}
