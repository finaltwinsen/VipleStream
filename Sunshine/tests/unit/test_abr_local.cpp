/**
 * @file tests/unit/test_abr_local.cpp
 * @brief §ABR-LOCAL：2D 送出端飽和視窗的單元測試（逐條連線）。
 *
 * 獨立編譯：scripts/ut/ut_build.cmd test_abr_local（只依賴標頭 src/abr_local.h）。
 */
#include "../tests_common.h"

#include <initializer_list>

#include <src/abr_local.h>

namespace al = abr_local;
using namespace std::chrono_literals;

namespace {
  struct AbrLocalTest: testing::Test {
    // 故意用很小的起點：比較都不可以因為時間起點小而溢位或誤判
    al::steady::time_point t0 = al::steady::time_point {} + 10ms;
    al::local_signal_t s;

    static al::link_in_t L(uint64_t tx, uint64_t drop, bool sending = true, int id = 0) {
      al::link_in_t x;
      x.tx_pkts = tx;
      x.drop_pkts = drop;
      x.sending = sending;
      x.id = id;
      return x;
    }

    /// 前幾格是這一輪的連線（slot 順序穩定）。stale 是這一場的 client 回報，不是每條一個
    al::local_out_t upd(al::steady::time_point now, std::initializer_list<al::link_in_t> in, bool stale = false) {
      std::array<al::link_in_t, al::k_max_links> links {};
      int n = 0;
      for (const auto &x : in) {
        if (n >= al::k_max_links) {
          break;
        }
        links[(size_t) n++] = x;
      }
      return s.update(links, n, stale, now);
    }
  };
}  // namespace

// 未滿 500 ms 不判。第一筆只取基準；中間就算已經累積到重度飽和的量，也不算飽和過
TEST_F(AbrLocalTest, ShortWindowIsNotJudged) {
  auto o = upd(t0, {L(0, 100)});
  EXPECT_EQ(o.verdict, al::verdict_e::none);
  EXPECT_EQ(o.win_ms, 0);
  EXPECT_FALSE(s.saturated_recently(t0));

  o = upd(t0 + 499ms, {L(70, 130)});
  EXPECT_EQ(o.verdict, al::verdict_e::none);
  EXPECT_EQ(o.win_ms, 499);
  EXPECT_EQ(o.links, 0);  // 未滿窗還沒有代表連線
  EXPECT_FALSE(s.saturated_recently(t0 + 499ms));

  o = upd(t0 + 500ms, {L(70, 130)});
  EXPECT_EQ(o.verdict, al::verdict_e::cut);  // 30/100 = 30%，滿窗才成立
  EXPECT_EQ(o.win_tx, uint64_t {70});
  EXPECT_EQ(o.win_drop, uint64_t {30});
  EXPECT_EQ(o.win_ms, 500);
  EXPECT_EQ(o.drop_permille, 300);
  EXPECT_EQ(o.links, 1);
  EXPECT_EQ(o.link_id, 0);
  EXPECT_TRUE(s.saturated_recently(t0 + 500ms));
}

// 有在送、沒有丟：none，也不會留下「最近飽和」
TEST_F(AbrLocalTest, CleanWindowIsNone) {
  upd(t0, {L(0, 0)});
  const auto o = upd(t0 + 500ms, {L(1000, 0)});
  EXPECT_EQ(o.verdict, al::verdict_e::none);
  EXPECT_EQ(o.win_tx, uint64_t {1000});
  EXPECT_EQ(o.win_drop, uint64_t {0});
  EXPECT_EQ(o.drop_permille, 0);
  EXPECT_EQ(o.links, 1);
  EXPECT_FALSE(s.saturated_recently(t0 + 500ms));
}

// 這一窗封包數完全沒增加：分母是 0，條數不夠，不判
TEST_F(AbrLocalTest, ZeroPacketsIsNotJudged) {
  upd(t0, {L(100, 5)});
  const auto o = upd(t0 + 500ms, {L(100, 5)});
  EXPECT_EQ(o.verdict, al::verdict_e::none);
  EXPECT_EQ(o.links, 0);
  EXPECT_EQ(o.link_id, -1);
  EXPECT_EQ(o.win_tx, uint64_t {0});
  EXPECT_EQ(o.win_drop, uint64_t {0});
  EXPECT_FALSE(s.saturated_recently(t0 + 500ms));
}

// 5% 且至少 20 個：一窗 hold、連續第二窗 cut。差 1 個封包（20/401）不到 5%，不算
TEST_F(AbrLocalTest, TwoSaturatedWindowsCut) {
  upd(t0, {L(0, 0)});
  auto o = upd(t0 + 500ms, {L(381, 20)});  // 20/401 = 49‰
  EXPECT_EQ(o.verdict, al::verdict_e::none);
  EXPECT_EQ(o.drop_permille, 49);
  EXPECT_EQ(o.links, 1);
  EXPECT_FALSE(s.saturated_recently(t0 + 500ms));

  o = upd(t0 + 1000ms, {L(761, 40)});  // 20/400 = 50‰
  EXPECT_EQ(o.verdict, al::verdict_e::hold);
  EXPECT_EQ(o.drop_permille, 50);
  EXPECT_EQ(o.win_drop, uint64_t {20});

  o = upd(t0 + 1500ms, {L(1141, 60)});
  EXPECT_EQ(o.verdict, al::verdict_e::cut);
  EXPECT_EQ(o.win_drop, uint64_t {20});
  EXPECT_EQ(o.win_tx, uint64_t {380});
}

// 299‰ 還是一般飽和（要第二窗）；剛好 300‰ 一窗就 cut
TEST_F(AbrLocalTest, SevereThresholdIs300Permille) {
  upd(t0, {L(0, 0)});
  auto o = upd(t0 + 500ms, {L(701, 299)});
  EXPECT_EQ(o.verdict, al::verdict_e::hold);
  EXPECT_EQ(o.drop_permille, 299);

  al::local_signal_t other;
  s = other;
  upd(t0, {L(0, 0)});
  o = upd(t0 + 500ms, {L(700, 300)});
  EXPECT_EQ(o.verdict, al::verdict_e::cut);
  EXPECT_EQ(o.drop_permille, 300);
}

// 中間插一個乾淨窗，連續窗數重來：第三個飽和窗仍是 hold
TEST_F(AbrLocalTest, CleanWindowBreaksTheStreak) {
  upd(t0, {L(0, 0)});
  EXPECT_EQ(upd(t0 + 500ms, {L(380, 20)}).verdict, al::verdict_e::hold);
  EXPECT_EQ(upd(t0 + 1000ms, {L(1380, 20)}).verdict, al::verdict_e::none);
  const auto o = upd(t0 + 1500ms, {L(1760, 40)});
  EXPECT_EQ(o.verdict, al::verdict_e::hold);
  EXPECT_EQ(o.win_drop, uint64_t {20});
}

// 30% 且至少 20 個：一窗就 cut
TEST_F(AbrLocalTest, SevereWindowCutsImmediately) {
  upd(t0, {L(0, 0)});
  const auto o = upd(t0 + 500ms, {L(70, 30)});
  EXPECT_EQ(o.verdict, al::verdict_e::cut);
  EXPECT_EQ(o.drop_permille, 300);
  EXPECT_EQ(o.win_ms, 500);
}

// 一般飽和加上 client 回報斷了：沒有別人會降，一窗就 cut
TEST_F(AbrLocalTest, SaturatedAndStaleFeedbackCutsInOneWindow) {
  upd(t0, {L(0, 0)}, false);
  const auto o = upd(t0 + 500ms, {L(380, 20)}, true);
  EXPECT_EQ(o.verdict, al::verdict_e::cut);
  EXPECT_TRUE(o.feedback_stale);
  EXPECT_EQ(o.drop_permille, 50);
}

// 只有回報斷了、送得出去：none。既有的 held 機制處理 client 卡住，這裡不降
TEST_F(AbrLocalTest, StaleFeedbackAloneIsNone) {
  upd(t0, {L(0, 0)}, true);
  const auto o = upd(t0 + 500ms, {L(1000, 0)}, true);
  EXPECT_EQ(o.verdict, al::verdict_e::none);
  EXPECT_TRUE(o.feedback_stale);
  EXPECT_FALSE(s.saturated_recently(t0 + 500ms));
}

// 冷卻 2 秒內飽和只 hold；剛好滿 2 秒的下一窗可以再 cut。冷卻中不把連續窗數清掉
TEST_F(AbrLocalTest, CooldownSuppressesThenAllowsAnotherCut) {
  upd(t0, {L(0, 0)});
  EXPECT_EQ(upd(t0 + 500ms, {L(380, 20)}).verdict, al::verdict_e::hold);
  EXPECT_EQ(upd(t0 + 1000ms, {L(760, 40)}).verdict, al::verdict_e::cut);

  auto o = upd(t0 + 1500ms, {L(1140, 60)});
  EXPECT_EQ(o.verdict, al::verdict_e::hold);

  o = upd(t0 + 3000ms, {L(1520, 80)});  // 距上一刀剛好 2000 ms
  EXPECT_EQ(o.verdict, al::verdict_e::cut);
  EXPECT_EQ(o.win_drop, uint64_t {20});
  EXPECT_EQ(o.win_ms, 1500);
}

// 差 1 ms 仍在冷卻內：hold。上一刀在 t0+1000，這一窗結在 t0+2999
TEST_F(AbrLocalTest, CooldownAt1999msIsHold) {
  upd(t0, {L(0, 0)});
  EXPECT_EQ(upd(t0 + 500ms, {L(380, 20)}).verdict, al::verdict_e::hold);
  EXPECT_EQ(upd(t0 + 1000ms, {L(760, 40)}).verdict, al::verdict_e::cut);

  const auto o = upd(t0 + 2999ms, {L(1140, 60)});
  EXPECT_EQ(o.verdict, al::verdict_e::hold);
  EXPECT_EQ(o.win_ms, 1999);
  EXPECT_EQ(o.drop_permille, 50);
}

// 冷卻中的重度窗也只 hold；冷卻一滿，下一窗的重度可以再 cut
TEST_F(AbrLocalTest, SevereDuringCooldownIsHold) {
  upd(t0, {L(0, 0)});
  EXPECT_EQ(upd(t0 + 500ms, {L(70, 30)}).verdict, al::verdict_e::cut);

  auto o = upd(t0 + 1000ms, {L(140, 60)});
  EXPECT_EQ(o.verdict, al::verdict_e::hold);
  EXPECT_EQ(o.drop_permille, 300);

  o = upd(t0 + 2500ms, {L(210, 90)});  // 距上一刀剛好 2000 ms
  EXPECT_EQ(o.verdict, al::verdict_e::cut);
  EXPECT_EQ(o.drop_permille, 300);
}

// 冷卻中「飽和且回報斷了」也只 hold
TEST_F(AbrLocalTest, StaleDuringCooldownIsHold) {
  upd(t0, {L(0, 0)});
  EXPECT_EQ(upd(t0 + 500ms, {L(380, 20)}).verdict, al::verdict_e::hold);
  EXPECT_EQ(upd(t0 + 1000ms, {L(760, 40)}).verdict, al::verdict_e::cut);

  const auto o = upd(t0 + 1500ms, {L(1140, 60)}, true);
  EXPECT_EQ(o.verdict, al::verdict_e::hold);
  EXPECT_TRUE(o.feedback_stale);
}

// 冷卻中的乾淨窗把連續窗數清掉：冷卻結束後的第一個一般飽和窗是 hold，不是 cut
TEST_F(AbrLocalTest, CleanWindowDuringCooldownBreaksTheStreak) {
  upd(t0, {L(0, 0)});
  EXPECT_EQ(upd(t0 + 500ms, {L(380, 20)}).verdict, al::verdict_e::hold);
  EXPECT_EQ(upd(t0 + 1000ms, {L(760, 40)}).verdict, al::verdict_e::cut);
  EXPECT_EQ(upd(t0 + 1500ms, {L(1760, 40)}).verdict, al::verdict_e::none);

  const auto o = upd(t0 + 3000ms, {L(2140, 60)});  // 距上一刀剛好 2000 ms，20/400 = 50‰
  EXPECT_EQ(o.verdict, al::verdict_e::hold);
  EXPECT_EQ(o.drop_permille, 50);
}

// 計數器倒退：這一窗不合格、倒退點到結算之間不另開子窗、連續窗數在結算時歸零，「最近飽和」保留。
// 下一窗從結算那一筆起算，所以是 hold 而不是 cut
TEST_F(AbrLocalTest, CounterRegressionRebasesWithoutJudging) {
  upd(t0, {L(1000, 0)});
  ASSERT_EQ(upd(t0 + 500ms, {L(1380, 20)}).verdict, al::verdict_e::hold);

  const auto mid = upd(t0 + 600ms, {L(10, 0)});
  EXPECT_EQ(mid.verdict, al::verdict_e::none);
  EXPECT_EQ(mid.win_ms, 100);
  EXPECT_TRUE(s.saturated_recently(t0 + 600ms));

  const auto closed = upd(t0 + 1000ms, {L(500, 100)});
  EXPECT_EQ(closed.verdict, al::verdict_e::none);
  EXPECT_EQ(closed.links, 0);
  EXPECT_EQ(closed.link_id, -1);
  EXPECT_EQ(closed.win_tx, uint64_t {0});
  EXPECT_EQ(closed.win_drop, uint64_t {0});
  EXPECT_TRUE(s.saturated_recently(t0 + 1000ms));

  const auto o = upd(t0 + 1500ms, {L(880, 120)});  // 自結算基準 500/100 起，+380/+20
  EXPECT_EQ(o.verdict, al::verdict_e::hold);
  EXPECT_EQ(o.win_tx, uint64_t {380});
  EXPECT_EQ(o.win_drop, uint64_t {20});
  EXPECT_EQ(o.drop_permille, 50);
  EXPECT_EQ(o.links, 1);
}

// 一條倒退、另一條整窗乾淨：代表值是乾淨的那條，none
TEST_F(AbrLocalTest, RegressedLinkDoesNotHideACleanLink) {
  upd(t0, {L(1000, 0, true, 1), L(0, 0, true, 2)});
  upd(t0 + 200ms, {L(5, 0, true, 1), L(200, 0, true, 2)});
  const auto o = upd(t0 + 500ms, {L(5, 0, true, 1), L(400, 0, true, 2)});
  EXPECT_EQ(o.verdict, al::verdict_e::none);
  EXPECT_EQ(o.links, 1);
  EXPECT_EQ(o.link_id, 2);
  EXPECT_EQ(o.win_tx, uint64_t {400});
  EXPECT_EQ(o.win_drop, uint64_t {0});
  EXPECT_FALSE(s.saturated_recently(t0 + 500ms));
}

// 一條倒退、另一條整窗重度：只按還合格的那條 cut
TEST_F(AbrLocalTest, RegressedLinkDoesNotBlockTheOtherLinksCut) {
  upd(t0, {L(1000, 0, true, 1), L(0, 0, true, 2)});
  upd(t0 + 200ms, {L(10, 0, true, 1), L(0, 40, true, 2)});
  const auto o = upd(t0 + 500ms, {L(10, 0, true, 1), L(0, 100, true, 2)});
  EXPECT_EQ(o.verdict, al::verdict_e::cut);
  EXPECT_EQ(o.links, 1);
  EXPECT_EQ(o.link_id, 2);
  EXPECT_EQ(o.win_tx, uint64_t {0});
  EXPECT_EQ(o.win_drop, uint64_t {100});
  EXPECT_EQ(o.drop_permille, 1000);
}

// 封包數夠、丟掉不滿 20 個：比例再高也不算飽和。代表連線的數字仍印得出來
TEST_F(AbrLocalTest, FewerThanMinDropsIsNotSaturated) {
  upd(t0, {L(0, 0)});
  const auto o = upd(t0 + 500ms, {L(81, 19)});  // 19/100 = 190‰，但不到 k_min_drop
  EXPECT_EQ(o.verdict, al::verdict_e::none);
  EXPECT_EQ(o.links, 1);
  EXPECT_EQ(o.win_tx, uint64_t {81});
  EXPECT_EQ(o.win_drop, uint64_t {19});
  EXPECT_EQ(o.drop_permille, 190);
  EXPECT_FALSE(s.saturated_recently(t0 + 500ms));
}

// 剛好 3000 ms 還算最近飽和；3001 ms 不算。中間的乾淨窗不把記憶清掉
TEST_F(AbrLocalTest, SaturatedRecentlyBoundary) {
  upd(t0, {L(0, 0)});
  ASSERT_EQ(upd(t0 + 500ms, {L(380, 20)}).verdict, al::verdict_e::hold);

  const auto satAt = t0 + 500ms;
  EXPECT_FALSE(s.saturated_recently(t0));
  EXPECT_TRUE(s.saturated_recently(satAt));
  EXPECT_TRUE(s.saturated_recently(satAt + 3000ms));
  EXPECT_FALSE(s.saturated_recently(satAt + 3001ms));

  EXPECT_EQ(upd(t0 + 1000ms, {L(1380, 20)}).verdict, al::verdict_e::none);
  EXPECT_TRUE(s.saturated_recently(satAt + 3000ms));
  EXPECT_FALSE(s.saturated_recently(satAt + 3001ms));
}

// reset 清掉視窗、冷卻與「最近飽和」。下一窗可以立刻再判，重度飽和不用再等 2 秒
TEST_F(AbrLocalTest, ResetClearsWindowCooldownAndRecent) {
  upd(t0, {L(0, 0)});
  ASSERT_EQ(upd(t0 + 500ms, {L(70, 30)}).verdict, al::verdict_e::cut);
  EXPECT_TRUE(s.saturated_recently(t0 + 500ms));

  s.reset();
  EXPECT_FALSE(s.saturated_recently(t0 + 500ms));
  EXPECT_EQ(upd(t0 + 600ms, {L(70, 30)}).verdict, al::verdict_e::none);

  const auto o = upd(t0 + 1100ms, {L(140, 60)});
  EXPECT_EQ(o.verdict, al::verdict_e::cut);
  EXPECT_EQ(o.win_drop, uint64_t {30});
  EXPECT_EQ(o.drop_permille, 300);
}

// 兩條：一條全丟、一條乾淨。代表值是乾淨的那條，即使 client 回報已斷也不降
TEST_F(AbrLocalTest, OneLinkAllDropAndOneCleanIsNone) {
  upd(t0, {L(0, 0, true, 1), L(0, 0, true, 2)});
  const auto o = upd(t0 + 500ms, {L(0, 400, true, 1), L(400, 0, true, 2)}, true);
  EXPECT_EQ(o.verdict, al::verdict_e::none);
  EXPECT_TRUE(o.feedback_stale);
  EXPECT_EQ(o.link_id, 2);
  EXPECT_EQ(o.links, 2);
  EXPECT_EQ(o.win_tx, uint64_t {400});
  EXPECT_EQ(o.win_drop, uint64_t {0});
  EXPECT_EQ(o.drop_permille, 0);
  EXPECT_FALSE(s.saturated_recently(t0 + 500ms));
}

// 兩條都飽和：以丟棄比例較低的那條判。第一窗 hold，第二窗仍按較低的那條 cut（不是較差那條的重度）
TEST_F(AbrLocalTest, TwoSaturatedLinksFollowTheLowerRatio) {
  upd(t0, {L(0, 0, true, 1), L(0, 0, true, 2)});
  auto o = upd(t0 + 500ms, {L(70, 30, true, 1), L(380, 20, true, 2)});  // 30% 與 5%
  EXPECT_EQ(o.verdict, al::verdict_e::hold);
  EXPECT_EQ(o.link_id, 2);
  EXPECT_EQ(o.links, 2);
  EXPECT_EQ(o.win_tx, uint64_t {380});
  EXPECT_EQ(o.win_drop, uint64_t {20});
  EXPECT_EQ(o.drop_permille, 50);

  o = upd(t0 + 1000ms, {L(140, 60, true, 1), L(760, 40, true, 2)});
  EXPECT_EQ(o.verdict, al::verdict_e::cut);
  EXPECT_EQ(o.link_id, 2);
  EXPECT_EQ(o.links, 2);
  EXPECT_EQ(o.drop_permille, 50);
}

// 比例相同留較小的 slot，log 用的是該格的 id
TEST_F(AbrLocalTest, EqualRatioKeepsTheLowerSlot) {
  upd(t0, {L(0, 0, true, 7), L(0, 0, true, 8)});
  const auto o = upd(t0 + 500ms, {L(950, 50, true, 7), L(950, 50, true, 8)});
  EXPECT_EQ(o.verdict, al::verdict_e::hold);
  EXPECT_EQ(o.link_id, 7);
  EXPECT_EQ(o.links, 2);
  EXPECT_EQ(o.drop_permille, 50);
}

// 這一窗中途退出：那一條不合格，退出前的丟棄不補進這一窗。結算時又回到送出也不追認
TEST_F(AbrLocalTest, LeavingMidWindowDisqualifiesThatLink) {
  upd(t0, {L(0, 0, true, 1)});
  upd(t0 + 200ms, {L(0, 1000, false, 1)});
  auto o = upd(t0 + 500ms, {L(0, 1000, false, 1)});
  EXPECT_EQ(o.verdict, al::verdict_e::none);
  EXPECT_EQ(o.links, 0);
  EXPECT_EQ(o.link_id, -1);
  EXPECT_EQ(o.win_drop, uint64_t {0});
  EXPECT_FALSE(s.saturated_recently(t0 + 500ms));

  al::local_signal_t back;
  s = back;
  upd(t0, {L(0, 0, true, 1)});
  upd(t0 + 200ms, {L(0, 400, false, 1)});
  o = upd(t0 + 500ms, {L(0, 800, true, 1)});
  EXPECT_EQ(o.verdict, al::verdict_e::none);
  EXPECT_EQ(o.links, 0);
  EXPECT_EQ(o.win_drop, uint64_t {0});
  EXPECT_FALSE(s.saturated_recently(t0 + 500ms));
}

// 一條中途退出、另一條整窗乾淨：合格的只有乾淨那條
TEST_F(AbrLocalTest, ExitingLinkDoesNotSpoilTheCleanLink) {
  upd(t0, {L(0, 0, true, 1), L(0, 0, true, 2)});
  upd(t0 + 200ms, {L(0, 1000, false, 1), L(200, 0, true, 2)});
  const auto o = upd(t0 + 500ms, {L(0, 1000, false, 1), L(400, 0, true, 2)});
  EXPECT_EQ(o.verdict, al::verdict_e::none);
  EXPECT_EQ(o.links, 1);
  EXPECT_EQ(o.link_id, 2);
  EXPECT_EQ(o.win_tx, uint64_t {400});
  EXPECT_EQ(o.win_drop, uint64_t {0});
  EXPECT_FALSE(s.saturated_recently(t0 + 500ms));
}

// 這一窗中途才開始送：這一窗不合格。下一窗從結算那一筆起算，才看得到重度
TEST_F(AbrLocalTest, EnteringMidWindowIsUnqualifiedUntilTheNextWindow) {
  upd(t0, {L(0, 0, false, 1)});
  upd(t0 + 200ms, {L(0, 100, true, 1)});
  auto o = upd(t0 + 500ms, {L(0, 200, true, 1)});
  EXPECT_EQ(o.verdict, al::verdict_e::none);
  EXPECT_EQ(o.links, 0);
  EXPECT_EQ(o.link_id, -1);
  EXPECT_FALSE(s.saturated_recently(t0 + 500ms));

  o = upd(t0 + 1000ms, {L(0, 280, true, 1)});  // 自 200 起 +80，100%
  EXPECT_EQ(o.verdict, al::verdict_e::cut);
  EXPECT_EQ(o.links, 1);
  EXPECT_EQ(o.link_id, 1);
  EXPECT_EQ(o.win_drop, uint64_t {80});
  EXPECT_EQ(o.drop_permille, 1000);
}

// 只有一條封包數夠：不夠的那條（含幾乎全丟但不到 50 個）不參與
TEST_F(AbrLocalTest, OnlyOneQualifiedLink) {
  upd(t0, {L(0, 0, true, 1), L(0, 0, true, 2)});
  auto o = upd(t0 + 500ms, {L(10, 0, true, 1), L(380, 20, true, 2)});
  EXPECT_EQ(o.verdict, al::verdict_e::hold);
  EXPECT_EQ(o.links, 1);
  EXPECT_EQ(o.link_id, 2);
  EXPECT_EQ(o.win_tx, uint64_t {380});
  EXPECT_EQ(o.win_drop, uint64_t {20});
  EXPECT_EQ(o.drop_permille, 50);

  al::local_signal_t other;
  s = other;
  upd(t0, {L(0, 0, true, 1), L(0, 0, true, 2)});
  o = upd(t0 + 500ms, {L(0, 40, true, 1), L(400, 0, true, 2)});  // 40 個全丟，不到 50
  EXPECT_EQ(o.verdict, al::verdict_e::none);
  EXPECT_EQ(o.links, 1);
  EXPECT_EQ(o.link_id, 2);
  EXPECT_EQ(o.win_drop, uint64_t {0});
  EXPECT_FALSE(s.saturated_recently(t0 + 500ms));
}

// 整窗不到 50 個：不判、不算飽和。下一窗補滿 50 個全丟才 cut
TEST_F(AbrLocalTest, FewerThanMinLinkPacketsIsNotJudged) {
  upd(t0, {L(0, 0)});
  auto o = upd(t0 + 500ms, {L(0, 49)});
  EXPECT_EQ(o.verdict, al::verdict_e::none);
  EXPECT_EQ(o.links, 0);
  EXPECT_EQ(o.link_id, -1);
  EXPECT_EQ(o.win_drop, uint64_t {0});
  EXPECT_EQ(o.win_ms, 500);
  EXPECT_FALSE(s.saturated_recently(t0 + 500ms));

  o = upd(t0 + 1000ms, {L(0, 99)});  // 基準停在 49，這一窗 50 個
  EXPECT_EQ(o.verdict, al::verdict_e::cut);
  EXPECT_EQ(o.links, 1);
  EXPECT_EQ(o.win_drop, uint64_t {50});
  EXPECT_EQ(o.win_tx, uint64_t {0});
  EXPECT_EQ(o.drop_permille, 1000);
}

// 時間倒退：這一窗作廢、連續窗數歸零，最近飽和與上一刀的時刻留著。
// 倒退之後的新窗落在上一刀之前，now < last_cut_ 不算冷卻中，重度仍 cut
TEST_F(AbrLocalTest, ClockRewindDoesNotStickCooldown) {
  upd(t0, {L(0, 0)});
  ASSERT_EQ(upd(t0 + 5000ms, {L(0, 80)}).verdict, al::verdict_e::cut);

  const auto rewound = upd(t0 + 100ms, {L(0, 80)});
  EXPECT_EQ(rewound.verdict, al::verdict_e::none);
  EXPECT_EQ(rewound.links, 0);
  EXPECT_TRUE(s.saturated_recently(t0 + 5000ms));
  EXPECT_TRUE(s.saturated_recently(t0 + 8000ms));
  EXPECT_FALSE(s.saturated_recently(t0 + 100ms));  // now 比上次飽和還早，記憶留著但不算「現在」

  const auto o = upd(t0 + 600ms, {L(0, 160)});  // 自倒退後的基準 +80，100%；時刻仍早於上一刀
  EXPECT_EQ(o.verdict, al::verdict_e::cut);
  EXPECT_EQ(o.win_drop, uint64_t {80});
  EXPECT_EQ(o.drop_permille, 1000);
  EXPECT_EQ(o.links, 1);
}
