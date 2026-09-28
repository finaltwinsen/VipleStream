/**
 * @file src/vr/vr_session.h
 * @brief VipleStream 2.0 §VR：/launch 的 VR 參數解析、協商結果與每個 VR session 的共享狀態。
 *
 * 權威設計：docs/vr_protocol.md §4、docs/vr_architecture.md §3.4–§3.6。
 * 線上格式的單一定義來源是 moonlight-common-c/src/VipleVr.h（與 client 端 byte-identical）。
 *
 * 這個模組刻意只依賴標準函式庫與 VipleVr.h（不碰 logging／config／stream），
 * 所以參數解析與 wave 狀態機可以脫離整個 Sunshine 單獨編譯測試；log 一律由呼叫端印。
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
    std::chrono::steady_clock::time_point arrival {};
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
     */
    session_state_t(negotiated_t neg, const void *stream_session);

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
     * @return true 表示成為最新樣本。
     */
    bool on_tracking(const VIPLE_VR_TRACKING &sample, bool via_quic);

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

    /// control 執行緒：TLV 長度超出剩餘長度
    void count_c2s_truncated();

    stats_snapshot_t stats() const;

    // ── S→C outbox（任何執行緒都能排入；只有 control 執行緒 drain 後送出）──

    void queue_s2c(std::vector<uint8_t> tlv, bool reliable);

    /// REFRESH_START 連排 2 份（unsequenced 不重傳，互為備援）
    void queue_refresh_start(const wave_info_t &wave);

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

    // pose
    mutable std::mutex pose_mtx_;
    std::optional<pose_sample_t> latest_;

    // wave／LOSS
    mutable std::mutex wave_mtx_;
    wave_phase_e wave_phase_ = wave_phase_e::idle;
    wave_info_t wave_ {};
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
