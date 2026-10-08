/**
 * @file src/vr/vr_session.h
 * @brief VipleStream 2.0 §VR：/launch 的 VR 參數解析、協商結果與每個 VR session 的共享狀態。
 *
 * 權威設計：docs/vr_protocol.md §4、docs/vr_architecture.md §3.4–§3.6。
 * 線上格式的單一定義來源是 moonlight-common-c/src/VipleVr.h（與 client 端 byte-identical）。
 *
 * 這個模組刻意只依賴標準函式庫、VipleVr.h 與同樣是純模組的 vr_clock.h（不碰 platform／
 * logging／config／stream），所以參數解析、wave 狀態機與時鐘對映可以脫離整個 Sunshine
 * 單獨編譯測試；log 一律由呼叫端印，時間（vr_clock tick）也由呼叫端取好傳入。
 *
 * M1a（stub 模式）沒有 SteamVR driver：VR session 直接跑在一般的桌面擷取上，
 * server 把最新收到的 tracking 樣本回填進每一幀的 24 B 0x81 header（回聲），
 * 並依 client 的 LOSS 開 intra-refresh wave。
 */
#pragma once

// standard includes
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <moonlight-common-c/src/VipleVr.h>

// local includes
#include "vr_clock.h"
#include "vr_latch.h"

namespace vr {

  // ── /launch 參數（docs/vr_protocol.md §4.2）──────────────────────────

  /**
   * @brief client 在 /launch 帶來的 VR 參數（全部是十進位定點數，vrCaps 是十六進位）。
   */
  struct launch_params_t {
    int eye_width = 0;  ///< vrEye 的 W（每眼像素）
    int eye_height = 0;  ///< vrEye 的 H
    int hz = 0;  ///< vrHz：顯示更新率
    int64_t period_ns = 0;  ///< vrPeriodNs：平均顯示週期（預設 1e9/hz）
    std::array<int, 8> fov {};  ///< vrFov：tan×10000，左眼 left,right,up,down，右眼同順序
    int ipd = 6300;  ///< vrIpd：mm×100
    std::array<int, VIPLE_VR_EYE_TO_HEAD_COUNT> eye_to_head {};  ///< vrEyeToHead：每眼 pos(mm×100)×3＋quat x,y,z,w(×10000)×4，左眼在前
    uint32_t caps = 0;  ///< vrCaps：VIPLE_VR_CLIENT_CAP_*
    uint32_t codecs = 0;  ///< vrCodecs：VIPLE_VR_CODEC_*
    std::string ctrl = "touch";  ///< vrCtrl："touch"|"index"
    int overscan = 0;  ///< vrOverscan：deg×10
    int fovea = 100;  ///< vrFovea（§VR-FOVEA）：正前方的像素密度是均勻取樣的幾 %（100＝關，上限 250）
    bool force = false;  ///< vrForce：0|1

    bool operator==(const launch_params_t &) const = default;
  };

  /**
   * @brief 查詢 /launch query 參數的介面：有帶就回傳值，沒帶回 nullopt。
   *
   * 用函式而不是直接吃 SimpleWeb 的 multimap，讓這個模組不必依賴 Simple-Web-Server，
   * 單元測試也能用普通的 map 餵資料。
   */
  using query_lookup_t = std::function<std::optional<std::string>(std::string_view name)>;

  /**
   * @brief parse_launch_params() 的結果：成功時 params 有值；失敗時 error 是給 log 與
   *        VR_BAD_PARAMS 訊息用的英文說明（不含錯誤碼前綴）。
   */
  struct parse_result_t {
    std::optional<launch_params_t> params;
    std::string error;
  };

  /**
   * @brief 嚴格解析 /launch 的 vr* 參數。
   *
   * 規則：十進位整數（只有 vrFov 與 vrEyeToHead 的元素可以是負值），不接受 '+'、空白與
   * 尾端垃圾；超出 VipleVr.h 的 VIPLE_VR_*_MIN/MAX 範圍就失敗。vrCaps 是十六進位（可帶
   * 0x）；vrEye 是 WxH；vrFov 8 個、vrEyeToHead 14 個逗號分隔值。
   * 必填：vr=1、vrEye、vrHz、vrCodecs、vrCaps；其餘缺席時套預設值。
   */
  parse_result_t parse_launch_params(const query_lookup_t &lookup);

  /**
   * @brief 產生與 client（moonlight-qt VrLaunchConfig::toQuery）相同順序與格式的 query 片段，
   *        開頭不帶 '&'。用於單元測試的 round-trip。
   */
  std::string format_launch_params(const launch_params_t &params);

  // ── 協商結果（/launch 回應的 <VipleStreamVRSession>）──────────────────

  enum class codec_e {
    h264,  ///< H.264
    hevc,  ///< HEVC
  };

  const char *codec_name(codec_e codec);

  /**
   * @brief /launch 協商後的 VR session 設定。nvhttp 建立後經 launch_session 帶到 RTSP 與
   *        stream session，建立後不再修改。
   */
  struct negotiated_t {
    launch_params_t params;
    codec_e codec = codec_e::hevc;
    bool recovery_intra = false;  ///< true：LOSS → intra-refresh wave；false：LOSS → IDR
    int ir_frames = 0;  ///< vr_intra_refresh_frames（encoder 一律照這個設 intraRefreshCnt）
    int safety_ms = 0;  ///< vr_intra_refresh_safety_ms（0 = 關閉週期性安全網）
    std::string guid;  ///< session GUID
    std::string owner_uuid;  ///< 發起 VR session 的 client（TLS 憑證 UUID）
    /// M1b S1-10：true = pcvr（display_vr_t 擷取 driver 的 SBS ring）；false = M1a stub（桌面擷取＋回聲）。
    /// V3 只有 RTSP 端讀它；由 `vr_pcvr=enabled` 的 /launch 設定是 S1-11（V5）。
    bool pcvr = false;
    /// §VR-MULTILINK：client 帶了 VIPLE_VR_CLIENT_CAP_MULTILINK 且 vr_multilink 不是 disabled。
    /// true 時 <VipleStreamVRSession> 多一個 multilink=1，client 才會送 LINK_HELLO。
    bool multilink = false;

    int packed_width() const {
      return params.eye_width * 2;  // layout=sbs
    }

    int packed_height() const {
      return params.eye_height;
    }

    /// video::config_t::videoFormat 的編號（0 = H.264、1 = HEVC）
    int video_format() const {
      return codec == codec_e::hevc ? 1 : 0;
    }
  };

  /**
   * @brief 組 <VipleStreamVRSession> 的內容：單一元素、';' 分隔的 key=value。
   *        recovery=idr 時 irFrames 寫 0（這時沒有 LOSS 觸發的 wave）。
   */
  std::string format_session_element(const negotiated_t &neg);

  /**
   * @brief log 用的 session GUID 縮寫（前 8 hex，§M1b S1-02 VR log 衛生）。
   *        寫給 client 的 `<VipleStreamVRSession>` 一律用完整值，只有 log 用這個。
   */
  inline std::string log_guid(std::string_view guid) {
    return std::string(guid.substr(0, 8));
  }

  /**
   * @brief `format_session_element` 的 log 版：`session=` 的值換成前 8 hex。
   */
  inline std::string format_session_element_for_log(const negotiated_t &neg) {
    std::string s = format_session_element(neg);
    if (!neg.guid.empty()) {
      if (auto pos = s.find(neg.guid); pos != std::string::npos) {
        s.replace(pos, neg.guid.size(), log_guid(neg.guid));
      }
    }
    return s;
  }

  // ── 每個 VR session 的共享狀態 ──────────────────────────────────────

  /**
   * @brief 最新收到的 tracking 樣本（只留 M1a 回聲需要的欄位）。
   */
  struct pose_sample_t {
    uint32_t sample_id = 0;
    uint16_t space_epoch = 0;
    uint64_t sample_time_ns = 0;
    uint32_t predict_ns = 0;
    float pos[3] {};  ///< HMD 位置（公尺）
    float rot[4] {};  ///< HMD 四元數 x, y, z, w
    /// 到達時刻：0x5506 handler 一進來取的 `platf::vr_clock_ticks()`（Windows = raw QPC，§M1b S1-12；
    /// 取代 M1a 的 steady_clock，跨行程只傳 raw QPC）
    int64_t arrival_ticks = 0;
    /// 時鐘對映的結果：sampleTime + predictNs + offset，換成 server vr_clock tick；
    /// 時鐘對映停用（建構時沒給頻率）時為 0
    int64_t target_server_ticks = 0;
  };

  /**
   * @brief on_tracking() 的時間資訊（選填輸出）。pcvr 的 `bridge::publish_tracking` 需要
   *        arrival 與 target 兩個 tick 值；clock 的事件旗標讓呼叫端決定要不要記 log。
   */
  struct tracking_timing_t {
    bool clock_enabled = false;  ///< 建構時有給 vr_clock 頻率
    bool clock_fed = false;  ///< 這個樣本進了時鐘估計器（版本不合、sampleId=0 的不進）
    int64_t arrival_ticks = 0;
    int64_t target_server_ticks = 0;  ///< clock_enabled && clock_fed 時才有意義
    clk::sample_result_t clock {};  ///< step／transport_switch／outlier／clock_reset／target_clamped
  };

  /**
   * @brief on_loss() 對一筆 LOSS 的處置。
   */
  enum class loss_action_e {
    new_wave,  ///< 要求 encoder 開一波新的 intra refresh
    duplicate,  ///< 與上一筆完全相同（client 一次送 2 份）→ 吸收
    absorbed,  ///< 進行中（或已排定）的 wave 會涵蓋 → 吸收
    idr,  ///< recovery=idr：改要 IDR
  };

  const char *loss_action_name(loss_action_e action);

  /**
   * @brief 一波 intra refresh 的線上描述（REFRESH_START 的內容）。
   */
  struct wave_info_t {
    uint32_t start_frame = 0;  ///< 第一幀在線上的 frameIndex（NV_VIDEO_PACKET.frameIndex）
    uint8_t frame_cnt = 0;  ///< wave 長度（最後一幀帶 VIPLE_VR_FF_REFRESH_DONE）
    uint8_t reason = 0;  ///< VIPLE_VR_REFRESH_*
  };

  /**
   * @brief S→C 待送訊息（0x5508 的 TLV 內容）。只有 control 執行緒能送出。
   */
  struct s2c_msg_t {
    std::vector<uint8_t> tlv;
    bool reliable = false;  ///< true：channel 0 RELIABLE；false：VIPLE_VR_CTRL_CHANNEL UNSEQUENCED
  };

  /**
   * @brief 統計快照（log 與 STATS 用）。全部是 session 開始以來的累計值（STATS 也送累計值，
   *        client 以相鄰兩筆相減得到區間值；見 VipleVr.h 的 VIPLE_VR_TLV_STATS）。
   */
  struct stats_snapshot_t {
    uint64_t pose_rx = 0;  ///< 收到的 tracking 樣本（含亂序）
    uint64_t pose_bad = 0;  ///< 長度或版本不合、sampleId=0 而丟棄
    uint64_t pose_ooo = 0;  ///< sampleId 倒退或重複
    uint64_t pose_gap = 0;  ///< sampleId 跳號累計（估丟失）
    uint64_t pose_via_quic = 0;  ///< 經 QUIC fallback（flow 0x04）抵達
    uint64_t pose_resets = 0;  ///< sampleId 大幅倒退、視為 client 重新計數
    uint32_t last_sample_id = 0;
    uint64_t frames_tagged = 0;  ///< 帶 echo 的 VR 幀
    uint64_t frames_fallback = 0;  ///< 沒有可用樣本的 VR 幀
    uint64_t loss_rx = 0;  ///< 收到的 LOSS TLV
    uint64_t refresh_waves = 0;  ///< encoder 實際開始的 wave（含 IDR 代替）
    uint64_t latch_rx = 0;  ///< LATCH TLV（M1a 只計數）
    uint64_t timing_rx = 0;  ///< CLIENT_TIMING TLV（M1a 只計數）
    uint64_t other_c2s = 0;  ///< 其他／不認得的 C2S subtype
    uint64_t c2s_truncated = 0;  ///< TLV 長度超出剩餘長度而丟棄其餘部分的訊息
    uint64_t s2c_dropped = 0;  ///< outbox 滿了丟掉的 S→C 訊息
  };

  /**
   * @brief 一個 VR session 一份，以 std::shared_ptr 持有。
   *
   * 會被 control 執行緒（ENet）、picoquic IO 執行緒（QUIC fallback 重入 control handler）、
   * videoBroadcast 執行緒與 encode 執行緒同時存取：協商結果建立後唯讀；pose、wave、
   * outbox 各自一把 mutex；統計一律 atomic。
   */
  class session_state_t {
  public:
    /**
     * @param neg 協商結果。
     * @param stream_session 所屬的 stream::session_t（只拿來比對身分，永不解參考）。
     * @param clock_frequency `platf::vr_clock_frequency()`（§M1b S1-12 時鐘對映）。0 = 停用時鐘對映
     *        （只給不經 platform 的單元測試用；stream.cpp 一律要傳，pcvr 沒有它無法發布 tracking）。
     */
    session_state_t(negotiated_t neg, const void *stream_session, int64_t clock_frequency = 0);

    session_state_t(const session_state_t &) = delete;
    session_state_t &operator=(const session_state_t &) = delete;

    const negotiated_t &negotiated() const {
      return neg_;
    }

    const void *stream_session() const {
      return stream_session_;
    }

    // ── 0x5506 tracking ──

    /**
     * @brief 收到一筆 tracking 樣本。version<1 或 sampleId=0 丟棄；sampleId 倒退或重複只計
     *        ooo、不覆蓋最新；跳號累計 gap。
     *
     * 時鐘對映（啟用時）：所有通過版本／sampleId 檢查的樣本都餵進估計器，包括亂序與重複的
     * （d = 到達 − sampleTime 仍是有效量測；亂序的 d 較大，不影響最小值）。RTT 用
     * set_rtt_quic_min_ns()／set_rtt_enet_ms() 最近發布的值（§F.1：QUIC 優先、ENet 後備）。
     * 可以在 ENet control 與 picoquic IO 兩條執行緒同時呼叫：估計器有自己的 mutex，
     * 與 pose 的 mutex 不巢狀。
     *
     * @param arrival_ticks handler 一進來取的 `platf::vr_clock_ticks()`（要在任何其他處理之前取，
     *        min 濾波才吃得掉 service loop 的延遲）。
     * @param timing 選填：帶出 arrival／target tick 與時鐘事件。
     * @return true 表示成為最新樣本（pcvr 只有這時才呼叫 bridge::publish_tracking）。
     */
    bool on_tracking(const VIPLE_VR_TRACKING &sample, bool via_quic, int64_t arrival_ticks, tracking_timing_t *timing = nullptr);

    // ── 時鐘對映（§M1b S1-12、§F.1）──

    /// 建構時有給 vr_clock 頻率
    bool clock_enabled() const {
      return clock_.has_value();
    }

    /**
     * @brief control 執行緒發布 QUIC path 的 rtt_min（ns）。≤ 0 表示沒有 QUIC session 或還沒有樣本。
     *        0x5506 handler 只讀這個 atomic，絕不自己去碰 peer／QuicSession（scout F §2.2）。
     */
    void set_rtt_quic_min_ns(int64_t rtt_min_ns) {
      rtt_quic_ns_.store(rtt_min_ns, std::memory_order_relaxed);
    }

    /**
     * @brief control 執行緒（IDX_PERIODIC_PING handler，持 abrMutex）發布 ENet 的 RTT 基線
     *        （`abr_rtt_baseline()`，ms）。< 0 表示還沒取樣過（abrRttSampled == false）。
     */
    void set_rtt_enet_ms(int64_t rtt_ms) {
      rtt_enet_ms_.store(rtt_ms, std::memory_order_relaxed);
    }

    /**
     * @brief 時鐘對映的統計快照（停用時 nullopt）。
     * @param with_jitter 見 clk::clock_map_t::stats()：true 要排序（1 Hz／10 s 用），false 是 O(1)。
     */
    std::optional<clk::stats_t> clock_stats(bool with_jitter = true) const;

    /// 長度不足等在呼叫端就被丟棄的 tracking 訊息
    void count_bad_tracking();

    /// 最新樣本（還沒收到任何樣本時為 nullopt）。encode 執行緒每幀呼叫一次。
    std::optional<pose_sample_t> snapshot() const;

    // ── 0x5507 LOSS 與 intra-refresh wave ──

    /**
     * @brief 收到一筆 LOSS。回傳 new_wave 時呼叫端要 raise mail::vr_refresh；回傳 idr 時要
     *        raise idr_events。
     *
     * recovery=intra 的規則：與上一筆完全相同 → duplicate；wave 已排定（encoder 還沒開始）
     * 或進行中且 lastLost < waveStart（32-bit 迴繞安全比較）→ absorbed；其他 → new_wave
     * （wave 進行中收到更晚的 LOSS 就是重啟 wave）。
     */
    loss_action_e on_loss(uint32_t first_lost, uint32_t last_lost, uint8_t reason);

    /// encode 執行緒：wave 的第一幀即將編碼
    void on_wave_begin(uint32_t start_frame, uint8_t frame_cnt, uint8_t reason);

    /// encode 執行緒：wave 的最後一幀已編碼
    void on_wave_end();

    /// encode 執行緒：要求被放棄（IDR cooldown 擋下、encoder 重建）
    void on_wave_abort();

    /// 進行中的 wave（沒有就 nullopt）
    std::optional<wave_info_t> active_wave() const;

    /**
     * @brief 進行中或已排定的 wave（都沒有就 nullopt）。已排定、第一幀還沒編時，回的是 on_loss() 接受那筆 LOSS 當下記的
     *        估計（start_frame＝當時 encoder 即將編的幀號），一定不晚於實際起點。不能每次重新取 next_frame()——encoder
     *        決定從第 F 幀開 wave 之後、等影像的那一個幀週期裡 next_frame() 已經是 F+1，client 採用它會把 wave 的結尾
     *        多算一幀，真正帶 REFRESH_DONE 的那一幀反而被當成 wave 還沒結束。排定期間又回報了不在估計值之前的掉幀時，
     *        on_loss() 會把估計值往後推到 lastLost + 1（仍然不晚於實際起點），之後回的都是推過的值。
     */
    std::optional<wave_info_t> scheduled_wave() const;

    /**
     * @brief LOSS log 的節流閘門：每秒最多放行一次。
     * @param suppressed 放行時帶出上次放行之後被壓掉的筆數。
     * @return true 表示這次可以印。
     */
    bool loss_log_gate(uint32_t &suppressed);

    // ── 其他統計 ──

    /// encode 執行緒：一幀已標記（tagged=true 帶 echo；false 為 fallback）
    void count_frame(bool tagged);

    /// control 執行緒：LOSS 以外的 C2S subtype
    void count_c2s(uint8_t subtype);

    // ── M4a R3：LATCH／CLIENT_TIMING／HAPTIC ──

    /// §VR-LATCH-V2：session 開始、第一則 LATCH 之前設定控制器（config vr_latch_mode／vr_latch_target_pct）
    void configure_latch(latch::mode_e mode, int target_pct, uint32_t host_period_ns);

    /**
     * @brief control 執行緒（或 QUIC IO 執行緒）：一則 LATCH。更新頻率鎖控制器並回傳建議的
     *        pacing 修正；result.update 為 true 時呼叫端才寫 pacing（stub 模式只記錄）。
     */
    latch::result_t on_latch(const VIPLE_VR_TLV_LATCH &latch, int64_t now_ns);

    /// control 執行緒的 1 s tick：LATCH 停了太久時讓修正衰減回 0
    latch::result_t latch_idle(int64_t now_ns);

    /// 最近一次 LATCH 控制器狀態（10 s log 用）
    latch::result_t latch_snapshot() const;

    /// 一則 CLIENT_TIMING（依 len 能讀多少算多少；缺的欄位為 0）
    void on_client_timing(const uint8_t *body, std::size_t len);

    /// 最近一則 CLIENT_TIMING（沒收過回 nullopt）
    std::optional<VIPLE_VR_TLV_CLIENT_TIMING> last_client_timing() const;

    /**
     * @brief bridge 執行緒（haptic sink）：把 driver 的一則 haptic 打包成 0x5508/01（unsequenced）排入 outbox。
     *        device：VIPLE_VR_POSE_LEFT／RIGHT；其他值丟棄。回傳是否排入。
     */
    bool queue_haptic(uint8_t device, uint32_t duration_us, float frequency_hz, float amplitude);

    uint64_t haptics_queued() const {
      return haptic_tx_.load(std::memory_order_relaxed);
    }

    /// control 執行緒：TLV 長度超出剩餘長度
    void count_c2s_truncated();

    stats_snapshot_t stats() const;

    /// 2026-10-05：被採用的 tracking 樣本之間的到達間隔（server 收到的時刻），10 s 視窗
    struct arrival_stats_t {
      int64_t max_us = 0;  ///< 最大間隔
      uint64_t gt2t = 0;  ///< 間隔 > 2 個顯示週期的次數（driver 會因此觸發 stale 規則）
    };

    /// 取出目前視窗的統計並歸零（10 s log 用）
    arrival_stats_t take_arrival_stats();

    // ── S→C outbox（任何執行緒都能排入；只有 control 執行緒 drain 後送出）──

    void queue_s2c(std::vector<uint8_t> tlv, bool reliable);

    /// REFRESH_START 連排 2 份（unsequenced 不重傳，互為備援）
    void queue_refresh_start(const wave_info_t &wave);

    /// REFRESH_START 的 TLV 位元組（§VR-LINK-CTRL：同一則訊息另外經連線層送出時用）
    static std::vector<uint8_t> refresh_start_tlv(const wave_info_t &wave);

    /**
     * @brief encode 執行緒在每次 encode 之後更新：下一個要編的幀號（= 線上的 frameIndex）。
     *        LOSS handler 用它當 REFRESH_START.startFrame 的估計值，接受 LOSS 當下就回覆，
     *        不必等 wave 的第一幀打包出去。實際 wave 從這一幀或下一幀開始；client 只把
     *        startFrame 當 REFRESH_DONE 的下界，所以估計值安全。
     */
    void set_next_frame(uint32_t frame_index) {
      next_frame_.store(frame_index, std::memory_order_relaxed);
    }

    uint32_t next_frame() const {
      return next_frame_.load(std::memory_order_relaxed);
    }

    /// STATE（reliable）
    void queue_state(uint8_t state, uint8_t progress, uint16_t code);

    /// 1 Hz STATS（unsequenced）；所有計數欄位都是累計值
    void queue_stats();

    std::vector<s2c_msg_t> drain_s2c();

    /// 組一筆 TLV：{u8 subtype, u8 len, payload[len]}
    static std::vector<uint8_t> make_tlv(uint8_t subtype, const void *payload, std::size_t len);

  private:
    enum class wave_phase_e {
      idle,
      pending,  ///< 已要求 encoder 開 wave，第一幀還沒編
      active,  ///< wave 進行中
    };

    void expire_stale_wave_locked(std::chrono::steady_clock::time_point now);

    const negotiated_t neg_;
    const void *const stream_session_;
    const int64_t clock_frequency_;

    // pose
    mutable std::mutex pose_mtx_;
    std::optional<pose_sample_t> latest_;
    arrival_stats_t arrival_win_ {};  ///< pose_mtx_ 保護

    // 時鐘對映（clk::clock_map_t 不是執行緒安全的；只在 clock_mtx_ 內碰，不與 pose_mtx_ 巢狀）
    mutable std::mutex clock_mtx_;
    std::optional<clk::clock_map_t> clock_;
    std::atomic<int64_t> rtt_quic_ns_ {-1};
    std::atomic<int64_t> rtt_enet_ms_ {-1};

    // wave／LOSS
    mutable std::mutex wave_mtx_;
    wave_phase_e wave_phase_ = wave_phase_e::idle;
    wave_info_t wave_ {};
    wave_info_t pending_wave_ {};  ///< wave_phase_ == pending 時有效：排定當下的估計
    std::chrono::steady_clock::time_point wave_since_ {};
    bool have_last_loss_ = false;
    uint32_t last_loss_first_ = 0;
    uint32_t last_loss_last_ = 0;
    uint8_t last_loss_reason_ = 0;
    std::chrono::steady_clock::time_point last_loss_time_ {};
    std::chrono::steady_clock::time_point loss_log_last_ {};
    uint32_t loss_log_suppressed_ = 0;

    // outbox
    std::mutex outbox_mtx_;
    std::vector<s2c_msg_t> outbox_;
    std::vector<s2c_msg_t> outbox_next_;  ///< 下一次 drain 才送（REFRESH_START 的第二份）

    // 統計
    std::atomic<uint32_t> next_frame_ {0};
    std::atomic<uint64_t> pose_rx_ {0};
    std::atomic<uint64_t> pose_bad_ {0};
    std::atomic<uint64_t> pose_ooo_ {0};
    std::atomic<uint64_t> pose_gap_ {0};
    std::atomic<uint64_t> pose_via_quic_ {0};
    std::atomic<uint64_t> pose_resets_ {0};
    std::atomic<uint32_t> last_sample_id_ {0};
    std::atomic<uint64_t> frames_tagged_ {0};
    std::atomic<uint64_t> frames_fallback_ {0};
    std::atomic<uint64_t> loss_rx_ {0};
    std::atomic<uint64_t> refresh_waves_ {0};
    std::atomic<uint64_t> latch_rx_ {0};
    std::atomic<uint64_t> timing_rx_ {0};
    std::atomic<uint64_t> other_c2s_ {0};
    std::atomic<uint64_t> c2s_truncated_ {0};
    std::atomic<uint64_t> s2c_dropped_ {0};

    // M4a R3
    mutable std::mutex latch_mtx_;
    latch::controller_t latch_ctl_;
    latch::result_t latch_last_ {};
    mutable std::mutex timing_mtx_;
    std::optional<VIPLE_VR_TLV_CLIENT_TIMING> timing_last_;
    std::atomic<uint32_t> haptic_event_id_ {0};
    std::atomic<uint64_t> haptic_tx_ {0};
  };

  // ── 全域「目前的 VR session」（VR 獨佔：同時最多一個）──────────────────
  //
  // encode 執行緒（video.cpp）拿不到 stream session，靠 active() 取樣本；nvhttp 靠
  // busy_for() 擋其他 client。全部以同一把 mutex 保護。

  /// stream session 開始時設定（同時清掉 /launch 留下的預約）
  void set_active(std::shared_ptr<session_state_t> state);

  std::shared_ptr<session_state_t> active();

  /// session 結束時清除；只有目前的 active 正是 state 時才清（舊 session 晚到的 join 不會清掉新的）
  void clear_active_if(const session_state_t *state);

  /**
   * @brief /launch（或 /resume）接受 VR 之後、RTSP 建立 session 之前的預約：這段期間其他
   *        client 也要收到 VR_BUSY。預約在 set_active() 或逾時後失效。
   */
  void reserve(const std::string &owner_uuid, std::chrono::steady_clock::duration ttl);

  void clear_reservation();

  /**
   * @brief 目前是否有屬於其他 client 的 VR session（進行中或預約中）。
   * @param caller_uuid 呼叫者的 TLS 憑證 UUID；空字串（身分不明）一律視為其他 client。
   */
  bool busy_for(const std::string &caller_uuid);

  /// 32-bit 幀號的迴繞安全比較：a 在 b 之前
  inline bool is_before32(uint32_t a, uint32_t b) {
    return (int32_t) (a - b) < 0;
  }
}  // namespace vr
