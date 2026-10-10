/**
 * @file tests/unit/test_vr_capture_publish.cpp
 * @brief VipleStream 2.0 §VR-CADENCE：擷取端發布快照的時機與狀態變化的 log 行（src/vr/vr_capture_publish.h）。
 *
 * 和 test_vr_capture_health.cpp 同樣的約定：不 include ../tests_common.h（受測的標頭只用標準函式庫，可以脫離整個 server
 * 單獨編譯執行，-Wall -Wextra -Wshadow -Wconversion -Werror 也過得了）；規格的數字（每秒發布一次、同一秒內最多一行）
 * 在這裡寫死。時間的單位由呼叫端定：這裡大多用 ns（一秒＝1e9），差分那一組用 QPC（一秒＝1e7 tick，和 display_vr.cpp 一樣）。
 *
 * 讀測試時要記得：log 行只在「發布的時候」檢查。節拍狀態自己變了不會觸發發布（要等每秒那一次），三個旗標任何一個
 * 變了才會立刻發布——所以要在同一秒內弄出第二次發布，測試裡是讓過載旗標跟著變。
 */
#include <gtest/gtest.h>

#ifdef _WIN32
  // 放在受測標頭之前：受測標頭的識別字不可以撞到 Windows 標頭的巨集（它會被先 include <Windows.h> 的 display_vr.cpp include）
  #include <windows.h>
#endif

#include <cstdint>
#include <limits>
#include <ostream>
#include <random>
#include <src/vr/vr_capture_publish.h>

using namespace vr::capture;

namespace vr::capture {
  // 斷言失敗時印名字（不然 googletest 只會印「1-byte object <02>」）。static：test_vr_capture_health.cpp 也替同一個型別
  // 定義了 PrintTo，兩個檔會連結進同一個 test_sunshine
  static void PrintTo(cadence_e e, std::ostream *os) {
    *os << cadence_name(e);
  }
}  // namespace vr::capture

namespace {
  using action_t = publisher_t::action_t;

  constexpr int64_t kMs = 1'000'000;
  constexpr int64_t kSec = 1'000'000'000;
  constexpr int64_t kStart = 7'000 * kSec + 321 * kMs;  // 任意的起點（不是 0，也不在整秒上）
  constexpr int64_t kI64Max = std::numeric_limits<int64_t>::max();
  constexpr int64_t kI64Min = std::numeric_limits<int64_t>::min();

  snapshot_t snap(bool overload, bool latch_hold, bool predict_hold, cadence_e cadence = cadence_e::unknown, bool abandon = false) {
    snapshot_t s;
    s.overload = overload;
    s.latch_hold = latch_hold;
    s.predict_hold = predict_hold;
    s.abandon_overload = abandon;
    s.cadence = cadence;
    return s;
  }

  /// late 模式的快照：兩個決定就是過載旗標
  snapshot_t late_snap(bool overload, cadence_e cadence = cadence_e::unknown) {
    return snap(overload, overload, overload, cadence);
  }

  publisher_t make(overload_mode_e mode = overload_mode_e::late, int64_t ticks_per_s = kSec) {
    publisher_t p;
    p.configure(mode, ticks_per_s);
    return p;
  }

  /// 呼叫端什麼都不必做
  bool nothing(const action_t &a) {
    return !a.publish && !a.log_overload && !a.log_cadence && !a.log_hold && a.cadence_skipped == 0 && a.hold_skipped == 0;
  }

  /// 只發布，一行都不印
  bool publish_only(const action_t &a) {
    return a.publish && !a.log_overload && !a.log_cadence && !a.log_hold && a.cadence_skipped == 0 && a.hold_skipped == 0;
  }
}  // namespace

// ── 發布的時機 ───────────────────────────────────────────────────────────

TEST(VrCapturePublish, FirstSnapshotThenOncePerSecond) {
  auto p = make();
  EXPECT_FALSE(p.published());
  EXPECT_FALSE(p.overload());
  EXPECT_FALSE(p.latch_hold());
  EXPECT_FALSE(p.predict_hold());
  EXPECT_FALSE(p.abandon_overload());
  EXPECT_EQ(p.cadence(), cadence_e::unknown);
  int64_t now = kStart;
  // 還沒發布過：第一份一定發布（即使內容就是預設值——其他執行緒看到的數字要開始更新）
  EXPECT_TRUE(publish_only(p.on_snapshot(snapshot_t {}, now)));
  EXPECT_TRUE(p.published());
  // 旗標沒變：不到 1 s 不發布，滿 1 s 發布
  snapshot_t s;
  s.late_permille = 123;  // 數字變了不算
  s.pushed_fps = 119;
  EXPECT_TRUE(nothing(p.on_snapshot(s, now + 1)));
  EXPECT_TRUE(nothing(p.on_snapshot(s, now + 999 * kMs)));
  EXPECT_TRUE(nothing(p.on_snapshot(s, now + kSec - 1)));
  EXPECT_TRUE(publish_only(p.on_snapshot(s, now + kSec)));
  // 從上一次發布起算
  now += kSec;
  EXPECT_TRUE(nothing(p.on_snapshot(s, now + kSec - 1)));
  EXPECT_TRUE(publish_only(p.on_snapshot(s, now + kSec + 5 * kMs)));
  // 時間倒退：當成沒有經過時間（不發布，也不出事）
  EXPECT_TRUE(nothing(p.on_snapshot(s, now - 3600 * kSec)));
}

TEST(VrCapturePublish, ConfigureStartsOver) {
  // 每次進 capture() 都從頭來：之前發布過什麼都當成預設快照（離開 capture() 時已經清過了）
  auto p = make(overload_mode_e::cadence);
  ASSERT_TRUE(p.on_snapshot(snap(true, true, true, cadence_e::slipping, true), kStart).publish);
  p.configure(overload_mode_e::late, kSec);
  EXPECT_FALSE(p.published());
  EXPECT_FALSE(p.overload() || p.latch_hold() || p.predict_hold() || p.abandon_overload());
  EXPECT_EQ(p.cadence(), cadence_e::unknown);
  EXPECT_TRUE(nothing(p.clear()));
  // 接著的第一份一定發布（不等 1 s），旗標相對預設值變了就印
  const action_t a = p.on_snapshot(late_snap(true, cadence_e::slipping), kStart + kMs);
  EXPECT_TRUE(a.publish);
  EXPECT_TRUE(a.log_overload);
  EXPECT_TRUE(a.log_cadence);
  EXPECT_FALSE(a.log_hold);  // 現在是 late 模式
}

TEST(VrCapturePublish, TheUnitOfTimeIsTheCallers) {
  // display_vr.cpp 給的是 QPC：一秒＝QPC 的頻率（這裡 10 MHz）
  auto p = make(overload_mode_e::late, 10'000'000);
  const int64_t t0 = 123'456'789'012;
  EXPECT_TRUE(p.on_snapshot(snapshot_t {}, t0).publish);
  EXPECT_TRUE(nothing(p.on_snapshot(snapshot_t {}, t0 + 9'999'999)));
  EXPECT_TRUE(publish_only(p.on_snapshot(snapshot_t {}, t0 + 10'000'000)));
}

TEST(VrCapturePublish, EachOfTheThreeFlagsPublishesAtOnce) {
  // stream.cpp 讀的是已發布的值：過載旗標與兩個決定任何一個變了，不等每秒那一次
  for (int which = 0; which < 3; ++which) {
    SCOPED_TRACE(which);
    auto p = make(overload_mode_e::cadence);
    int64_t now = kStart;
    ASSERT_TRUE(p.on_snapshot(snapshot_t {}, now).publish);
    now += 10 * kMs;
    const snapshot_t s = snap(which == 0, which == 1, which == 2);
    const action_t a = p.on_snapshot(s, now);
    EXPECT_TRUE(a.publish);
    EXPECT_EQ(a.log_overload, which == 0);
    EXPECT_EQ(a.log_hold, which != 0);
    EXPECT_FALSE(a.log_cadence);
    EXPECT_EQ(p.overload(), which == 0);
    EXPECT_EQ(p.latch_hold(), which == 1);
    EXPECT_EQ(p.predict_hold(), which == 2);
    // 變回去也一樣
    now += 10 * kMs;
    EXPECT_TRUE(p.on_snapshot(snapshot_t {}, now).publish);
    EXPECT_FALSE(p.overload() || p.latch_hold() || p.predict_hold());
  }
}

TEST(VrCapturePublish, AbandonFlagAndCadenceWaitForThePeriodicPublish) {
  // 放棄的旗標與節拍狀態只是跟著快照出去的資訊（stream.cpp 不讀）：自己變了不觸發發布，每秒那一次才帶出去
  auto p = make();
  int64_t now = kStart;
  ASSERT_TRUE(p.on_snapshot(snapshot_t {}, now).publish);
  const snapshot_t s = snap(false, false, false, cadence_e::locked, true);
  EXPECT_TRUE(nothing(p.on_snapshot(s, now + 500 * kMs)));
  EXPECT_FALSE(p.abandon_overload());
  EXPECT_EQ(p.cadence(), cadence_e::unknown);
  now += kSec;
  const action_t a = p.on_snapshot(s, now);
  EXPECT_TRUE(a.publish);
  EXPECT_TRUE(a.log_cadence);
  EXPECT_TRUE(p.abandon_overload());
  EXPECT_EQ(p.cadence(), cadence_e::locked);
}

// ── overload on／off ─────────────────────────────────────────────────────

TEST(VrCapturePublish, OverloadLineOnEveryChange) {
  // 不限速：過載旗標每變一次一行（旗標本身設要 ≥ 30%、清要 < 5% 持續 2 s）
  auto p = make();
  int64_t now = kStart;
  ASSERT_TRUE(publish_only(p.on_snapshot(late_snap(false), now)));
  for (int i = 0; i < 10; ++i) {
    now += kMs;
    const bool on = i % 2 == 0;
    const action_t a = p.on_snapshot(late_snap(on), now);
    EXPECT_TRUE(a.publish) << i;
    EXPECT_TRUE(a.log_overload) << i;
    EXPECT_FALSE(a.log_hold) << i;  // late 模式：兩個決定就是過載旗標，不另外印
    EXPECT_EQ(p.overload(), on) << i;
    EXPECT_EQ(p.latch_hold(), on) << i;
    EXPECT_EQ(p.predict_hold(), on) << i;
  }
  // 沒變的定期發布不印
  now += kSec;
  EXPECT_TRUE(publish_only(p.on_snapshot(late_snap(false), now)));
}

// ── cadence 行 ───────────────────────────────────────────────────────────

TEST(VrCapturePublish, CadenceLineWhenTheStateDiffersFromTheLastLine) {
  // late 模式也印（影子）：狀態和上一行印的不同就一行
  auto p = make();
  int64_t now = kStart;
  action_t a = p.on_snapshot(late_snap(false, cadence_e::unsure), now);
  EXPECT_TRUE(a.publish);
  EXPECT_TRUE(a.log_cadence);  // 第一行不受限速
  EXPECT_EQ(a.cadence_skipped, 0u);
  // 沒變：不印
  now += kSec;
  EXPECT_TRUE(publish_only(p.on_snapshot(late_snap(false, cadence_e::unsure), now)));
  // 距上一行滿 1 s：照印
  now += kSec;
  a = p.on_snapshot(late_snap(false, cadence_e::locked), now);
  EXPECT_TRUE(a.log_cadence);
  EXPECT_EQ(a.cadence_skipped, 0u);
  EXPECT_EQ(p.cadence(), cadence_e::locked);
}

TEST(VrCapturePublish, CadenceLineIsLimitedToOnePerSecondAndCaughtUpLater) {
  auto p = make();
  int64_t now = kStart;
  ASSERT_TRUE(p.on_snapshot(late_snap(false, cadence_e::locked), now).log_cadence);
  const int64_t line = now;
  // 0.3 s 後變了（過載旗標跟著變，所以立刻發布）：同一秒內已經印過一行，先不印
  action_t a = p.on_snapshot(late_snap(true, cadence_e::slipping), line + 300 * kMs);
  EXPECT_TRUE(a.publish);
  EXPECT_TRUE(a.log_overload);
  EXPECT_FALSE(a.log_cadence);
  EXPECT_EQ(p.cadence(), cadence_e::slipping);  // 已發布的值是新的，只是那一行還沒印
  // 還在同一秒內的其他發布：仍然不印
  a = p.on_snapshot(late_snap(false, cadence_e::slipping), line + 999 * kMs);
  EXPECT_TRUE(a.publish);
  EXPECT_FALSE(a.log_cadence);
  // 滿 1 s 之後的下一次發布：狀態還是和上一行不同，補印。只是晚印，沒有變化被略過
  a = p.on_snapshot(late_snap(false, cadence_e::slipping), line + 1300 * kMs);  // 距上一次發布不到 1 s、旗標也沒變：不發布
  EXPECT_TRUE(nothing(a));
  a = p.on_snapshot(late_snap(false, cadence_e::slipping), line + 2 * kSec);
  EXPECT_TRUE(a.publish);
  EXPECT_TRUE(a.log_cadence);
  EXPECT_EQ(a.cadence_skipped, 0u);
}

TEST(VrCapturePublish, CadenceLineCountsTheChangesThatWereNeverPrinted) {
  auto p = make();
  int64_t now = kStart;
  ASSERT_TRUE(p.on_snapshot(late_snap(false, cadence_e::locked), now).log_cadence);
  // 同一秒內 locked → unsure → slip：unsure 那一次沒有機會印
  EXPECT_FALSE(p.on_snapshot(late_snap(true, cadence_e::unsure), now + 200 * kMs).log_cadence);
  EXPECT_FALSE(p.on_snapshot(late_snap(false, cadence_e::slipping), now + 400 * kMs).log_cadence);
  now += kSec;
  action_t a = p.on_snapshot(late_snap(true, cadence_e::slipping), now);
  EXPECT_TRUE(a.log_cadence);
  EXPECT_EQ(a.cadence_skipped, 1u);
  // 同一份狀態被多發布幾次不會重複算
  EXPECT_FALSE(p.on_snapshot(late_snap(false, cadence_e::unsure), now + 100 * kMs).log_cadence);
  EXPECT_FALSE(p.on_snapshot(late_snap(true, cadence_e::unsure), now + 200 * kMs).log_cadence);
  EXPECT_FALSE(p.on_snapshot(late_snap(false, cadence_e::unsure), now + 300 * kMs).log_cadence);
  EXPECT_FALSE(p.on_snapshot(late_snap(true, cadence_e::locked), now + 400 * kMs).log_cadence);
  now += kSec;
  a = p.on_snapshot(late_snap(false, cadence_e::locked), now);
  EXPECT_TRUE(a.log_cadence);
  EXPECT_EQ(a.cadence_skipped, 1u);  // 只有 unsure 那一次
}

TEST(VrCapturePublish, CadenceGoingBackToThePrintedStateIsNotPrintedButCounted) {
  auto p = make();
  int64_t now = kStart;
  ASSERT_TRUE(p.on_snapshot(late_snap(false, cadence_e::locked), now).log_cadence);
  // 同一秒內 locked → unsure → locked：最後和上一行印的一樣，沒有東西要印
  EXPECT_FALSE(p.on_snapshot(late_snap(true, cadence_e::unsure), now + 200 * kMs).log_cadence);
  EXPECT_FALSE(p.on_snapshot(late_snap(false, cadence_e::locked), now + 400 * kMs).log_cadence);
  now += 400 * kMs;
  for (int i = 0; i < 5; ++i) {
    now += kSec;  // 之後每秒的那一次發布
    EXPECT_TRUE(publish_only(p.on_snapshot(late_snap(false, cadence_e::locked), now))) << i;
  }
  // 之後再變：那一行說出中間有兩次變化沒印
  now += kSec;
  const action_t a = p.on_snapshot(late_snap(false, cadence_e::slipping), now);
  EXPECT_TRUE(a.log_cadence);
  EXPECT_EQ(a.cadence_skipped, 2u);
}

// ── hold 行 ──────────────────────────────────────────────────────────────

TEST(VrCapturePublish, HoldLineOnlyInCadenceMode) {
  // late 模式：兩個決定跟著過載旗標，overload on／off 已經說了——不論怎麼變、怎麼清，都沒有 hold 行
  auto p = make(overload_mode_e::late);
  int64_t now = kStart;
  for (int i = 0; i < 20; ++i) {
    now += (i % 3 == 0 ? 1500 : 200) * kMs;
    const action_t a = p.on_snapshot(late_snap(i % 2 == 0, cadence_e::slipping), now);
    EXPECT_FALSE(a.log_hold) << i;
    EXPECT_EQ(a.hold_skipped, 0u) << i;
  }
  ASSERT_TRUE(p.on_snapshot(late_snap(true, cadence_e::slipping), now + kMs).publish);
  ASSERT_TRUE(p.latch_hold());
  const action_t a = p.clear();
  EXPECT_FALSE(a.log_hold);
  EXPECT_EQ(a.hold_skipped, 0u);
  EXPECT_TRUE(a.log_overload);
}

TEST(VrCapturePublish, HoldLineIsLimitedToOnePerSecondAndCaughtUpLater) {
  auto p = make(overload_mode_e::cadence);
  const int64_t now = kStart;
  // 0／0 → 0／1（每一張都晚、節拍鎖得住）：第一行
  action_t a = p.on_snapshot(snap(true, false, true, cadence_e::locked), now - kSec);
  ASSERT_TRUE(a.publish);
  ASSERT_TRUE(a.log_hold);
  // 1 s 後 0／1 → 1／1：一行
  a = p.on_snapshot(snap(true, true, true, cadence_e::locked), now);
  EXPECT_TRUE(a.publish);
  EXPECT_TRUE(a.log_hold);
  EXPECT_EQ(a.hold_skipped, 0u);
  // 0.3 s 後 → 0／1：立刻發布（stream.cpp 要讀到），但同一秒內已經印過一行
  a = p.on_snapshot(snap(true, false, true, cadence_e::locked), now + 300 * kMs);
  EXPECT_TRUE(a.publish);
  EXPECT_FALSE(a.log_hold);
  EXPECT_FALSE(p.latch_hold());
  EXPECT_TRUE(p.predict_hold());
  // 滿 1 s 後的下一次發布補印（只是晚印，沒有變化被略過）
  a = p.on_snapshot(snap(true, false, true, cadence_e::locked), now + 1300 * kMs);
  EXPECT_TRUE(a.publish);
  EXPECT_TRUE(a.log_hold);
  EXPECT_EQ(a.hold_skipped, 0u);
}

TEST(VrCapturePublish, HoldLineCountsTheChangesThatWereNeverPrinted) {
  // 節拍在「鎖得住」與「說不準」之間來回、而每一張都晚到：決定每 0.3 s 換一次。不限速的話每秒三行
  auto p = make(overload_mode_e::cadence);
  int64_t now = kStart;
  ASSERT_TRUE(p.on_snapshot(snap(true, false, true, cadence_e::locked), now).publish);
  int lines = 0;
  uint32_t skipped = 0;
  bool latch = false;
  for (int i = 0; i < 100; ++i) {  // 30 s
    now += 300 * kMs;
    latch = !latch;
    const action_t a = p.on_snapshot(snap(true, latch, true, latch ? cadence_e::unsure : cadence_e::locked), now);
    ASSERT_TRUE(a.publish) << i;  // 每一次都立刻發布
    ASSERT_EQ(p.latch_hold(), latch) << i;
    lines += a.log_hold ? 1 : 0;
    skipped += a.hold_skipped;
  }
  // 每 0.3 s 換一次、同一秒內最多一行：每 1.5 s 印得出一行（換到和上一行不同、又滿 1 s 的那一次），
  // 中間的 4 次變化記在那一行的 skipped
  EXPECT_EQ(lines, 20);
  EXPECT_EQ(skipped, 80u);
  // 變回上一行印的那一對：不印，之後的行帶出來
  auto q = make(overload_mode_e::cadence);
  now = kStart;
  ASSERT_TRUE(q.on_snapshot(snap(false, true, true, cadence_e::slipping), now).log_hold);
  EXPECT_FALSE(q.on_snapshot(snap(false, false, false, cadence_e::locked), now + 200 * kMs).log_hold);
  EXPECT_FALSE(q.on_snapshot(snap(false, true, true, cadence_e::slipping), now + 400 * kMs).log_hold);
  now += 5 * kSec;
  EXPECT_FALSE(q.on_snapshot(snap(false, true, true, cadence_e::slipping), now).log_hold);
  now += kSec;
  const action_t a = q.on_snapshot(snap(false, false, false, cadence_e::locked), now);
  EXPECT_TRUE(a.log_hold);
  EXPECT_EQ(a.hold_skipped, 2u);
}

// ── 清除 ─────────────────────────────────────────────────────────────────

TEST(VrCapturePublish, ClearPrintsWhatWasSetAndPublishesTheDefault) {
  auto p = make(overload_mode_e::cadence);
  int64_t now = kStart;
  // 還沒發布過：什麼都不做
  EXPECT_TRUE(nothing(p.clear()));
  action_t a = p.on_snapshot(snap(true, true, true, cadence_e::slipping, true), now);
  ASSERT_TRUE(a.publish && a.log_overload && a.log_cadence && a.log_hold);
  // 清除不受限速（剛剛才各印過一行）：有設著的各一行，發預設快照
  a = p.clear();
  EXPECT_TRUE(a.publish);
  EXPECT_TRUE(a.log_overload);
  EXPECT_TRUE(a.log_hold);
  EXPECT_TRUE(a.log_cadence);
  EXPECT_EQ(a.hold_skipped, 0u);
  EXPECT_EQ(a.cadence_skipped, 0u);
  EXPECT_FALSE(p.published());
  EXPECT_FALSE(p.overload() || p.latch_hold() || p.predict_hold() || p.abandon_overload());
  EXPECT_EQ(p.cadence(), cadence_e::unknown);
  // 已經是預設：再清什麼都不做
  EXPECT_TRUE(nothing(p.clear()));
  // 清除之後的第一份一定發布（不等 1 s），第一行也不受限速
  now += kMs;
  a = p.on_snapshot(snap(false, false, false, cadence_e::unsure), now);
  EXPECT_TRUE(a.publish);
  EXPECT_TRUE(a.log_cadence);
  EXPECT_FALSE(a.log_overload);
  EXPECT_FALSE(a.log_hold);
  now += kMs;
  a = p.on_snapshot(snap(false, true, true, cadence_e::unsure), now);
  EXPECT_TRUE(a.log_hold);
}

TEST(VrCapturePublish, ClearOfADefaultLookingSnapshotOnlyPublishes) {
  // 發布過、但旗標都沒設、節拍也沒印過：清除只是把快照寫回預設（數字歸零），一行都不印
  auto p = make(overload_mode_e::cadence);
  snapshot_t s;
  s.late_permille = 40;
  s.pushed_fps = 120;
  ASSERT_TRUE(publish_only(p.on_snapshot(s, kStart)));
  EXPECT_TRUE(publish_only(p.clear()));
  EXPECT_FALSE(p.published());
}

TEST(VrCapturePublish, ClearInLateModePrintsNoHoldLine) {
  auto p = make(overload_mode_e::late);
  ASSERT_TRUE(p.on_snapshot(late_snap(true, cadence_e::locked), kStart).publish);
  const action_t a = p.clear();
  EXPECT_TRUE(a.publish);
  EXPECT_TRUE(a.log_overload);
  EXPECT_TRUE(a.log_cadence);
  EXPECT_FALSE(a.log_hold);
}

TEST(VrCapturePublish, ClearCarriesTheUnprintedChangesAndDoesNotKeepThem) {
  {
    // cadence 行：locked 印了、0.2 s 後變 slip（沒印）、然後清除——清除那一行說出有一次變化沒印
    auto p = make(overload_mode_e::late);
    int64_t now = kStart;
    ASSERT_TRUE(p.on_snapshot(late_snap(false, cadence_e::locked), now).log_cadence);
    ASSERT_FALSE(p.on_snapshot(late_snap(true, cadence_e::slipping), now + 200 * kMs).log_cadence);
    action_t a = p.clear();
    EXPECT_TRUE(a.log_cadence);
    EXPECT_EQ(a.cadence_skipped, 1u);
    // 下一個 generation 從頭算
    a = p.on_snapshot(late_snap(false, cadence_e::unsure), now + 300 * kMs);
    EXPECT_TRUE(a.log_cadence);
    EXPECT_EQ(a.cadence_skipped, 0u);
  }
  {
    // 上一行印的就是 unknown、之後變了又來不及印：清除仍然印一行，把那一次帶出來
    auto p = make(overload_mode_e::late);
    int64_t now = kStart;
    ASSERT_TRUE(p.on_snapshot(late_snap(false, cadence_e::locked), now).log_cadence);
    now += kSec;
    ASSERT_TRUE(p.on_snapshot(late_snap(false, cadence_e::unknown), now).log_cadence);
    ASSERT_FALSE(p.on_snapshot(late_snap(true, cadence_e::unsure), now + 200 * kMs).log_cadence);
    const action_t a = p.clear();
    EXPECT_TRUE(a.log_cadence);
    EXPECT_EQ(a.cadence_skipped, 1u);
    EXPECT_TRUE(nothing(p.clear()));
  }
  {
    // hold 行：1／1 印了、0.2 s 後變 0／0（沒印）、然後清除——清除那一行就是那次變化，沒有被略過的
    auto p = make(overload_mode_e::cadence);
    int64_t now = kStart;
    ASSERT_TRUE(p.on_snapshot(snap(false, true, true, cadence_e::slipping), now).log_hold);
    ASSERT_FALSE(p.on_snapshot(snap(false, false, false, cadence_e::slipping), now + 200 * kMs).log_hold);
    action_t a = p.clear();
    EXPECT_TRUE(a.log_hold);
    EXPECT_EQ(a.hold_skipped, 0u);
    // 清除之後的第一行不受限速（清除那一行才剛印）
    a = p.on_snapshot(snap(false, false, true, cadence_e::slipping), now + 300 * kMs);
    EXPECT_TRUE(a.log_hold);
    EXPECT_EQ(a.hold_skipped, 0u);
    // 同一秒內 0／1 → 1／1（沒印）→ 清除：1／1 那一次沒印過
    ASSERT_FALSE(p.on_snapshot(snap(false, true, true, cadence_e::slipping), now + 400 * kMs).log_hold);
    a = p.clear();
    EXPECT_TRUE(a.log_hold);
    EXPECT_EQ(a.hold_skipped, 1u);
  }
  {
    // 上一行印的就是 0／0、同一秒內變成 1／1 又變回來（都沒印）：清除時兩個決定已經是 0，仍然印一行把那一次帶出來
    auto p = make(overload_mode_e::cadence);
    int64_t now = kStart;
    ASSERT_TRUE(p.on_snapshot(snap(false, true, true, cadence_e::slipping), now).log_hold);
    now += kSec;
    ASSERT_TRUE(p.on_snapshot(snap(false, false, false, cadence_e::slipping), now).log_hold);
    ASSERT_FALSE(p.on_snapshot(snap(false, true, true, cadence_e::slipping), now + 200 * kMs).log_hold);
    ASSERT_FALSE(p.on_snapshot(snap(false, false, false, cadence_e::slipping), now + 400 * kMs).log_hold);
    const action_t a = p.clear();
    EXPECT_TRUE(a.log_hold);
    EXPECT_EQ(a.hold_skipped, 1u);
    EXPECT_TRUE(nothing(p.clear()));
  }
}

// ── 預設組態：和加這兩個決定之前的發布逐步相同 ──────────────────────────────

namespace {
  /// 加 §VR-CADENCE 之前 display_vr.cpp 的 publish_health／clear_health（c2275ea0；log 換成回傳值）
  struct old_publisher_t {
    bool overload_pub = false;
    int64_t health_pub_qpc = 0;  // 0＝目前發布的是預設值
    int64_t qpf;

    explicit old_publisher_t(int64_t ticks_per_s):
        qpf(ticks_per_s) {
    }

    /// 回傳 bit 0＝有發布、bit 1＝印了 overload 行
    int publish(const snapshot_t &s, int64_t now_qpc) {
      if (s.overload == overload_pub && health_pub_qpc != 0 && now_qpc - health_pub_qpc < qpf) {
        return 0;
      }
      health_pub_qpc = now_qpc;
      if (s.overload != overload_pub) {
        overload_pub = s.overload;
        return 3;
      }
      return 1;
    }

    int clear() {
      int r = 0;
      if (overload_pub) {
        overload_pub = false;
        r |= 2;
      }
      if (health_pub_qpc != 0) {
        health_pub_qpc = 0;
        r |= 1;
      }
      return r;
    }
  };
}  // namespace

TEST(VrCapturePublish, DefaultConfigurationsPublishExactlyAsBefore) {
  // 開關關著（旗標恆為 false、節拍恆為 unknown）與 late 模式（兩個決定等於過載旗標；節拍任意）：
  // 發布的時機、overload 行、清除，每一步都和舊的相同；另外沒有任何 hold 行，開關關著時也沒有 cadence 行
  constexpr int64_t kQpf = 10'000'000;
  std::mt19937_64 g(20261010);
  uint64_t publishes = 0;
  uint64_t lines = 0;
  for (int r = 0; r < 200; ++r) {
    const bool enabled = r % 2 == 1;
    auto p = make(overload_mode_e::late, kQpf);
    old_publisher_t o {kQpf};
    int64_t now = static_cast<int64_t>(1'000'000 + g() % 1'000'000'000'000ull);  // QPC 不會是 0
    bool overload = false;
    cadence_e cadence = cadence_e::unknown;
    for (int i = 0; i < 20000; ++i) {
      const uint64_t k = g() % 1000;
      if (k < 3) {
        // 沒有 frame source／換 generation／離開 capture()
        const action_t a = p.clear();
        const int b = o.clear();
        ASSERT_EQ(a.publish, (b & 1) != 0) << "round " << r << " step " << i;
        ASSERT_EQ(a.log_overload, (b & 2) != 0) << "round " << r << " step " << i;
        ASSERT_FALSE(a.log_hold) << "round " << r << " step " << i;
        if (!enabled) {
          ASSERT_FALSE(a.log_cadence) << "round " << r << " step " << i;
        }
        ASSERT_EQ(p.published(), o.health_pub_qpc != 0);
        continue;
      }
      // 影像的間隔：多半是一個週期上下，偶爾是 100 ms 的 idle 或更久
      now += k < 900 ? static_cast<int64_t>(60'000 + g() % 60'000) : static_cast<int64_t>(g() % 15'000'000);
      if (enabled) {
        if (g() % 400 == 0) {
          overload = !overload;
        }
        if (g() % 150 == 0) {
          cadence = static_cast<cadence_e>(g() % 4);
        }
      }
      snapshot_t s = late_snap(overload, cadence);
      s.abandon_overload = overload && g() % 4 == 0;
      s.late_permille = static_cast<uint16_t>(g() % 1001);
      s.pushed_fps = static_cast<uint16_t>(g() % 130);
      const action_t a = p.on_snapshot(s, now);
      const int b = o.publish(s, now);
      ASSERT_EQ(a.publish, (b & 1) != 0) << "round " << r << " step " << i;
      ASSERT_EQ(a.log_overload, (b & 2) != 0) << "round " << r << " step " << i;
      ASSERT_FALSE(a.log_hold) << "round " << r << " step " << i;
      ASSERT_EQ(a.hold_skipped, 0u) << "round " << r << " step " << i;
      if (!enabled) {
        ASSERT_FALSE(a.log_cadence) << "round " << r << " step " << i;
      }
      ASSERT_EQ(p.overload(), o.overload_pub) << "round " << r << " step " << i;
      ASSERT_EQ(p.latch_hold(), o.overload_pub) << "round " << r << " step " << i;  // stream.cpp 讀到的兩個決定＝舊的過載旗標
      ASSERT_EQ(p.predict_hold(), o.overload_pub) << "round " << r << " step " << i;
      ASSERT_EQ(p.published(), o.health_pub_qpc != 0) << "round " << r << " step " << i;
      publishes += a.publish ? 1 : 0;
      lines += a.log_overload ? 1 : 0;
    }
  }
  // 序列真的有走到（數字只是下限）
  EXPECT_GT(publishes, 20'000u);
  EXPECT_GT(lines, 1'000u);
}

// ── 亂數序列：不論怎麼餵都要成立的性質 ─────────────────────────────────────

TEST(VrCapturePublish, InvariantsHoldUnderRandomSnapshots) {
  // cadence 模式、三個旗標各自亂跳、時間偶爾倒退或跳到極值。每一步核對：
  //   沒發布就什麼都不做；旗標和已發布的不同一定發布；發布之後已發布的值就是那一份；
  //   skipped 等於上一行之後的變化次數減一；同一種行（不含清除的）兩行之間至少隔 1 s
  std::mt19937_64 g(20261013);
  uint64_t cadence_lines = 0;
  uint64_t hold_lines = 0;
  uint64_t skipped_total = 0;
  // 兩行之間至少隔 1 s（無號相減：時刻可以是 INT64 的兩端）
  const auto a_second_later = [](int64_t now, int64_t since) {
    return now > since && static_cast<uint64_t>(now) - static_cast<uint64_t>(since) >= static_cast<uint64_t>(kSec);
  };
  for (int r = 0; r < 100; ++r) {
    auto p = make(overload_mode_e::cadence);
    int64_t now = kStart + static_cast<int64_t>(g() % 1'000'000'000ull);
    snapshot_t pub {};  // 測試自己記的「已發布」
    bool published = false;
    uint32_t cad_changes = 0;
    uint32_t hold_changes = 0;
    bool have_cad_line = false;
    bool have_hold_line = false;
    int64_t cad_line_at = 0;
    int64_t hold_line_at = 0;
    snapshot_t s {};
    for (int i = 0; i < 20000; ++i) {
      const uint64_t k = g() % 1000;
      if (k < 2) {
        const action_t a = p.clear();
        ASSERT_EQ(a.publish, published) << "round " << r << " step " << i;
        ASSERT_EQ(a.log_overload, pub.overload) << "round " << r << " step " << i;
        cad_changes += pub.cadence != cadence_e::unknown ? 1u : 0u;
        hold_changes += pub.latch_hold || pub.predict_hold ? 1u : 0u;
        if (a.log_cadence) {
          ASSERT_EQ(a.cadence_skipped, cad_changes > 0 ? cad_changes - 1 : 0u) << "round " << r << " step " << i;
        } else {
          ASSERT_EQ(cad_changes, 0u) << "round " << r << " step " << i;  // 沒印＝沒有任何東西要說
        }
        if (a.log_hold) {
          ASSERT_EQ(a.hold_skipped, hold_changes > 0 ? hold_changes - 1 : 0u) << "round " << r << " step " << i;
        } else {
          ASSERT_EQ(hold_changes, 0u) << "round " << r << " step " << i;
        }
        pub = snapshot_t {};
        published = false;
        cad_changes = hold_changes = 0;
        have_cad_line = have_hold_line = false;
        ASSERT_FALSE(p.published());
        ASSERT_EQ(p.cadence(), cadence_e::unknown);
        ASSERT_FALSE(p.overload() || p.latch_hold() || p.predict_hold() || p.abandon_overload());
        continue;
      }
      if (k == 2) {
        now = kI64Min;
      } else if (k == 3) {
        now = kI64Max;
      } else if (k == 4) {
        if (now > kI64Min + 6 * kSec) {
          now -= static_cast<int64_t>(g() % 5'000'000'000ull);  // 倒退
        }
      } else if (now < kI64Max - 2 * kSec) {
        now += static_cast<int64_t>(g() % (k < 900 ? 20'000'000ull : 1'500'000'000ull));
      }
      if (g() % 60 == 0) {
        s.overload = g() % 2 == 0;
      }
      if (g() % 25 == 0) {
        s.latch_hold = g() % 2 == 0;
      }
      if (g() % 40 == 0) {
        s.predict_hold = g() % 2 == 0;
      }
      if (g() % 30 == 0) {
        s.cadence = static_cast<cadence_e>(g() % 4);
      }
      s.abandon_overload = g() % 7 == 0;
      const bool flags_differ = s.overload != pub.overload || s.latch_hold != pub.latch_hold || s.predict_hold != pub.predict_hold;
      const action_t a = p.on_snapshot(s, now);
      if (!a.publish) {
        ASSERT_TRUE(nothing(a)) << "round " << r << " step " << i;
        ASSERT_FALSE(flags_differ) << "round " << r << " step " << i;
        ASSERT_TRUE(published) << "round " << r << " step " << i;
        continue;
      }
      ASSERT_EQ(a.log_overload, s.overload != pub.overload) << "round " << r << " step " << i;
      cad_changes += s.cadence != pub.cadence ? 1u : 0u;
      hold_changes += s.latch_hold != pub.latch_hold || s.predict_hold != pub.predict_hold ? 1u : 0u;
      if (a.log_cadence) {
        ASSERT_GE(cad_changes, 1u) << "round " << r << " step " << i;
        ASSERT_EQ(a.cadence_skipped, cad_changes - 1) << "round " << r << " step " << i;
        if (have_cad_line) {
          ASSERT_TRUE(a_second_later(now, cad_line_at)) << "round " << r << " step " << i;
        }
        have_cad_line = true;
        cad_line_at = now;
        cad_changes = 0;
        ++cadence_lines;
        skipped_total += a.cadence_skipped;
      } else {
        ASSERT_EQ(a.cadence_skipped, 0u);
      }
      if (a.log_hold) {
        ASSERT_GE(hold_changes, 1u) << "round " << r << " step " << i;
        ASSERT_EQ(a.hold_skipped, hold_changes - 1) << "round " << r << " step " << i;
        if (have_hold_line) {
          ASSERT_TRUE(a_second_later(now, hold_line_at)) << "round " << r << " step " << i;
        }
        have_hold_line = true;
        hold_line_at = now;
        hold_changes = 0;
        ++hold_lines;
        skipped_total += a.hold_skipped;
      } else {
        ASSERT_EQ(a.hold_skipped, 0u);
      }
      pub = s;
      published = true;
      ASSERT_TRUE(p.published());
      ASSERT_EQ(p.overload(), s.overload);
      ASSERT_EQ(p.latch_hold(), s.latch_hold);
      ASSERT_EQ(p.predict_hold(), s.predict_hold);
      ASSERT_EQ(p.abandon_overload(), s.abandon_overload);
      ASSERT_EQ(p.cadence(), s.cadence);
    }
  }
  EXPECT_GT(cadence_lines, 5'000u);
  EXPECT_GT(hold_lines, 5'000u);
  EXPECT_GT(skipped_total, 1'000u);
}

TEST(VrCapturePublish, TheLastStateIsAlwaysPrintedWithinASecondOrSo) {
  // 不論之前怎麼來回，狀態停下來之後：最多再等一次每秒的發布，最後的狀態就會印出來（不會停在舊的那一行）
  std::mt19937_64 g(20261014);
  for (int r = 0; r < 300; ++r) {
    auto p = make(overload_mode_e::cadence);
    int64_t now = kStart;
    snapshot_t s {};
    cadence_e cad_line = cadence_e::unknown;
    bool latch_line = false;
    bool predict_line = false;
    const auto step = [&](int64_t dt) {
      now += dt;
      const action_t a = p.on_snapshot(s, now);
      if (a.log_cadence) {
        cad_line = s.cadence;
      }
      if (a.log_hold) {
        latch_line = s.latch_hold;
        predict_line = s.predict_hold;
      }
    };
    const int n = 5 + static_cast<int>(g() % 60);
    for (int i = 0; i < n; ++i) {
      s.overload = g() % 2 == 0;
      s.latch_hold = g() % 2 == 0;
      s.predict_hold = g() % 2 == 0;
      s.cadence = static_cast<cadence_e>(g() % 4);
      step(static_cast<int64_t>(1 + g() % 400) * kMs);
    }
    // 狀態停住，照擷取迴圈的節拍（約每 8 ms 一張）再跑 2.1 s
    for (int i = 0; i < 260; ++i) {
      step(8 * kMs);
    }
    ASSERT_EQ(cad_line, s.cadence) << "round " << r;
    ASSERT_EQ(latch_line, s.latch_hold) << "round " << r;
    ASSERT_EQ(predict_line, s.predict_hold) << "round " << r;
  }
}
