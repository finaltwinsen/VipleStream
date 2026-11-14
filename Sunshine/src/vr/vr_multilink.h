/**
 * @file src/vr/vr_multilink.h
 * @brief VipleStream 2.0 §VR-MULTILINK：VR session 的多連線（每張 client 網卡一條）。
 *
 * 每條連線有自己的影像／音訊 UDP socket（綁在 client 配對到的 server 位址、埠由系統配）、自己的影像送出
 * 佇列與執行緒；所有連線共用一條接收執行緒（PING／PONG、加密的追蹤資料報）。
 *
 *   - 影像：stream.cpp 的影像執行緒把每一批封包複製進每條可用連線的佇列就返回，不會被任何一條鏈路
 *     卡住。送出執行緒用非阻塞送出；批次超過期限（預設兩個幀週期）還沒送完就丟，並回報飽和偵測。
 *   - 音訊：音訊執行緒直接在每條連線的音訊 socket 非阻塞送出（量很小，不排隊）。
 *   - 追蹤：client 把 0x5506 加密後在每條連線各送一份；這裡以 seq 去重後交給 stream.cpp 的處理函式
 *     （它再以 sampleId 與 ENet 那一份去重）。
 *   - 存活：client 每 250 ms 在每條連線送 PING，server 回 PONG；client 收得到 PONG 的連線才會在 PING 裡
 *     帶 CONFIRMED。最近 2 s 內收過 CONFIRMED 的連線才算可用（兩個方向都通）。
 *
 * 只要有一條連線可用，stream.cpp 就不再走原本的單一路徑送影像與音訊；全部連線都不可用時自動退回。
 * 沒有協商多連線的 session 不會建立這個物件（不變式 5）。
 */
#pragma once

// standard includes
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// local includes
#include "src/crypto.h"
#include "vr_multilink_logic.h"

namespace vr::multilink {
  struct options_t {
    /// all＝每條可用的連線都送影像（送不動的暫停一段時間後直接再試）；
    /// primary＝只有 session 本身那條送影像，其餘待命（追蹤、音訊、ping 照送）；
    /// automatic＝session 本身那條先送，其餘每秒只送一批當探測，量到的速率夠快才開始送影像；
    ///            送不動的退回探測（不盲目重試，免得一再把另一條拖下去）
    enum class video_e {
      all,
      primary,
      automatic,
      /// always＝每條可用的連線都一直送影像；送不完的批次過期就丟（dropAge／dropBlock），不暫停任何一條。
      /// 2026-10-07 配戴對照：Steam 在收訊差的位置照樣兩條全送，差的那條送到多少算多少，好的那條瞬間掉包時
      /// 另一條手上已經有同一幀的一部分；我們暫停差的那條之後只剩一條，它一掉就整段卡
      always,
    };

    video_e video = video_e::all;
    bool video_qos = false;  ///< client 要求了影像 QoS：連線的影像 socket 也要標記（和單一路徑一致）
    bool audio_qos = false;
    std::chrono::microseconds video_max_age {22222};  ///< 影像批次在佇列＋送出的期限（兩個幀週期）
    bool ctrl = false;  ///< §VR-LINK-CTRL（vr_multilink_ctrl）：client 也支援時，LOSS／LATCH／REFRESH_START 走連線
    bool repair = false;  ///< §VR-LINK-REPAIR（vr_multilink_repair）：補包；要 ctrl 也開
    /// §VR-REPAIR-RTT（vr_multilink_repair_rtt）：client 也支援時，補包的時間預算由它照量到的往返時間決定，
    /// server 對帶旗標的回報放寬年齡上限與「還在路上」的視窗；要 repair 也開
    bool repair_rtt = false;
    /// §VR-MULTILINK-HOTADD（vr_multilink_hotadd）：client 也支援時，允許同 id 重綁與死亡 slot 回收
    bool hotadd = false;
    /// §PERF-1113（審查 05 第 1 項）：影像批次改成一次 WSASendMsg＋UDP_SEND_MSG_SIZE（USO）交給系統，不再逐包 send_to。
    /// 預設 false＝行為和之前完全一樣（逐包）；線上的封包位元組不變。只有 Windows 有這條路，其他平台忽略這個旗標。
    /// 設定項是 vr_multilink_uso（預設 disabled）：還沒在實機上對照過（飽和偵測、dropBlock、頭盔端幀到齊跨度）。
    bool video_uso = false;
    /// §VR-PACE（vr_pace_spread_pct，2026-11-13）：把每一幀的封包平均攤開在「串流幀週期 × 這個百分比」的時間內送，
    /// 不再一次送完。0＝不攤開（預設，行為和之前相同）。外出逐封包量測（經 VPN，線路每秒數次、每次幾毫秒的全斷）：
    /// 一次送完的整幀常整個落在中斷裡；攤開之後每次中斷只損失一小部分，FEC 補得回來（72 fps／12 Mbps：每 10 秒
    /// 救不回 27 次 → 攤開 13 ms 時 3 次）。代價是幀尾晚這麼久才到齊；144 fps 的幀週期太短，攤開幫助不大。
    /// 排程以每一幀自己入列的時刻為起點（晚開始就連續送出追上，不往後累積）；封包間隔最多 1 ms、不到 20 µs 就不排。
    /// 不攤開的批次：探測中的連線（頭盔靠整批到齊的速度量鏈路）、always 模式下只留四分之一期限的連線、補包。
    int pace_spread_pct = 0;
    fault_t fault;
  };

  /**
   * @brief 收到一則解密並去重後的 C→S 訊息。在 hub 的接收執行緒上呼叫。
   * @return true＝這一份被採用（例如追蹤樣本是最新的）；false＝重複或被忽略。只用於統計。
   */
  using c2s_cb_t = std::function<bool(uint8_t link_id, uint16_t ptype, std::string_view payload)>;

  class hub_t {
  public:
    hub_t(const crypto::aes_t &key, options_t opt, c2s_cb_t on_c2s);
    ~hub_t();

    hub_t(const hub_t &) = delete;
    hub_t &operator=(const hub_t &) = delete;

    /// 停掉所有執行緒（可重複呼叫）。session 拆除時先呼叫，之後的 send_* 都是 no-op。
    void stop();

    /**
     * @brief 處理 LINK_HELLO（control 執行緒）。新連線建立 socket 與執行緒；已存在且描述相同的連線回原來的埠。
     * @param primary_client_addr session 本身連線的 client 位址（video=primary 用來認哪一條是主連線）。
     */
    std::vector<ready_link_t> on_hello(const std::vector<hello_link_t> &links, const std::array<uint8_t, 4> &primary_client_addr);

    /// 有連線可用（影像執行緒每幀問一次）
    bool active() const;

    /// 和 client 協商出來的連線層功能（VIPLE_VR_LINK_F_*；LINK_HELLO 之前是 0）。任何執行緒。
    uint8_t features() const;

    /**
     * @brief §VR-LINK-CTRL：把一則 0x5508 TLV 加密後在每條可用的連線各送一份（T_S2C）。任何執行緒；非阻塞。
     *        只給時間敏感、掉了也無妨的小訊息（整個資料報要 < VIPLE_VR_LINK_CTRL_MAX_LEN）；要可靠送達的仍走 ENet。
     * @return 送出的份數（0＝沒協商這個功能、沒有可用連線，或訊息太長）
     */
    int send_s2c(std::string_view tlv);

    /// 開發用故障注入的 c: 規則現在是不是在「丟掉 ENet 上的 VR 即時訊息」的期間。任何執行緒。
    bool fault_ctrl_blocked() const;

    /**
     * @brief 影像執行緒：一批大小相同、連續存放的封包。複製進每條可用連線的佇列後立刻返回。
     * @param meta 這一批屬於哪一幀、哪個 FEC block 的哪幾個 shard（§VR-LINK-REPAIR 的封包倉用；nullptr＝不存）
     * @param frame_pkts §VR-PACE：這一幀總共大約幾個封包（data＋parity，所有 FEC block 合計；0＝不知道，不攤開）
     * @param frame_end §PERF-1113：這一批是這一幀的最後一批（要有 meta 才有幀號）。送出執行緒把它整批送完的時刻記下來，
     *        給 frame_sent_us() 查
     */
    void send_video(const uint8_t *pkts, size_t pkt_size, size_t count, const video_meta_t *meta = nullptr, bool frame_end = false, uint32_t frame_pkts = 0);

    /**
     * @brief §PERF-1113（審查 05 第 8 項）：這一幀的最後一批「真正交給系統送出」的時刻（steady_clock 的 µs；多條連線取最早
     *        送完的那一條）。send_video 返回時封包只是進了佇列，[VIPLE-VR-LAT10] 的送出段要用這個才含佇列與逐包送出的時間。
     *        任何執行緒。
     * @return 0＝查不到（最後一批沒有整批送完、每條連線都沒送這一幀，或已被後面的幀蓋掉——每條連線只留最近 256 幀）
     */
    int64_t frame_sent_us(uint32_t frame, bool *paced = nullptr) const;

    /// on_nack() 的結果（呼叫端拿去記 ABR 的帳）
    struct nack_result_t {
      unsigned resent = 0;  ///< 這一份回報補送了幾個封包（每條連線各送一份只算一次）
      unsigned needed = 0;  ///< 其中「補之前還差幾個才夠還原」的部分（≤ resent；其餘是餘裕）——這才是 client 實際缺的
      uint8_t gone = 0;  ///< 非 0＝補不了，已經回 REPAIR_GONE（VIPLE_VR_GONE_*）
    };

    /**
     * @brief §VR-LINK-REPAIR：處理一則 NACK（0x5507/0B 的 TLV 本體；hub 的接收執行緒）。從封包倉挑出 client 缺的
     *        shard，原樣排進正在送影像的連線的補包佇列（插在影像前面）；補不了就回 REPAIR_GONE。
     */
    nack_result_t on_nack(const uint8_t *body, size_t len);

    /// §VR-LINK-REPAIR 的 10 秒統計（control 執行緒；沒協商補包或這 10 秒沒有任何動靜時回空字串）
    std::string take_repair_line();

    /// 音訊執行緒：在每條可用連線送一份
    void send_audio(const uint8_t *hdr, size_t hdr_len, const uint8_t *payload, size_t payload_len);

    /// ENet 那一份追蹤先到（統計用；可在任何執行緒呼叫）
    void note_enet_tracking_first() {
      enet_trk_first_.fetch_add(1, std::memory_order_relaxed);
    }

    /// 10 秒統計行（control 執行緒；回傳空字串＝還沒有連線）
    std::string take_stats_line();

    /// §VR-LINK-GRACE：有沒有任何一條連線還活著（最近 2 s 內收過帶 CONFIRMED 的 PING）。任何執行緒
    bool any_alive() const;
    /// §VR-RATE-AUTO：影像批次的期限（補包期限跟著它走）。串流中途改送出週期時呼叫；任何執行緒
    void set_video_max_age(std::chrono::microseconds age);
    /// 目前的影像批次期限（µs；兩個串流幀週期）。任何執行緒
    int64_t video_max_age_us() const;
    /// §VR-PACE：改攤開的比例（0～95；0＝一次送完）。任何執行緒；下一幀起生效
    void set_pace_spread_pct(int pct);
    /// §VR-PACE：目前攤開的比例（0＝沒開）。任何執行緒
    int pace_spread_pct() const;

    /// §VR-ABR-FAST（2026-10-30）：現在活著的連線數（定義同 any_alive）。任何執行緒
    int alive_count() const;
    /// §VR-LINK-DEDICATED（11-08）：活著的連線裡走頭盔轉接器熱點（專用 6 GHz）的有幾條。任何執行緒
    int alive_dedicated_count() const;

  private:
    struct link_t;
    struct impl_t;

    std::unique_ptr<impl_t> impl_;
    std::atomic<uint64_t> enet_trk_first_ {0};
    uint64_t enet_trk_first_prev_ = 0;
  };
}  // namespace vr::multilink
