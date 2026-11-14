/**
 * @file src/vr/vr_abr_fast.h
 * @brief VipleStream 2.0 §ABR-ADAPT：VR 自適應快速碼率控制（第 1、2、3 期：純邏輯＋單元測試）。
 *
 * 專為 VR 高速動態環境設計（流暢度第一、寧可糊不要卡）。依環境自適應：
 *   - 延遲趨勢（delay_trend_t）：自適應門檻 γ 追蹤排隊延遲膨脹，進入 overuse 即快速降碼。
 *   - 單向延遲（on_owd）：2 秒內有樣本時優先以 owd_trend_ 判定壅塞、降碼與暫停回升。
 *     §ABR-OWD-STALL（11-08 在家）：判過載時最近 8 張的單向延遲中位數已回到基準（高出不到 max(4 ms, 0.5γ)）＝Wi-Fi 停頓，
 *     不降碼、不記塞車點（實測每次停頓都把塞車點往下記 75→59→50 Mbps，之後被防振盪上限鎖 30 秒，整場卡在 42～51 Mbps）。
 *   - §VR-NETSEL-HOLD（11-08 傍晚）：頭盔通知要切網路（暫停／接回家用 Wi-Fi）時，那幾秒內網路類訊號（丟包、延遲、送達率）
 *     不降碼（解碼過載也算：湧進來的畫面會拉高解碼 p95）、隨機丟包不計數；畫質修剪照常。實測接回家用 Wi-Fi 時熱點跟著掉包，
 *     一路降到 2 Mbps、90 秒才回到 60。
 *   - 實際送達速率（on_rates）：鏈路飽和（send >= recv * 1.05）時，壅塞降碼一步落至 clamp(0.95 * recv / send, 0.5, 0.85)。
 *     §ABR-SHORTFALL（11-05）：送出比收到多 15% 以上連續 2 次回報（約 1 秒）＝已經在排隊，直接降（原因 rate-shortfall+delay），
 *     不等延遲趨勢累積（模擬頭盔實測：送 84.8／收 58.6 Mbps 時單向延遲 p95 83 ms，趨勢要約 10 秒才判 overuse）。
 *   - 畫質飽和（on_quality）：H.264/HEVC QP <= 22 時暫停回升；連續 10 秒 QP <= 15 時每 3 秒回收 5%～25%（quality-trim，QP 離門檻越遠收越多）。
 *   - 解碼負載（on_decode）：p95 > 0.85 * period 時暫停回升（有效 3 秒）；連 2 筆 > 1.0 * period 觸發 decode-overload 中度降碼。
 *   - 丟包分類：
 *     * 延遲過載時丟包（或剛脫離過載 1.5 秒內）-> 視為網路壅塞降碼（原因加註 +delay）。
 *     * 延遲平穩時零星丟包 -> 視為無線隨機丟包，不降碼，累計至 random_losses()。
 *     * 延遲平穩但 3 秒內嚴重累積丟包 >= 12 -> 例外判定為持續送不動，降碼並加註 +persistent。
 *   - 下限：k_floor_tech_kbps = 2000 kbps（技術最低值），不超過設定值。
 *   - 冷卻與回升隨 base RTT 動態調整：
 *     * HOLD 冷卻長度 = clamp(8 * base_rtt, 600 ms, 2000 ms)。
 *     * RAMP 回升：applied < 0.8 * last_congested 時採乘法快速探頂（每秒約 +8%）；接近時改加法慢爬（每秒 +0.5%），
 *       超過上次塞車點後更慢（每秒 +0.25%）。上次塞車點只記同一次塞車的第一刀（5 秒內的後續刀不覆寫），
 *       且 30 秒沒再塞車後，慢爬的上界每秒放寬 0.1%（最多 +50%，§ABR-RATCHET），避免一路往下記。
 *   - 丟包率預算（§ABR-LOSSRATE，11-04 桌球實測）：延遲沒升的「隨機」丟包若 20 秒內 >= 4 次且橫跨至少 10 秒（約每 10 秒 2 張救不回），
 *     代表丟包跟碼率有關（20 Mbps 幾乎不丟、75 Mbps 每 10 秒 2 張）→ 降 10% 並把上限暫壓在降後的值 60 秒；
 *     低於 10 Mbps 不再因此降（那種丟包多半是無線環境本身，降碼救不了）。
 *     * §ABR-LOSSRATE-VERIFY（11-07 遠端桌球）：每次因此降碼都是一次試驗。降後 30 秒內隨機丟包的頻率若沒有降到降碼前的
 *       六成以下，代表丟包跟碼率無關 → 撤銷這一刀（回到降碼前的碼率、拿掉暫壓的上限、塞車點還原），這條規則停用 10 分鐘。
 *       實測 75 Mbps 與 10 Mbps 都是每 10 秒約 2.5 次，舊規則把碼率一路壓到 38 Mbps、之後 10 Mbps 變成實際上限（QP 25→36）。
 *     * 延遲趨勢過載或 trend > 0.5γ、解碼吃緊、畫質飽和時暫停回升。
 *   - 斷訊後拉回（on_outage_restored，§VR-ABR-OUTAGE）：斷訊期間連降多刀會順便觸發防振盪上限（applied × 0.85），
 *     拉回斷訊前碼率時一起清掉，否則下一輪就被上限壓回斷訊時的低點（11-07：拉回 29.6 Mbps 後 0.6 秒被壓回 5.9 Mbps）。
 *     但只拉回到斷訊前穩定撐住 10 秒的碼率（§ABR-RESTORE-STABLE，11-08：拉回沒撐住過的 75 Mbps，3 分鐘內塞了 6 次）。
 *   - 防振盪懲罰：RAMP 中 5 秒內再次降碼 -> 上限暫壓至 applied * 0.85 維持 30 秒。
 *   - ※ 以下四組（§ABR-PERSIST-VERIFY、§ABR-STALL-LOSS、§ABR-OUTAGE-HOLD、§ABR-PATH-UPGRADE）都在 set_experimental_rules(true)
 *     才生效（host 設定 vr_abr_experimental，預設關）。11-13 晚的逐封包量測推翻了它們的前提：那條線路的丟包隨碼率上升
 *     （3 Mbps 1.0% → 24 Mbps 5.8%）、超過 0.1 秒的斷訊每分鐘約 1 次，並不是「與碼率無關的頻繁長斷訊」；配戴結果也是
 *     碼率撐高之後定格變多。關著時控制器的決定和 2.0.175 相同。真正有效的是送出端把每一幀攤開送（§VR-PACE）。
 *   - §ABR-PERSIST-VERIFY（11-13 外出實測，場地對外線路每分鐘 15～35 次短暫斷訊、一次掉 11～36 張）：+persistent 的降碼也要驗證。
 *     * 判定：同一串（相隔 20 秒內、中間沒有別種降碼）+persistent 實際降過 2 刀以上、累計降到這一串開始時的六成以下、過了 2 秒，
 *       而「收到比送出少的比例」（最近 3 秒）沒有降到這一串開始前的一半以下＝丟包跟碼率無關 → 不再降、規則停用 5 分鐘
 *       （每多判定一次加倍，上限 30 分鐘），碼率拉回這一串開始時的一半（之後照常回升），這一串記下的塞車點等還原。
 *       真的送不動（限速器直接丟）時，碼率降了丟的比例就跟著降（75→37.5 Mbps、容量 34：55% → 9%），不會被判成無效。
 *       沒有收／送速率的 client 不做這個判定（照舊每次都降）。
 *     * 停用期間若連續 2 秒每一筆回報都收得比送的少、而且少的比例比判定當時高出一截（兩倍或多 5 個百分點）
 *       ＝情況變了（例如回升後真的超過線路容量、延遲又沒漲）→ 規則立刻恢復。
 *       實測那天 40 次降碼有 30 次是 +persistent，75 Mbps 一路降到 2 Mbps，丟包頻率沒有變。
 *     * rate-shortfall+delay 也歸同一串、受同一個判定管（11-13 第二輪實測：一次一秒以上的斷訊讓「送出比收到多」連續成立，
 *       2.5 秒內連降三刀 25 → 8.7 Mbps，斷訊照舊）。
 *     * 判定無效期間丟包率規則（loss-rate）也不降：同一個證據（丟包跟碼率無關），11-13 第三輪實測它每次 −10% 把碼率慢慢往下帶。
 *     * 判定無效後拉回的目標：有穩定值（先前 10 秒沒降碼時撐住的碼率）就拉回它（不超過這一串開始時的碼率），
 *       沒有才拉回開始時的一半。
 *   - §ABR-STALL-LOSS（11-13 第三輪）：斷訊結束時積壓的封包一次湧到，單向延遲瞬間升高、延遲趨勢判過載，緊接著的丟包就被歸成
 *     「+delay」快速降碼。is_congested() 在單向延遲可用時多看一眼「最近 8 張的延遲是不是已經回到基準」（同 §ABR-OWD-STALL），
 *     已經回到＝停頓不是排隊，這時的丟包照隨機／成串丟包處理。
 *   - §ABR-OUTAGE-HOLD（11-13 第四輪）：場地線路的斷訊長到 2 秒，斷訊前後單向延遲都正常（p50 19～27 ms、送達率 99%），
 *     但每次斷訊結束那一下延遲與往返時間的量測被灌爆，丟包被歸成「+delay」連降，3 分鐘從 17 Mbps 降到 4～6 Mbps。
 *     client 回報一次成串丟包（斷訊）時，如果延遲趨勢在這段空檔「之前」並沒有過載、收／送也沒有連續 3 秒以上的落差，
 *     就判定是斷訊：之後 1.5 秒內網路類的訊號都不降碼、不計入丟包統計（解碼過載照常）。斷訊前就已經在過載＝塞車，照舊降。
 *   - §ABR-PATH-UPGRADE（11-13 第三輪）：Tailscale 從中繼轉成直連（或其他「換到更好的路」）時，舊路徑上記下的塞車點、
 *     各種暫壓的上限、延遲基準都作廢，之後 30 秒內回升加快（每秒 +25%，原本 +8%），第一次降碼就結束加速。
 *     實測：開場在中繼上被壓到 2 Mbps，30 秒後轉直連，因為塞車點記的是 2.1 Mbps，只能每秒 +0.5% 慢爬，兩分鐘後還在 3 Mbps。
 *
 * 純邏輯、無 I/O、無 log、無全域狀態，可單元測試。呼叫端（stream.cpp）持 abrMutex 加鎖呼叫並負責記錄 log。
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <string>
#include <utility>

#include "abr_delay_trend.h"

namespace vr::abr {

  using steady = std::chrono::steady_clock;

  // 狀態定義
  enum class fast_state_e {
    healthy,  ///< 健康狀態
    cut,      ///< 降碼狀態（當輪有降碼事件生效）
    hold,     ///< 冷卻冷凍期（禁止回升）
    ramp,     ///< 平滑回升期
  };

  inline const char *fast_state_name(fast_state_e s) {
    switch (s) {
      case fast_state_e::healthy: return "healthy";
      case fast_state_e::cut:     return "cut";
      case fast_state_e::hold:    return "hold";
      case fast_state_e::ramp:    return "ramp";
    }
    return "unknown";
  }

  // 常數定義
  constexpr int k_floor_tech_kbps = 2000;                                    ///< 技術最低碼率 2000 Kbps (2 Mbps)
  constexpr auto k_fast_cut_window = std::chrono::milliseconds(1000);        ///< 嚴重降碼同秒判定視窗 1 秒
  constexpr auto k_fast_anti_osc_window = std::chrono::milliseconds(5000);   ///< 防振盪 RAMP 觸發視窗 5 秒
  constexpr auto k_fast_anti_osc_duration = std::chrono::seconds(30);        ///< 防振盪懲罰維持 30 秒
  constexpr float k_fast_anti_osc_cap_factor = 0.85f;                        ///< 防振盪懲罰上限係數 applied × 0.85
  constexpr auto k_fast_persistent_window = std::chrono::milliseconds(3000); ///< 持續丟包判定視窗 3 秒
  constexpr int k_fast_persistent_threshold = 12;                            ///< 持續丟包權重門檻 12
  constexpr auto k_fast_delay_overuse_recent_window = std::chrono::milliseconds(1500); ///< 丟包分類之 delay 關聯視窗 1.5 秒

  // §ABR-ADAPT 第 2、3 期常數
  constexpr auto k_fast_owd_active_window = std::chrono::milliseconds(2000);   ///< OWD 樣本有效視窗 2 秒
  constexpr std::size_t k_fast_owd_recent_n = 8;        ///< §ABR-OWD-STALL：看最近 8 張（144 Hz 約 55 ms）的單向延遲中位數
  constexpr double k_fast_owd_stall_drained_ms = 4.0;   ///< §ABR-OWD-STALL：高出基準不到 max(4 ms, 0.5γ)＝已經消了
  constexpr int k_fast_net_switch_max_ms = 20000;       ///< §VR-NETSEL-HOLD：頭盔要求的暫停降碼最長 20 秒
  constexpr auto k_fast_rates_fresh_window = std::chrono::milliseconds(1000);  ///< 送達速率有效視窗 1 秒
  constexpr double k_fast_rates_saturated_ratio = 1.05;  ///< 鏈路飽和判定倍數 1.05
  constexpr auto k_fast_saturated_no_restore = std::chrono::milliseconds(6000);  ///< 飽和降碼後這段時間內不做斷訊拉回
  constexpr auto k_fast_saturated_startup_grace = std::chrono::seconds(10);     ///< 開場這段時間的飽和降碼不算
  constexpr double k_fast_shortfall_ratio = 1.15;    ///< §ABR-SHORTFALL：送出 ≥ 收到 × 1.15 才算排隊（避開無線零星掉包的小落差）
  constexpr int k_fast_shortfall_reports = 2;        ///< 連續幾次回報（每 500 ms 一次）
  constexpr int k_fast_shortfall_min_kbps = 5000;    ///< 送出太少時不判（量測誤差大）
  constexpr int k_fast_shortfall_outage_div = 4;     ///< 收到 < 送出 ÷ 4＝斷訊，rate-shortfall 不降
  constexpr auto k_fast_quality_recent_window = std::chrono::milliseconds(1500); ///< QP 飽和判定視窗 1.5 秒
  constexpr auto k_fast_quality_trim_duration = std::chrono::seconds(10);      ///< quality-trim 連續門檻 10 秒
  constexpr auto k_fast_quality_trim_interval = std::chrono::seconds(3);       ///< quality-trim 間隔 3 秒
  constexpr int k_fast_qp_saturated_x100 = 2000;  ///< 畫質飽和門檻 QP 20（×100）；11-04 實測 22 對 VR 偏鬆（鏡片放大壓縮瑕疵）
  constexpr int k_fast_qp_trim_threshold_x100 = 1500;  ///< 畫質多到用不完的門檻 QP 15（×100）
  constexpr auto k_fast_decode_tight_duration = std::chrono::seconds(3);       ///< 解碼吃緊有效時間 3 秒

  // §ABR-LOSSRATE／慢爬常數（11-04）
  constexpr auto k_fast_congest_episode = std::chrono::seconds(5);         ///< 同一次塞車的視窗：5 秒內後續刀不覆寫塞車點
  constexpr auto k_fast_loss_rate_window = std::chrono::seconds(20);       ///< 丟包率視窗 20 秒
  constexpr int k_fast_loss_rate_threshold = 4;                            ///< 視窗內隨機丟包次數門檻
  constexpr auto k_fast_loss_rate_cap_duration = std::chrono::seconds(60); ///< 降後上限維持 60 秒
  constexpr int k_fast_loss_rate_min_kbps = 10000;                         ///< 低於此值不再因丟包率降碼
  // §ABR-LOSSRATE-VERIFY（11-07）
  constexpr auto k_fast_loss_rate_trial = std::chrono::seconds(30);        ///< 降碼後觀察多久
  constexpr auto k_fast_loss_rate_settle = std::chrono::seconds(2);        ///< 降碼生效前的丟包不算
  constexpr double k_fast_loss_rate_effective_ratio = 0.6;                 ///< 降後頻率要低於降前的這個比例才算有效
  constexpr auto k_fast_loss_rate_off = std::chrono::minutes(10);          ///< 無效時停用這條規則多久
  // §ABR-PERSIST-VERIFY（11-13）
  constexpr auto k_fast_persist_episode_gap = std::chrono::seconds(20);      ///< 相隔多久以內算同一串
  constexpr auto k_fast_persist_min_trial = std::chrono::seconds(2);         ///< 這一串至少觀察多久才判定
  constexpr int k_fast_persist_min_cuts = 2;                                 ///< 這一串至少降過幾刀才判定
  constexpr double k_fast_persist_ineffective_ratio = 0.6;                   ///< 碼率已降到開始時的這個比例以下還在丟＝無效
  constexpr double k_fast_persist_restore_ratio = 0.5;                       ///< 判定無效後拉回到開始時的這個比例
  constexpr double k_fast_persist_loss_drop_ratio = 0.5;                     ///< 丟的比例要降到開始前的這個比例以下才算「降碼有效」
  constexpr auto k_fast_persist_before_window = std::chrono::seconds(3);     ///< 這一串開始前看多久的收／送
  constexpr auto k_fast_persist_now_window = std::chrono::seconds(3);        ///< 判定時看最近多久的收／送（和開始前同長，才能比）
  constexpr auto k_fast_persist_rates_keep = std::chrono::seconds(8);        ///< 收／送紀錄保留多久
  constexpr int k_fast_persist_sustained_reports = 4;                        ///< 連續幾筆回報（每 500 ms）都收得比送的少才算持續
  constexpr auto k_fast_persist_off = std::chrono::minutes(5);               ///< 無效時停用這條規則多久（每多判定一次加倍）
  constexpr auto k_fast_persist_off_max = std::chrono::minutes(30);          ///< 停用時間上限
  // §ABR-OUTAGE-HOLD（11-13）
  constexpr auto k_fast_outage_hold = std::chrono::milliseconds(1500);  ///< 斷訊回報之後多久內不降碼
  constexpr auto k_fast_outage_pre_margin = std::chrono::milliseconds(500);  ///< 過載比空檔早這麼多才算「斷訊前就在塞」
  constexpr int k_fast_outage_sustained_reports = 6;  ///< 收／送連續這麼多筆（每 500 ms）都有落差＝不是斷訊
  // §ABR-PATH-UPGRADE（11-13）
  constexpr auto k_fast_path_boost = std::chrono::seconds(30);   ///< 換到更好的路之後加速回升多久
  constexpr double k_fast_path_boost_rate = 0.25;                ///< 加速期間每秒回升的比例
  // §ABR-RESTORE-STABLE（11-08）
  constexpr auto k_fast_stable_window = std::chrono::seconds(10);          ///< 撐住多久才算穩定
  constexpr auto k_fast_restore_fail_window = std::chrono::seconds(5);     ///< 拉回後多久內又降碼算拉回失敗

  class fast_controller_t {
  public:
    enum class severity_e {
      moderate,
      fast,
    };

    explicit fast_controller_t(int configured_kbps)
      : configured_kbps_(std::max(1, configured_kbps)),
        cap_kbps_(std::max(1, configured_kbps)),
        applied_kbps_(std::max(1, configured_kbps)),
        current_target_kbps_(std::max(1, configured_kbps)) {}

    void set_configured_kbps(int configured_kbps) {
      if (configured_kbps > 0) {
        configured_kbps_ = configured_kbps;
        if (cap_kbps_ > configured_kbps_) {
          cap_kbps_ = configured_kbps_;
        }
      }
    }

    void set_cap_kbps(int cap_kbps) {
      if (cap_kbps > 0) {
        cap_kbps_ = std::min(configured_kbps_, cap_kbps);
      }
    }

    void set_applied_kbps(int applied_kbps) {
      if (applied_kbps > 0) {
        applied_kbps_ = applied_kbps;
      }
    }

    int configured_kbps() const { return configured_kbps_; }
    int cap_kbps() const { return cap_kbps_; }
    int applied_kbps() const { return applied_kbps_; }
    int current_target_kbps() const { return current_target_kbps_; }
    fast_state_e state() const { return state_; }
    const char *state_str() const { return fast_state_name(state_); }
    const char *last_reason() const { return last_reason_.c_str(); }
    int cuts() const { return cuts_count_; }
    int ramps() const { return ramps_count_; }
    int trims() const { return trims_count_; }
    int random_losses() const { return random_loss_events_; }
    int loss_rate_cuts() const { return loss_rate_cuts_; }
    int loss_rate_reverts() const { return loss_rate_reverts_; }
    /// §ABR-LOSSRATE-VERIFY：撤銷時記下降前／降後的丟包頻率（每 10 秒幾次），給呼叫端印 log
    double loss_rate_pre_per10s() const { return lr_pre_rate_ * 10.0; }
    double loss_rate_post_per10s() const { return lr_post_rate_ * 10.0; }
    bool loss_rate_disabled(steady::time_point now) const { return now < lr_disabled_until_; }
    /// §ABR-PERSIST-VERIFY：判定「丟包跟碼率無關」的次數、停用期間略過的降碼次數、目前是否停用中
    int persistent_reverts() const { return persist_reverts_; }
    int persistent_skips() const { return persist_skips_; }
    int shortfall_outage_skips() const { return shortfall_outage_skips_; }
    bool persistent_disabled(steady::time_point now) const { return now < persist_off_until_; }
    /// 最近一次判定時這一串開始的碼率、開始前與判定時「收到比送出少」的百分比（給呼叫端印 log）
    int persistent_episode_start_kbps() const { return pe_logged_start_kbps_; }
    double persistent_loss_pct_before() const { return pe_logged_f_start_ * 100.0; }
    double persistent_loss_pct_now() const { return pe_logged_f_now_ * 100.0; }
    int persistent_off_minutes(steady::time_point now) const {
      return now < persist_off_until_ ? static_cast<int>(std::chrono::duration_cast<std::chrono::minutes>(persist_off_until_ - now).count()) + 1 : 0;
    }
    /// 有新的判定還沒印過 log 時回 true 一次
    bool take_persistent_revert_event() {
      if (persist_reverts_reported_ == persist_reverts_) return false;
      persist_reverts_reported_ = persist_reverts_;
      return true;
    }

    /// §VR-ABR-OUTAGE：斷訊結束、呼叫端想把碼率拉回斷訊前的值（kbps）。回傳實際允許拉回的值，0＝不拉回。
    /// 只拉回到「斷訊前 10 秒沒有任何降碼、穩定撐住」的碼率（§ABR-RESTORE-STABLE，11-08）：那晚的路只撐得住約 30 Mbps，
    /// 斷訊前的值是開場的 75 Mbps（開場 6 秒就塞），每次拉回 75 都馬上再塞，3 分鐘重複 6 次。拉回後 5 秒內又降碼＝拉回失敗，
    /// 穩定值作廢，要重新撐滿 10 秒才算數。允許拉回時，清掉斷訊期間連刀觸發的防振盪上限、丟包率上限與進行中的試驗
    ///
    /// §VR-ABR-STARTUP-RESTORE（11-09）：還沒有穩定值、而且這一場從沒因為延遲上升降過碼（不是塞車，例如開場一陣無線丟包）
    /// -> 拉回斷訊前的一半。11-09 拳擊開場：75 Mbps 時 0.7 秒內丟包連降三刀到 9.4 Mbps、延遲沒漲，因為沒有穩定值不拉回，
    /// 之後每 0.5 秒 +2% 爬了將近一分鐘、整段畫面偏糊。拉回一半後 5 秒內又降碼＝這一場不再這樣拉
    ///
    /// §VR-ABR-SATURATED-NO-RESTORE（11-09 午）：最近 k_fast_saturated_no_restore 內有過「鏈路飽和」的降碼（送出 ≥ 收到 × 1.05）
    /// -> 這次的「斷訊」是塞車造成的，不拉回。11-09 拳擊：碼率爬到 140 Mbps 塞爆轉接器那條（host「link 2 cannot keep up」），
    /// 降到 75 後被當成斷訊拉回 126、2 秒後又斷，自動半速啟動，最後 1 分半停在 72 Hz
    int on_outage_restored(int kbps, steady::time_point now) {
      if (kbps <= 0) return 0;
      if (last_saturated_cut_ != steady::time_point {} && now >= last_saturated_cut_ && now - last_saturated_cut_ <= k_fast_saturated_no_restore) {
        return 0;
      }
      int ref = stable_kbps_;
      half_restore_ = false;
      if (ref <= 0) {
        if (delay_congestion_seen_ || half_restore_blocked_) return 0;
        ref = kbps / 2;
        half_restore_ = true;
      }
      const int allowed = std::min({kbps, ref, std::max(floor_kbps(), cap_kbps_)});
      if (allowed <= applied_kbps_) return 0;
      kbps = allowed;
      restore_time_ = now;
      penalty_until_ = {};
      loss_cap_until_ = {};
      lr_trial_active_ = false;
      pending_target_.reset();
      pending_restore_.reset();  // §ABR-PERSIST-VERIFY：斷訊拉回有自己的上限（穩定值），不可以再被「拉回一半」蓋過去
      applied_kbps_ = std::min(kbps, std::max(floor_kbps(), cap_kbps_));
      current_target_kbps_ = applied_kbps_;
      if (last_congested_kbps_ > 0 && last_congested_kbps_ < applied_kbps_) {
        last_congested_kbps_ = applied_kbps_;
      }
      state_ = fast_state_e::healthy;
      hold_until_ = now;
      last_reason_ = half_restore_ ? "outage-restore-half" : "outage-restore";
      return applied_kbps_;
    }
    /// 11-13 當天加的四組規則的總開關（見檔頭 ※）。預設關
    void set_experimental_rules(bool on) { experimental_rules_ = on; }
    bool experimental_rules() const { return experimental_rules_; }

    /// §ABR-OUTAGE-HOLD：client 回報了一次成串丟包，涵蓋約 gap_ms。要在 on_loss 之前呼叫。
    /// 回傳 true＝判定是斷訊（接下來 k_fast_outage_hold 內不降碼）
    bool on_outage(int gap_ms, steady::time_point now) {
      if (!experimental_rules_) {
        return false;
      }
      const auto since = owd_active(now) ? owd_overuse_since_ : rtt_overuse_since_;
      const auto gap = std::chrono::milliseconds(std::clamp(gap_ms, 0, 10000));
      // 斷訊結束時積壓的封包一次湧到也會讓趨勢判過載，所以只看「比這段空檔更早」就開始的過載
      const bool pre_congested = since != steady::time_point {} && now > since && now - since > gap + k_fast_outage_pre_margin;
      const bool sustained_shortfall = sat_run_ >= k_fast_outage_sustained_reports;
      if (pre_congested || sustained_shortfall) {
        return false;
      }
      const auto until = now + k_fast_outage_hold;
      if (until > outage_hold_until_) {
        outage_hold_until_ = until;
      }
      ++outage_holds_;
      return true;
    }
    bool outage_hold_active(steady::time_point now) const { return now < outage_hold_until_; }
    int outage_holds() const { return outage_holds_; }
    int outage_hold_skips() const { return outage_hold_skips_; }

    /// §ABR-PATH-UPGRADE：傳輸路徑換成更好的（例如 Tailscale 中繼 → 直連）。舊路徑上學到的東西全部作廢，
    /// 之後一段時間加速回升；碼率本身不跳（新路徑的容量還不知道），由回升去探
    void on_path_upgraded(steady::time_point now) {
      if (!experimental_rules_) {
        return;
      }
      last_congested_kbps_ = 0;
      last_congested_time_ = {};
      penalty_until_ = {};
      loss_cap_until_ = {};
      lr_trial_active_ = false;
      lr_disabled_until_ = {};
      loss_rate_events_.clear();
      stable_kbps_ = 0;
      applied_hist_.clear();
      last_cut_time_ = {};
      first_tick_ = now;
      delay_congestion_seen_ = false;
      half_restore_blocked_ = false;
      last_saturated_cut_ = {};
      pe_last_cut_ = {};
      persist_off_until_ = {};
      pending_restore_.reset();
      persistent_losses_.clear();
      fec_fails_.clear();
      rates_hist_.clear();
      sat_run_ = 0;
      shortfall_reports_ = 0;
      // 延遲基準是舊路徑的（中繼的往返比直連長得多），重新學
      trend_ = delay_trend_t {};
      owd_trend_ = delay_trend_t {};
      owd_have_offset_ = false;
      owd_recent_pos_ = 0;
      last_owd_time_ = {};
      outage_hold_until_ = {};
      owd_overuse_since_ = {};
      rtt_overuse_since_ = {};
      path_boost_until_ = now + k_fast_path_boost;
      ++path_upgrades_;
      if (!pending_target_.has_value() && (state_ == fast_state_e::hold || state_ == fast_state_e::cut || state_ == fast_state_e::healthy)) {
        state_ = fast_state_e::ramp;
        hold_until_ = now;
        ramp_start_ = {};
        next_ramp_ = now;
      }
    }
    int path_upgrades() const { return path_upgrades_; }
    bool path_boost_active(steady::time_point now) const { return now < path_boost_until_; }

    /// §ABR-RESTORE-STABLE：最近一次「10 秒內沒有降碼」時撐住的碼率（0＝還沒有）
    int stable_kbps() const { return stable_kbps_; }
    int last_congested_kbps() const { return last_congested_kbps_; }
    /// 塞車點隨「多久沒再塞車」往上放寬後的值（§ABR-RATCHET）；log 與回升分區用
    int effective_congested_kbps() const { return eff_congested_kbps(); }
    const delay_trend_t &trend() const { return trend_; }
    const delay_trend_t &owd_trend() const { return owd_trend_; }
    bool owd_active(steady::time_point now) const {
      if (last_owd_time_ == steady::time_point{}) return false;
      if (now < last_owd_time_) return true;
      return (now - last_owd_time_) <= k_fast_owd_active_window;
    }
    int recv_kbps() const { return recv_kbps_; }

    /// §ABR-ADAPT（審查）：延遲沒上升的成串丟包要降碼，只限「真的送不動」：有新鮮的收／送速率時必須是鏈路飽和
    /// （送出 ≥ 收到 × 1.05）；成串缺包但收得到（無線空檔）交給 FEC／補包。沒有速率資料（舊 client）時照舊用累計門檻
    bool persistent_allowed(steady::time_point now) const {
      const bool fresh = last_rates_time_ != steady::time_point {} && now >= last_rates_time_ && now - last_rates_time_ <= k_fast_rates_fresh_window &&
                         send_kbps_ > 0;
      if (!fresh) {
        return true;
      }
      return static_cast<double>(send_kbps_) >= static_cast<double>(recv_kbps_) * k_fast_rates_saturated_ratio;
    }
    int send_kbps() const { return send_kbps_; }

    /// 回傳目前是否處於網路壅塞（或剛脫離過載 1.5 秒內），與內部 is_congested(now) 相同
    bool congested(steady::time_point now) const {
      return is_congested(now);
    }

    bool quality_saturated(steady::time_point now) const {
      if (last_quality_time_ == steady::time_point{}) return false;
      if (now < last_quality_time_ || (now - last_quality_time_ > k_fast_quality_recent_window)) {
        return false;
      }
      if (last_codec_ == 2) {
        return false;  // AV1 先不判斷，一律視為未飽和
      }
      return last_qp_x100_ <= k_fast_qp_saturated_x100;
    }

    const char *ramp_block_reason(steady::time_point now) const {
      const auto &t = owd_active(now) ? owd_trend_ : trend_;
      // §ABR-ADAPT（審查）：underuse＝佇列正在排空，也要等它回到 normal 再加碼（設計第 5 節「排空中維持」）
      if (t.state() == trend_state_e::overuse || t.state() == trend_state_e::underuse || t.trend_ms() > 0.5 * t.gamma_ms()) {
        return "delay";
      }
      if (decode_tight_until_ != steady::time_point{} && now <= decode_tight_until_) {
        return "decode";
      }
      if (quality_saturated(now)) {
        return "quality";
      }
      return "none";
    }

    int floor_kbps() const {
      const int f = k_floor_tech_kbps;
      // §REVIEW-1103（B039-2）：設定碼率比技術最低值更低時，下限不可超過設定值
      return configured_kbps_ > 0 ? std::min(f, configured_kbps_) : f;
    }

    int effective_cap_kbps(steady::time_point now) const {
      int cap = cap_kbps_;
      if (penalty_until_ != steady::time_point {} && now < penalty_until_) {
        cap = std::min(cap, penalty_cap_);
      }
      if (loss_cap_until_ != steady::time_point {} && now < loss_cap_until_) {
        cap = std::min(cap, loss_cap_);
      }
      return std::max(floor_kbps(), cap);
    }

    /// 接收 RTT 回報，餵給 delay_trend_t 估計器；進入 overuse 時觸發降碼（OWD 可用時不觸發）
    void on_rtt(int rtt_ms, steady::time_point now) {
      if (rtt_ms < 0) return;
      // §ABR-ADAPT（審查）：次毫秒 LAN 上 ENet 的 RTT 是 0，這是合法樣本（delay_trend 只收正值，用 0.5 ms 代表）
      const auto prev_state = trend_.state();
      trend_.on_rtt(rtt_ms == 0 ? 0.5 : static_cast<double>(rtt_ms), now);
      const auto cur_state = trend_.state();
      if (cur_state != trend_state_e::overuse) {
        rtt_overuse_since_ = {};
      } else if (prev_state != trend_state_e::overuse) {
        rtt_overuse_since_ = now;  // §ABR-OUTAGE-HOLD
      }

      if (!owd_active(now)) {
        if (cur_state == trend_state_e::overuse) {
          if (prev_state != trend_state_e::overuse) {
            trigger_cut(severity_e::fast, "delay-trend", now);
            last_delay_trend_cut_time_ = now;
          } else if (last_delay_trend_cut_time_ == steady::time_point{} ||
                     now - last_delay_trend_cut_time_ >= std::chrono::milliseconds(1000)) {
            trigger_cut(severity_e::fast, "delay-trend", now);
            last_delay_trend_cut_time_ = now;
          }
        }
      }
    }

    /// 接收單向延遲（OWD）回報，餵給 owd_trend_ 估計器；進入 overuse 時觸發降碼
    void on_owd(double owd_ms, steady::time_point now) {
      if (!std::isfinite(owd_ms)) return;
      if (!owd_have_offset_) {
        owd_have_offset_ = true;
        owd_offset_ms_ = owd_ms;
      }
      // 扣除首筆固定時鐘偏移並平移 +1000 ms，確保餵給 delay_trend_t 的相對值恆為正數
      // §ABR-ADAPT（審查）：餘裕 10 s——第一筆若是慢的（大 IDR、開場擁塞），之後較快的幀不會被夾成同一個值
      const double rel_owd_ms = std::max(1.0, (owd_ms - owd_offset_ms_) + 10000.0);
      last_owd_time_ = now;
      owd_recent_[owd_recent_pos_++ % k_fast_owd_recent_n] = rel_owd_ms;
      const auto prev_state = owd_trend_.state();
      owd_trend_.on_rtt(rel_owd_ms, now);
      const auto cur_state = owd_trend_.state();
      if (cur_state != trend_state_e::overuse) {
        owd_overuse_since_ = {};
      } else if (prev_state != trend_state_e::overuse) {
        owd_overuse_since_ = now;  // §ABR-OUTAGE-HOLD
      }

      if (cur_state == trend_state_e::overuse) {
        const bool first = prev_state != trend_state_e::overuse;
        if (first || last_owd_trend_cut_time_ == steady::time_point{} ||
            now - last_owd_trend_cut_time_ >= std::chrono::milliseconds(1000)) {
          last_owd_trend_cut_time_ = now;
          if (owd_spike_drained()) {
            ++owd_stall_skips_;
          } else {
            trigger_cut(severity_e::fast, "owd-trend", now);
          }
        }
      }
    }

    /// §ABR-OWD-STALL：延遲趨勢判過載的這一刻，最近幾張的單向延遲已經回到基準＝無線停頓（一段時間收不到、之後一次全到），
    /// 不是排隊。排隊時佇列要等碼率降下來才會消，判過載時最新的幀一定還是慢的。
    bool owd_spike_drained() const {
      if (owd_recent_pos_ < k_fast_owd_recent_n) return false;
      double v[k_fast_owd_recent_n];
      std::copy(std::begin(owd_recent_), std::end(owd_recent_), v);
      std::nth_element(v, v + k_fast_owd_recent_n / 2, v + k_fast_owd_recent_n);
      const double excess = v[k_fast_owd_recent_n / 2] - owd_trend_.base_ms();
      return excess <= std::max(k_fast_owd_stall_drained_ms, 0.5 * owd_trend_.gamma_ms());
    }

    int owd_stall_skips() const { return owd_stall_skips_; }

    /// §VR-NETSEL-HOLD：頭盔要切網路了。hold_ms 內網路類訊號不降碼（重複通知取最晚的結束時間）
    void begin_net_switch(int hold_ms, steady::time_point now) {
      const auto until = now + std::chrono::milliseconds(std::clamp(hold_ms, 0, k_fast_net_switch_max_ms));
      if (until > net_switch_until_) {
        net_switch_until_ = until;
      }
    }
    bool net_switch_active(steady::time_point now) const { return now < net_switch_until_; }
    int net_switch_skips() const { return net_switch_skips_; }

    /// 接收實際送達速率回報（每 500 ms 一次，單位 kbps）
    void on_rates(int recv_kbps, int send_kbps, steady::time_point now) {
      if (first_rates_time_ == steady::time_point {}) {
        first_rates_time_ = now;  // §VR-ABR-SATURATED-NO-RESTORE：開場起點
      }
      recv_kbps_ = std::max(0, recv_kbps);
      send_kbps_ = std::max(0, send_kbps);
      last_rates_time_ = now;
      // §ABR-PERSIST-VERIFY：留最近幾秒的收／送，判定「降碼有沒有讓丟的比例變小」用
      rates_hist_.push_back({now, send_kbps_, recv_kbps_});
      while (!rates_hist_.empty() && now - rates_hist_.front().at > k_fast_persist_rates_keep) {
        rates_hist_.pop_front();
      }
      if (send_kbps_ > 0 && static_cast<double>(send_kbps_) >= static_cast<double>(recv_kbps_) * k_fast_rates_saturated_ratio) {
        ++sat_run_;
      } else {
        sat_run_ = 0;
      }
      // §ABR-SHORTFALL：收到的明顯少於送出＝鏈路已經塞住在排隊，直接降（降幅由 trigger_cut 依收／送比算）
      if (send_kbps_ >= k_fast_shortfall_min_kbps &&
          static_cast<double>(send_kbps_) >= static_cast<double>(recv_kbps_) * k_fast_shortfall_ratio) {
        if (++shortfall_reports_ >= k_fast_shortfall_reports) {
          shortfall_reports_ = 0;
          // §ABR-PERSIST-VERIFY：收到的不到送出的四分之一＝斷訊，不是排隊（排隊時還是收得到一大半）。
          // 斷訊時降碼沒有用，交給斷訊恢復那一套；真的超出容量好幾倍時，成串丟包與延遲的規則照樣會降。
          // 其餘情況和 +persistent 同一串、同一個判定——降了之後落差沒有變小就不要再降
          if (experimental_rules_ && static_cast<int64_t>(recv_kbps_) * k_fast_shortfall_outage_div < send_kbps_) {
            ++shortfall_outage_skips_;
          } else if (persistent_gate(now)) {
            trigger_cut(severity_e::moderate, "rate-shortfall+delay", now);
          }
        }
      } else {
        shortfall_reports_ = 0;
      }
    }

    /// 接收畫質回報（每 500 ms 一次，codec：0=H.264、1=HEVC、2=AV1）
    void on_quality(int avg_qp_x100, int codec, steady::time_point now) {
      if (avg_qp_x100 < 0) return;  // 沒有資料、忽略

      const bool gap_too_long = (last_quality_time_ != steady::time_point{} &&
                                 now > last_quality_time_ &&
                                 (now - last_quality_time_ > k_fast_quality_recent_window));
      last_quality_time_ = now;
      last_qp_x100_ = avg_qp_x100;
      last_codec_ = codec;

      // 檢查是否符合 quality-trim 條件（codec != 2 且 QP <= 17）
      if (codec != 2 && avg_qp_x100 <= k_fast_qp_trim_threshold_x100) {
        if (qp_low_start_ == steady::time_point{} || gap_too_long) {
          qp_low_start_ = now;
          last_trim_time_ = {};
        }
      } else {
        qp_low_start_ = {};
        last_trim_time_ = {};
      }
    }

    /// 接收 client 解碼負載回報（每 1 s 一次）
    void on_decode(int decode_p95_us, int frame_period_us, steady::time_point now) {
      if (frame_period_us <= 0) return;

      // p95 > 0.85 × period 視為解碼吃緊，有效 3 秒
      if (static_cast<double>(decode_p95_us) > 0.85 * static_cast<double>(frame_period_us)) {
        decode_tight_until_ = now + k_fast_decode_tight_duration;
      } else {
        decode_tight_until_ = {};
      }

      // 連續 2 筆 p95 > 1.0 × period 觸發 moderate 降碼，原因 decode-overload
      if (static_cast<double>(decode_p95_us) > 1.0 * static_cast<double>(frame_period_us)) {
        ++consecutive_decode_overload_;
        if (consecutive_decode_overload_ >= 2) {
          trigger_cut(severity_e::moderate, "decode-overload", now);
          consecutive_decode_overload_ = 0;
        }
      } else {
        consecutive_decode_overload_ = 0;
      }
    }

    /// FEC 救不回回報（丟包分類處理）
    void on_fec_failed(int n, steady::time_point now) {
      if (n <= 0) return;
      clean_old_fec_fails(now);
      fec_fails_.push_back({n, now});
      int total = 0;
      for (const auto &item : fec_fails_) {
        total += item.count;
      }

      if (is_congested(now)) {
        if (total >= 2) {
          trigger_cut(severity_e::fast, "fec-failed-ge-2+delay", now);
        } else if (total == 1) {
          trigger_cut(severity_e::moderate, "fec-failed-1+delay", now);
        }
      } else {
        // 延遲平穩時視為隨機丟包，累計至 persistent 評估
        clean_old_persistent_losses(now);
        persistent_losses_.push_back({n, now});
        int p_sum = get_persistent_loss_sum();
        if (p_sum >= k_fast_persistent_threshold && persistent_gate(now)) {
          persistent_losses_.clear();
          if (total >= 2) {
            trigger_cut(severity_e::fast, "fec-failed-ge-2+persistent", now);
          } else {
            trigger_cut(severity_e::moderate, "fec-failed-1+persistent", now);
          }
        } else {
          note_random_frame_loss(n, now);
        }
      }
    }

    /// LOSS 封包丟失回報（丟包分類處理）
    void on_loss(uint32_t lost_frames, steady::time_point now) {
      if (lost_frames >= 2) {
        if (is_congested(now)) {
          trigger_cut(severity_e::fast, "loss-ge-2+delay", now);
        } else {
          clean_old_persistent_losses(now);
          persistent_losses_.push_back({static_cast<int>(lost_frames), now});
          int p_sum = get_persistent_loss_sum();
          if (p_sum >= k_fast_persistent_threshold && persistent_gate(now)) {
            persistent_losses_.clear();
            trigger_cut(severity_e::fast, "loss-ge-2+persistent", now);
          } else {
            note_random_frame_loss(static_cast<int>(lost_frames), now);
          }
        }
      }
    }

    /// 送影像連線 dropBlock 回報（算 3 權重）
    void on_drop_block(steady::time_point now) {
      if (is_congested(now)) {
        trigger_cut(severity_e::fast, "drop-block+delay", now);
      } else {
        clean_old_persistent_losses(now);
        persistent_losses_.push_back({3, now});
        int p_sum = get_persistent_loss_sum();
        if (p_sum >= k_fast_persistent_threshold && persistent_gate(now)) {
          persistent_losses_.clear();
          trigger_cut(severity_e::fast, "drop-block+persistent", now);
        } else {
          ++random_loss_events_;
        }
      }
    }

    /// LATCH 連續 2 筆 slack < −1 ms -> 中度 (MODERATE)，不改 latch-slack-late 行為
    void on_latch_slack(int64_t slack_us, steady::time_point now) {
      if (slack_us < -1000) {
        ++consecutive_late_slack_;
        if (consecutive_late_slack_ >= 2) {
          trigger_cut(severity_e::moderate, "latch-slack-late", now);
          consecutive_late_slack_ = 0;
        }
      } else {
        consecutive_late_slack_ = 0;
      }
    }

    /// 主連線送達率 < 90% 回報（丟包分類處理）
    void on_delivery(double pct, steady::time_point now) {
      if (pct >= 0.0 && pct < 90.0) {
        if (is_congested(now)) {
          trigger_cut(severity_e::moderate, "delivery-low+delay", now);
        } else {
          clean_old_persistent_losses(now);
          persistent_losses_.push_back({1, now});
          int p_sum = get_persistent_loss_sum();
          if (p_sum >= k_fast_persistent_threshold && persistent_gate(now)) {
            persistent_losses_.clear();
            trigger_cut(severity_e::moderate, "delivery-low+persistent", now);
          } else {
            ++random_loss_events_;
          }
        }
      }
    }

    /// 推進狀態機，返回新目標碼率（若無調整則為 std::nullopt）
    std::optional<int> tick(steady::time_point now) {
      last_tick_ = now;
      update_stable(now);
      // 1. 若有 pending 的降碼要求，立刻回傳並轉入 HOLD 冷卻
      if (pending_target_.has_value()) {
        const int target = *pending_target_;
        pending_target_.reset();
        applied_kbps_ = target;
        state_ = fast_state_e::hold;
        return target;
      }

      // 1'. §ABR-PERSIST-VERIFY：判定 +persistent 無效後的拉回
      if (pending_restore_.has_value()) {
        const int target = std::min(*pending_restore_, effective_cap_kbps(now));
        pending_restore_.reset();
        if (target > applied_kbps_) {
          applied_kbps_ = target;
          current_target_kbps_ = target;
          state_ = fast_state_e::healthy;
          hold_until_ = now;
          last_reason_ = "persistent-revert";
          return target;
        }
      }

      // 1a. §ABR-LOSSRATE-VERIFY：丟包率降碼的試驗到期 -> 判定有沒有效，沒效就撤銷
      if (auto revert = evaluate_loss_rate_trial(now)) {
        return revert;
      }

      // 1b. 上限往下調時立刻壓到上限
      if (const int eff_cap = effective_cap_kbps(now); applied_kbps_ > eff_cap) {
        applied_kbps_ = eff_cap;
        current_target_kbps_ = eff_cap;
        last_reason_ = "cap";
        return eff_cap;
      }

      // 1c. 上限放寬而目前在 HEALTHY、低於上限且未畫質飽和 -> 進 RAMP 慢慢爬回去
      if (state_ == fast_state_e::healthy && applied_kbps_ < effective_cap_kbps(now) && !quality_saturated(now)) {
        state_ = fast_state_e::ramp;
        ramp_start_ = {};
        next_ramp_ = now + get_ramp_interval();
        current_target_kbps_ = applied_kbps_;
      }

      // 2. 若處於 CUT，進入 HOLD
      if (state_ == fast_state_e::cut) {
        state_ = fast_state_e::hold;
      }

      // 3. 冷卻觀測期（HOLD）：未滿 hold_duration 禁止回升
      if (state_ == fast_state_e::hold) {
        if (now >= hold_until_) {
          state_ = fast_state_e::ramp;
          ramp_start_ = now;
          next_ramp_ = now + get_ramp_interval();
        } else {
          return std::nullopt;
        }
      }

      // 3b. 畫質多到用不完時小幅回收（quality-trim，省流量）
      // 連續 >= 10 s 的 QP 樣本都 <= 17 且狀態為 HEALTHY 或 RAMP -> 每 3 s 降 5%
      if ((state_ == fast_state_e::healthy || state_ == fast_state_e::ramp) && check_quality_trim_eligible(now)) {
        if (last_trim_time_ == steady::time_point{} || now - last_trim_time_ >= k_fast_quality_trim_interval) {
          const int eff_floor = floor_kbps();
          // §VR-ABR-TRIM-PROP（2026-11-11）：步幅隨「QP 比門檻低多少」加大。QP 每低 6 碼率約多一倍，所以離門檻
          // gap 個 QP 時碼率約多了 2^(gap/6) 倍；每步只收回其中一半（留餘裕，不要一步收過頭又得回升），再乘原本的 5%，
          // 單步最多收 25%。QP 剛好在門檻時仍是原本的 −5%。實測（桌球、分塊小）開場 QP 約 10：原本 30 秒只從 49 降到
          // 36 Mbps，約一分鐘後才到穩態的 32 Mbps，這段時間頭盔白白多解碼、多耗電。
          const double gap_qp = std::max(0, k_fast_qp_trim_threshold_x100 - last_qp_x100_) / 100.0;
          const double factor = std::max(0.75, 0.95 * std::pow(2.0, -gap_qp / 12.0));
          int new_target = static_cast<int>(std::round(applied_kbps_ * factor));
          new_target = std::max(eff_floor, new_target);
          if (new_target < applied_kbps_) {
            trim_origin_kbps_ = std::max(trim_origin_kbps_, applied_kbps_);
            last_trim_time_ = now;
            applied_kbps_ = new_target;
            current_target_kbps_ = new_target;
            last_reason_ = "quality-trim";
            ++trims_count_;
            return new_target;
          }
        }
      }

      // 4. 平滑回升期（RAMP）
      if (state_ == fast_state_e::ramp) {
        if (now >= next_ramp_) {
          const int eff_cap = effective_cap_kbps(now);
          if (applied_kbps_ >= eff_cap) {
            current_target_kbps_ = eff_cap;
            applied_kbps_ = eff_cap;
            state_ = fast_state_e::healthy;
            return std::nullopt;
          }

          // 依優先序檢查是否被擋住（delay / decode / quality）
          const char *block = ramp_block_reason(now);
          if (std::strcmp(block, "none") != 0) {
            next_ramp_ = now + get_ramp_interval();
            return std::nullopt;
          }

          const auto interval = get_ramp_interval();
          const int step = calculate_ramp_step(interval);
          int new_target = applied_kbps_ + step;
          const char *reason = "ramp-clean";
          // §VR-ABR-TRIM-PROP（回程）：這個碼率是 quality-trim 收下來的（不是網路塞車降的），畫面一變複雜、QP 回到飽和門檻
          // 以上，就照 QP 離「門檻中間值」的距離一步補回去，上限是收之前的碼率——那個碼率幾十秒前網路才剛送得動。
          // 實測（桌球）：載入畫面 QP 約 4，碼率被收到 3 Mbps；進場景後 QP 27，原本的回升 40 秒才到 12 Mbps。
          if (trim_origin_kbps_ > applied_kbps_ && last_qp_x100_ > k_fast_qp_saturated_x100) {
            const double mid_x100 = (k_fast_qp_saturated_x100 + k_fast_qp_trim_threshold_x100) / 2.0;
            const double up = std::pow(2.0, (last_qp_x100_ - mid_x100) / 600.0);
            const int restored = static_cast<int>(std::min<double>(trim_origin_kbps_, std::round(applied_kbps_ * up)));
            if (restored > new_target) {
              new_target = restored;
              reason = "quality-restore";
            }
          }
          if (new_target >= trim_origin_kbps_) {
            trim_origin_kbps_ = 0;
          }
          if (new_target >= eff_cap) {
            new_target = eff_cap;
            state_ = fast_state_e::healthy;
          }
          current_target_kbps_ = new_target;
          applied_kbps_ = new_target;
          last_reason_ = reason;
          next_ramp_ = now + get_ramp_interval();
          ++ramps_count_;
          return new_target;
        }
      }

      return std::nullopt;
    }

  private:
    struct fec_record_t {
      int count;
      steady::time_point at;
    };

    struct loss_record_t {
      int weight;
      steady::time_point at;
    };

    bool is_congested(steady::time_point now) const {
      const bool owd = owd_active(now);
      const auto &t = owd ? owd_trend_ : trend_;
      if (!(t.state() == trend_state_e::overuse || t.overuse_recently(now, k_fast_delay_overuse_recent_window))) {
        return false;
      }
      // §ABR-STALL-LOSS：趨勢判過載，但最近幾張的單向延遲已經回到基準＝一段時間收不到、之後一次全到（停頓），不是排隊。
      // 排隊時佇列要等碼率降下來才會消，最新的幀一定還是慢的
      if (experimental_rules_ && owd && owd_spike_drained()) {
        return false;
      }
      return true;
    }

    bool check_quality_trim_eligible(steady::time_point now) const {
      if (qp_low_start_ == steady::time_point{}) return false;
      if (last_quality_time_ == steady::time_point{}) return false;
      if (now < last_quality_time_ || (now - last_quality_time_ > k_fast_quality_recent_window)) {
        return false;
      }
      return (now >= qp_low_start_) && (now - qp_low_start_ >= k_fast_quality_trim_duration);
    }

    void clean_old_fec_fails(steady::time_point now) {
      while (!fec_fails_.empty() && now - fec_fails_.front().at > k_fast_cut_window) {
        fec_fails_.pop_front();
      }
    }

    /// §ABR-PERSIST-VERIFY：[now - window, now] 內「收到比送出少」的比例（0～1）。沒有資料回 -1。
    /// 紀錄還不滿一個視窗時（開場），缺的那一段當成沒有丟——不然開場第一次斷訊那一筆會把比例灌得很高，
    /// 之後怎麼比都像是「降碼有效」
    double loss_fraction(steady::time_point now, std::chrono::milliseconds window) const {
      double send = 0.0, recv = 0.0;
      int n = 0;
      for (const auto &r : rates_hist_) {
        if (r.at <= now && now - r.at <= window) {
          send += r.send_kbps;
          recv += r.recv_kbps;
          ++n;
        }
      }
      if (n == 0 || send <= 0.0) return -1.0;
      double interval_ms = 500.0;  // client 每 500 ms 回報一次
      if (rates_hist_.size() >= 2) {
        const double span = std::chrono::duration<double, std::milli>(rates_hist_.back().at - rates_hist_.front().at).count();
        if (span > 0.0) interval_ms = span / static_cast<double>(rates_hist_.size() - 1);
      }
      const double expected = std::max(1.0, static_cast<double>(window.count()) / interval_ms);
      const double f = std::clamp(1.0 - recv / send, 0.0, 1.0);
      return n < expected ? f * static_cast<double>(n) / expected : f;
    }

    /// §ABR-PERSIST-VERIFY：+persistent 這一刀可不可以降。呼叫端已確認視窗內的累計權重過門檻。
    /// 回 false 時呼叫端照「隨機丟包」記帳（不降碼）。回 true 之後呼叫端一定接著呼叫 trigger_cut（原因含 persistent），
    /// 刀數與累計降幅在那邊確定真的降了才記。
    bool persistent_gate(steady::time_point now) {
      if (!persistent_allowed(now)) {
        return false;
      }
      if (!experimental_rules_) {
        return true;  // 開關關著：和原本一樣，允許就降
      }
      const double f_now = loss_fraction(now, k_fast_persist_now_window);
      if (now < persist_off_until_) {
        // 停用中。只有「情況變了」才恢復：連續 2 秒每一筆都收得比送的少，而且少的比例比判定當時高出一截
        const bool sustained = sat_run_ >= k_fast_persist_sustained_reports;
        const bool worse = f_now >= 0.0 && f_now >= std::max(2.0 * pe_judged_f_, pe_judged_f_ + 0.05);
        if (!(sustained && worse)) {
          ++persist_skips_;
          persistent_losses_.clear();  // 這一批已經處理過（不降），不要讓視窗裡同一批事件一筆一筆再進來
          return false;
        }
        persist_off_until_ = {};
        pe_last_cut_ = {};
      }
      // 新的一串
      if (pe_last_cut_ == steady::time_point {} || now - pe_last_cut_ > k_fast_persist_episode_gap) {
        pe_start_kbps_ = applied_kbps_;
        pe_start_time_ = now;
        pe_cuts_ = 0;
        pe_ratio_ = 1.0;
        pe_f_start_ = loss_fraction(now, k_fast_persist_before_window);
        pe_prev_congested_kbps_ = last_congested_kbps_;
        pe_prev_congested_time_ = last_congested_time_;
        pe_prev_delay_seen_ = delay_congestion_seen_;
        pe_prev_saturated_cut_ = last_saturated_cut_;
        pe_last_cut_ = now;  // 這一串從現在算起；trigger_cut 真的降了會再更新
      }
      // 已經降了不少、也等過了，丟的比例卻沒有跟著變小 → 跟碼率無關。沒有收／送資料（舊 client）不判定
      if (pe_cuts_ >= k_fast_persist_min_cuts && now - pe_start_time_ >= k_fast_persist_min_trial &&
          pe_ratio_ <= k_fast_persist_ineffective_ratio && pe_f_start_ >= 0.0 && f_now >= 0.0 &&
          f_now >= pe_f_start_ * k_fast_persist_loss_drop_ratio) {
        ++persist_reverts_;
        // 每多判定一次，停用時間加倍（5、10、20、30 分鐘）：這條線路就是這樣，不必每 5 分鐘再試兩刀
        auto off = std::chrono::duration_cast<std::chrono::seconds>(k_fast_persist_off) * (1 << std::min(persist_reverts_ - 1, 3));
        persist_off_until_ = now + std::min<std::chrono::seconds>(off, k_fast_persist_off_max);
        pe_logged_start_kbps_ = pe_start_kbps_;
        pe_logged_f_start_ = pe_f_start_;
        pe_logged_f_now_ = f_now;
        pe_judged_f_ = std::max(f_now, pe_f_start_);
        pe_last_cut_ = {};
        persistent_losses_.clear();
        loss_rate_events_.clear();  // 已判定跟碼率無關：這一串的事件不要再拿去觸發丟包率規則
        // 這一串全是 +persistent 的刀（別種降碼會讓這一串作廢），它們記下的塞車點、防振盪上限、「曾經塞車」都不是真的
        last_congested_kbps_ = pe_prev_congested_kbps_;
        last_congested_time_ = pe_prev_congested_time_;
        delay_congestion_seen_ = pe_prev_delay_seen_;
        last_saturated_cut_ = pe_prev_saturated_cut_;
        penalty_until_ = {};
        // 拉回目標：先前穩定撐住的碼率（有的話，不超過這一串開始時的碼率）；沒有穩定值才拉回開始時的一半
        const int restore = stable_kbps_ > 0 ? std::min(stable_kbps_, pe_start_kbps_) :
                                               static_cast<int>(pe_start_kbps_ * k_fast_persist_restore_ratio);
        // 還沒套用的那一刀若也屬於這一串就不必降了；別的規則排的降碼要留著
        const bool pending_is_persistent = pending_target_.has_value() &&
                                           (last_reason_.find("persistent") != std::string::npos || last_reason_ == "rate-shortfall+delay");
        if (pending_is_persistent) {
          pending_target_.reset();
        }
        if (restore > applied_kbps_ && !pending_target_.has_value()) {
          pending_restore_ = restore;
        }
        return false;
      }
      return true;
    }

    void clean_old_persistent_losses(steady::time_point now) {
      while (!persistent_losses_.empty() && now - persistent_losses_.front().at > k_fast_persistent_window) {
        persistent_losses_.pop_front();
      }
    }

    int get_persistent_loss_sum() const {
      int sum = 0;
      for (const auto &item : persistent_losses_) {
        sum += item.weight;
      }
      return sum;
    }

    std::chrono::milliseconds get_hold_duration() const {
      double base = trend_.base_ms();
      if (base <= 0.0) {
        return std::chrono::milliseconds(2000);
      }
      int hold_ms = std::clamp(static_cast<int>(std::round(8.0 * base)), 600, 2000);
      return std::chrono::milliseconds(hold_ms);
    }

    bool is_multiplicative_ramp() const {
      return last_congested_kbps_ == 0 || applied_kbps_ < static_cast<int>(0.8 * last_congested_kbps_);
    }

    std::chrono::milliseconds get_ramp_interval() const {
      double base = trend_.base_ms();
      int base_int = (base > 0.0) ? static_cast<int>(std::round(4.0 * base)) : 250;
      int interval_ms = std::clamp(base_int, 250, 1000);
      if (!is_multiplicative_ramp()) {
        interval_ms = 1000;  // 接近／超過上次塞車點：每秒最多一步
      }
      return std::chrono::milliseconds(interval_ms);
    }

    int calculate_ramp_step(std::chrono::milliseconds interval) const {
      if (is_multiplicative_ramp()) {
        double sec = interval.count() / 1000.0;
        // §ABR-PATH-UPGRADE：剛換到更好的路，新容量還不知道，探快一點
        const double rate = last_tick_ < path_boost_until_ ? k_fast_path_boost_rate : 0.08;
        int step = static_cast<int>(std::round(applied_kbps_ * rate * sec));
        return std::max(50, step);  // §ABR-ADAPT（審查）：窄路徑上 200 kbps 下限會讓每秒回升遠超過 8%
      } else {
        // 11-04 桌球實測：舊的每 0.5 s +2%（約 4%/s）9 秒就從 54 衝回 75 Mbps 再塞一次，形成鋸齒、每輪丟幾張
        const int lc = eff_congested_kbps();
        const double frac = (lc > 0 && applied_kbps_ >= lc) ? 0.0025 : 0.005;
        int step = static_cast<int>(std::round(applied_kbps_ * frac));
        return std::max(50, step);
      }
    }

    // §ABR-RATCHET（11-04 第 3 場桌球）：每次塞車都把塞車點記成當下（較低）的碼率，網路一時變差就一路往下記
    // （67→59→55→52→50 Mbps），之後只能在越來越低的點附近慢爬。改成：30 秒沒再塞車後，「超過塞車點就再減半」的
    // 那條界線每秒往上放寬 0.1%（最多 +50%）。乘法快爬的分界仍用原本的塞車點（不然塞車後幾秒就又全速衝回去）。
    // 真的塞車時照樣重新記。
    int eff_congested_kbps() const {
      if (last_congested_kbps_ <= 0 || last_congested_time_ == steady::time_point {} || last_tick_ <= last_congested_time_) {
        return last_congested_kbps_;
      }
      const double quiet_s = std::chrono::duration<double>(last_tick_ - last_congested_time_).count() - 30.0;
      if (quiet_s <= 0.0) {
        return last_congested_kbps_;
      }
      return static_cast<int>(last_congested_kbps_ * std::min(1.5, 1.0 + 0.001 * quiet_s));
    }

    void note_congested(int kbps, steady::time_point now) {
      if (last_congested_time_ != steady::time_point {} && now - last_congested_time_ <= k_fast_congest_episode) {
        last_congested_kbps_ = std::max(last_congested_kbps_, kbps);
      } else {
        last_congested_kbps_ = kbps;
      }
      last_congested_time_ = now;
    }

    /// 延遲平穩時救不回的幀：照舊算隨機丟包，但 20 秒內太多次就視為跟碼率有關（§ABR-LOSSRATE）
    void note_random_frame_loss(int n, steady::time_point now) {
      if (net_switch_active(now)) {
        return;  // §VR-NETSEL-HOLD：切網路時的掉包不算進丟包率
      }
      if (outage_hold_active(now)) {
        return;  // §ABR-OUTAGE-HOLD：斷訊那一下的掉包同樣不算
      }
      ++random_loss_events_;
      if (pending_restore_.has_value()) {
        return;  // §ABR-PERSIST-VERIFY：剛判定這批丟包跟碼率無關、正要拉回，不要同一筆又觸發丟包率降碼
      }
      while (!loss_rate_events_.empty() && now - loss_rate_events_.front() > k_fast_loss_rate_window) {
        loss_rate_events_.pop_front();
      }
      (void) n;  // 一筆回報算一次：要的是「多常出事」，同一筆裡幾張由 persistent 規則處理
      loss_rate_events_.push_back(now);
      // §ABR-LOSSRATE-VERIFY：試驗中只計數、不再降；規則停用中也不降
      if (lr_trial_active_) {
        if (now >= lr_trial_start_ + k_fast_loss_rate_settle) {
          ++lr_trial_events_;
        }
        return;
      }
      if (now < lr_disabled_until_) return;
      if (now < persist_off_until_) return;  // §ABR-PERSIST-VERIFY：已判定丟包跟碼率無關，這條規則同樣不降
      if (static_cast<int>(loss_rate_events_.size()) < k_fast_loss_rate_threshold) return;
      // 要「持續」：最早一筆至少 10 秒前，短時間的密集丟包交給 persistent／延遲規則
      if (now - loss_rate_events_.front() < k_fast_loss_rate_window / 2) return;
      if (applied_kbps_ <= k_fast_loss_rate_min_kbps) return;
      if (now < hold_until_ || pending_target_.has_value()) return;  // 剛降過，等它生效再看
      // 降碼前的頻率：視窗內 n 筆、首尾相隔 span → (n-1)/span（用 n/span 會高估，撤銷門檻就變鬆）
      const double span_s = std::chrono::duration<double>(now - loss_rate_events_.front()).count();
      const double pre_rate = span_s > 0.0 ? static_cast<double>(loss_rate_events_.size() - 1) / span_s : 0.0;
      loss_rate_events_.clear();
      const int before = applied_kbps_;
      const int prev_congested = last_congested_kbps_;
      const auto prev_congested_time = last_congested_time_;
      trigger_cut(severity_e::moderate, "loss-rate", now);
      if (pending_target_.has_value()) {
        lr_trial_active_ = true;
        lr_trial_start_ = now;
        lr_trial_events_ = 0;
        lr_pre_rate_ = pre_rate;
        lr_before_kbps_ = before;
        lr_prev_congested_kbps_ = prev_congested;
        lr_prev_congested_time_ = prev_congested_time;
        // trigger_cut 的中度降碼＝×0.90；不要因為這條規則掉到 10 Mbps 以下
        pending_target_ = std::max(*pending_target_, std::min(before, k_fast_loss_rate_min_kbps));
        current_target_kbps_ = *pending_target_;
        loss_cap_ = *pending_target_;
        loss_cap_until_ = now + k_fast_loss_rate_cap_duration;
        note_congested(before, now);
        ++loss_rate_cuts_;
      }
    }

    /// §ABR-LOSSRATE-VERIFY：試驗到期時判定。降後頻率 ≥ 降前 × 0.6 → 撤銷並停用規則，回傳要套用的碼率
    std::optional<int> evaluate_loss_rate_trial(steady::time_point now) {
      if (!lr_trial_active_ || now < lr_trial_start_ + k_fast_loss_rate_trial) {
        return std::nullopt;
      }
      lr_trial_active_ = false;
      const double window_s = std::chrono::duration<double>(k_fast_loss_rate_trial - k_fast_loss_rate_settle).count();
      lr_post_rate_ = static_cast<double>(lr_trial_events_) / window_s;
      if (lr_post_rate_ < lr_pre_rate_ * k_fast_loss_rate_effective_ratio) {
        return std::nullopt;  // 有效：保留上限，規則照常
      }
      ++loss_rate_reverts_;
      lr_disabled_until_ = now + k_fast_loss_rate_off;
      loss_cap_until_ = {};
      loss_rate_events_.clear();
      // 這一刀記下的塞車點不是真的塞車，還原
      last_congested_kbps_ = lr_prev_congested_kbps_;
      last_congested_time_ = lr_prev_congested_time_;
      last_reason_ = "loss-rate-revert";
      // 正在塞車就不往上拉（之後由 RAMP 依塞車點回升）
      if (is_congested(now) || state_ == fast_state_e::cut || now < hold_until_) {
        return std::nullopt;
      }
      const int target = std::min(lr_before_kbps_, effective_cap_kbps(now));
      if (target <= applied_kbps_) {
        return std::nullopt;
      }
      applied_kbps_ = target;
      current_target_kbps_ = target;
      state_ = fast_state_e::healthy;
      return target;
    }

    /// §ABR-RESTORE-STABLE：10 秒內沒有降碼時，穩定值＝這 10 秒內套用過的最低碼率（回升中途的值也算，取最低最保守）
    void update_stable(steady::time_point now) {
      if (first_tick_ == steady::time_point {}) {
        first_tick_ = now;
      }
      if (applied_hist_.empty() || applied_hist_.back().second != applied_kbps_) {
        applied_hist_.emplace_back(now, applied_kbps_);
      }
      // 只留視窗內的紀錄，外加視窗起點當下生效的那一筆
      while (applied_hist_.size() >= 2 && now - applied_hist_[1].first >= k_fast_stable_window) {
        applied_hist_.pop_front();
      }
      const auto quiet_since = std::max(first_tick_, last_cut_time_);
      if (now - quiet_since < k_fast_stable_window) {
        return;
      }
      int lowest = applied_kbps_;
      for (const auto &h : applied_hist_) {
        lowest = std::min(lowest, h.second);
      }
      stable_kbps_ = lowest;
    }

    void trigger_cut(severity_e sev, const char *reason, steady::time_point now) {
      // 降碼後的冷卻期（hold_until_ 之前）只接受嚴重訊號：送達率每 250 ms、LATCH 每 100 ms 就回報一次，
      // 中度訊號不擋的話一兩秒內會連降好幾次直接壓到下限（HOLD 期間只有嚴重訊號能二度打折）
      if (sev == severity_e::moderate && now < hold_until_) {
        return;
      }
      // §VR-NETSEL-HOLD：頭盔正在切網路——丟包、延遲、送達率都是切換本身造成的，不是塞車。解碼過載也擋：
      // 11-08 晚實測接回家用 Wi-Fi 時畫面一次湧進來，解碼 p95 跟著拉高，decode-overload 連降五刀 70→11 Mbps
      if (net_switch_active(now)) {
        ++net_switch_skips_;
        return;
      }
      // §ABR-OUTAGE-HOLD：剛判定是斷訊——丟包、延遲、收送落差都是斷訊本身造成的。解碼過載與 latch 不是網路訊號，照常
      if (outage_hold_active(now) && std::strcmp(reason, "decode-overload") != 0 && std::strcmp(reason, "latch-slack-late") != 0) {
        ++outage_hold_skips_;
        return;
      }
      // §ABR-PERSIST-VERIFY：真的要降了 → 還沒套用的「拉回一半」作廢（不然下一個 tick 會把這一刀蓋掉）
      pending_restore_.reset();
      path_boost_until_ = {};  // §ABR-PATH-UPGRADE：探到頂了，回到平常的回升速度
      // 同一串＝+persistent 與 rate-shortfall（兩者的依據都是「收到的比送出的少」）
      const bool persistent_cut = std::strstr(reason, "persistent") != nullptr || std::strcmp(reason, "rate-shortfall+delay") == 0;
      if (!persistent_cut) {
        pe_last_cut_ = {};  // 這一串中間有別種降碼（真塞車、解碼過載…）→ 作廢，之後重新開始算
      }
      const int pe_applied_before = applied_kbps_;
      // §ABR-RESTORE-STABLE：拉回後 5 秒內又降碼＝拉回的值撐不住，穩定值作廢
      if (restore_time_ != steady::time_point {} && now - restore_time_ <= k_fast_restore_fail_window) {
        stable_kbps_ = 0;
        if (half_restore_) {
          half_restore_blocked_ = true;  // §VR-ABR-STARTUP-RESTORE：拉回一半也撐不住
        }
      }
      // §VR-ABR-STARTUP-RESTORE：延遲上升造成的降碼＝真的塞車，這一場沒有穩定值時不再拉回
      // §REVIEW-1111：開場 10 秒內的 rate-shortfall+delay 不算。那個原因只看「送出比收到多」，沒有看延遲趨勢
      // （+delay 只是分類用的後綴），而開場的 IDR 與起步突發本來就會讓送出暫時大於收到——下面的飽和判斷已經把這段
      // 排除，這裡卻先一步把旗標設上，整場之後的斷訊恢復都不再拉回。
      const bool startup_shortfall = std::strcmp(reason, "rate-shortfall+delay") == 0 &&
                                     first_rates_time_ != steady::time_point {} &&
                                     now - first_rates_time_ < k_fast_saturated_startup_grace;
      if (!startup_shortfall && (std::strstr(reason, "delay") != nullptr || std::strstr(reason, "owd") != nullptr)) {
        delay_congestion_seen_ = true;
      }
      restore_time_ = {};
      last_cut_time_ = now;
      // §ABR-LOSSRATE-VERIFY：試驗期間有真的塞車降碼 -> 這次試驗作廢（比較基準被干擾），上限照留
      if (lr_trial_active_ && std::strcmp(reason, "loss-rate") != 0) {
        lr_trial_active_ = false;
      }

      // 防振盪：RAMP 中 5 秒內又降碼 -> 上限暫時壓到 applied × 0.85 維持 30 秒
      if (state_ == fast_state_e::ramp && ramp_start_ != steady::time_point {} &&
          now - ramp_start_ <= k_fast_anti_osc_window) {
        penalty_cap_ = std::max(floor_kbps(), static_cast<int>(applied_kbps_ * k_fast_anti_osc_cap_factor));
        penalty_until_ = now + k_fast_anti_osc_duration;
      }

      // 記住最後一次因 delay / persistent / owd 降碼前的 applied
      // 11-04：同一次塞車常連降兩刀（例：owd-trend 75→63.5，1.6 s 後 fec-failed+delay 63.5→54），
      // 舊寫法第二刀把塞車點覆寫成 63.5，回升時就不知道真正塞住的是 75。5 秒內的後續刀只取較大值。
      if (!startup_shortfall && (std::strstr(reason, "delay") != nullptr ||
                                 std::strstr(reason, "persistent") != nullptr ||
                                 std::strstr(reason, "owd") != nullptr)) {
        note_congested(applied_kbps_, now);
      }

      int new_target = applied_kbps_;
      bool is_delay_trend = (std::strcmp(reason, "delay-trend") == 0 || std::strcmp(reason, "owd-trend") == 0);
      bool is_congestion_cut = (is_delay_trend ||
                                std::strstr(reason, "+delay") != nullptr ||
                                std::strstr(reason, "+persistent") != nullptr);

      // 檢查鏈路是否飽和（1 秒內有新鮮 rates 且 send_kbps >= recv_kbps * 1.05）
      bool has_saturated_rates = false;
      double saturated_factor = 0.85;
      if (is_congestion_cut &&
          last_rates_time_ != steady::time_point{} &&
          now >= last_rates_time_ &&
          (now - last_rates_time_ <= k_fast_rates_fresh_window) &&
          send_kbps_ > 0 &&
          recv_kbps_ >= 0 &&
          static_cast<double>(send_kbps_) >= static_cast<double>(recv_kbps_) * k_fast_rates_saturated_ratio) {
        has_saturated_rates = true;
        // §VR-ABR-SATURATED-NO-RESTORE：開場 10 秒內不算（IDR 與起步的突發讓送出暫時大於收到，11-09 拳擊開場那一刀就是）；
        // 之後的飽和降碼＝真的塞車：6 秒內不拉回，這一場也不再「沒有穩定值時拉回一半」
        if (first_rates_time_ != steady::time_point {} && now - first_rates_time_ >= k_fast_saturated_startup_grace) {
          last_saturated_cut_ = now;
          delay_congestion_seen_ = true;
        }
        double raw_f = 0.95 * static_cast<double>(recv_kbps_) / static_cast<double>(send_kbps_);
        saturated_factor = std::clamp(raw_f, 0.50, 0.85);
      }

      if (sev == severity_e::fast) {
        if (last_fast_cut_time_ != steady::time_point {} && now - last_fast_cut_time_ <= k_fast_cut_window) {
          // 同一秒內第二次嚴重降碼
          if (has_saturated_rates) {
            double factor = std::min(saturated_factor, 0.70);
            new_target = static_cast<int>(std::round(fast_cut_base_applied_ * factor));
          } else {
            float factor = is_delay_trend ? 0.70f : 0.60f;
            new_target = static_cast<int>(fast_cut_base_applied_ * factor);
          }
        } else {
          // 視窗內第一次嚴重降碼
          fast_cut_base_applied_ = applied_kbps_;
          if (has_saturated_rates) {
            new_target = static_cast<int>(std::round(applied_kbps_ * saturated_factor));
          } else {
            float factor = is_delay_trend ? 0.85f : 0.75f;
            new_target = static_cast<int>(applied_kbps_ * factor);
          }
          last_fast_cut_time_ = now;
        }
      } else {
        // 中度降碼
        if (has_saturated_rates) {
          new_target = static_cast<int>(std::round(applied_kbps_ * saturated_factor));
        } else {
          new_target = static_cast<int>(applied_kbps_ * 0.90f);
        }
      }

      const int eff_cap = effective_cap_kbps(now);
      const int eff_floor = floor_kbps();
      new_target = std::clamp(new_target, eff_floor, eff_cap);

      if (persistent_cut && pe_last_cut_ != steady::time_point {}) {
        // 這一串實際降了一刀：刀數與累計降幅（只算 +persistent 自己降的，quality-trim 之類不算）
        pe_last_cut_ = now;
        ++pe_cuts_;
        if (pe_applied_before > 0) {
          pe_ratio_ *= static_cast<double>(new_target) / static_cast<double>(pe_applied_before);
        }
      }

      state_ = fast_state_e::cut;
      last_reason_ = reason;
      pending_target_ = new_target;
      current_target_kbps_ = new_target;
      hold_until_ = now + get_hold_duration();
      ++cuts_count_;
      trim_origin_kbps_ = 0;  // 網路降的碼率不可以用 quality-restore 一步補回去
    }

    int configured_kbps_ = 0;
    int cap_kbps_ = 0;
    int applied_kbps_ = 0;
    int current_target_kbps_ = 0;
    fast_state_e state_ = fast_state_e::healthy;
    std::string last_reason_ = "none";
    std::optional<int> pending_target_;

    // 嚴重降碼同秒判定
    steady::time_point last_fast_cut_time_ {};
    int fast_cut_base_applied_ = 0;

    // 冷卻與回升定時
    steady::time_point hold_until_ {};
    steady::time_point ramp_start_ {};
    steady::time_point next_ramp_ {};

    // 防振盪懲罰
    int penalty_cap_ = 0;
    steady::time_point penalty_until_ {};

    // 統計與壅塞記錄
    int cuts_count_ = 0;
    int ramps_count_ = 0;
    int trims_count_ = 0;
    int random_loss_events_ = 0;
    int last_congested_kbps_ = 0;
    steady::time_point last_congested_time_ {};
    steady::time_point last_tick_ {};

    // §ABR-LOSSRATE
    std::deque<steady::time_point> loss_rate_events_;
    int loss_cap_ = 0;
    steady::time_point loss_cap_until_ {};
    int loss_rate_cuts_ = 0;

    // §ABR-LOSSRATE-VERIFY
    bool lr_trial_active_ = false;
    steady::time_point lr_trial_start_ {};
    int lr_trial_events_ = 0;
    double lr_pre_rate_ = 0.0;   ///< 降碼前每秒幾次
    double lr_post_rate_ = 0.0;  ///< 試驗期間每秒幾次
    int lr_before_kbps_ = 0;
    int lr_prev_congested_kbps_ = 0;
    steady::time_point lr_prev_congested_time_ {};
    steady::time_point lr_disabled_until_ {};
    int loss_rate_reverts_ = 0;

    // §ABR-PERSIST-VERIFY
    steady::time_point pe_last_cut_ {};       ///< 這一串最近一次 +persistent 降碼
    steady::time_point pe_start_time_ {};     ///< 這一串第一刀的時刻
    int pe_start_kbps_ = 0;                   ///< 這一串開始時的碼率
    int pe_cuts_ = 0;                         ///< 這一串已經降了幾刀
    int pe_prev_congested_kbps_ = 0;
    steady::time_point pe_prev_congested_time_ {};
    steady::time_point persist_off_until_ {};
    std::optional<int> pending_restore_;
    int persist_reverts_ = 0;
    int persist_skips_ = 0;
    int pe_logged_start_kbps_ = 0;
    double pe_ratio_ = 1.0;                   ///< 這一串 +persistent 累計降到開始時的幾成
    double pe_f_start_ = -1.0;                ///< 這一串開始前「收到比送出少」的比例（-1＝沒有資料）
    double pe_judged_f_ = 0.0;                ///< 判定無效當時的比例（停用期間用來看情況有沒有變）
    double pe_logged_f_start_ = 0.0;
    double pe_logged_f_now_ = 0.0;
    bool pe_prev_delay_seen_ = false;
    steady::time_point pe_prev_saturated_cut_ {};
    int persist_reverts_reported_ = 0;
    bool experimental_rules_ = false;         ///< 見 set_experimental_rules
    steady::time_point path_boost_until_ {};  ///< §ABR-PATH-UPGRADE：這之前回升加速
    // §ABR-OUTAGE-HOLD
    steady::time_point outage_hold_until_ {};
    steady::time_point owd_overuse_since_ {};  ///< 單向延遲趨勢這一次進入過載的時刻（不在過載＝空）
    steady::time_point rtt_overuse_since_ {};
    int outage_holds_ = 0;
    int outage_hold_skips_ = 0;
    int path_upgrades_ = 0;
    struct rates_record_t {
      steady::time_point at;
      int send_kbps;
      int recv_kbps;
    };
    std::deque<rates_record_t> rates_hist_;
    int sat_run_ = 0;                         ///< 連續幾筆回報收得比送的少（≥ 5%）

    // §ABR-RESTORE-STABLE
    steady::time_point first_tick_ {};
    steady::time_point last_cut_time_ {};
    steady::time_point restore_time_ {};
    std::deque<std::pair<steady::time_point, int>> applied_hist_;
    int stable_kbps_ = 0;
    bool delay_congestion_seen_ = false;  ///< §VR-ABR-STARTUP-RESTORE：這一場曾因延遲上升降碼
    steady::time_point last_saturated_cut_ {};
    steady::time_point first_rates_time_ {};  ///< 第一次收到送達速率（≈ 串流開始）  ///< §VR-ABR-SATURATED-NO-RESTORE：最近一次鏈路飽和時的降碼
    bool half_restore_ = false;           ///< 最近一次拉回是「沒有穩定值、拉回一半」
    bool half_restore_blocked_ = false;   ///< 拉回一半後很快又降碼：這一場不再拉回一半

    // 丟包與延遲追蹤
    std::deque<fec_record_t> fec_fails_;
    std::deque<loss_record_t> persistent_losses_;

    int consecutive_late_slack_ = 0;
    steady::time_point last_delay_trend_cut_time_{};
    delay_trend_t trend_;

    // §ABR-ADAPT 第 2、3 期新增追蹤狀態
    bool owd_have_offset_ = false;
    double owd_offset_ms_ = 0.0;
    steady::time_point last_owd_time_{};
    double owd_recent_[k_fast_owd_recent_n] = {};
    std::size_t owd_recent_pos_ = 0;
    int owd_stall_skips_ = 0;
    steady::time_point net_switch_until_ {};
    int net_switch_skips_ = 0;
    steady::time_point last_owd_trend_cut_time_{};
    delay_trend_t owd_trend_;

    steady::time_point last_rates_time_{};
    int recv_kbps_ = 0;
    int send_kbps_ = 0;
    int shortfall_reports_ = 0;  ///< §ABR-SHORTFALL：連續幾次收／送落差超過門檻
    int shortfall_outage_skips_ = 0;  ///< 落差大到算斷訊而沒降的次數

    steady::time_point last_quality_time_{};
    int last_qp_x100_ = -1;
    int last_codec_ = -1;
    steady::time_point qp_low_start_{};
    steady::time_point last_trim_time_{};
    int trim_origin_kbps_ = 0;  ///< §VR-ABR-TRIM-PROP：這一串 quality-trim 開始前的碼率（0＝沒有）；壅塞降碼時歸零

    steady::time_point decode_tight_until_{};
    int consecutive_decode_overload_ = 0;
  };

}  // namespace vr::abr
