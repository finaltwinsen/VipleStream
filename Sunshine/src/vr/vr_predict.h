/**
 * @file src/vr/vr_predict.h
 * @brief VipleStream 2.0 §VR-PREDICT：依 client 量到的姿態落後，調整 HMD 的 Prop_SecondsFromVsyncToPhotons。
 *
 * SteamVR 讓遊戲把頭部姿態預測到「虛擬 vsync＋SecondsFromVsyncToPhotons」。串流時畫面還要再經過編碼、
 * 傳輸、解碼、頭盔合成才顯示，若只填一個顯示週期，遊戲用的姿態會比頭盔實際顯示時落後；頭盔的重投影
 * 只修轉動、不修平移，近處物體就會隨頭部移動晃動（2026-10-03 Frame 實測：120 Hz 下落後約 33 ms，
 * 填 44 ms 後降到 0～3 ms）。
 *
 * client 每秒在 CLIENT_TIMING 回報落後中位數（正值＝落後、負值＝超前，只算頭部有移動、第一次顯示的幀）。
 * 樣本不到「串流半秒的幀數」的視窗不用（min_samples_for：夾在 20～60；串流 120 Hz 以上是 60、90 Hz 45、72 Hz 36、
 * 60 Hz 30）；每個視窗先夾到 ±20 ms。單一視窗的雜訊約 ±3.5 ms（第六輪 a 實測），所以：
 *
 * 1. 探測（還不知道 SteamVR 是否採用中途改值）：3 個視窗平均出落後 L0；|L0| < 1.5 ms 就不動、繼續量。
 *    否則往補償方向改至少 8 ms（≤ 10 ms），等 2 s 再取 3 個視窗平均 L1；落後往預期方向移動至少步長的
 *    一半才算採用。不採用：退回起始值、這個 session 停調，下一個 session 從「起始值＋(L0＋L1)/2」開始。
 * 2. 追蹤（已確認採用；同一個行程之後的 session 直接從這裡開始）：落後的指數移動平均（α＝0.25，時間
 *    常數約 4 s）≥ 1 ms 時補一半、每秒最多 3 ms，並依「v2p 加 1 ms、落後減 1 ms」修正平均值。第六輪 a
 *    的第一版每 2 s 照單一視窗補 60%，預測值在 40～50 ms 間來回跳，使用者覺得畫面有一點「呼吸感」。
 *
 * 一個 session 最多偏離起始值 ±40 ms。收斂值存在行程內（服務重啟後回到預設）。設定檔
 * vr_vsync_to_photons_us > 0 時固定、不調。純模組（不碰 bridge／log），可單元測試；呼叫端負責送 driver 與記 log。
 *
 * 2026-10-05 修正（Frame 第六～八輪）：
 *   - 暖機：每個 session 前 k_warmup_windows 個可靠視窗不用（遊戲載入、host 掉拍時落後亂跳；第六輪的探測就落在這段）。
 *   - 穩定度：探測前、探測後的 3 個視窗最大差 > 6 ms 視為不穩——探測前不探測、重量；探測後判不出來就退回起始值、重量
 *     （action_e::inconclusive），不再直接判成「不採用」。
 *   - 「不採用」只停這個 session；連續兩個 session 都判不採用，才當成 runtime 不採用（之後的 session 只量、不探測）。
 *     舊版判一次就整個行程放棄，第七輪根本沒探測。
 *   - 只量不探測的結果改回報 action_e::learned（舊版借用 not_honored 的訊息，log 看起來像又探測了一次）。
 *   - 學到的值依更新率分開存（週期差 ≤ 3% 視為同一個）；沒學過的更新率用最接近的那一筆換算：
 *     v' = F + (v − F)·T'/T，F＝k_fixed_part_us（10-04 兩點：120 Hz 約 44.6 ms、90 Hz 約 54.7 ms → F≈14 ms）。
 *     舊版只有一個值，120 Hz 學到的 38.7 ms 被拿去給 90 Hz 用，第七輪落後 16 ms。
 *     2026-10-10 起存的鍵是（host 的週期, 頭盔的顯示週期）（全速串流時兩者相同），「最接近」與 T 看的都是顯示週期；
 *     顯示週期相同、只是串流幀率不同（全速↔半速）的那一筆直接沿用、不換算（原因見下）。
 *
 * 2026-10-10 修正（半速串流 §VR-HALF-RATE：頭盔照樣 120 Hz 顯示，host 的 SteamVR、遊戲、編碼跑 60 Hz；兩場配戴場次的 log）：
 *   - 樣本數門檻原本固定 60。client 只對「第一次顯示的新影像、而且頭部有移動」取樣，60 Hz 串流每秒最多 60 筆，配戴實測
 *     每秒 41～45 筆（每 10 s 407～451）→ 整場沒有一個視窗被採用、完全不調整。門檻改成跟著串流幀率（串流半秒的幀數，
 *     夾在 20～60）。串流 120 Hz 以上的場次不變；90 Hz、72 Hz 的全速場次門檻跟著變寬（45、36）——這是這次唯一會影響
 *     非半速場次的改動。
 *   - 起始值的換算式是 10-04 用「頭盔的顯示更新率」120 Hz 與 90 Hz 兩點定出來的。半速只有 host 的週期加倍、頭盔的顯示
 *     週期沒變，不適用：120 Hz 學到的 33.7 ms 被換成 53.4 ms，那一場整場姿態超前約 23 ms（client 的 [VIPLE-VR-POSEERR]
 *     lag p50 −19.7～−25.5 ms；同一個行程先跑的全速場次在 0 ms 上下、追蹤在 34.1～36.4 ms）。由這一場反推半速該用約
 *     31 ms、另一場較短的約 36 ms——和全速學到的值接近，不是加倍。沒學過任何值時的預設「host 的週期＋30 ms」在半速是
 *     46.7 ms，同樣超前十幾毫秒。所以 begin_session 多收頭盔的顯示週期（/launch 的 vrDispHz；沒帶＝和 host 的週期
 *     相同）：換算與預設值（顯示週期＋30 ms，半速 38.3 ms）都改看它，同一個顯示更新率學到的值直接沿用。
 *     不帶 vrDispHz 的舊 client 開半速時 host 分不出來（看起來就是 60 Hz 的頭盔），起始值照舊偏大，要靠追蹤收回來
 *     （門檻修正之後才追得動）。
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace vr::predict {

  constexpr uint32_t k_min_us = 4000;
  constexpr uint32_t k_max_us = 100000;
  constexpr uint32_t k_default_extra_us = 30000;  ///< 沒有學到的值時：一個顯示週期＋30 ms
  constexpr int16_t k_lag_none = 0x7FFF;  ///< 同 VIPLE_VR_POSE_LAG_NONE
  constexpr int k_warmup_windows = 10;  ///< 每個 session 開頭丟掉的可靠視窗數
  constexpr int32_t k_unstable_spread_us = 6000;  ///< 3 個視窗的最大差超過這個值＝不穩
  constexpr int32_t k_fixed_part_us = 14000;  ///< 跨更新率換算時不隨週期縮放的部分
  constexpr uint16_t k_min_samples_lo = 20;  ///< 樣本數門檻（min_samples_for）的下限
  constexpr uint16_t k_min_samples_hi = 60;  ///< 上限：串流 120 Hz 以上都是這個值（2026-10-10 之前一律用它）

  enum class start_e {
    fixed,  ///< 設定檔指定
    learned,  ///< 這個（host 的週期, 顯示週期）上一個 session 收斂（或推算）的值
    converted,  ///< 由別的顯示更新率學到的值換算（from_display_us → 這一場的顯示週期）
    initial,  ///< 預設：一個顯示週期＋30 ms
    same_display,  ///< 沿用同一個顯示更新率、不同串流幀率學到的值（全速↔半速），不換算；來源的串流週期在 from_period_us
  };

  struct start_t {
    uint32_t us = 0;
    start_e source = start_e::initial;
    uint32_t from_period_us = 0;  ///< converted／same_display：來源那一筆的 host（串流）週期
    uint32_t from_display_us = 0;  ///< converted／same_display：來源那一筆的顯示週期（換算用的是這個）
  };

  /// 編排器建 session 設定時呼叫：回傳這個 session 一開始的值並重設控制狀態。period_us＝host（串流）的週期；fixed_us > 0＝固定；
  /// display_period_us＝頭盔的顯示週期（半速串流時是 period_us 的一半），0＝和 period_us 相同。
  start_t begin_session(uint32_t period_us, uint32_t fixed_us, uint32_t display_period_us = 0);

  /// 跨顯示更新率換算學到的值：v' = F + (v − F)·T'/T（T＝頭盔的顯示週期，F＝k_fixed_part_us），夾在 [k_min_us, k_max_us]
  uint32_t convert_for_period(uint32_t v2p_us, uint32_t from_period_us, uint32_t to_period_us);

  /// 一個 1 s 視窗至少要有幾筆樣本才採用：串流半秒的幀數 round(0.5e6 / period_us)，夾在 [k_min_samples_lo, k_min_samples_hi]；
  /// period_us＝0 時用上限。period_us 是 host（串流）的週期：120 Hz → 60、90 Hz → 45、72 Hz → 36、60 Hz → 30
  uint16_t min_samples_for(uint32_t period_us);

  enum class action_e {
    update,  ///< 送新值給 driver（to_us）。quiet＝追蹤階段的小幅修正（只進 10 s 摘要，不逐筆記 log）
    honored,  ///< 探測確認 SteamVR 採用中途改值（不必送 driver）；之後進入追蹤
    not_honored,  ///< 中途改值無效：送回起始值（to_us）、本 session 停調；next_session_us 是下一個 session 的起始值。
                  ///< runtime_ignored＝這是連續第二次，之後的 session 只量不探測
    inconclusive,  ///< 探測後的視窗不穩、判不出來：送回起始值（to_us）、重新量後再探測
    learned,  ///< runtime 已知不採用：只量，next_session_us 是下一個 session 的起始值（不必送 driver）
  };

  struct result_t {
    action_e action = action_e::update;
    uint32_t from_us = 0;
    uint32_t to_us = 0;
    uint32_t next_session_us = 0;
    int32_t lag_us = 0;  ///< 判斷依據：探測是 3 個視窗的平均，追蹤是移動平均
    int32_t lag_after_us = 0;  ///< honored／not_honored：探測後 3 個視窗的平均
    bool quiet = false;
    bool runtime_ignored = false;  ///< not_honored：連續第二次，之後只量不探測
    int32_t spread_us = 0;  ///< inconclusive：探測後 3 個視窗的最大差
  };

  /// 收到 CLIENT_TIMING 時呼叫（任何執行緒）。lag_100us：落後中位數（0.1 ms），count：有效樣本數
  /// （不到這個 session 的 min_samples_for 就不用）。
  std::optional<result_t> on_pose_lag(int16_t lag_100us, uint16_t count, std::chrono::steady_clock::time_point now);

  /// update 沒送到 driver（bridge 沒連線）時呼叫：退回 from_us；探測重新量，追蹤階段的修正不算數
  void revert(const result_t &r);

  /// 追蹤階段每 10 s 的摘要（沒有新視窗時不回）
  struct summary_t {
    uint32_t now_us = 0;
    uint32_t min_us = 0;
    uint32_t max_us = 0;
    int32_t ema_us = 0;
    uint32_t updates = 0;
    uint32_t windows = 0;
  };

  std::optional<summary_t> take_summary(std::chrono::steady_clock::time_point now);

  /// 測試用：清掉行程內學到的值與控制狀態
  void reset_for_test();

}  // namespace vr::predict
