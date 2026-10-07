/**
 * @file tests/unit/test_vr_abr_outage.cpp
 * @brief VipleStream 2.0 §VR-ABR-OUTAGE：VR 斷訊後把位元率拉回斷訊前的值。
 */
#include "../tests_common.h"

#include <src/vr/vr_abr_outage.h>

namespace abr = vr::abr;
using namespace std::chrono_literals;

namespace {
  constexpr int kMax = 178000;

  struct AbrOutageTest: testing::Test {
    abr::outage_state_t s;
    abr::steady::time_point t0 = abr::steady::now();
  };
}  // namespace

TEST_F(AbrOutageTest, RestoresAfterQuietAndTwoCleanWindows) {
  EXPECT_EQ(abr::on_outage(s, 140000, t0), abr::outage_e::started);
  EXPECT_EQ(s.ref_kbps, 140000);
  // 斷訊期間 AIMD 照常砍到底；恢復後要 1 s 安靜＋2 個乾淨視窗才拉回
  EXPECT_EQ(abr::on_clean(s, 11250, kMax, t0 + 1000ms), 11250);  // 第 1 個
  EXPECT_EQ(abr::on_clean(s, 11250, kMax, t0 + 1500ms), 140000);  // 第 2 個
  // 每次斷訊只拉一次
  EXPECT_EQ(abr::on_clean(s, 120000, kMax, t0 + 2s), 120000);
}

TEST_F(AbrOutageTest, RequiresOneSecondOfQuiet) {
  abr::on_outage(s, 140000, t0);
  EXPECT_EQ(abr::on_clean(s, 11250, kMax, t0 + 300ms), 11250);
  EXPECT_EQ(abr::on_clean(s, 11250, kMax, t0 + 600ms), 11250);  // 2 個乾淨視窗，但還不到 1 s
  EXPECT_EQ(abr::on_clean(s, 11250, kMax, t0 + 1100ms), 140000);
}

TEST_F(AbrOutageTest, RepeatedOutageWindowsKeepTheOriginalReference) {
  abr::on_outage(s, 140000, t0);
  EXPECT_EQ(abr::on_outage(s, 70000, t0 + 500ms), abr::outage_e::extended);
  EXPECT_EQ(abr::on_outage(s, 19776, t0 + 10s), abr::outage_e::extended);
  EXPECT_EQ(s.ref_kbps, 140000);
  EXPECT_EQ(abr::on_clean(s, 11250, kMax, t0 + 11100ms), 11250);
  EXPECT_EQ(abr::on_clean(s, 11250, kMax, t0 + 11600ms), 140000);
}

TEST_F(AbrOutageTest, NothingToRestoreDoesNotConsumeTheRestore) {
  // client LOSS 先到（ABR 還沒降碼）→ 乾淨視窗時 target 本來就是 140000：不算拉回
  abr::on_outage(s, 140000, t0);
  EXPECT_EQ(abr::on_clean(s, 140000, kMax, t0 + 1200ms), 140000);
  EXPECT_EQ(abr::on_clean(s, 140000, kMax, t0 + 1700ms), 140000);
  // 同一次斷訊的 ABR ≥30% 視窗晚到：仍是延長，不可被當成「拉回後又斷」而打折
  EXPECT_EQ(abr::on_outage(s, 140000, t0 + 2s), abr::outage_e::extended);
  EXPECT_EQ(s.ref_kbps, 140000);
  EXPECT_EQ(abr::on_clean(s, 11250, kMax, t0 + 3100ms), 11250);
  EXPECT_EQ(abr::on_clean(s, 11250, kMax, t0 + 3600ms), 140000);
}

TEST_F(AbrOutageTest, OutageRightAfterRestoreAtHighBitrateLowersTheReference) {
  abr::on_outage(s, 140000, t0);
  abr::on_clean(s, 11250, kMax, t0 + 1000ms);
  ASSERT_EQ(abr::on_clean(s, 11250, kMax, t0 + 1500ms), 140000);
  // 這次事件只在高位元率時斷過，拉回 1.5 s 後又斷 → 拉回值可能太高，打 75 折
  EXPECT_EQ(abr::on_outage(s, 140000, t0 + 3s), abr::outage_e::lowered);
  EXPECT_EQ(s.ref_kbps, 105000);
  abr::on_clean(s, 11250, kMax, t0 + 4s);
  EXPECT_EQ(abr::on_clean(s, 11250, kMax, t0 + 4500ms), 105000);
}

TEST_F(AbrOutageTest, FlappingLinkIsNotLowered) {
  abr::on_outage(s, 140000, t0);
  // 已經降到 39 Mbps（≤ 參考值一半）還在斷：連線閃斷，與位元率無關
  abr::on_outage(s, 39000, t0 + 500ms);
  abr::on_clean(s, 11250, kMax, t0 + 1600ms);
  ASSERT_EQ(abr::on_clean(s, 11250, kMax, t0 + 2s), 140000);
  EXPECT_EQ(abr::on_outage(s, 140000, t0 + 3s), abr::outage_e::extended);
  EXPECT_EQ(s.ref_kbps, 140000);
  // 不打折，而且可以再拉回一次
  abr::on_clean(s, 11250, kMax, t0 + 4100ms);
  EXPECT_EQ(abr::on_clean(s, 11250, kMax, t0 + 4600ms), 140000);
}

TEST_F(AbrOutageTest, OutageLongAfterRestoreIsANewEpisode) {
  abr::on_outage(s, 140000, t0);
  abr::on_clean(s, 11250, kMax, t0 + 1000ms);
  ASSERT_EQ(abr::on_clean(s, 11250, kMax, t0 + 1500ms), 140000);
  EXPECT_EQ(abr::on_outage(s, 150000, t0 + 11s), abr::outage_e::started);
  EXPECT_EQ(s.ref_kbps, 150000);
}

TEST_F(AbrOutageTest, EpisodeTimesOut) {
  abr::on_outage(s, 140000, t0);
  EXPECT_EQ(abr::on_clean(s, 60000, kMax, t0 + 31s), 60000);
  EXPECT_EQ(s.ref_kbps, 0);
}

TEST_F(AbrOutageTest, RespectsTheCapAndNeverLowersTarget) {
  abr::on_outage(s, 140000, t0);
  abr::on_clean(s, 11250, 120000, t0 + 1000ms);
  // 上限（FEC 升高後扣掉份額）比參考值低：拉到上限
  EXPECT_EQ(abr::on_clean(s, 11250, 120000, t0 + 1500ms), 120000);
  abr::outage_state_t s2;
  abr::on_outage(s2, 60000, t0);
  abr::on_clean(s2, 80000, kMax, t0 + 1000ms);
  // target 已經比參考值高：不往下拉
  EXPECT_EQ(abr::on_clean(s2, 80000, kMax, t0 + 1500ms), 80000);
}

TEST_F(AbrOutageTest, ClientWithLatchMustShowVideoProgress) {
  abr::on_progress(s, 100, t0 - 100ms);
  abr::on_outage(s, 140000, t0);
  // 有 LATCH 的 client，幀號一直停在 100：影像沒在送達，不拉回
  abr::on_progress(s, 100, t0 + 900ms);
  EXPECT_EQ(abr::on_clean(s, 11250, kMax, t0 + 1000ms), 11250);
  EXPECT_EQ(abr::on_clean(s, 11250, kMax, t0 + 1500ms), 11250);
  // 幀號前進了 → 下一個乾淨視窗拉回
  abr::on_progress(s, 160, t0 + 1600ms);
  EXPECT_EQ(abr::on_clean(s, 11250, kMax, t0 + 1700ms), 140000);
}

TEST_F(AbrOutageTest, RecentCutBeforeTheOutageSignalIsTheReference) {
  // 稀釋過的視窗（< 30%）先把 140 砍成 70，client 的斷訊 LOSS 0.5 s 後才到
  abr::note_cut(s, 140000, t0);
  EXPECT_EQ(abr::on_outage(s, 70000, t0 + 500ms), abr::outage_e::started);
  EXPECT_EQ(s.ref_kbps, 140000);
  // 太久以前的降碼不算
  abr::outage_state_t s2;
  abr::note_cut(s2, 140000, t0);
  abr::on_outage(s2, 70000, t0 + 3s);
  EXPECT_EQ(s2.ref_kbps, 70000);
}

TEST_F(AbrOutageTest, ResendOnlyExtends) {
  // 沒有事件時，重送的 LOSS 什麼都不做
  EXPECT_EQ(abr::on_outage(s, 140000, t0, true), abr::outage_e::extended);
  EXPECT_EQ(s.ref_kbps, 0);
  abr::on_outage(s, 140000, t0);
  abr::on_clean(s, 11250, kMax, t0 + 1000ms);
  ASSERT_EQ(abr::on_clean(s, 11250, kMax, t0 + 1500ms), 140000);
  // 拉回後重送：不打折、不開新事件
  EXPECT_EQ(abr::on_outage(s, 140000, t0 + 2s, true), abr::outage_e::extended);
  EXPECT_EQ(s.ref_kbps, 140000);
}

TEST_F(AbrOutageTest, NoEpisodeNoChange) {
  EXPECT_EQ(abr::on_clean(s, 50000, kMax, t0), 50000);
  EXPECT_EQ(s.ref_kbps, 0);
}
