/**
 * @file tests/unit/test_abr_congestion.cpp
 * @brief §P2 壅塞感知 ABR（2D，第 1 期）：壅塞等級、緊急降碼冷卻、塞車點記憶、IDR 間隔提示的單元測試。
 *
 * 獨立編譯：scripts/ut/ut_build.cmd test_abr_congestion（只依賴標頭 src/abr_congestion.h）。
 */
#include "../tests_common.h"

#include <src/abr_congestion.h>

namespace ac = abr_cong;
using namespace std::chrono_literals;

namespace {
  struct AbrCongestionTest: testing::Test {
    // 故意用很小的起點：所有比較都不可以因為時間起點小而溢位或誤判（§S.19 那一類）
    ac::steady::time_point t0 = ac::steady::time_point {} + 10ms;
    ac::congestion_t c;

    static ac::cong_in_t in(uint32_t rtt, uint32_t base, double jitter = 5.0, bool hold = false, bool trend_over = false, bool severe = false) {
      ac::cong_in_t i;
      i.sampled = true;
      i.rtt_ms = rtt;
      i.base_ms = base;
      i.jitter_ms = jitter;
      i.hold = hold;
      i.trend_over = trend_over;
      i.severe = severe;
      return i;
    }
  };
}  // namespace

// 1. RTT 正常：永遠 L0、沒有動作、沒有提示、IDR 請求不降碼（回歸安全的底線）
TEST_F(AbrCongestionTest, CleanLinkNeverLeavesL0) {
  for (int i = 0; i < 200; ++i) {
    const auto o = c.update(in(40 + (i % 7), 38), t0 + i * 500ms);
    EXPECT_EQ(o.level, ac::level_e::clean);
    EXPECT_EQ(o.action, ac::action_e::none);
    EXPECT_FALSE(c.idr_request_wants_cut(t0 + i * 500ms + 100ms));
  }
  EXPECT_EQ(c.idr_gap_hint(), 0);
  EXPECT_EQ(c.ramp_cap_kbps(t0 + 100s), 0);
}

// 2. 基線 1 ms 的區網：RTT 跳到幾 ms 不算壅塞
TEST_F(AbrCongestionTest, LanBaselineDoesNotMisfire) {
  for (int i = 0; i < 20; ++i) {
    const auto o = c.update(in(i % 2 ? 9 : 1, 1, 2.0), t0 + i * 500ms);
    EXPECT_EQ(o.level, ac::level_e::clean);
    EXPECT_EQ(o.action, ac::action_e::none);
  }
}

// 3. 沒有 RTT 樣本：RTT 欄位再大也固定 L0，已經在高等級時也立刻歸零
TEST_F(AbrCongestionTest, NoSampleIsAlwaysL0) {
  c.update(in(900, 40, 5.0, true), t0);
  auto o = c.update(in(900, 40, 5.0, true), t0 + 500ms);
  EXPECT_EQ(o.level, ac::level_e::deep);

  ac::cong_in_t none;
  none.rtt_ms = 5000;
  none.hold = true;
  none.severe = true;
  o = c.update(none, t0 + 1000ms);
  EXPECT_EQ(o.level, ac::level_e::clean);
  EXPECT_EQ(o.action, ac::action_e::none);
  EXPECT_EQ(c.idr_gap_hint(), 0);
  EXPECT_FALSE(c.idr_request_wants_cut(t0 + 1100ms));
}

// 4. L1：升立刻生效，降要連續 1 秒都在較低等級
TEST_F(AbrCongestionTest, HoldLevelEntersAtOnceAndLeavesAfterHold) {
  auto o = c.update(in(120, 40, 5.0, true), t0);
  EXPECT_EQ(o.prev_level, ac::level_e::clean);
  EXPECT_EQ(o.level, ac::level_e::hold);
  EXPECT_EQ(o.action, ac::action_e::none);
  EXPECT_EQ(c.idr_gap_hint(), 0);  // L1 不改 IDR 的間隔（Wi-Fi 一陣丟包時 §IDR-FAST 要照常快速補送）

  o = c.update(in(45, 40), t0 + 500ms);
  EXPECT_EQ(o.level, ac::level_e::hold);  // 還在遲滯時間內
  // 中途又成立一次：重新起算
  o = c.update(in(120, 40, 5.0, true), t0 + 1000ms);
  EXPECT_EQ(o.level, ac::level_e::hold);
  o = c.update(in(45, 40), t0 + 1500ms);
  EXPECT_EQ(o.level, ac::level_e::hold);
  o = c.update(in(45, 40), t0 + 2000ms);
  EXPECT_EQ(o.level, ac::level_e::hold);
  o = c.update(in(45, 40), t0 + 2500ms);
  EXPECT_EQ(o.level, ac::level_e::clean);
  EXPECT_EQ(c.idr_gap_hint(), 0);
}

// 5. L2 的分級降幅與 2 秒冷卻
TEST_F(AbrCongestionTest, CutLevelGradesAndCooldown) {
  auto o = c.update(in(150, 40, 5.0, true, true), t0);
  EXPECT_EQ(o.level, ac::level_e::cut);
  EXPECT_EQ(o.action, ac::action_e::cut_trend);
  EXPECT_EQ(ac::action_pct(o.action), 85);
  c.commit_cut(o.action, 20000, t0);

  // 冷卻內：條件還在也不建議
  o = c.update(in(300, 40, 5.0, true, true, true), t0 + 1500ms);
  EXPECT_EQ(o.level, ac::level_e::cut);
  EXPECT_EQ(o.action, ac::action_e::none);

  // 冷卻過了：嚴重（4 倍且多 200 ms 連 2 輪）優先於趨勢，×0.70
  o = c.update(in(300, 40, 5.0, true, true, true), t0 + 2000ms);
  EXPECT_EQ(o.action, ac::action_e::cut_severe);
  EXPECT_EQ(ac::action_pct(o.action), 70);

  // 沒有 commit（例如被別的分支蓋掉）就不起算冷卻
  o = c.update(in(300, 40, 5.0, true, false, true), t0 + 2500ms);
  EXPECT_EQ(o.action, ac::action_e::cut_severe);
}

// 6. L3：高出基線 500 ms 要連 2 輪才進；緊急降碼 3 秒冷卻
TEST_F(AbrCongestionTest, DeepNeedsTwoRoundsAndEmergencyCooldown) {
  auto o = c.update(in(700, 40, 5.0, true), t0);
  EXPECT_EQ(o.level, ac::level_e::hold);  // 第一輪還不算
  EXPECT_EQ(o.action, ac::action_e::none);

  o = c.update(in(700, 40, 5.0, true), t0 + 500ms);
  EXPECT_EQ(o.level, ac::level_e::deep);
  EXPECT_EQ(o.action, ac::action_e::cut_emergency);
  EXPECT_EQ(ac::action_pct(o.action), 50);
  c.commit_cut(o.action, 20000, t0 + 500ms);

  // 3 秒內不再緊急降碼；一般降碼的 2 秒冷卻也從這一刀起算
  o = c.update(in(700, 40, 5.0, true, false, true), t0 + 1500ms);
  EXPECT_EQ(o.action, ac::action_e::none);
  o = c.update(in(700, 40, 5.0, true, false, true), t0 + 2500ms);
  EXPECT_EQ(o.action, ac::action_e::cut_severe);  // 2 秒到了、3 秒還沒到：只能是一般的那一級
  o = c.update(in(700, 40, 5.0, true), t0 + 3500ms);
  EXPECT_EQ(o.action, ac::action_e::cut_emergency);
}

// 7. 中斷一輪就重新數（不是累計）
TEST_F(AbrCongestionTest, DeepRoundsMustBeConsecutive) {
  c.update(in(700, 40), t0);
  c.update(in(100, 40), t0 + 500ms);
  auto o = c.update(in(700, 40), t0 + 1000ms);
  EXPECT_NE(o.level, ac::level_e::deep);
  EXPECT_EQ(o.action, ac::action_e::none);
  // 兩輪擠在一起（別的入口插進來）也不算：要滿 k_deep_sustain
  o = c.update(in(700, 40), t0 + 1100ms);
  EXPECT_NE(o.level, ac::level_e::deep);
  o = c.update(in(700, 40), t0 + 1500ms);
  EXPECT_EQ(o.level, ac::level_e::deep);
}

// 8. 抖動大的路徑門檻跟著放寬（8 倍抖動）
TEST_F(AbrCongestionTest, DeepThresholdScalesWithJitter) {
  EXPECT_DOUBLE_EQ(ac::deep_enter_threshold_ms(10.0), 500.0);
  EXPECT_DOUBLE_EQ(ac::deep_enter_threshold_ms(100.0), 800.0);
  EXPECT_DOUBLE_EQ(ac::deep_enter_threshold_ms(-3.0), 500.0);
  for (int i = 0; i < 6; ++i) {
    const auto o = c.update(in(640, 40, 100.0), t0 + i * 500ms);  // 高出 600 < 800
    EXPECT_EQ(o.level, ac::level_e::clean);
  }
}

// 9. L3 的遲滯：500 進、300 出；出來先落在 L2，再過 1 秒才往下
TEST_F(AbrCongestionTest, DeepHysteresisAndStepDown) {
  c.update(in(700, 40), t0);
  auto o = c.update(in(700, 40), t0 + 500ms);
  ASSERT_EQ(o.level, ac::level_e::deep);

  // 遲滯帶內（高出 400）：留在 L3，但不再建議緊急降碼
  for (int i = 2; i < 12; ++i) {
    o = c.update(in(440, 40), t0 + i * 500ms);
    EXPECT_EQ(o.level, ac::level_e::deep);
    EXPECT_EQ(o.action, ac::action_e::none);
  }
  // 低於 300：第一輪還在、第二輪出來到 L2
  o = c.update(in(240, 40), t0 + 6000ms);
  EXPECT_EQ(o.level, ac::level_e::deep);
  o = c.update(in(240, 40), t0 + 6500ms);
  EXPECT_EQ(o.level, ac::level_e::cut);
  o = c.update(in(60, 40), t0 + 7000ms);
  EXPECT_EQ(o.level, ac::level_e::cut);
  o = c.update(in(60, 40), t0 + 7500ms);
  EXPECT_EQ(o.level, ac::level_e::clean);
}

// 10. IDR 請求：L0、L1 不降碼；L2 以上降一次、3 秒內不再降；等級太舊不降
TEST_F(AbrCongestionTest, IdrRequestCutNeedsCongestionAndCooldown) {
  EXPECT_FALSE(c.idr_request_wants_cut(t0));  // 還沒有任何一輪
  c.update(in(42, 40), t0);
  EXPECT_FALSE(c.idr_request_wants_cut(t0 + 10ms));  // L0：乾淨網路上掉一張

  c.update(in(130, 40, 5.0, true), t0 + 500ms);
  ASSERT_EQ(c.level(), ac::level_e::hold);
  EXPECT_FALSE(c.idr_request_wants_cut(t0 + 520ms));  // L1：只是暫停回升，不砍
  c.update(in(130, 40, 5.0, true, true), t0 + 550ms);
  ASSERT_EQ(c.level(), ac::level_e::cut);
  EXPECT_TRUE(c.idr_request_wants_cut(t0 + 600ms));
  c.commit_cut(ac::action_e::cut_emergency, 20000, t0 + 600ms);
  EXPECT_EQ(c.level(), ac::level_e::deep);  // IDR 閘門改用長間隔

  // 成對的第二個請求、之後 watchdog 每秒再要：3 秒內都不再降
  EXPECT_FALSE(c.idr_request_wants_cut(t0 + 620ms));
  c.update(in(900, 40, 5.0, true), t0 + 1000ms);
  EXPECT_FALSE(c.idr_request_wants_cut(t0 + 1600ms));
  c.update(in(900, 40, 5.0, true), t0 + 3500ms);
  EXPECT_FALSE(c.idr_request_wants_cut(t0 + 3599ms));
  EXPECT_TRUE(c.idr_request_wants_cut(t0 + 3600ms));

  // ABR 那一側停擺超過 2 秒：等級不可信，不降
  EXPECT_FALSE(c.idr_request_wants_cut(t0 + 3500ms + 2001ms));
}

// 11. IDR 請求把等級推到 L3 之後，RTT 其實不高時照退出規則出來
TEST_F(AbrCongestionTest, IdrTriggeredDeepLeavesByExitRule) {
  c.update(in(130, 40, 5.0, true), t0);
  c.commit_cut(ac::action_e::cut_emergency, 20000, t0 + 100ms);
  auto o = c.update(in(130, 40, 5.0, true), t0 + 500ms);
  EXPECT_EQ(o.level, ac::level_e::deep);
  EXPECT_EQ(o.action, ac::action_e::none);  // 不是 RTT 自己到深度門檻，不會自己再砍
  o = c.update(in(130, 40, 5.0, true), t0 + 1000ms);
  EXPECT_EQ(o.level, ac::level_e::cut);
  o = c.update(in(130, 40, 5.0, true), t0 + 1500ms);
  o = c.update(in(130, 40, 5.0, true), t0 + 2000ms);
  EXPECT_EQ(o.level, ac::level_e::hold);
}

// 12. 塞車點：5 秒內的後續刀不覆寫、上限是 9 成、30 秒沒有再塞就清掉
TEST_F(AbrCongestionTest, CongestionPointMemory) {
  EXPECT_EQ(c.ramp_cap_kbps(t0), 0);
  EXPECT_TRUE(c.commit_cut(ac::action_e::cut_trend, 20000, t0));
  EXPECT_EQ(c.congested_kbps(), 20000);
  EXPECT_EQ(c.ramp_cap_kbps(t0 + 1s), 18000);

  EXPECT_FALSE(c.commit_cut(ac::action_e::cut_trend, 17000, t0 + 3s));  // 同一次塞車
  EXPECT_EQ(c.congested_kbps(), 20000);
  EXPECT_TRUE(c.commit_cut(ac::action_e::cut_severe, 14000, t0 + 6s));  // 5 秒之後：新的塞車點
  EXPECT_EQ(c.ramp_cap_kbps(t0 + 7s), 12600);

  // 還在 L2 以上的每一輪都把 30 秒往後延
  auto o = c.update(in(300, 40, 5.0, true, true), t0 + 20s);
  EXPECT_FALSE(o.point_expired);
  EXPECT_EQ(c.ramp_cap_kbps(t0 + 45s), 12600);
  // 之後一直乾淨：最後一次看到塞車的 30 秒後清掉
  o = c.update(in(42, 40), t0 + 49s);
  EXPECT_FALSE(o.point_expired);
  o = c.update(in(42, 40), t0 + 50s);
  EXPECT_TRUE(o.point_expired);
  EXPECT_EQ(o.expired_kbps, 14000);
  EXPECT_EQ(c.ramp_cap_kbps(t0 + 50s), 0);
  EXPECT_EQ(c.congested_kbps(), 0);
  // none 不記帳
  EXPECT_FALSE(c.commit_cut(ac::action_e::none, 9000, t0 + 51s));
  EXPECT_EQ(c.congested_kbps(), 0);
}

// 13. IDR 間隔提示的數值
TEST(AbrCongestionIdr, GapHintByLevel) {
  EXPECT_EQ(ac::idr_gap_hint_ms(ac::level_e::clean, 4000), 0);
  EXPECT_EQ(ac::idr_gap_hint_ms(ac::level_e::hold, 4000), 0);
  EXPECT_EQ(ac::idr_gap_hint_ms(ac::level_e::cut, 4000), 1500);
  EXPECT_EQ(ac::idr_gap_hint_ms(ac::level_e::deep, 100), 1500);  // 650 → 下限
  EXPECT_EQ(ac::idr_gap_hint_ms(ac::level_e::deep, 1000), 2000);
  EXPECT_EQ(ac::idr_gap_hint_ms(ac::level_e::deep, 4000), 6000);  // 6500 → 上限
  EXPECT_EQ(ac::idr_gap_hint_ms(ac::level_e::deep, 0xFFFFFFFFu), 6000);  // 不溢位
}

// 14. 閘門間隔：沒有提示時就是 §IDR-FAST 原本的式子；有提示時不走快速間隔、不短於冷卻、不超過 6 秒
TEST(AbrCongestionIdr, GateGap) {
  // 桌面
  EXPECT_EQ(ac::idr_gate_gap_ms(true, 1500, 250, 0), 250);
  EXPECT_EQ(ac::idr_gate_gap_ms(false, 1500, 250, 0), 1500);
  EXPECT_EQ(ac::idr_gate_gap_ms(true, 1500, 250, 1500), 1500);  // 平常狀態遇到壅塞：快速路徑關掉
  EXPECT_EQ(ac::idr_gate_gap_ms(true, 1500, 250, 2300), 2300);
  EXPECT_EQ(ac::idr_gate_gap_ms(false, 1500, 250, 6000), 6000);
  EXPECT_EQ(ac::idr_gate_gap_ms(false, 1500, 250, 60000), 6000);  // 任何情況最長 6 秒一定放行
  EXPECT_EQ(ac::idr_gate_gap_ms(true, 1500, 250, 800), 1500);
  // VR：calm 恆為 false、提示恆為 0 → 300
  EXPECT_EQ(ac::idr_gate_gap_ms(false, 300, 250, 0), 300);
  EXPECT_EQ(ac::idr_gate_gap_ms(true, 1500, 250, -5), 250);
}

// 15. 提示的包裝：值與時刻不撕裂、2 秒過期、時刻不合理當成沒有
TEST(AbrCongestionIdr, HintPackExpiry) {
  const int64_t at = 123'456'789'000;
  EXPECT_EQ(ac::idr_hint_pack(0, at), 0);
  EXPECT_EQ(ac::idr_hint_unpack(0, at), 0);
  const auto p = ac::idr_hint_pack(2300, at);
  EXPECT_EQ(ac::idr_hint_unpack(p, at), 2300);
  EXPECT_EQ(ac::idr_hint_unpack(p, at + 2'000'000), 2300);
  EXPECT_EQ(ac::idr_hint_unpack(p, at + 2'000'001), 0);  // 過期
  EXPECT_EQ(ac::idr_hint_unpack(p, at - 1), 0);
  EXPECT_EQ(ac::idr_hint_unpack(ac::idr_hint_pack(6000, at), at + 1), 6000);
  EXPECT_EQ(ac::idr_hint_unpack(ac::idr_hint_pack(99999, at), at), 8191);  // 超出欄位就夾住（閘門再夾到 6 秒）
  EXPECT_EQ(ac::idr_hint_pack(1500, 0), 0);

  // 全域 atomic 的存取：寫入、讀出、過期、清掉
  const auto now = ac::steady::now();
  ac::idr_hint_store(1500, now);
  EXPECT_EQ(ac::idr_hint_load(now + 1s), 1500);
  EXPECT_EQ(ac::idr_hint_load(now + 3s), 0);
  ac::idr_hint_store(0, now);
  EXPECT_EQ(ac::idr_hint_load(now), 0);
}

// 16. 壅塞中提示 6000：距上一張滿 6 秒必放行；提示消失後立刻回到原本的間隔（延後中的請求跟著補送）
TEST(AbrCongestionIdr, DeferredRequestFollowsHint) {
  const auto t0 = ac::steady::time_point {} + 1h;
  const auto last_emit = t0;
  const auto due = [&](ac::steady::time_point now, bool calm, int hint) {
    return now >= last_emit + std::chrono::milliseconds {ac::idr_gate_gap_ms(calm, 1500, 250, hint)};
  };
  EXPECT_FALSE(due(t0 + 300ms, true, 6000));
  EXPECT_FALSE(due(t0 + 5999ms, true, 6000));
  EXPECT_TRUE(due(t0 + 6000ms, true, 6000));
  // 等到一半提示過期（ABR 那一側停了）：照平常規則，250 ms 早就過了
  EXPECT_TRUE(due(t0 + 2100ms, true, 0));
  // 丟包連發狀態下提示過期：回到 1500 ms
  EXPECT_FALSE(due(t0 + 1400ms, false, 0));
  EXPECT_TRUE(due(t0 + 1500ms, false, 0));
}

// §P2：深度壅塞時上一刀之後 RTT 已經降了一成以上就不再追加一刀（管線在排空）
TEST_F(AbrCongestionTest, DeepDoesNotCutAgainWhileRttIsFalling) {
  c.update(in(1000, 40), t0);
  auto o = c.update(in(1000, 40), t0 + 500ms);
  ASSERT_EQ(o.action, ac::action_e::cut_emergency);
  c.commit_cut(o.action, 20000, t0 + 500ms);
  // 3 秒冷卻過了、仍在深度壅塞，但 RTT 從 1000 降到 800：不砍
  o = c.update(in(800, 40), t0 + 3600ms);
  o = c.update(in(800, 40), t0 + 4100ms);
  EXPECT_EQ(o.level, ac::level_e::deep);
  EXPECT_NE(o.action, ac::action_e::cut_emergency);
  // RTT 又回到 1000：再砍
  o = c.update(in(1000, 40), t0 + 4600ms);
  o = c.update(in(1000, 40), t0 + 5100ms);
  EXPECT_EQ(o.action, ac::action_e::cut_emergency);
}
