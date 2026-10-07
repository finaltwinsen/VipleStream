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
     */
    void send_video(const uint8_t *pkts, size_t pkt_size, size_t count, const video_meta_t *meta = nullptr);

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

  private:
    struct link_t;
    struct impl_t;

    std::unique_ptr<impl_t> impl_;
    std::atomic<uint64_t> enet_trk_first_ {0};
    uint64_t enet_trk_first_prev_ = 0;
  };
}  // namespace vr::multilink
