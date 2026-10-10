/**
 * @file tests/unit/test_vr_fec_spread.cpp
 * @brief VipleStream 2.0：client 回報的 FEC status 的分類與 10 秒計數（src/vr/vr_fec_spread.h）。
 */
#include "../tests_common.h"

#include <src/vr/vr_fec_spread.h>

namespace fs = vr::fecspread;

TEST(VrFecStatus, Classify) {
  // (data, parity, 收到的 data, 收到的 parity)
  EXPECT_EQ(fs::classify_status(6, 6, 2, 4), fs::status_e::recovered);  // 缺 4 個 data，parity 剛好補齊
  EXPECT_EQ(fs::classify_status(6, 3, 5, 1), fs::status_e::recovered);
  EXPECT_EQ(fs::classify_status(6, 6, 2, 3), fs::status_e::failed);  // 一共 5 個，不夠還原
  EXPECT_EQ(fs::classify_status(6, 3, 0, 3), fs::status_e::failed);
  EXPECT_EQ(fs::classify_status(6, 3, 4, 1), fs::status_e::failed);
  EXPECT_EQ(fs::classify_status(6, 3, 0, 0), fs::status_e::gap);  // 一個都沒收到：client 依幀號跳號合成的回報
  EXPECT_EQ(fs::classify_status(6, 6, 6, 0), fs::status_e::none);  // data 全到，不該回報
}

TEST(VrFecStatus, ImplausibleFieldsAreIgnored) {
  EXPECT_EQ(fs::classify_status(0, 3, 0, 0), fs::status_e::none);  // 沒有 data 的 block
  EXPECT_EQ(fs::classify_status(6, 3, 7, 0), fs::status_e::none);  // 收到的比送的多
  EXPECT_EQ(fs::classify_status(6, 3, 2, 4), fs::status_e::none);
}

TEST(VrFecStatus, WindowCounters) {
  fs::status_stats_t s;
  EXPECT_FALSE(s.any());
  s.add(6, 6, 6, 0);  // none：不算
  EXPECT_FALSE(s.any());

  s.add(6, 6, 2, 4);  // 還原 4 個
  s.add(72, 29, 71, 5);  // 還原 1 個
  s.add(6, 3, 4, 1);  // 放棄
  s.add(6, 3, 0, 0);  // 整幀沒到
  s.add(6, 3, 0, 0);
  EXPECT_TRUE(s.any());
  EXPECT_EQ(s.recovered, 2u);
  EXPECT_EQ(s.recovered_shards, 5u);
  EXPECT_EQ(s.recovered_max, 4u);
  EXPECT_EQ(s.failed, 1u);
  EXPECT_EQ(s.gap, 2u);
  EXPECT_EQ(s.line(), "recovered=2 (shards=5 max=4) failed=1 wholeFrame=2");

  s = {};
  s.add(6, 3, 0, 0);
  EXPECT_TRUE(s.any());  // 只有整幀沒到的也要印
}
