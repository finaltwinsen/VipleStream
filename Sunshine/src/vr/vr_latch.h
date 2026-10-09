/**
 * @file src/vr/vr_latch.h
 * @brief VipleStream 2.0 §VR（M4a R3）：LATCH 相位回授的頻率鎖控制器（純函式，可單元測試）。
 *
 * client 每 100 ms 送一則 0x5507/08 LATCH（`slackUs`：串流幀比 XR latch 早到多少，正值＝早到）。
 * slack 太大代表幀在 client 端白等（多了延遲），太小或負值代表幀常趕不上 latch（會重用舊幀）。
 * 控制器把 slack 拉向目標值：slack 偏大就讓 server 的虛擬 vsync 稍微放慢（ppm > 0，週期變長，
 * 之後的幀漸漸晚到），偏小就加快。輸出的 ppm 夾在 ±VR_LATCH_PPM_MAX（設計：±200 ppm），由
 * server 換成 pacing.period_q32 交給 driver；driver 端對週期本身不再 slew，所以上限只由這裡保證。
 *
 * 這是頻率回授：兩台機器的時鐘差（通常數十 ppm）由積分項吸收，比例項處理相位誤差。
 *
 * §VR-LATCH-V2（2026-10-05，Frame 實測）：client 量到的 slack 其實是「取模一個週期」的相位——晚到 δ 的幀
 * 會被下一個 latch 接走，量成 T−δ，不會是負值。legacy 把它當線性量（夾在 ±T、對 slack 做 EMA、輸出飽和時
 * 照樣積分），結果 ppm 長時間頂在 +200、每 35～60 s 整格滑移一次（每次 MTP 與姿態落後跳一個週期），
 * 鎖住時 T/4 的目標餘裕也讓約每秒一張幀趕不上。v2：誤差在上側折返（e ≥ T/2 時減 T，負值夾在 −T/2，
 * 協定「負值＝晚到」的語意不變）、EMA 作用在誤差上、離群值與重複 frameId 丟棄、更新率改變時重來、
 * 條件式 anti-windup、目標改成週期的 40%（25～50% 可設）、增益提高（阻尼比約 0.4→0.67）。
 * 模式由 config `vr_latch_mode` 決定（預設 legacy，等 Frame A/B 後再改預設）。
 *
 * §VR-LATCH-V3（2026-10-08，Frame 三場配戴）：v2 把每一筆樣本各自折返再做 EMA，樣本落在 latch 邊緣兩側時平均值
 * 沒有意義；開場遊戲開始出幀時 host 管線多 4～5 ms，±200 ppm 每秒只能移 0.15～0.25 ms，相位繞過邊緣一整圈才收斂；
 * 黑幀（每 100 ms 一張）與 host 沒在出幀時的樣本也照樣進控制器；LATCH 一停，學到的時鐘差（積分）每秒減半。
 * v3（`vr_latch_mode = v3`；v2 與 legacy 的路徑不變、LATCH 仍是 12 B）：
 *   - 相位＝最近幾筆 slack 取模後的環狀中位數；誤差＝到目標的最短路徑（±T/2），靠近反相點時鎖住方向不來回翻。
 *   - 節拍閘門：最近 5 個 LATCH 間隔裡 frameId 前進的張數不到協商幀率的七成、或有一個間隔超過 300 ms，就不取樣本、
 *     輸出只留積分項。影像停住、client 只重送同一個 frameId 時也一樣（那幾則照樣記成「前進 0 張」的間隔，閘門一關
 *     就把比例項拿掉）；閘門還開著時，單獨一則重複的 LATCH 只是不當新樣本。
 *   - 積分保留：LATCH 停 2 s 只把比例項歸零，停滿 60 s 才衰減；穩定追蹤滿 60 s 的積分記在行程內（remember／recall），
 *     下一個 session 當起點。
 *   - 選配（options_t::acquire，config `vr_latch_acquire`）：誤差大時把週期暫時改 ±step_ppm 一段算好的時間（burst），
 *     一次把相位移到位。driver 本來就每格整份採用 server 給的週期（只檢查 ±5%），所以走的是同一條路徑，只是數值變大；
 *     burst 期間與之後 250 ms 不取樣本、不寫 pacing（盲區）。成敗只看最後有沒有進到門檻內，失敗只退避、不停用。
 *     一步之後、量完之前閘門關了或 LATCH 停了：那一步不評估（kEvalVoid，不算成敗），之後從新的一輪開始。
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace vr::latch {

  constexpr int32_t VR_LATCH_PPM_MAX = 200;

  /// §VR-LATCH-V2：控制器版本
  enum class mode_e : uint8_t {
    legacy,  ///< M4a R3 原版
    v2,  ///< 相位取模、anti-windup、目標 0.4T
    v3,  ///< §VR-LATCH-V3：環狀中位數＋最短路徑、節拍閘門、積分保留（選配定時 burst）
  };

  inline const char *mode_name(mode_e m) {
    switch (m) {
      case mode_e::legacy:
        return "legacy";
      case mode_e::v2:
        return "v2";
      case mode_e::v3:
        return "v3";
    }
    return "?";
  }

  // §VR-LATCH-V3：result_t::phase 的值
  constexpr uint8_t kPhaseWait = 0;  ///< 閘門關著（黑幀、host 沒在出幀、LATCH 剛開始或剛停過）：不取樣本
  constexpr uint8_t kPhaseMeasure = 1;  ///< 閘門開了，正在收第一批樣本
  constexpr uint8_t kPhaseBurst = 2;  ///< 定時 burst 與它的盲區
  constexpr uint8_t kPhaseTrack = 3;  ///< PI 追蹤

  inline const char *phase_name(uint8_t phase) {
    switch (phase) {
      case kPhaseWait:
        return "wait";
      case kPhaseMeasure:
        return "measure";
      case kPhaseBurst:
        return "burst";
      case kPhaseTrack:
        return "track";
    }
    return "?";
  }

  // §VR-LATCH-V3：result_t::step_why 的值（0＝這一筆沒有 burst）
  constexpr uint8_t kStepAcq = 1;  ///< 第一批樣本量完，誤差超過進入門檻
  constexpr uint8_t kStepTrim = 2;  ///< 上一步之後還差超過修整門檻：再補一步
  constexpr uint8_t kStepReacq = 3;  ///< 追蹤中誤差連續幾筆超過進入門檻（負載改變）

  inline const char *step_why_name(uint8_t why) {
    switch (why) {
      case kStepAcq:
        return "acq";
      case kStepTrim:
        return "trim";
      case kStepReacq:
        return "reacq";
    }
    return "none";
  }

  // §VR-LATCH-V3：result_t::eval_outcome 的值
  constexpr uint8_t kEvalTrack = 0;  ///< 進到門檻內（或這一輪的步數用完但誤差在進入門檻內）：開始追蹤
  constexpr uint8_t kEvalTrim = 1;  ///< 還差超過修整門檻：再補一步
  constexpr uint8_t kEvalBackoff = 2;  ///< 這一輪走完誤差仍在進入門檻外：算一次失敗，退避後再試
  /// 這一步量不到結果：burst 之後、量完之前閘門關了（host 掉拍、黑幀、影像停住）或 LATCH 停了。之後量到的相位和這一步
  /// 無關，不拿來評估、也不算成敗；這一輪到此為止，下一次從新的一輪開始。eval_moved_us／eval_err_us 沒有意義（0）。
  constexpr uint8_t kEvalVoid = 3;

  inline const char *eval_outcome_name(uint8_t outcome) {
    switch (outcome) {
      case kEvalTrack:
        return "track";
      case kEvalTrim:
        return "trim";
      case kEvalBackoff:
        return "backoff";
      case kEvalVoid:
        return "void";
    }
    return "?";
  }

  /// §VR-LATCH-V3：configure() 的選配項（只有 v3 會用；reset() 保留）
  struct options_t {
    bool acquire = false;  ///< vr_latch_acquire：誤差大時用定時 burst 把相位一次移到位
    int step_ppm = 4000;  ///< vr_latch_acquire_ppm：burst 相對基準的週期修正量（夾在 kV3StepPpmMin～kV3StepPpmMax）
    bool have_seed = false;  ///< seed_integral_ppm 有效（上一個 session 學到的積分）
    double seed_integral_ppm = 0;
  };

  struct result_t {
    bool update = false;  ///< 要不要更新 pacing（ppm 變化夠大、或距上次送出 ≥ 1 s）
    int32_t ppm = 0;  ///< 建議的週期修正（正值＝週期變長）
    double slack_ema_us = 0;  ///< 平滑後的 slack（v2：目標＋平滑後的誤差）
    double target_us = 0;  ///< 目前的目標 slack
    double err_us = 0;  ///< 平滑後的誤差（正值＝幀太早到）；legacy 為 slack_ema − target
    double integral_ppm = 0;  ///< 積分項（ppm）
    uint64_t slips = 0;  ///< 相位滑移次數（相鄰兩筆新量測跳超過 kSlipPeriods 個週期）
    uint64_t dropped = 0;  ///< v2 丟掉的樣本（重複 frameId、離群值）
    mode_e mode = mode_e::legacy;

    // §VR-LATCH-V3（legacy／v2 一律是初值）
    uint8_t phase = kPhaseWait;  ///< kPhase*
    bool gate_open = false;  ///< 節拍閘門（最近 5 個 LATCH 間隔的幀率夠、沒有長空檔）
    uint32_t steps = 0;  ///< 累計的 burst 次數
    uint32_t bad_steps = 0;  ///< 累計的失敗輪數（一輪步進走完誤差仍在進入門檻外）
    double center_us = 0;  ///< 最近一次算出的相位（環狀中位數，0～T）

    // §VR-LATCH-V3 定時 burst：burst_us > 0 時，呼叫端先寫 burst_ppm、burst_us 之後改回 ppm（＝基準）
    int32_t burst_ppm = 0;
    uint32_t burst_us = 0;
    uint8_t step_why = 0;  ///< kStep*
    double step_err_us = 0;  ///< 決定這一步時的誤差（最短路徑）
    // 上一步的結果（每一步最多帶出一次：量完的那一筆，或確定量不到的那一筆；update 為 false 時也可能有。
    // 換更新率、switch_mode() 時走到一半的那一步直接丟掉，不帶出）
    bool eval_valid = false;
    double eval_moved_us = 0;  ///< 量到的相位移動量（與 eval_cmd_us 同號＝往要求的方向動）；只供 log，不拿來判成敗
    double eval_cmd_us = 0;  ///< 這一步要求的移動量
    double eval_err_us = 0;  ///< 這一步之後的誤差
    uint8_t eval_outcome = kEvalTrack;  ///< kEval*（kEvalVoid 時 eval_moved_us／eval_err_us 是 0，沒有量測值）
  };

  /// §VR-LATCH-V3：circ_median() 的結果
  struct circ_stat_t {
    double center_us = 0;  ///< 環狀中位數（0～period）
    double mad_us = 0;  ///< 各樣本到中位數的環狀距離的中位數（分散程度）
  };

  /**
   * @brief §VR-LATCH-V3：取模一個週期的相位樣本的環狀中位數。
   *
   * 參考點取「到其他樣本的環狀距離總和最小」的那一筆，其餘樣本換成相對它的最短路徑偏移（±period/2）再取中位數。
   * 樣本落在 0／period 邊緣兩側、或混進一兩筆晚到被下一個 latch 接走的樣本，結果都不會被拉走。n == 0 回 {0, 0}。
   */
  circ_stat_t circ_median(const double *xs, std::size_t n, double period_us);

  class controller_t {
  public:
    /**
     * @brief session 開始時設定（config：vr_latch_mode、vr_latch_target_pct）。不呼叫＝legacy。
     * @param host_period_ns 協商的 host 虛擬 vsync 週期（0＝不知道）；v2 的模數取它與顯示週期的較小值
     * @param options §VR-LATCH-V3 的選配項；v3 且 have_seed 時積分從 seed_integral_ppm 開始（夾在 ±kIntegralMax）
     */
    void configure(mode_e mode, int target_pct, uint32_t host_period_ns, const options_t &options = {});

    /**
     * @brief 處理一則 LATCH。
     * @param slack_us LATCH.slackUs
     * @param display_period_ns LATCH.displayPeriodNs（0 或不合理時沿用上次，預設 11.1 ms）
     * @param now_ns 單調時鐘（ns）
     * @param frame_id LATCH.frameId（v2：與上一筆相同＝client 沒有新幀、重送舊值，不當新量測）
     */
    result_t on_latch(int32_t slack_us, uint32_t display_period_ns, int64_t now_ns, uint32_t frame_id = 0);

    /**
     * @brief 沒有新 LATCH 時定期呼叫（例如 control 迴圈的 1 s tick）：超過 2 s 沒有 LATCH 就讓 ppm 衰減回 0，
     *        避免 client 暫停送 LATCH（XR 失焦、斷線）時 server 一直維持舊的修正。
     *        v3：超過 2 s 沒有 LATCH 只把比例項歸零（輸出＝積分項）、回到等待狀態；停滿 kV3IdleDecayNs 才每次把積分減半。
     *        走到一半的那一輪步進到此為止（結果帶 kEvalVoid）。
     */
    result_t on_idle(int64_t now_ns);

    /**
     * @brief §VR-FENCE-LATE：收到一則 LATCH，但這一筆不能當相位量測（host 的擷取端回報過載：遊戲把 GPU 壓滿，
     *        影像晚到是因為 GPU 排隊，不是節拍）。呼叫端在那段期間以它取代 on_latch()。
     *
     * 光是「不呼叫 on_latch」不夠：on_idle() 會把它當成 LATCH 停了——legacy／v2 停 2 s 就每秒把輸出與積分減半，
     * v3 停 2 s 把比例項拿掉、停滿 kV3IdleDecayNs 連積分也開始減半（過載可以持續整場）。這裡把「最近一則 LATCH」
     * 的時刻與 frameId 照常記下（過載期間量的那一張被 client 重送時仍然當重複丟掉；legacy／v2 恢復後第一筆樣本的
     * 積分 dt 是距上一則 LATCH 多久，不會被算成整段暫停），其餘三種模式都一樣：**輸出只留積分（學到的時鐘差），
     * 比例項拿掉，積分不動**。
     *   - 比例項不留的理由：過載旗標設起來之前已經有幾筆過載的樣本進了控制器，v2 的增益下 3 筆就把輸出推到上限、
     *     積分只動 2 ppm 上下。把那個輸出原樣凍結，相位會以最多 200 ppm（每秒 0.2 ms）一路漂到過載結束，
     *     恢復時要從任意的相位重新鎖；只留積分的話虛擬 vsync 照學到的時鐘差走，相位留在原地附近。
     *     （同 on_latch_v3 對「影像停住」的處理、on_idle() 對「LATCH 停了」的處理。）
     *   - legacy／v2：誤差的平滑值作廢，恢復後從第一筆新樣本重新開始。
     *   - v3：視同節拍閘門關著——回到等待、樣本與閘門的紀錄清掉；走到一半的那一輪步進到此為止（kEvalVoid）。
     *     恢復後要重新收滿 kV3GateN 個間隔、再量 kV3AcqN 筆才動相位。盲區內照舊不寫 pacing。重新進追蹤的第一筆，
     *     PI 的 dt 和閘門關過之後一樣是夾出來的 1 s（見 last_pi_ns_ 的說明）。
     * 和平常一樣約每秒寫一次 pacing（例行重送）。
     * 顯示週期不在這裡更新。過載期間換了更新率：v3 恢復後第一則 on_latch 照常偵測到並重來；legacy／v2 的平滑值
     * 本來就已經作廢，積分（時鐘差，和更新率無關）留著。
     * @param frame_id LATCH.frameId
     */
    result_t on_hold(int64_t now_ns, uint32_t frame_id = 0);

    int32_t last_sent_ppm() const {
      return sent_ppm_;
    }

    uint64_t latch_count() const {
      return count_;
    }

    void reset();

    // 參數（公開給單元測試）
    static constexpr double kEmaAlpha = 0.2;  ///< 10 Hz 取樣下約 0.5 s 的時間常數
    static constexpr double kKp = 0.05;  ///< ppm／µs：誤差 4 ms 時飽和
    static constexpr double kKi = 0.004;  ///< ppm／(µs·s)
    static constexpr double kIntegralMax = 150.0;  ///< 積分項上限（ppm）
    static constexpr int32_t kMinDelta = 2;  ///< ppm 變化 ≥ 這個值才更新
    static constexpr int64_t kRefreshNs = 1'000'000'000;  ///< 即使沒變化也每 1 s 更新一次
    static constexpr int64_t kStaleNs = 2'000'000'000;  ///< 超過這麼久沒 LATCH 就衰減

    /// 目標 slack：顯示週期的 1/4，夾在 [1, 4] ms（legacy）
    static double target_for_period(uint32_t display_period_ns);

    // §VR-LATCH-V2 參數
    static constexpr double kKpV2 = 0.12;  ///< ppm／µs
    static constexpr double kKiV2 = 0.008;  ///< ppm／(µs·s)
    static constexpr double kOutlierPeriods = 1.5;  ///< |slack| 超過 1.5 個週期的樣本丟掉
    static constexpr double kSlipPeriods = 0.6;  ///< 相鄰兩筆新量測跳超過 0.6 個週期＝一次相位滑移
    static constexpr double kPeriodChange = 0.02;  ///< 顯示週期變化超過 2%（換更新率）就重來
    static constexpr int kTargetPctMin = 25;
    static constexpr int kTargetPctMax = 50;
    static constexpr int kTargetPctDefault = 40;

    // §VR-LATCH-V3 參數（增益沿用 kKpV2／kKiV2）
    static constexpr int kTargetPctMaxV3 = 65;  ///< v3 的目標上限（v2 仍是 kTargetPctMax）
    static constexpr std::size_t kV3MedianN = 7;  ///< 追蹤時取最近幾筆的環狀中位數
    static constexpr std::size_t kV3AcqN = 5;  ///< 閘門開了（或一次 burst 之後）先收幾筆才算出相位
    static constexpr std::size_t kV3BufMax = 16;  ///< 最多留幾筆樣本
    static constexpr std::size_t kV3GateN = 5;  ///< 節拍閘門看最近幾個 LATCH 間隔
    static constexpr double kV3GateRatio = 0.7;  ///< 這幾個間隔裡 frameId 前進的張數至少要有協商幀率的幾成
    static constexpr int64_t kV3GateGapNs = 300'000'000;  ///< 其中任何一個間隔超過這麼久，閘門就關
    static constexpr double kV3DirLatchFrac = 0.35;  ///< |誤差| 超過這個比例的週期：鎖住方向（靠近反相點時不來回翻）
    static constexpr double kV3DirReleaseFrac = 0.15;  ///< |誤差| 回到這個比例內：放開方向
    static constexpr int64_t kV3IdleDecayNs = 60'000'000'000;  ///< LATCH 停這麼久之後積分才開始衰減
    static constexpr int64_t kV3SettledNs = 3'000'000'000;  ///< 進入追蹤滿這麼久才算穩定（unsettled()）
    static constexpr int64_t kV3LearnTrackNs = 60'000'000'000;  ///< 連續追蹤滿這麼久，積分才可以帶到下一個 session
    static constexpr double kV3LearnMaxPpm = 120.0;  ///< 積分的移動平均超過這個值不帶（不像時鐘差）

    // §VR-LATCH-V3 定時 burst（options_t::acquire）
    static constexpr int kV3StepPpmMin = 1000;
    static constexpr int kV3StepPpmMax = 5000;
    static constexpr double kV3StepEnterFrac = 0.15;  ///< 進入門檻：|誤差| 超過這個比例的週期才步進（120 Hz＝1250 µs）
    static constexpr double kV3TrimFrac = 0.12;  ///< 修整門檻：一步之後還差這麼多就再補一步（120 Hz＝1000 µs）
    static constexpr uint32_t kV3ReacqRun = 5;  ///< 追蹤中誤差連續幾筆超過進入門檻才重新步進
    static constexpr int64_t kV3StepMaxUs = 1'500'000;  ///< 單次 burst 最長多久
    static constexpr int64_t kV3SettleNs = 250'000'000;  ///< burst 結束後再等多久才取樣本（管線裡還有舊節拍的幀）
    /// 一輪最多幾步（第一步＋一次修整）。一輪最多移 kV3MaxIter × kV3StepMaxUs × step_ppm＝3 µs／ppm；從反相點（T/2）進到
    /// 進入門檻內要移 0.35T，所以 step_ppm 低於 0.35T ÷ 3（120 Hz 972、90 Hz 1296、72 Hz 1620 ppm）時一輪走不完：
    /// 那一輪會記成失敗，剩下的交給退避期間的 PI（±200 ppm）與退避之後的下一輪。預設 4000、退路 2000 都夠
    /// （單元測試 V3LowStepPpmCostsOneBackoff）。
    static constexpr uint8_t kV3MaxIter = 2;
    static constexpr int64_t kV3StepGapNs = 1'500'000'000;  ///< 追蹤中重新步進：距上一次 burst 結束至少這麼久
    static constexpr int64_t kV3BackoffNs = 5'000'000'000;  ///< 一輪失敗後多久不步進（每次失敗加倍）
    static constexpr int64_t kV3BackoffMaxNs = 60'000'000'000;
    static constexpr std::size_t kV3StepsPerMin = 8;  ///< 任何 60 s 內最多幾步
    static constexpr int64_t kV3StepWindowNs = 60'000'000'000;
    static constexpr double kV3MadMaxFrac = 1.0 / 6.0;  ///< 樣本的分散程度（MAD）超過這個比例的週期：相位還不可信，再量

    mode_e mode() const {
      return mode_;
    }

    /// v2 的模數（µs）：host 虛擬 vsync 週期與顯示週期的較小值
    double modulus_us() const;

    /// v2 的目標 slack（µs）；v3 也用同一個定義（模數 × 目標百分比）
    double target_v2_us() const;

    // ── §VR-LATCH-V3 ──

    /// v3 的相位還沒穩定（不是追蹤狀態、或進入追蹤未滿 kV3SettledNs）。legacy／v2 恆為 false。
    bool unsettled(int64_t now_ns) const;

    /// v3 連續追蹤滿 kV3LearnTrackNs 之後的積分移動平均（可以當下一個 session 的起點）；其他情況 nullopt
    std::optional<double> learned(int64_t now_ns) const;

    /**
     * @brief 同一個 session 裡換模式或目標（同場 A/B 用）：積分、最後送出的 ppm、顯示週期保留，
     *        誤差的平滑值、v3 的樣本與閘門清掉，v3 從等待狀態重新開始。
     *        走到一半的 burst 不再評估（不帶出結果），盲區不取消：換過去的模式是 v3 時，照舊等 bridge 把 burst 走完、
     *        盲區過了才重新取樣本；v2／legacy 不看盲區，它們第一次寫 pacing 時 bridge 會把還沒結束的 burst 取消掉。
     */
    void switch_mode(mode_e mode, int target_pct);

  private:
    result_t decide(int64_t now_ns, int32_t ppm);
    result_t on_latch_legacy(int32_t slack_us, int64_t now_ns);
    result_t on_latch_v2(int32_t slack_us, int64_t now_ns, uint32_t frame_id);
    void fill_observability(result_t &r) const;
    void count_slip(double slack_us, double period_us);

    // §VR-LATCH-V3
    result_t on_latch_v3(int32_t slack_us, int64_t now_ns, uint32_t frame_id);
    result_t on_idle_v3(int64_t now_ns);
    result_t hold() const;  ///< 不更新：ppm＝最後送出的值
    result_t v3_gate_closed(int64_t now_ns);  ///< 閘門關著：回到等待、輸出只留積分；走到一半的那一輪步進到此為止
    bool v3_abort_round();  ///< 這一輪步進到此為止（不評估、不算成敗）；回傳有沒有一步還沒量結果
    result_t start_step(double err_us, double center_us, int64_t now_ns, uint8_t why);
    bool can_step(int64_t now_ns) const;
    int64_t stream_period_ns() const;  ///< 串流的幀週期：協商的 host 虛擬 vsync 週期（半速串流時是顯示週期的兩倍），不知道就用顯示週期
    void v3_to_wait();  ///< 回到等待狀態：清樣本與方向鎖存（閘門的紀錄、積分、步進的退避都不動）
    void v3_clear_gate();  ///< 清閘門的紀錄（閘門關上，要再收滿 kV3GateN 個間隔才會開）
    void v3_push_sample(double phase_us);
    circ_stat_t v3_center(std::size_t last_n) const;  ///< 最近 last_n 筆樣本的環狀中位數

    // 設定（reset() 保留）
    mode_e mode_ = mode_e::legacy;
    int target_pct_ = kTargetPctDefault;
    uint32_t host_period_ns_ = 0;
    options_t options_ {};

    bool have_ema_ = false;
    double ema_ = 0;  ///< legacy：slack 的 EMA；v2：誤差的 EMA
    double integral_ = 0;
    uint32_t period_ns_ = 11'111'111;
    int64_t last_latch_ns_ = 0;
    int64_t last_sent_ns_ = 0;
    int32_t sent_ppm_ = 0;
    bool sent_once_ = false;
    uint64_t count_ = 0;

    bool have_prev_slack_ = false;
    double prev_slack_us_ = 0;
    uint64_t slips_ = 0;
    uint64_t dropped_ = 0;
    bool have_frame_ = false;
    uint32_t last_frame_id_ = 0;

    // §VR-LATCH-V3 狀態（legacy／v2 不讀不寫）
    struct gate_rec_t {
      int64_t dt_ns = 0;  ///< 與上一則 LATCH 的間隔
      uint32_t adv = 0;  ///< 這段間隔裡 frameId 前進了幾張（重複的 frameId＝0）
    };

    uint8_t phase_ = kPhaseWait;
    std::array<gate_rec_t, kV3GateN> gate_ {};  ///< 環狀：最近 kV3GateN 個 LATCH 間隔
    std::size_t gate_n_ = 0;
    std::size_t gate_head_ = 0;
    bool gate_open_ = false;
    std::array<double, kV3BufMax> buf_ {};  ///< 環狀：最近的相位樣本（slack 取模到 0～T）
    std::size_t buf_n_ = 0;
    std::size_t buf_head_ = 0;
    int dir_ = 0;  ///< 方向鎖存：0＝沒鎖，±1＝誤差維持這個符號
    /// 上一次 PI 更新的時刻（0＝還沒有）。離開追蹤時不歸零：重新進追蹤的第一筆，積分的 dt 是「距上一次更新多久」夾在
    /// 1 s（同 v2 的寫法）——那一筆最多積 Ki × 誤差 × 1 s（誤差 1500 µs 時 12 ppm）。
    int64_t last_pi_ns_ = 0;
    int64_t track_since_ns_ = 0;
    double integ_avg_ = 0;  ///< 追蹤中積分的移動平均（時間常數約 10 s）
    double err_v3_ = 0;  ///< 最近一次算出的誤差（方向鎖存之後）
    double center_ = 0;  ///< 最近一次算出的相位

    // §VR-LATCH-V3 定時 burst
    int64_t blind_until_ns_ = 0;  ///< 盲區：在這之前不取樣本、不寫 pacing
    int64_t last_step_end_ns_ = 0;  ///< 最近一次 burst 結束的時刻（steps_ > 0 才有意義）
    int64_t no_step_until_ns_ = 0;  ///< 失敗後的退避
    int64_t backoff_ns_ = kV3BackoffNs;
    double step_c_before_ = 0;  ///< 步進前的相位
    double step_cmd_us_ = 0;  ///< 這一步要求的移動量（帶符號）
    bool step_pending_ = false;  ///< 有一步還沒量結果
    uint8_t iters_ = 0;  ///< 這一輪已經走了幾步
    uint32_t reacq_run_ = 0;  ///< 追蹤中誤差連續超過進入門檻的筆數
    uint32_t steps_ = 0;
    uint32_t bad_steps_ = 0;
    std::array<int64_t, kV3StepsPerMin> step_times_ {};  ///< 環狀：最近幾次步進的時刻（60 s 內的步數上限）
  };

  // ── §VR-LATCH-V3：行程內記住的積分（上一個 session 學到的時鐘差，下一個 session 當起點）──
  // 依 host 的幀週期分開存（週期差 3% 以內視為同一個，同 §VR-PREDICT）；服務重啟後從 0 開始。任何執行緒都可以呼叫。

  /// 記下這個週期學到的積分（夾在 ±kIntegralMax；週期為 0 或數值不是有限值時忽略）
  void remember(uint32_t period_ns, double integral_ppm);

  /// 取出這個週期記下的積分（沒有就 nullopt）
  std::optional<double> recall(uint32_t period_ns);

  /// 測試用：清掉行程內記住的積分
  void reset_learned_for_test();

  // ── §VR-LATCH-V3：同場 A/B（config `vr_latch_ab`）──

  /// `vr_latch_ab = <秒>:<模式>:<target%>`：每隔這麼多秒在設定的模式與這裡指定的模式之間切換一次
  struct ab_spec_t {
    int seconds = 0;  ///< 每一段的長度（20～600）
    mode_e mode = mode_e::legacy;  ///< 另一段用的模式
    int target_pct = 0;  ///< 另一段的目標（v3：25～65；其他：25～50）
  };

  /// 解析 `vr_latch_ab`；格式或範圍不對回 nullopt（呼叫端警告後當成沒設）
  std::optional<ab_spec_t> parse_ab(std::string_view spec);

}  // namespace vr::latch
