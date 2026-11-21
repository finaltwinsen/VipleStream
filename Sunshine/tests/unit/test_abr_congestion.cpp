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

  // RTT 200 ms：夠長（k_idr_slow_enter_rtt_ms 以上），IDR 請求降碼才會考慮
  c.update(in(200, 40, 5.0, true), t0 + 500ms);
  ASSERT_EQ(c.level(), ac::level_e::hold);
  EXPECT_FALSE(c.idr_request_wants_cut(t0 + 520ms));  // L1：只是暫停回升，不砍
  c.update(in(200, 40, 5.0, true, true), t0 + 550ms);
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

// ── 審查 11-20 的六項 ─────────────────────────────────────────────────────

// R1. RTT 很短時 L2 不提示、IDR 請求也不觸發緊急降碼（§IDR-FAST 的 250 ms 快速補送照常）；RTT 夠長才有
TEST_F(AbrCongestionTest, ShortRttCutLevelKeepsIdrFastPath) {
  auto o = c.update(in(15, 2, 5.0, true, true), t0);
  EXPECT_EQ(o.level, ac::level_e::cut);
  EXPECT_EQ(o.action, ac::action_e::cut_trend);  // 趨勢的 ×0.85 照舊
  EXPECT_FALSE(c.rtt_slow());
  EXPECT_EQ(c.idr_gap_hint(), 0);
  EXPECT_FALSE(c.idr_request_wants_cut(t0 + 50ms));

  // L2 且 RTT 400 ms：提示 1500、IDR 請求會降碼
  o = c.update(in(400, 40, 5.0, true, true), t0 + 500ms);
  EXPECT_EQ(o.level, ac::level_e::cut);
  EXPECT_TRUE(c.rtt_slow());
  EXPECT_EQ(c.idr_gap_hint(), 1500);
  EXPECT_TRUE(c.idr_request_wants_cut(t0 + 550ms));

  // L3 且 RTT 1240 ms：提示 1.5 × 1240 + 500 = 2360
  c.update(in(1240, 40), t0 + 1000ms);
  o = c.update(in(1240, 40), t0 + 1500ms);
  EXPECT_EQ(o.level, ac::level_e::deep);
  EXPECT_EQ(c.idr_gap_hint(), 2360);
}

// R1. 遲滯：150 ms 開、100 ms 以下才關，中間維持原狀
TEST_F(AbrCongestionTest, IdrSlowRttHysteresis) {
  const auto step = [&](uint32_t rtt, int i) {
    c.update(in(rtt, 40, 5.0, true, true), t0 + i * 500ms);  // 趨勢一直過載：等級固定 L2，只看 RTT 的效果
    EXPECT_EQ(c.level(), ac::level_e::cut);
    return c.idr_gap_hint();
  };
  EXPECT_EQ(step(149, 0), 0);  // 還沒到 150
  EXPECT_EQ(step(150, 1), 1500);
  EXPECT_EQ(step(120, 2), 1500);  // 遲滯帶：維持
  EXPECT_EQ(step(100, 3), 1500);
  EXPECT_EQ(step(99, 4), 0);
  EXPECT_EQ(step(120, 5), 0);  // 遲滯帶：維持
  EXPECT_EQ(step(149, 6), 0);
  EXPECT_EQ(step(150, 7), 1500);
  // 沒有樣本：關掉
  c.update(ac::cong_in_t {}, t0 + 4000ms);
  EXPECT_FALSE(c.rtt_slow());
  EXPECT_EQ(c.idr_gap_hint(), 0);
}

// R2. 行程內同時超過一場 session：提示一律 0（寫 0 也會清掉另一場留下的值）
TEST(AbrCongestionIdr, HintDisabledWithMultipleSessions) {
  EXPECT_EQ(ac::idr_hint_for_publish(1500, 0), 1500);
  EXPECT_EQ(ac::idr_hint_for_publish(1500, 1), 1500);
  EXPECT_EQ(ac::idr_hint_for_publish(6000, 1), 6000);
  EXPECT_EQ(ac::idr_hint_for_publish(1500, 2), 0);
  EXPECT_EQ(ac::idr_hint_for_publish(6000, 3), 0);
  EXPECT_EQ(ac::idr_hint_for_publish(0, 1), 0);

  const auto now = ac::steady::now();
  ac::idr_hint_store(ac::idr_hint_for_publish(6000, 1), now);  // 遠端那一場獨自在跑
  EXPECT_EQ(ac::idr_hint_load(now + 100ms), 6000);
  ac::idr_hint_store(ac::idr_hint_for_publish(6000, 2), now + 200ms);  // 第二場加入後的下一次寫入
  EXPECT_EQ(ac::idr_hint_load(now + 300ms), 0);
  // 閘門回到原本的規則
  EXPECT_EQ(ac::idr_gate_gap_ms(true, 1500, 250, ac::idr_hint_load(now + 300ms)), 250);
  ac::idr_hint_store(0, now);
}

// R3. host 自發的 IDR：有壅塞提示才取消延後中的請求；沒有提示時不取消（平常的行為不變）
TEST(AbrCongestionIdr, HostIdrCancelsDeferredOnlyWhenCongested) {
  EXPECT_FALSE(ac::host_idr_cancels_deferred(0));
  EXPECT_FALSE(ac::host_idr_cancels_deferred(-1));
  EXPECT_TRUE(ac::host_idr_cancels_deferred(1500));
  EXPECT_TRUE(ac::host_idr_cancels_deferred(6000));
  // 提示過期（ABR 那一側停了 2 秒）＝沒有提示
  const int64_t at = 5'000'000'000;
  EXPECT_TRUE(ac::host_idr_cancels_deferred(ac::idr_hint_unpack(ac::idr_hint_pack(2360, at), at + 1'000'000)));
  EXPECT_FALSE(ac::host_idr_cancels_deferred(ac::idr_hint_unpack(ac::idr_hint_pack(2360, at), at + 2'000'001)));
}

// R4. 「排空中」的基準只在同一次壅塞事件內有效：離開 L3 之後，RTT 較低的下一次深度壅塞照樣緊急降碼
TEST_F(AbrCongestionTest, DrainingBaselineEndsWithTheEpisode) {
  c.update(in(3000, 30), t0);
  auto o = c.update(in(3000, 30), t0 + 500ms);
  ASSERT_EQ(o.action, ac::action_e::cut_emergency);
  c.commit_cut(o.action, 20000, t0 + 500ms, 10000);

  // 回到基線：兩輪後離開 L3，再 1 秒回到 L0
  o = c.update(in(35, 30), t0 + 1000ms);
  EXPECT_EQ(o.level, ac::level_e::deep);
  o = c.update(in(35, 30), t0 + 1500ms);
  EXPECT_EQ(o.level, ac::level_e::cut);
  c.update(in(35, 30), t0 + 2000ms);
  o = c.update(in(35, 30), t0 + 2500ms);
  ASSERT_EQ(o.level, ac::level_e::clean);

  // 一分鐘後再塞一次，RTT 只有 900 ms（不到上次 3000 的九成）：不可以被當成還在排空
  o = c.update(in(900, 30), t0 + 60s);
  EXPECT_EQ(o.action, ac::action_e::none);  // 第一輪還不算
  o = c.update(in(900, 30), t0 + 60s + 500ms);
  EXPECT_EQ(o.level, ac::level_e::deep);
  EXPECT_EQ(o.action, ac::action_e::cut_emergency);
}

// R4. 沒有樣本也算事件結束（等級歸零的同時基準作廢）
TEST_F(AbrCongestionTest, DrainingBaselineClearedWithoutSample) {
  c.update(in(3000, 30), t0);
  auto o = c.update(in(3000, 30), t0 + 500ms);
  ASSERT_EQ(o.action, ac::action_e::cut_emergency);
  c.commit_cut(o.action, 20000, t0 + 500ms, 10000);
  o = c.update(ac::cong_in_t {}, t0 + 1000ms);
  EXPECT_EQ(o.level, ac::level_e::clean);
  // 樣本回來、RTT 2000 ms（低於 3000 的九成）、3 秒冷卻已過：照樣緊急降碼
  c.update(in(2000, 30), t0 + 3600ms);
  o = c.update(in(2000, 30), t0 + 4100ms);
  EXPECT_EQ(o.action, ac::action_e::cut_emergency);
}

// R5. RTT 樣本的新鮮度：超過 3 秒沒更新就當成沒有樣本，凍結的高 RTT 不再觸發任何動作
TEST_F(AbrCongestionTest, StaleRttSampleIsNoSample) {
  const auto at = t0;
  EXPECT_FALSE(ac::rtt_sample_fresh(false, at, at));
  EXPECT_TRUE(ac::rtt_sample_fresh(true, at, at));
  EXPECT_TRUE(ac::rtt_sample_fresh(true, at, at + 3000ms));
  EXPECT_FALSE(ac::rtt_sample_fresh(true, at, at + 3001ms));
  EXPECT_FALSE(ac::rtt_sample_fresh(true, at + 1s, at));  // 時刻不合理
  EXPECT_FALSE(ac::rtt_sample_fresh(true, ac::steady::time_point {}, t0 + 1h));  // 從來沒有寫過時刻

  // 最後一個樣本是 1500 ms（t0 取樣），之後 ping 停了：run_abr_aimd 每 500 ms 還是跑一輪
  int cuts = 0;
  for (int i = 0; i < 40; ++i) {
    const auto now = t0 + i * 500ms;
    ac::cong_in_t ci;  // stream.cpp：沒有樣本時其他欄位都不填
    if (ac::rtt_sample_fresh(true, at, now)) {
      ci = in(1500, 40, 5.0, true, false, true);
    }
    const auto o = c.update(ci, now);
    if (o.action != ac::action_e::none) {
      c.commit_cut(o.action, 20000, now, 10000);
      ++cuts;
    }
    if (i > 6) {  // 樣本超過 3 秒之後
      EXPECT_EQ(o.level, ac::level_e::clean);
      EXPECT_EQ(o.action, ac::action_e::none);
      EXPECT_EQ(c.idr_gap_hint(), 0);
      EXPECT_FALSE(c.idr_request_wants_cut(now + 10ms));
    }
  }
  // 新鮮的那 3 秒（7 輪）內：嚴重一次（第 1 輪）、緊急一次（第 2 輪）、2 秒冷卻後再嚴重一次；之後 17 秒一刀都沒有
  EXPECT_EQ(cuts, 3);
}

// R6. 丟包分支與壅塞建議合併：目標取小的；丟包降得一樣多或更多、而且真的有降時也要記帳
TEST(AbrCongestionMerge, LossCutIsAccounted) {
  // 相等（丟包 ×0.50 對 emergency ×0.50）：以前不記帳
  auto m = ac::merge_loss_cut(10000, 10000, 20000, 1500);
  EXPECT_FALSE(m.cong_wins);
  EXPECT_EQ(m.target, 10000);
  EXPECT_TRUE(m.commit);
  // 丟包降得更深（×0.50 對 severe ×0.70）
  m = ac::merge_loss_cut(10000, 14000, 20000, 1500);
  EXPECT_FALSE(m.cong_wins);
  EXPECT_EQ(m.target, 10000);
  EXPECT_TRUE(m.commit);
  // 壅塞建議降得多（丟包分支維持）：照舊
  m = ac::merge_loss_cut(20000, 17000, 20000, 1500);
  EXPECT_TRUE(m.cong_wins);
  EXPECT_EQ(m.target, 17000);
  EXPECT_TRUE(m.commit);
  // 已經在下限、丟包那一側較深：這一輪沒有東西可降，不記帳（冷卻不起算、不記塞車點）
  m = ac::merge_loss_cut(750, 750, 1500, 1500);
  EXPECT_FALSE(m.commit);
  m = ac::merge_loss_cut(750, 1050, 1500, 1500);
  EXPECT_FALSE(m.commit);
  // 已經在下限、壅塞建議較深：照舊呼叫 commit_cut（塞車點由 commit_cut 自己擋）
  m = ac::merge_loss_cut(1500, 1275, 1500, 1500);
  EXPECT_TRUE(m.cong_wins);
  EXPECT_TRUE(m.commit);
}

// R6. 相等降幅記帳之後：下一輪不再建議動作（冷卻有起算），回升有塞車點壓住
TEST_F(AbrCongestionTest, EqualLossCutStartsCooldownAndRecordsPoint) {
  c.update(in(1000, 40), t0);
  auto o = c.update(in(1000, 40), t0 + 500ms);
  ASSERT_EQ(o.action, ac::action_e::cut_emergency);
  const int applied = 20000;
  const int floor_kbps = 1500;
  const auto m = ac::merge_loss_cut(applied * 50 / 100, applied * ac::action_pct(o.action) / 100, applied, floor_kbps);
  ASSERT_TRUE(m.commit);
  EXPECT_TRUE(c.commit_cut(o.action, applied, t0 + 500ms, std::max(floor_kbps, m.target)));

  // 下一輪剛好沒有丟包、嚴重條件也成立：緊急降碼 3 秒冷卻、一般降碼 2 秒冷卻都在算，不再砍
  o = c.update(in(1000, 40, 5.0, true, false, true), t0 + 1000ms);
  EXPECT_EQ(o.action, ac::action_e::none);
  EXPECT_EQ(c.congested_kbps(), 20000);
  EXPECT_EQ(c.ramp_cap_kbps(t0 + 5s), 18000);

  // 已經在下限：commit_cut 不記塞車點
  ac::congestion_t c2;
  EXPECT_FALSE(c2.commit_cut(ac::action_e::cut_severe, 1500, t0, 1500));
  EXPECT_EQ(c2.congested_kbps(), 0);
  EXPECT_EQ(c2.ramp_cap_kbps(t0 + 1s), 0);
}

// ── 審查 11-20 第二輪 ─────────────────────────────────────────────────────

// C09. 平面補包拒補與 IDR 提示同一個條件：L2 以上且 RTT 夠長；區網一陣丟包（RTT 幾毫秒、趨勢短暫過載）不拒補
TEST_F(AbrCongestionTest, RefuseRepairNeedsSlowRtt) {
  EXPECT_FALSE(c.refuse_repair(t0));  // 還沒有任何一輪
  // 區網：RTT 15 ms、趨勢過載＝L2，但不拒補（也不提示 IDR 長間隔）
  auto o = c.update(in(15, 2, 5.0, true, true), t0);
  ASSERT_EQ(o.level, ac::level_e::cut);
  EXPECT_FALSE(c.refuse_repair(t0));
  EXPECT_EQ(c.refuse_repair(t0), c.idr_gap_hint() > 0);
  // k_level_exit_hold 撐住 L2 的那 1 秒裡也不拒補
  o = c.update(in(3, 2), t0 + 500ms);
  ASSERT_EQ(o.level, ac::level_e::cut);
  EXPECT_FALSE(c.refuse_repair(t0 + 500ms));
  // RTT 400 ms 的 L2：拒補（和 IDR 提示一致）
  o = c.update(in(400, 40, 5.0, true, true), t0 + 1000ms);
  ASSERT_EQ(o.level, ac::level_e::cut);
  EXPECT_TRUE(c.refuse_repair(t0 + 1000ms));
  EXPECT_EQ(c.refuse_repair(t0 + 1000ms), c.idr_gap_hint() > 0);
  // 遲滯帶（120 ms）：維持
  c.update(in(120, 40, 5.0, true, true), t0 + 1500ms);
  EXPECT_TRUE(c.refuse_repair(t0 + 1500ms));
  // L1（暫停回升）不論 RTT 都不拒
  ac::congestion_t c2;
  c2.update(in(400, 40, 5.0, true), t0);
  ASSERT_EQ(c2.level(), ac::level_e::hold);
  EXPECT_FALSE(c2.refuse_repair(t0));
  // L3：拒
  c.update(in(1500, 40), t0 + 2000ms);
  o = c.update(in(1500, 40), t0 + 2500ms);
  ASSERT_EQ(o.level, ac::level_e::deep);
  EXPECT_TRUE(c.refuse_repair(t0 + 2500ms));
  // 沒有樣本：歸零、不拒
  c.update(ac::cong_in_t {}, t0 + 3000ms);
  EXPECT_FALSE(c.refuse_repair(t0 + 3000ms));
}

// 審查 11-21：進到 L3 且 RTT 夠長時拒補；之後 3 秒沒有新的一輪（last_update_ 不前進）就不再拒。
// 以前不看新鮮度，ABR 停在 L3 之後平面補包會被永久拒絕。
TEST_F(AbrCongestionTest, RefuseRepairExpiresWhenRoundStops) {
  c.update(in(1500, 40), t0);
  const auto o = c.update(in(1500, 40), t0 + 500ms);
  ASSERT_EQ(o.level, ac::level_e::deep);
  EXPECT_TRUE(c.rtt_slow());
  const auto at = t0 + 500ms;
  EXPECT_TRUE(c.refuse_repair(at));
  EXPECT_FALSE(c.refuse_repair(at + 3s));
  EXPECT_EQ(c.level(), ac::level_e::deep);  // 沒有新的一輪，等級還停在 L3，只是拒補過期
}

// D1／D3. 維持範圍：max(3 s, min(4 × RTT, 4 s) + 1 s)；RTT 500 ms 以下沒有維持段
TEST(AbrCongestionHold, HoldWindow) {
  EXPECT_EQ(ac::rtt_hold_window(0).count(), 3000);
  EXPECT_EQ(ac::rtt_hold_window(5).count(), 3000);
  EXPECT_EQ(ac::rtt_hold_window(500).count(), 3000);
  EXPECT_EQ(ac::rtt_hold_window(600).count(), 3400);
  EXPECT_EQ(ac::rtt_hold_window(1000).count(), 5000);
  EXPECT_EQ(ac::rtt_hold_window(4000).count(), 5000);
  EXPECT_EQ(ac::rtt_hold_window(0xFFFFFFFFu).count(), 5000);  // 不溢位、最長 5 秒

  const auto at = ac::steady::time_point {} + 1h;
  EXPECT_FALSE(ac::rtt_sample_held(true, at, at + 3000ms, 1500));  // 還新鮮，不是維持
  EXPECT_TRUE(ac::rtt_sample_held(true, at, at + 3001ms, 1500));
  EXPECT_TRUE(ac::rtt_sample_held(true, at, at + 5000ms, 1500));
  EXPECT_FALSE(ac::rtt_sample_held(true, at, at + 5001ms, 1500));
  EXPECT_FALSE(ac::rtt_sample_held(true, at, at + 3001ms, 5));  // 區網：3 秒一過就是沒有樣本（與以前相同）
  EXPECT_FALSE(ac::rtt_sample_held(false, at, at + 3500ms, 1500));  // peer 逾時作廢過的樣本
  EXPECT_FALSE(ac::rtt_sample_held(true, at + 1s, at, 1500));  // 時刻不合理
}

// §ABR-TAKEOVER-HOLD：接手把樣本作廢，維持從作廢那一刻起算（不必先等過 3 秒新鮮期）。
// 1500 ms 的窗是 5 秒（公式在 HoldWindow）。forgot＝false 是真的斷線，不能把舊等級留著。
TEST(AbrCongestionHold, TakeoverHold) {
  const auto at = ac::steady::time_point {} + 1h;
  const uint32_t rtt = 1500;
  const auto win = ac::rtt_hold_window(rtt);
  ASSERT_EQ(win.count(), 5000);

  EXPECT_FALSE(ac::rtt_takeover_held(false, at, at + 1000ms, rtt));  // 不是接手作廢
  EXPECT_TRUE(ac::rtt_takeover_held(true, at, at, rtt));  // 窗的起點（含）：作廢當下就要維持
  EXPECT_TRUE(ac::rtt_takeover_held(true, at, at + 1000ms, rtt));  // 窗內
  EXPECT_TRUE(ac::rtt_takeover_held(true, at, at + win, rtt));  // 剛好等於窗
  EXPECT_FALSE(ac::rtt_takeover_held(true, at, at + win + 1ms, rtt));  // 超過窗，之後照沒有樣本
  EXPECT_FALSE(ac::rtt_takeover_held(true, at + 1s, at, rtt));  // 時刻不合理
}

namespace {
  // 照 stream.cpp 的組法：新鮮＝完整輸入、維持段＝held、其餘＝空的（沒有樣本）
  ac::cong_in_t staged(const ac::cong_in_t &fresh, ac::steady::time_point at, ac::steady::time_point now, uint32_t rtt) {
    if (ac::rtt_sample_fresh(true, at, now)) {
      return fresh;
    }
    ac::cong_in_t ci;
    ci.held = ac::rtt_sample_held(true, at, now, rtt);
    return ci;
  }
}  // namespace

// D1／D3. 深度壅塞中 ping 被 ENet 重傳擋了 4 秒（同一個 ping 連丟兩次）：L3、IDR 長間隔都撐住，不做新的降碼；
// 樣本回來後接著原本的事件（不會重新數輪、不會因為「排空基準」被清掉而多砍）
TEST_F(AbrCongestionTest, BlockedPingKeepsDeepState) {
  auto fresh = in(1500, 40);
  c.update(fresh, t0);
  auto o = c.update(fresh, t0 + 500ms);
  ASSERT_EQ(o.action, ac::action_e::cut_emergency);
  c.commit_cut(o.action, 20000, t0 + 500ms, 10000);
  const int hint = c.idr_gap_hint();
  ASSERT_EQ(hint, 2750);  // 1.5 × 1500 + 500

  // 佇列開始排空：最後一個樣本 1200 ms（t0 + 1000），之後 ping 被擋到 t0 + 5100
  const auto at = t0 + 1000ms;
  o = c.update(in(1200, 40), at);
  EXPECT_EQ(o.action, ac::action_e::none);  // 排空中
  const int hint2 = c.idr_gap_hint();
  for (auto now = at + 500ms; now < t0 + 5100ms; now += 500ms) {
    o = c.update(staged(in(1200, 40), at, now, 1200), now);
    EXPECT_EQ(o.level, ac::level_e::deep);
    EXPECT_EQ(o.action, ac::action_e::none);
    EXPECT_EQ(c.idr_gap_hint(), hint2);
    EXPECT_TRUE(c.refuse_repair(now));
    if (now - at > 3000ms) {
      EXPECT_TRUE(o.held);
      EXPECT_FALSE(c.idr_request_wants_cut(now + 10ms));  // 量不到 RTT 的那幾秒不做新的降碼
    }
  }
  // 卡住的 ping 一起到：RTT 900 ms（還在排空，低於 1500 的九成）——不追加一刀，等級還是 L3
  o = c.update(in(900, 40), t0 + 5100ms);
  EXPECT_FALSE(o.held);
  EXPECT_TRUE(o.prev_held);
  EXPECT_EQ(o.level, ac::level_e::deep);
  EXPECT_EQ(o.action, ac::action_e::none);
}

// D1／D3. 維持段過了還沒有樣本（ENet 真的不通）：照舊歸零，凍結的值不會一直撐著 L3 與 IDR 長間隔
TEST_F(AbrCongestionTest, HeldExpiresToNoSample) {
  auto fresh = in(1500, 40);
  c.update(fresh, t0);
  auto o = c.update(fresh, t0 + 500ms);
  c.commit_cut(o.action, 20000, t0 + 500ms, 10000);
  const auto at = t0 + 500ms;
  int cuts = 0;
  for (auto now = at + 500ms; now <= at + 8000ms; now += 500ms) {
    o = c.update(staged(fresh, at, now, 1500), now);
    if (o.action != ac::action_e::none) {
      c.commit_cut(o.action, 10000, now, 5000);
      cuts += now - at > 3000ms;  // 只數新鮮期之後的
    }
    if (now - at <= 5000ms) {
      EXPECT_EQ(o.level, ac::level_e::deep);
    } else {
      EXPECT_FALSE(o.held);
      EXPECT_EQ(o.level, ac::level_e::clean);
      EXPECT_EQ(c.idr_gap_hint(), 0);
      EXPECT_FALSE(c.refuse_repair(now));
    }
  }
  EXPECT_EQ(cuts, 0);  // 新鮮期內冷卻滿 3 秒時照樣一刀；之後維持段與沒有樣本的 5 秒，凍結的 1500 ms 不觸發任何刀
}

// D1／D3. 區網（RTT 幾毫秒）沒有維持段：3 秒沒樣本就歸零，和以前完全相同
TEST_F(AbrCongestionTest, LanStaleSampleUnchanged) {
  const auto at = t0;
  c.update(in(15, 2, 5.0, true, true), at);
  ASSERT_EQ(c.level(), ac::level_e::cut);
  const auto o = c.update(staged(in(15, 2, 5.0, true, true), at, at + 3500ms, 15), at + 3500ms);
  EXPECT_FALSE(o.held);
  EXPECT_EQ(o.level, ac::level_e::clean);
}

// ── 審查 11-20 第三輪 ────────────────────────────────────────────────────

// C01. RTT 一路膨脹時趨勢估計器的抖動（p90−p10）≈ 膨脹量本身；L3 門檻要用壅塞前的抖動，超出 500 ms 後兩輪內就鎖上
TEST_F(AbrCongestionTest, DeepThresholdUsesPreCongestionJitter) {
  auto t = t0;
  for (int i = 0; i < 24; ++i, t += 500ms) {
    ASSERT_EQ(c.update(in(40, 38, 5.0), t).level, ac::level_e::clean);
  }
  // 20 Mbps 進 15 Mbps 的瓶頸：每 500 ms RTT 多 166 ms；送進去的抖動就是視窗被汙染後的值（＝超出量）
  bool deep = false;
  int rounds_over = 0;
  for (int k = 1; k <= 8 && !deep; ++k, t += 500ms) {
    const uint32_t rtt = 40 + 166 * k;
    const double excess = (double) rtt - 38.0;
    const auto o = c.update(in(rtt, 38, excess, true, true), t);
    if (excess >= 500.0) {
      ++rounds_over;
    }
    if (o.level == ac::level_e::deep) {
      deep = true;
      EXPECT_EQ(o.action, ac::action_e::cut_emergency);
    }
  }
  EXPECT_TRUE(deep);
  EXPECT_LE(rounds_over, 2);
}

// C01. 8 × 抖動有上限 1000 ms
TEST(AbrCongestionDeep, JitterTermIsCapped) {
  EXPECT_DOUBLE_EQ(ac::deep_enter_threshold_ms(100.0), 800.0);
  EXPECT_DOUBLE_EQ(ac::deep_enter_threshold_ms(125.0), 1000.0);
  EXPECT_DOUBLE_EQ(ac::deep_enter_threshold_ms(300.0), 1000.0);
  ac::congestion_t c;
  const auto t0 = ac::steady::time_point {} + 10ms;
  ac::cong_in_t i;
  i.sampled = true;
  i.rtt_ms = 40;
  i.base_ms = 40;
  i.jitter_ms = 300.0;  // 很吵的路徑：平常抖動 300 ms
  c.update(i, t0);
  i.rtt_ms = 1140;  // 高出 1100 > 1000（沒有上限時門檻是 2400）
  i.jitter_ms = 900.0;
  i.hold = true;
  c.update(i, t0 + 500ms);
  EXPECT_EQ(c.update(i, t0 + 1000ms).level, ac::level_e::deep);
}

// C01. 平常抖動往上調要先連續乾淨 10 秒：壅塞剛結束、視窗裡還有尾巴時不採用；乾淨夠久才採用
TEST_F(AbrCongestionTest, CalmJitterRaiseNeedsSettle) {
  for (int i = 0; i < 5; ++i) {
    c.update(in(42, 40, 5.0), t0 + i * 500ms);
  }
  for (int i = 5; i < 9; ++i) {
    c.update(in(150, 40, 5.0, true), t0 + i * 500ms);  // 一陣 L1
  }
  for (int i = 9; i < 16; ++i) {
    c.update(in(42, 40, 200.0), t0 + i * 500ms);  // 乾淨了，但抖動還是上一陣的尾巴
  }
  // 平常抖動還是 5（門檻 500）：高出 600 兩輪就進 L3
  EXPECT_DOUBLE_EQ(c.calm_jitter_ms(), 5.0);
  c.update(in(640, 40, 200.0, true), t0 + 8000ms);
  EXPECT_EQ(c.update(in(640, 40, 200.0, true), t0 + 8500ms).level, ac::level_e::deep);

  // 另一條路徑：一直乾淨、抖動真的是 100 ms，滿 10 秒後門檻變 800，高出 600 不進 L3
  ac::congestion_t d;
  for (int i = 0; i < 2; ++i) {
    d.update(in(42, 40, 5.0), t0 + i * 500ms);
  }
  for (int i = 2; i < 24; ++i) {
    d.update(in(42, 40, 100.0), t0 + i * 500ms);
  }
  EXPECT_DOUBLE_EQ(d.calm_jitter_ms(), 100.0);
  for (int i = 24; i < 30; ++i) {
    EXPECT_NE(d.update(in(640, 40, 100.0, true), t0 + i * 500ms).level, ac::level_e::deep);
  }
}

// 第四輪 (a). 乾淨很久之後 RTT 緩慢爬升、還沒判成 L1 的幾輪：抖動已經被膨脹量拉大，平常抖動不可以跟著被推高
TEST_F(AbrCongestionTest, CalmJitterIgnoresRampBeforeL1) {
  auto t = t0;
  for (int i = 0; i < 30; ++i, t += 500ms) {  // 15 秒乾淨，平常抖動 20 ms
    c.update(in(160, 150, 20.0), t);
  }
  ASSERT_DOUBLE_EQ(c.calm_jitter_ms(), 20.0);
  // 基線 150 ms 的遠端路徑慢慢塞：RTT 還沒到 2 倍、趨勢也還沒判過載，但視窗的 p90−p10 一路變大
  for (int k = 1; k <= 6; ++k, t += 500ms) {
    ASSERT_EQ(c.update(in(160 + 20 * k, 150, 20.0 + 40.0 * k), t).level, ac::level_e::clean);
  }
  EXPECT_DOUBLE_EQ(c.calm_jitter_ms(), 20.0);  // 舊寫法是 260（門檻被推到 1000 ms）
  // 接著塞滿：高出 600 ms 兩輪就進 L3（門檻 max(500, 8×20)＝500）
  c.update(in(750, 150, 600.0, true, true), t);
  EXPECT_EQ(c.update(in(750, 150, 600.0, true, true), t + 500ms).level, ac::level_e::deep);

  // 抖動真的變大、而且穩定超過 10 秒：照樣往上調
  ac::congestion_t d;
  auto u = t0;
  for (int i = 0; i < 10; ++i, u += 500ms) {
    d.update(in(160, 150, 20.0), u);
  }
  for (int i = 0; i < 22; ++i, u += 500ms) {
    d.update(in(160, 150, 90.0), u);
  }
  EXPECT_DOUBLE_EQ(d.calm_jitter_ms(), 90.0);
}

// 第四輪 (b). 開場的抖動接近 0（趨勢視窗只有 1～2 個樣本）；抖動大的路徑上 L1 一直閃，乾淨期數不滿 10 秒，
// 平常抖動也不可以整場停在 0
TEST_F(AbrCongestionTest, CalmJitterWaitsForTrendSamplesAndSurvivesL1Flicker) {
  auto t = t0;
  // 開場兩輪：估計器的抖動是 max−min，接近 0；還沒數滿暖身輪數，只是暫定值（跟著走、不鎖住）
  c.update(in(100, 100, 0.0), t);
  t += 500ms;
  c.update(in(110, 100, 10.0), t);
  t += 500ms;
  EXPECT_DOUBLE_EQ(c.calm_jitter_ms(), 10.0);
  // 抖動 80 ms 的路徑，每 3 秒 L1 閃一下（暫停回升條件成立一輪）
  for (int i = 0; i < 40; ++i, t += 500ms) {
    const bool flick = i % 6 == 5;
    c.update(in(flick ? 210 : 140, 100, 80.0, flick), t);
  }
  EXPECT_DOUBLE_EQ(c.calm_jitter_ms(), 80.0);  // 舊寫法停在 0
  // 門檻 640：換基地台那種 RTT 跳高 600 ms 約 1 秒不進 L3
  for (int i = 0; i < 3; ++i, t += 500ms) {
    EXPECT_NE(c.update(in(700, 100, 80.0, true), t).level, ac::level_e::deep);
  }
}

// 第四輪 (b). 暖身輪數要間隔夠久才算：同一瞬間插進來的多次呼叫只算一次
TEST_F(AbrCongestionTest, CalmJitterWarmupCountsSpacedRounds) {
  for (int i = 0; i < 10; ++i) {
    c.update(in(42, 40, 1.0), t0 + i * 10ms);  // 算 1 輪（10 輪的話會在這裡把 1 定下來）
  }
  auto t = t0 + 500ms;
  for (int i = 0; i < 3; ++i, t += 500ms) {
    c.update(in(42, 40, 30.0), t);  // 第 2～4 輪：暫定值跟著到 30
  }
  EXPECT_DOUBLE_EQ(c.calm_jitter_ms(), 30.0);
  c.update(in(42, 40, 10.0), t);  // 第 5 輪：定下來
  t += 500ms;
  c.update(in(42, 40, 30.0), t);  // 已經定下來、乾淨還不滿 10 秒：不往上調
  EXPECT_DOUBLE_EQ(c.calm_jitter_ms(), 10.0);
}

// C02. 排空中收到 IDR 請求不降碼；RTT 回升後才降
TEST_F(AbrCongestionTest, IdrRequestDoesNotCutWhileDraining) {
  c.update(in(1000, 40, 5.0, true), t0);
  auto o = c.update(in(1000, 40, 5.0, true), t0 + 500ms);
  ASSERT_EQ(o.action, ac::action_e::cut_emergency);
  c.commit_cut(o.action, 20000, t0 + 500ms);
  c.update(in(850, 40, 5.0, true), t0 + 3600ms);  // 3 秒冷卻過了、RTT 降了一成以上
  ASSERT_EQ(c.level(), ac::level_e::deep);
  EXPECT_FALSE(c.idr_request_wants_cut(t0 + 3700ms));
  EXPECT_GT(c.idr_gap_hint(), 0);  // IDR 長間隔照舊
  c.update(in(1000, 40, 5.0, true), t0 + 4100ms);
  EXPECT_TRUE(c.idr_request_wants_cut(t0 + 4200ms));
}

// C03. 排空中嚴重膨脹不追加 ×0.70；趨勢過載仍可 ×0.85；停在平台上，下一個檢查點之後緊急降碼恢復
TEST_F(AbrCongestionTest, DrainingBlocksSevereAndEndsOnPlateau) {
  c.update(in(1000, 40, 5.0, true, false, true), t0);
  auto o = c.update(in(1000, 40, 5.0, true, false, true), t0 + 500ms);
  ASSERT_EQ(o.action, ac::action_e::cut_emergency);
  c.commit_cut(o.action, 20000, t0 + 500ms);

  o = c.update(in(800, 40, 5.0, true, false, true), t0 + 2600ms);  // 一般降碼的 2 秒冷卻過了
  EXPECT_EQ(o.level, ac::level_e::deep);
  EXPECT_EQ(o.action, ac::action_e::none);
  o = c.update(in(800, 40, 5.0, true, true, true), t0 + 2700ms);  // 佇列又在長
  EXPECT_EQ(o.action, ac::action_e::cut_trend);

  // RTT 停在 800：第一個檢查點（基準還是 1000）仍算排空，第二個檢查點基準換成 800，不再算排空
  for (int i = 0; i < 7; ++i) {
    o = c.update(in(800, 40, 5.0, true, false, true), t0 + 3100ms + i * 500ms);
    EXPECT_EQ(o.action, ac::action_e::none) << i;
  }
  o = c.update(in(800, 40, 5.0, true, false, true), t0 + 6600ms);
  EXPECT_EQ(o.action, ac::action_e::cut_emergency);
}

// C03. 持續有進展（每 3 秒低一成以上）就一直算排空：緊急降碼之後一刀都不追加
TEST_F(AbrCongestionTest, SteadyDrainKeepsBlocking) {
  c.update(in(3000, 40, 5.0, true, false, true), t0);
  auto o = c.update(in(3000, 40, 5.0, true, false, true), t0 + 500ms);
  ASSERT_EQ(o.action, ac::action_e::cut_emergency);
  c.commit_cut(o.action, 20000, t0 + 500ms);
  double rtt = 3000.0;
  for (int i = 1; i <= 24; ++i) {
    rtt *= 0.97;  // 每 500 ms 降 3%（2 秒就低過一成、之後每 3 秒約 17%）
    o = c.update(in((uint32_t) rtt, 40, 5.0, true, false, true), t0 + 500ms + i * 500ms);
    EXPECT_EQ(o.action, ac::action_e::none) << i;
  }
}
