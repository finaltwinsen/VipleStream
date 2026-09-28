/**
 * @file tests/unit/test_vr_clock.cpp
 * @brief Test src/vr/vr_clock.*（M1b S1-12：時鐘對映與 pacing 的合成情境；與 `--vr-selftest` T6 同一組）。
 */
#include "../tests_common.h"

#include <src/vr/vr_clock.h>

TEST(VrClockTest, SelftestScenarios) {
  const auto cases = vr::clk::run_selftests();
  ASSERT_FALSE(cases.empty());
  for (const auto &c : cases) {
    EXPECT_TRUE(c.pass) << c.name << " " << c.detail;
  }
}

TEST(VrClockTest, TickConversionSymmetric) {
  constexpr int64_t f = 10'000'000;
  EXPECT_EQ(vr::clk::ticks_to_ns(f, f), 1'000'000'000);
  EXPECT_EQ(vr::clk::ticks_to_ns(-f, f), -1'000'000'000);
  EXPECT_EQ(vr::clk::ns_to_ticks(1'000'000'000, f), f);
  EXPECT_EQ(vr::clk::ns_to_ticks(-1'000'000'000, f), -f);
}
