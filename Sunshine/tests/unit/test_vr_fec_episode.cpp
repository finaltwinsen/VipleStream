/**
 * @file tests/unit/test_vr_fec_episode.cpp
 * @brief VipleStream 2.0：FEC 升高期間的實效記帳（src/vr/vr_fec_spread.h 的 parity_for、account_status、episode_t）。
 */
#include "../tests_common.h"

#include <src/vr/vr_fec_spread.h>

namespace fs = vr::fecspread;
using namespace std::chrono_literals;

TEST(VrFecEpisode, ParityForMatchesPacketizer) {
  // stream.cpp 的 fec::encode：parity = ceil(data × pct / 100)，不到 min_parity 時補到 min_parity（總數不超過 255）
  EXPECT_EQ(fs::parity_for(72, 10, 0), 8u);
  EXPECT_EQ(fs::parity_for(72, 20, 0), 15u);
  EXPECT_EQ(fs::parity_for(72, 40, 0), 29u);
  EXPECT_EQ(fs::parity_for(6, 10, 0), 1u);
  EXPECT_EQ(fs::parity_for(6, 10, 2), 2u);  // 補到 client 要求的最少數量
  EXPECT_EQ(fs::parity_for(72, 10, 2), 8u);  // 已經夠多：不動
  EXPECT_EQ(fs::parity_for(254, 1, 5), 1u);  // 只剩 1 個位置
  EXPECT_EQ(fs::parity_for(255, 1, 5), 0u);  // 沒有位置
  EXPECT_EQ(fs::parity_for(0, 10, 2), 0u);
  EXPECT_EQ(fs::parity_for(72, 0, 2), 0u);  // FEC 關掉時不補
}

TEST(VrFecEpisode, ElevatedUseIsRecoveryBeyondBaseParity) {
  // 72 個 data、基準 10%（8 個 parity）、這個 block 以 40% 送（29 個 parity）、下一級仍是 40%（已在上限）
  auto use = fs::account_status(72, 29, 60, 20, 10, 40, 0);  // 缺 12 個：基準救不回
  EXPECT_EQ(use.st, fs::status_e::recovered);
  EXPECT_TRUE(use.elevated_use);
  EXPECT_FALSE(use.near_miss);

  use = fs::account_status(72, 29, 64, 20, 10, 40, 0);  // 缺 8 個：基準的 parity 全到就救得回，不算
  EXPECT_EQ(use.st, fs::status_e::recovered);
  EXPECT_FALSE(use.elevated_use);

  use = fs::account_status(72, 29, 63, 20, 10, 40, 0);  // 缺 9 個
  EXPECT_TRUE(use.elevated_use);
}

TEST(VrFecEpisode, NearMissIsFailureOneStepWouldFix) {
  // 72 個 data、這個 block 以 20% 送（15 個 parity）、下一級 30%（22 個 parity，多 7 個）
  auto use = fs::account_status(72, 15, 55, 12, 10, 30, 0);  // 一共 67，差 5 個
  EXPECT_EQ(use.st, fs::status_e::failed);
  EXPECT_TRUE(use.near_miss);
  EXPECT_FALSE(use.elevated_use);

  use = fs::account_status(72, 15, 53, 12, 10, 30, 0);  // 差 7 個：剛好
  EXPECT_TRUE(use.near_miss);

  use = fs::account_status(72, 15, 52, 12, 10, 30, 0);  // 差 8 個：多一級也不夠
  EXPECT_EQ(use.st, fs::status_e::failed);
  EXPECT_FALSE(use.near_miss);

  // 已經在上限（下一級＝目前這一級）：沒有下一級可升
  use = fs::account_status(72, 29, 40, 29, 10, 40, 0);
  EXPECT_EQ(use.st, fs::status_e::failed);
  EXPECT_FALSE(use.near_miss);
}

TEST(VrFecEpisode, GapAndNoneCountNothing) {
  auto use = fs::account_status(72, 15, 0, 0, 10, 30, 0);
  EXPECT_EQ(use.st, fs::status_e::gap);
  EXPECT_FALSE(use.elevated_use);
  EXPECT_FALSE(use.near_miss);

  use = fs::account_status(72, 15, 72, 3, 10, 30, 0);
  EXPECT_EQ(use.st, fs::status_e::none);
  EXPECT_FALSE(use.elevated_use);
  EXPECT_FALSE(use.near_miss);
}

TEST(VrFecEpisode, AccountsOnlyWhileElevated) {
  using clock = fs::episode_t::clock;
  const clock::time_point t0 {1000s};
  const fs::status_use_t elevated {fs::status_e::recovered, true, false};
  const fs::status_use_t plain_recovered {fs::status_e::recovered, false, false};
  const fs::status_use_t near_miss {fs::status_e::failed, false, true};
  const fs::status_use_t plain_failed {fs::status_e::failed, false, false};

  fs::episode_t ep;
  // 還在基準：什麼都不記、變動也不開始
  ep.on_status(elevated, t0);
  ep.on_nack_hold();
  EXPECT_TRUE(ep.on_fec_changed(10, 10, t0).empty());
  EXPECT_FALSE(ep.active);
  EXPECT_EQ(ep.recovered, 0u);
  EXPECT_EQ(ep.nack_holds, 0u);

  // 第一次升高：開始，這一次沒有東西可報
  EXPECT_TRUE(ep.on_fec_changed(20, 10, t0).empty());
  EXPECT_TRUE(ep.active);
  EXPECT_EQ(ep.peak_pct, 20);
  EXPECT_EQ(ep.summary(t0 + 3s), "peak=20% dur=3.0s elevatedUse=0/0 fixableFailed=0/0 lastUse=- nackHolds=0");

  ep.on_status(elevated, t0 + 1s);
  ep.on_status(plain_recovered, t0 + 1s);
  ep.on_status(near_miss, t0 + 1s);
  ep.on_status(plain_failed, t0 + 1s);
  ep.on_status({fs::status_e::gap, false, false}, t0 + 1s);
  ep.on_nack_hold();
  ep.on_nack_hold();

  EXPECT_EQ(ep.on_fec_changed(30, 10, t0 + 2s), "fec=30% peak=30% dur=2.0s elevatedUse=1/2 fixableFailed=1/2 lastUse=1.0s nackHolds=2");
  EXPECT_EQ(ep.summary(t0 + 12s), "peak=30% dur=12.0s elevatedUse=1/2 fixableFailed=1/2 lastUse=11.0s nackHolds=2");

  // 回降：最高值留著
  EXPECT_EQ(ep.on_fec_changed(25, 10, t0 + 40s), "fec=25% peak=30% dur=40.0s elevatedUse=1/2 fixableFailed=1/2 lastUse=39.0s nackHolds=2");
  EXPECT_TRUE(ep.active);

  // 降回基準：結算後清空
  EXPECT_EQ(ep.on_fec_changed(10, 10, t0 + 60s),
            "fec=10% peak=30% dur=60.0s elevatedUse=1/2 fixableFailed=1/2 lastUse=59.0s nackHolds=2 (ended)");
  EXPECT_FALSE(ep.active);
  EXPECT_EQ(ep.recovered, 0u);
  EXPECT_EQ(ep.elevated_use, 0u);
  EXPECT_EQ(ep.nack_holds, 0u);

  // 下一段從零開始
  EXPECT_TRUE(ep.on_fec_changed(20, 10, t0 + 100s).empty());
  EXPECT_TRUE(ep.active);
  EXPECT_EQ(ep.summary(t0 + 101s), "peak=20% dur=1.0s elevatedUse=0/0 fixableFailed=0/0 lastUse=- nackHolds=0");
}
