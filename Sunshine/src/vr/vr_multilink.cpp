/**
 * @file src/vr/vr_multilink.cpp
 * @brief VipleStream 2.0 §VR-MULTILINK：多連線的 socket、執行緒與加密。說明見 vr_multilink.h。
 */
// standard includes
#include <algorithm>
#include <format>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "src/logging.h"
#include "src/platform/common.h"
#include "vr_multilink.h"

using namespace std::literals;

namespace asio = boost::asio;
using asio::ip::udp;

namespace vr::multilink {
  namespace {
    using clock = std::chrono::steady_clock;

    constexpr int64_t kAliveNs = 2'000'000'000;  // 最近 2 s 內收過 CONFIRMED 的 PING 才算可用
    constexpr size_t kQueueMaxBytes = 1 << 20;  // 每條連線的影像佇列上限（200 Mbps 時約 3～4 幀），超過丟最舊的
    constexpr int kSockSndBuf = 256 * 1024;  // 小一點：鏈路送不動時要儘快在 send 看到 would_block
    constexpr auto kBlockRetry = 300us;
    constexpr auto kAutoHoldMax = 8000ms;  // automatic：退回探測後的最短探測時間（2 s 起加倍）封頂在這裡
    constexpr int64_t kProbeNoBatchNs = 2'000'000'000;  // 這麼久都沒有夠大的批次可以量（位元率太低）：直接開始送
    constexpr int kUsableDelivPct = 80;  // 另一條連線的實際送達率到這個程度，才算「有別條可用」
    constexpr int64_t kFreshNs = 700'000'000;  // 最近這麼久內收過 PING 才算「現在還通」（PING 每 250 ms 一個）
    constexpr int kMuchBetterPct = 30;  // 或者：另一條的送達率比自己實際送得出去的比例高出這麼多
    constexpr int64_t kProbeIntervalNs = 1'000'000'000;  // 探測中的連線每秒送一批
    constexpr int64_t kProbeFastIntervalNs = 300'000'000;  // 一條連線剛開始探測時的前幾批送快一點，約 1 s 內就能決定（之後每秒一批）
    constexpr uint32_t kProbeFastCount = 4;
    constexpr uint32_t kProbeMinPkts = 32;  // 太小的批次量不準，不拿來探測
    constexpr auto kRepairMaxAge = 8ms;  // 補包排進佇列後這麼久還送不出去就丟：補了也來不及
    constexpr int64_t kRepairGapMinNs = 4'000'000;  // 同一個 block 兩輪補包之間：上一輪補的在這段時間內當成「還在路上」
    constexpr int64_t kRepairGapMaxNs = 12'000'000;  // （＝連線的往返時間＋3 ms，夾在這兩個值之間）
    constexpr int64_t kProbeSpanNs = 6'000'000;  // 一次探測送這段時間內的所有批次（約一整幀）：只送一批的話，
                                                 // 64 KB 在快的鏈路上是一個 Wi-Fi 聚合框，頭盔端量不出速率

    int64_t to_ns(clock::time_point t) {
      return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
    }

    asio::ip::address_v4 to_v4(const std::array<uint8_t, 4> &a) {
      return asio::ip::address_v4 {asio::ip::address_v4::bytes_type {a[0], a[1], a[2], a[3]}};
    }

    struct batch_t {
      std::vector<uint8_t> data;
      uint32_t pkt_size = 0;
      uint32_t count = 0;
      clock::time_point enq;
    };
  }  // namespace

  struct hub_t::link_t {
    explicit link_t(asio::io_context &io):
        video_sock {io},
        audio_sock {io} {
    }

    uint8_t id = 0;
    hello_link_t desc;
    bool primary = false;
    udp::endpoint client_video;
    udp::endpoint client_audio;
    udp::socket video_sock;
    udp::socket audio_sock;
    // 影像 socket 有三個送出者（tx 執行緒送影像、接收執行緒回 PONG、任何執行緒的 send_s2c）：每次 send_to 都先拿這把鎖。
    // 只包住單一次非阻塞的 send_to，不會把別人擋在 would_block 的重試後面
    std::mutex sock_mtx;
    uint16_t server_video_port = 0;
    uint16_t server_audio_port = 0;

    // 接收（只在 io 執行緒存取）
    udp::endpoint rx_peer_v;
    udp::endpoint rx_peer_a;
    std::array<uint8_t, 2048> rx_buf_v {};
    std::array<uint8_t, 256> rx_buf_a {};

    std::atomic<int64_t> last_confirmed_ns {0};
    std::atomic<int64_t> last_ping_ns {0};  // 最近一個（來源正確的）PING
    std::atomic<int64_t> muted_until_ns {0};
    // automatic：探測中的連線不送影像，每秒一批當探測；量到夠快才轉成送影像
    std::atomic<bool> probing {false};
    std::atomic<bool> gate_reset {false};  // 送出執行緒要求接收執行緒把恢復閘門歸零
    std::atomic<bool> sat_reset {false};  // 接收／影像執行緒要求送出執行緒重新開始飽和統計（剛從探測轉為送影像）
    std::atomic<int64_t> last_probe_ns {0};
    std::atomic<int64_t> probe_until_ns {0};  // 這次探測送到什麼時候（只在影像執行緒存取，atomic 只是方便）
    std::atomic<int64_t> small_since_ns {0};  // 探測到期卻只有太小的批次：從什麼時候開始
    delivery_t deliv;  // 只在 io 執行緒存取
    std::atomic<int> deliv_pct {-1};
    std::unique_ptr<platf::deinit_t> qos_v;
    std::unique_ptr<platf::deinit_t> qos_a;
    std::atomic<int> fast_probes {(int) kProbeFastCount};  // 還剩幾批用較短的間隔送（剛建立、或剛退回探測時重設）
    probe_gate_t gate;  // 只在 io 執行緒存取
    clock::time_point fault_next_free {};  // 故障注入的限速（只在 tx 執行緒存取）

    // 影像佇列
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<batch_t> q;
    size_t q_bytes = 0;
    // §VR-LINK-REPAIR：補包佇列（握 mtx）。tx 執行緒一有空就先送它，送影像批次的途中也會插隊
    std::deque<batch_t> rq;
    std::atomic<bool> rq_pending {false};
    // 故障注入的成串掉包（只在 tx 執行緒存取）
    int64_t burst_next_ns = 0;
    uint32_t burst_left = 0;
    std::vector<std::vector<uint8_t>> pool;
    std::thread tx;
    saturation_t sat;  // 只在 tx 執行緒存取
    std::unique_ptr<platf::high_precision_timer> hp;  // 短暫睡眠用（tx 執行緒建立、只有它用）

    // 累計統計
    std::atomic<uint64_t> tx_pkts {0}, tx_bytes {0}, drop_age {0}, drop_block {0}, drop_full {0}, drop_muted {0}, drop_policy {0}, drop_fault {0}, tx_err {0};
    std::atomic<uint64_t> audio_pkts {0}, audio_drop {0};
    std::atomic<uint64_t> rx_ping {0}, rx_data {0}, rx_dup {0}, rx_bad {0}, trk_first {0};
    std::atomic<uint64_t> rx_ctl {0}, s2c_tx {0}, s2c_drop {0};  // §VR-LINK-CTRL：收到的 0x5507、送出／送不出去的 T_S2C
    std::atomic<uint64_t> repair_tx {0}, repair_drop {0};  // §VR-LINK-REPAIR：這條連線送出／來不及送的補包
    uint64_t repair_tx_prev = 0, repair_drop_prev = 0;  // 上一次統計時的值（只在 control 執行緒存取）
    std::atomic<uint32_t> repair_wait_max_us {0};  // 補包從排進佇列到開始送出，最久等了多久（10 s 統計）
    uint32_t repair_logs = 0;  // 逐筆 log 的節流（只在 tx 執行緒存取）
    std::atomic<uint32_t> block_max_us {0}, mutes {0}, probes {0}, promotions {0};
    std::atomic<uint64_t> forced {0};  // 保底送出的封包數（沒有任何連線在送影像時）
    std::atomic<uint16_t> cl_burst_mbps {0};
    // client 在 PING 回報的（它那端的累計值）
    std::atomic<uint32_t> cl_rx_pkts {0}, cl_rx_used {0};
    std::atomic<uint16_t> cl_rtt_ms10 {VIPLE_VR_LINK_RTT_UNKNOWN}, cl_gap_max_ms {0};

    struct prev_t {
      uint64_t tx_pkts = 0, tx_bytes = 0, drop_age = 0, drop_block = 0, drop_full = 0, drop_muted = 0, drop_policy = 0, drop_fault = 0, tx_err = 0;
      uint64_t audio_pkts = 0, audio_drop = 0, rx_ping = 0, rx_data = 0, rx_dup = 0, rx_bad = 0, trk_first = 0;
      uint64_t rx_ctl = 0, s2c_tx = 0, s2c_drop = 0;
      uint32_t cl_rx_pkts = 0, cl_rx_used = 0;
    } prev;
  };

  struct hub_t::impl_t {
    impl_t(const crypto::aes_t &key, options_t o, c2s_cb_t cb):
        opt {std::move(o)},
        on_c2s {std::move(cb)},
        cipher {key, false},
        s2c_cipher {key, false} {
      iv.resize(12);
      s2c_iv.resize(12);
    }

    options_t opt;
    c2s_cb_t on_c2s;

    // 解密與去重（只在 io 執行緒存取）
    crypto::cipher::gcm_t cipher;
    crypto::aes_t iv;
    std::vector<uint8_t> plaintext;
    replay_window_t replay;

    // §VR-LINK-CTRL：S→C 的加密與序號（任何執行緒，握 s2c_mtx）。和 C→S 用同一把 key，IV 尾端不同（'H','L'）
    std::mutex s2c_mtx;
    crypto::cipher::gcm_t s2c_cipher;
    crypto::aes_t s2c_iv;
    uint32_t s2c_seq = 0;
    std::atomic<uint8_t> features {0};  // 協商出來的 VIPLE_VR_LINK_F_*

    // §VR-LINK-REPAIR：封包倉（影像執行緒寫、接收執行緒讀）與額度（都握 store_mtx）
    mutable std::mutex store_mtx;
    packet_store_t store;
    repair_budget_t budget;
    std::atomic<uint32_t> pkt_bytes {0};  // 影像封包的大小（算封包率用）
    struct repair_stats_t {
      uint64_t nack = 0, rounds = 0, pkts = 0, gone_expired = 0, gone_budget = 0, gone_unknown = 0, nothing = 0, exhausted = 0;
    };
    repair_stats_t rstats;  // 握 store_mtx
    uint32_t nack_logs = 0;  // 逐筆 log 的節流（握 store_mtx）
    uint32_t gone_logs = 0;  // 回 GONE 的 log 另外節流（少數事件，要看得到；握 store_mtx）
    repair_stats_t rstats_prev;  // 只在 control 執行緒（take_repair_line）

    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work {io.get_executor()};
    std::thread rx;

    // 連線只增不減（session 結束才整批釋放）：count 之前的元素建好之後不再變動，讀取端不必加鎖
    std::array<std::unique_ptr<link_t>, kMaxLinks> links;
    std::atomic<int> count {0};
    std::mutex hello_mtx;
    std::atomic<bool> stop {false};
    clock::time_point stats_prev {};

    // 目前的影像位元率（send_video 每 500 ms 更新；automatic 的恢復門檻用）
    std::atomic<uint32_t> video_mbps {0};
    uint64_t rate_bytes = 0;  // 只在影像執行緒存取
    clock::time_point rate_since {};

    bool alive(const link_t &L, int64_t now_ns) const {
      const int64_t t = L.last_confirmed_ns.load(std::memory_order_relaxed);
      return t != 0 && now_ns - t < kAliveNs;
    }

    /// 這條連線「現在」還通：最近 kFreshNs 內收過 PING。alive() 的 2 s 太長，鏈路剛斷的頭兩秒不能把它當成可靠。
    bool fresh(const link_t &L, int64_t now_ns) const {
      const int64_t t = L.last_ping_ns.load(std::memory_order_relaxed);
      return alive(L, now_ns) && t != 0 && now_ns - t < kFreshNs;
    }

    /// 正在送影像、現在還通的連線
    bool carrying(const link_t &L, int64_t now_ns) const {
      return fresh(L, now_ns) && !L.probing.load(std::memory_order_relaxed) && now_ns >= L.muted_until_ns.load(std::memory_order_relaxed);
    }

    /// primary 模式下主連線（session 本身那條）還能送影像：這時其餘連線都待命，影像與補包都不走它們
    bool primary_usable(int64_t now_ns) const {
      if (opt.video != options_t::video_e::primary) {
        return false;
      }
      const int n = count.load(std::memory_order_acquire);
      for (int i = 0; i < n; ++i) {
        if (links[i]->primary && alive(*links[i], now_ns) && now_ns >= links[i]->muted_until_ns.load(std::memory_order_relaxed)) {
          return true;
        }
      }
      return false;
    }

    /// 影像（與補包）現在走不走這條連線。primary_ok＝primary_usable() 的結果（呼叫端每一批只算一次）
    bool video_carrier(const link_t &L, int64_t now_ns, bool primary_ok) const {
      return carrying(L, now_ns) && !(primary_ok && !L.primary);
    }

    /**
     * 除了 self 以外，有沒有「真的比 self 好」的連線在送影像（沒有就不暫停 self）。
     * 要有實際送達率的證據：送達率 ≥ 80%，或比 self 這一秒實際送得出去的比例高出一截（self 幾乎送不動、對方有六七成
     * 也算）。剛恢復、PING 已經停了、或頭盔端其實收不到的連線都不算。2026-10-07 頭盔實測：家用 Wi-Fi 漫遊的那幾秒，
     * 正在卡住的適配器被判成「送不動」而暫停，只剩一條不通的。
     * primary 模式的待命連線不送影像、沒有送達率可看，只要求現在還通。
     */
    bool other_usable(const link_t &self, int64_t now_ns, int self_ok_pct) const {
      const int n = count.load(std::memory_order_acquire);
      for (int i = 0; i < n; ++i) {
        const auto &L = *links[i];
        if (&L == &self) {
          continue;
        }
        if (opt.video == options_t::video_e::primary) {
          if (fresh(L, now_ns) && now_ns >= L.muted_until_ns.load(std::memory_order_relaxed)) {
            return true;
          }
          continue;
        }
        const int d = L.deliv_pct.load(std::memory_order_relaxed);
        if (carrying(L, now_ns) && (d >= kUsableDelivPct || (d >= 0 && d >= self_ok_pct + kMuchBetterPct))) {
          return true;
        }
      }
      return false;
    }

    /// 除了 self 以外，有沒有送達率夠好（或剛開始送、還不知道）的連線在送影像。沒有的話，探測中的 self 量到夠快
    /// 就可以提前回來，不必等最短探測時間。
    bool good_carrier_besides(const link_t &self, int64_t now_ns) const {
      const int n = count.load(std::memory_order_acquire);
      for (int i = 0; i < n; ++i) {
        const auto &L = *links[i];
        const int d = L.deliv_pct.load(std::memory_order_relaxed);
        if (&L != &self && carrying(L, now_ns) && (d < 0 || d >= kUsableDelivPct)) {
          return true;
        }
      }
      return false;
    }

    int64_t fault_ms() const {
      return to_ns(clock::now()) / 1'000'000;
    }

    void arm_video(link_t &L) {
      L.video_sock.async_receive_from(asio::buffer(L.rx_buf_v), L.rx_peer_v, [this, &L](const boost::system::error_code &ec, size_t n) {
        if (stop.load(std::memory_order_relaxed) || ec == asio::error::operation_aborted) {
          return;
        }
        if (!ec) {
          on_video_rx(L, n);
        }
        arm_video(L);
      });
    }

    void arm_audio(link_t &L) {
      // 音訊 socket 只會收到 PING（讓 client 端的防火牆狀態維持住），內容不用看
      L.audio_sock.async_receive_from(asio::buffer(L.rx_buf_a), L.rx_peer_a, [this, &L](const boost::system::error_code &ec, size_t) {
        if (stop.load(std::memory_order_relaxed) || ec == asio::error::operation_aborted) {
          return;
        }
        arm_audio(L);
      });
    }

    void on_video_rx(link_t &L, size_t n) {
      VIPLE_VR_LINK_HDR hdr;
      if (n < sizeof(hdr)) {
        L.rx_bad.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      std::memcpy(&hdr, L.rx_buf_v.data(), sizeof(hdr));
      // 來源必須是 LINK_HELLO（走加密控制通道）宣告的那個端點
      if (hdr.magic[0] != VIPLE_VR_LINK_MAGIC0 || hdr.magic[1] != VIPLE_VR_LINK_MAGIC1 || L.rx_peer_v != L.client_video) {
        L.rx_bad.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      if (opt.fault.blocked(L.id, fault_ms())) {
        return;
      }

      const int64_t now_ns = to_ns(clock::now());
      if (hdr.type == VIPLE_VR_LINK_T_PING) {
        L.rx_ping.fetch_add(1, std::memory_order_relaxed);
        L.last_ping_ns.store(now_ns, std::memory_order_relaxed);
        if (n >= sizeof(hdr) + sizeof(VIPLE_VR_LINK_PING)) {
          VIPLE_VR_LINK_PING ping;
          std::memcpy(&ping, L.rx_buf_v.data() + sizeof(hdr), sizeof(ping));
          L.cl_rx_pkts.store(ping.rxPkts, std::memory_order_relaxed);
          L.cl_rx_used.store(ping.rxUsed, std::memory_order_relaxed);
          L.cl_rtt_ms10.store(ping.rttMs10, std::memory_order_relaxed);
          L.cl_burst_mbps.store(ping.burstMbps, std::memory_order_relaxed);
          L.deliv.on_ping(L.tx_pkts.load(std::memory_order_relaxed), ping.rxPkts, now_ns);
          L.deliv_pct.store(L.deliv.pct, std::memory_order_relaxed);
          if (L.probing.load(std::memory_order_relaxed)) {
            if (L.gate_reset.exchange(false, std::memory_order_relaxed)) {
              L.gate.reset(ping.burstSeq);
            }
            const uint32_t need = probe_need_mbps(video_mbps.load(std::memory_order_relaxed));
            // 量到夠快之後，還要過了最短探測時間才回來——除非現在沒有任何一條送達率夠好的連線在送，那就不等了。
            // burstMbps 65535＝整個探測幀同一瞬間到（一個聚合框就送完），頭盔量不出數字但確定夠快，照樣算一次合格的量測
            if (L.gate.on_report(ping.burstSeq, ping.burstMbps, need, L.probes.load(std::memory_order_relaxed)) &&
                (now_ns >= L.muted_until_ns.load(std::memory_order_relaxed) || !good_carrier_besides(L, now_ns))) {
              L.muted_until_ns.store(0, std::memory_order_relaxed);
              L.sat_reset.store(true, std::memory_order_relaxed);
              L.probing.store(false, std::memory_order_relaxed);
              L.gate.reset();
              L.promotions.fetch_add(1, std::memory_order_relaxed);
              if (ping.burstMbps == 65535) {
                BOOST_LOG(info) << "[VIPLE-VR-LINK] link " << (int) L.id << " measured faster than measurable (need " << need
                                << " Mbps) - video starts on it";
              } else {
                BOOST_LOG(info) << "[VIPLE-VR-LINK] link " << (int) L.id << " measured " << ping.burstMbps << " Mbps (need " << need
                                << ") - video starts on it";
              }
            }
          }
          if (ping.maxGapMs > L.cl_gap_max_ms.load(std::memory_order_relaxed)) {
            L.cl_gap_max_ms.store(ping.maxGapMs, std::memory_order_relaxed);
          }
          if (ping.flags & VIPLE_VR_LINK_PING_F_CONFIRMED) {
            if (L.last_confirmed_ns.exchange(now_ns, std::memory_order_relaxed) == 0) {
              BOOST_LOG(info) << "[VIPLE-VR-LINK] link " << (int) L.id << " confirmed (both directions pass)";
            }
          }
        }
        VIPLE_VR_LINK_HDR pong = hdr;
        pong.type = VIPLE_VR_LINK_T_PONG;
        if (const int delay_ms = opt.fault.delay(L.id); delay_ms > 0) {
          // 故障注入的延遲：PONG 也晚「來回各一趟」才回，client 量到的往返時間才和影像／補包的延遲對得上
          auto timer = std::make_shared<asio::steady_timer>(io, std::chrono::milliseconds {2 * delay_ms});
          timer->async_wait([this, &L, pong, timer](const boost::system::error_code &tec) {
            if (tec || stop.load(std::memory_order_relaxed)) {
              return;
            }
            boost::system::error_code ec;
            std::lock_guard sk {L.sock_mtx};
            L.video_sock.send_to(asio::buffer(&pong, sizeof(pong)), L.client_video, 0, ec);
          });
          return;
        }
        boost::system::error_code ec;
        std::lock_guard sk {L.sock_mtx};
        L.video_sock.send_to(asio::buffer(&pong, sizeof(pong)), L.client_video, 0, ec);  // 非阻塞；送不出去就算了
        return;
      }

      if (hdr.type == VIPLE_VR_LINK_T_DATA) {
        if (n < sizeof(hdr) + crypto::cipher::tag_size + 2) {
          L.rx_bad.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        if (!replay.fresh(hdr.seq)) {
          L.rx_dup.fetch_add(1, std::memory_order_relaxed);  // 另一條連線已經送到
          return;
        }
        std::fill(iv.begin(), iv.end(), (uint8_t) 0);
        std::memcpy(iv.data(), &hdr.seq, sizeof(hdr.seq));
        iv[10] = 'C';  // Client originated
        iv[11] = 'L';  // Link datagram（控制通道是 'C'、影像是 'V'）
        const std::string_view tagged {(const char *) L.rx_buf_v.data() + sizeof(hdr), n - sizeof(hdr)};
        if (cipher.decrypt(tagged, plaintext, &iv) != 0 || plaintext.size() < 2) {
          L.rx_bad.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        replay.mark(hdr.seq);
        L.rx_data.fetch_add(1, std::memory_order_relaxed);
        const uint16_t ptype = (uint16_t) (plaintext[0] | (plaintext[1] << 8));
        if (ptype == VIPLE_VR_PTYPE_C2S) {
          // §VR-LINK-CTRL：沒協商這個功能就不收（舊行為：只認 0x5506）
          if (!(features.load(std::memory_order_relaxed) & VIPLE_VR_LINK_F_CTRL)) {
            return;
          }
          L.rx_ctl.fetch_add(1, std::memory_order_relaxed);
        }
        if (on_c2s && on_c2s(L.id, ptype, std::string_view {(const char *) plaintext.data() + 2, plaintext.size() - 2})) {
          L.trk_first.fetch_add(1, std::memory_order_relaxed);
        }
        return;
      }

      L.rx_bad.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * 短暫睡眠（tx 執行緒）。不能用 std::this_thread::sleep_for：Windows 11 上沒有可見視窗的行程（我們是 service）
     * 拿不到調高的系統計時解析度，要求睡 0.3 ms 實際會睡到下一個 15.6 ms 的 tick（2026-10-07 實測：要求 4 ms，
     * 睡了 7～15.6 ms）。would_block 之後「稍等再試」因此變成每次等十幾毫秒，一批的期限（兩個幀週期）內只試得了
     * 一兩次——鏈路只是短暫塞住也會被當成送不動。高解析度的 waitable timer（影像執行緒的節拍也用它）不受影響。
     */
    void precise_sleep(link_t &L, std::chrono::nanoseconds d) {
      if (d.count() <= 0) {
        return;
      }
      if (L.hp && *L.hp) {
        L.hp->sleep_for(d);
      } else {
        std::this_thread::sleep_for(d);
      }
    }

    /// 故障注入的限速：依速率上限逐封包排隊。回 false＝排到期限之後了（等同 would_block 到期限）
    bool fault_pace(link_t &L, uint32_t pkt_size, clock::time_point deadline) {
      const int cap_mbps = opt.fault.rate_cap(L.id);
      if (cap_mbps <= 0) {
        return true;
      }
      const auto due = std::max(L.fault_next_free, clock::now());
      if (due > deadline) {
        return false;
      }
      while (clock::now() < due) {
        std::this_thread::yield();
      }
      L.fault_next_free = due + std::chrono::nanoseconds {(int64_t) pkt_size * 8 * 1000 / cap_mbps};
      return true;
    }

    /// 非阻塞送一個封包，would_block 就重試到期限。回傳 1＝送出、0＝跳過（其他錯誤，例如網卡剛斷線）、-1＝期限到了
    int send_one(link_t &L, const uint8_t *data, uint32_t size, clock::time_point deadline) {
      const auto buf = asio::buffer(data, size);
      for (;;) {
        boost::system::error_code ec;
        {
          std::lock_guard sk {L.sock_mtx};
          L.video_sock.send_to(buf, L.client_video, 0, ec);
        }
        if (!ec) {
          return 1;
        }
        if (ec == asio::error::would_block || ec == asio::error::try_again) {
          if (clock::now() >= deadline || stop.load(std::memory_order_relaxed)) {
            return -1;
          }
          precise_sleep(L, kBlockRetry);
          continue;
        }
        L.tx_err.fetch_add(1, std::memory_order_relaxed);
        return 0;
      }
    }

    /// §VR-LINK-REPAIR：送一批補包（tx 執行緒）。期限很短（補了來不及就沒有意義）；不進飽和統計，也不受成串掉包的
    /// 注入影響（那條規則只算第一次送出的封包）。鏈路空檔的注入照樣適用：那段時間送什麼都到不了。
    void send_repair(link_t &L, batch_t &b) {
      const std::chrono::milliseconds fault_delay {opt.fault.delay(L.id)};
      if (fault_delay.count() > 0) {
        precise_sleep(L, b.enq + fault_delay - clock::now());
      }
      const auto deadline = b.enq + fault_delay + kRepairMaxAge;
      const auto begin = clock::now();
      const auto wait_us = (uint32_t) std::clamp<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(begin - b.enq).count(), 0, UINT32_MAX);
      if (wait_us > L.repair_wait_max_us.load(std::memory_order_relaxed)) {
        L.repair_wait_max_us.store(wait_us, std::memory_order_relaxed);
      }
      uint32_t sent = 0;
      if (opt.fault.blocked(L.id, to_ns(b.enq) / 1'000'000)) {
        L.drop_fault.fetch_add(b.count, std::memory_order_relaxed);
      } else if (begin > deadline) {
        // 在佇列裡等過頭了（tx 執行緒卡在某個影像封包的 would_block 重試，那一段不看補包佇列）：整批不送，
        // 算進 late。socket 剛恢復可寫就先塞一批來不及的補包，只會讓後面的新影像更晚
      } else {
        for (uint32_t k = 0; k < b.count; ++k) {
          // send_one 送得出去時不看期限：送到一半才過期的也在這裡停
          if (k != 0 && clock::now() > deadline) {
            break;
          }
          if (!fault_pace(L, b.pkt_size, deadline)) {
            break;
          }
          const int r = send_one(L, b.data.data() + (size_t) k * b.pkt_size, b.pkt_size, deadline);
          if (r < 0) {
            break;
          }
          sent += r > 0 ? 1 : 0;
        }
      }
      L.tx_pkts.fetch_add(sent, std::memory_order_relaxed);  // 補包也算這條連線送出的封包：頭盔端的收包數同樣會算到它
      L.tx_bytes.fetch_add(sent * (uint64_t) b.pkt_size, std::memory_order_relaxed);
      L.repair_tx.fetch_add(sent, std::memory_order_relaxed);
      L.repair_drop.fetch_add(b.count - sent, std::memory_order_relaxed);
      if (L.repair_logs++ < 12 || L.repair_logs % 50 == 0) {  // 每場每條連線前 12 筆照印，之後每 50 筆一筆
        BOOST_LOG(info) << "[VIPLE-VR-REPAIR] link " << (int) L.id << " sent " << sent << '/' << b.count << " repair pkts, queued "
                        << std::format("{:.1f}", wait_us / 1000.0) << " ms, sending took "
                        << std::format("{:.1f}", std::chrono::duration<double, std::milli>(clock::now() - begin).count()) << " ms";
      }
    }

    /// 補包佇列裡有東西就全部送掉（tx 執行緒）
    void flush_repairs(link_t &L) {
      for (;;) {
        batch_t r;
        {
          std::lock_guard lk {L.mtx};
          if (L.rq.empty()) {
            L.rq_pending.store(false, std::memory_order_relaxed);
            return;
          }
          r = std::move(L.rq.front());
          L.rq.pop_front();
        }
        send_repair(L, r);
        std::lock_guard lk {L.mtx};
        if (L.pool.size() < 16) {
          L.pool.push_back(std::move(r.data));
        }
      }
    }

    void tx_loop(link_t &L) {
      platf::set_thread_name("vr::link" + std::to_string((int) L.id));
      L.hp = platf::create_high_precision_timer();
      while (!stop.load(std::memory_order_relaxed)) {
        batch_t b;
        {
          std::unique_lock lk {L.mtx};
          L.cv.wait_for(lk, 100ms, [&] {
            return stop.load(std::memory_order_relaxed) || !L.q.empty() || !L.rq.empty();
          });
          if (stop.load(std::memory_order_relaxed)) {
            break;
          }
          if (!L.rq.empty()) {
            lk.unlock();
            flush_repairs(L);  // 補包優先：它的期限只有幾毫秒
            continue;
          }
          if (L.q.empty()) {
            continue;
          }
          b = std::move(L.q.front());
          L.q.pop_front();
          L.q_bytes -= b.data.size();
        }

        // 故障注入：這條連線的每一批都晚這麼久才送（模擬落後的鏈路）
        const std::chrono::milliseconds fault_delay {opt.fault.delay(L.id)};
        if (fault_delay.count() > 0) {
          precise_sleep(L, b.enq + fault_delay - clock::now());
        }

        const auto begin = clock::now();
        const auto deadline = b.enq + fault_delay + opt.video_max_age;
        size_t sent = 0;
        bool faulted = false;
        if (opt.fault.blocked(L.id, to_ns(b.enq) / 1'000'000)) {  // 以入列時刻判斷：每條連線的同一批入列時刻相同，互補的規則不會把同一批在兩條都丟掉
          faulted = true;
          L.drop_fault.fetch_add(b.count, std::memory_order_relaxed);
        } else if (begin > deadline) {
          L.drop_age.fetch_add(b.count, std::memory_order_relaxed);
        } else {
          const auto burst = opt.fault.burst_rule(L.id);
          for (uint32_t k = 0; k < b.count; ++k) {
            // §VR-LINK-REPAIR：補包插隊（送影像批次的途中也先送）。有延遲注入時不插，維持「晚到」的順序
            if (L.rq_pending.load(std::memory_order_relaxed) && fault_delay.count() == 0) {
              flush_repairs(L);
            }
            if (burst.pkts > 0) {
              // 故障注入：每隔一段時間連續掉幾個封包。算「送出了」（鏈路上掉的）：頭盔收不到，送達率會反映出來
              const int64_t t = to_ns(clock::now());
              const int64_t period = (int64_t) burst.period_ms * 1'000'000;
              if (L.burst_next_ns == 0) {
                L.burst_next_ns = t + period;
              } else if (t >= L.burst_next_ns) {
                L.burst_left = (uint32_t) burst.pkts;
                L.burst_next_ns += period;
                if (L.burst_next_ns <= t) {
                  L.burst_next_ns = t + period;
                }
              }
              if (L.burst_left > 0) {
                --L.burst_left;
                ++sent;
                L.drop_fault.fetch_add(1, std::memory_order_relaxed);
                continue;
              }
            }
            // 故障注入：模擬慢鏈路。每個封包依速率上限排隊，排到期限之後的就送不出去（等同 would_block 到期限）
            if (!fault_pace(L, b.pkt_size, deadline)) {
              break;
            }
            const int r = send_one(L, b.data.data() + (size_t) k * b.pkt_size, b.pkt_size, deadline);
            if (r < 0) {
              break;
            }
            sent += r > 0 ? 1 : 0;
          }
          if (sent < b.count) {
            L.drop_block.fetch_add(b.count - sent, std::memory_order_relaxed);
          }
          L.tx_pkts.fetch_add(sent, std::memory_order_relaxed);
          L.tx_bytes.fetch_add(sent * (uint64_t) b.pkt_size, std::memory_order_relaxed);
        }

        const auto end = clock::now();
        const auto spent_us = (uint32_t) std::min<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count(), UINT32_MAX);
        if (spent_us > L.block_max_us.load(std::memory_order_relaxed)) {
          L.block_max_us.store(spent_us, std::memory_order_relaxed);
        }

        // 飽和偵測：故障注入丟掉的不算（那是模擬鏈路空檔，不是送不動）
        if (L.sat_reset.exchange(false, std::memory_order_relaxed)) {
          L.sat.restart();  // 剛從探測轉為送影像：只統計真正在送影像的期間
        }
        // 探測中的批次也不算：一次探測是一整幀，會湊滿視窗，把還在探測的連線再判一次「送不動」（退避加倍、
        // 重新快速探測），或把探測期算成健康期而提早把退避歸零
        if (!faulted && !L.probing.load(std::memory_order_relaxed) && L.sat.report(sent == b.count, end)) {
          const int64_t now_ns = to_ns(end);
          if (other_usable(L, now_ns, L.sat.last_ok_pct)) {
            // all：時間到就再試，所以連續發生時要加倍退避；automatic：恢復由量測決定，只留一個很短的下限
            // 量測可能高估（時間取在讀取時刻）：連續被判送不動時要退避，免得每隔一兩秒就把另一條拖下去一次
            const auto hold = opt.video == options_t::video_e::automatic ? std::min<std::chrono::milliseconds>(L.sat.next_hold(), kAutoHoldMax) : L.sat.next_hold();
            L.muted_until_ns.store(now_ns + std::chrono::duration_cast<std::chrono::nanoseconds>(hold).count(), std::memory_order_relaxed);
            L.mutes.fetch_add(1, std::memory_order_relaxed);
            if (opt.video == options_t::video_e::automatic) {
              // automatic：不是時間到就重試，而是退回探測——量到夠快才再送影像
              L.gate_reset.store(true, std::memory_order_relaxed);
              L.fast_probes.store((int) kProbeFastCount, std::memory_order_relaxed);
              L.probing.store(true, std::memory_order_relaxed);
            }
            BOOST_LOG(info) << "[VIPLE-VR-LINK] link " << (int) L.id << " cannot keep up - video paused on it for "
                            << (opt.video == options_t::video_e::automatic ? "at least " : "")
                            << std::format("{:.0f}", hold.count() / 1000.0) << " s (tracking, audio and pings stay)";
            // 佇列裡剩下的也不送了
            std::lock_guard lk {L.mtx};
            for (auto &old : L.q) {
              L.drop_muted.fetch_add(old.count, std::memory_order_relaxed);
              if (L.pool.size() < 16) {
                L.pool.push_back(std::move(old.data));
              }
            }
            L.q.clear();
            L.q_bytes = 0;
          }
        }

        std::lock_guard lk {L.mtx};
        if (L.pool.size() < 16) {
          L.pool.push_back(std::move(b.data));
        }
      }
    }
  };

  hub_t::hub_t(const crypto::aes_t &key, options_t opt, c2s_cb_t on_c2s):
      impl_ {std::make_unique<impl_t>(key, std::move(opt), std::move(on_c2s))} {
    impl_->rx = std::thread {[p = impl_.get()] {
      platf::set_thread_name("vr::link-rx");
      p->io.run();
    }};
    BOOST_LOG(info) << "[VIPLE-VR-LINK] multi-link ready: video="
                    << (impl_->opt.video == options_t::video_e::all ? "all" : impl_->opt.video == options_t::video_e::primary ? "primary" : "auto")
                    << " maxAgeMs=" << std::format("{:.1f}", impl_->opt.video_max_age.count() / 1000.0)
                    << (impl_->opt.fault.any() ? " FAULT INJECTION ON (dev)" : "");
  }

  hub_t::~hub_t() {
    stop();
  }

  void hub_t::stop() {
    auto &m = *impl_;
    {
      // on_hello 整段都握著這把鎖：拿到鎖並設好 stop 之後，不會再有新的連線（與它的執行緒）出現
      std::lock_guard lk {m.hello_mtx};
      if (m.stop.exchange(true)) {
        return;
      }
    }
    const int n = m.count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
      m.links[i]->cv.notify_all();
    }
    m.work.reset();
    m.io.stop();
    if (m.rx.joinable()) {
      m.rx.join();
    }
    for (int i = 0; i < n; ++i) {
      auto &L = *m.links[i];
      if (L.tx.joinable()) {
        L.tx.join();
      }
      // socket 留到 hub 解構才關：音訊的廣播執行緒可能還在送最後幾個封包，這裡關會和它的 send_to 搶同一個物件
    }
  }

  std::vector<ready_link_t> hub_t::on_hello(const std::vector<hello_link_t> &links, const std::array<uint8_t, 4> &primary_client_addr) {
    auto &m = *impl_;
    std::vector<ready_link_t> out;
    std::lock_guard lk {m.hello_mtx};
    if (m.stop.load(std::memory_order_relaxed)) {
      return out;
    }

    // §VR-LINK-CTRL：client 在每條連線的 flags 填它支援的功能；server 的開關也開才啟用
    if (!links.empty()) {
      uint8_t offered = 0xFF;
      for (const auto &h : links) {
        offered &= h.flags;
      }
      uint8_t feat = 0;
      if (m.opt.ctrl && (offered & VIPLE_VR_LINK_F_CTRL)) {
        feat |= VIPLE_VR_LINK_F_CTRL;
        if (m.opt.repair && (offered & VIPLE_VR_LINK_F_REPAIR)) {
          feat |= VIPLE_VR_LINK_F_REPAIR;
        }
      }
      if (m.features.exchange(feat, std::memory_order_relaxed) != feat || m.count.load(std::memory_order_relaxed) == 0) {
        BOOST_LOG(info) << "[VIPLE-VR-LINK] link features: ctrl=" << ((feat & VIPLE_VR_LINK_F_CTRL) ? "on" : "off")
                        << " repair=" << ((feat & VIPLE_VR_LINK_F_REPAIR) ? "on" : "off")
                        << " (client offered 0x" << std::format("{:02x}", (unsigned) offered)
                        << ", server ctrl=" << (m.opt.ctrl ? "enabled" : "disabled") << " repair=" << (m.opt.repair ? "enabled" : "disabled") << ')';
      }
    }

    for (const auto &h : links) {
      ready_link_t r;
      r.id = h.id;

      // client 重送 HELLO（例如控制通道重連）：描述相同就回原來的埠，不重建
      link_t *existing = nullptr;
      const int n = m.count.load(std::memory_order_acquire);
      for (int i = 0; i < n; ++i) {
        if (m.links[i]->id == h.id) {
          existing = m.links[i].get();
        }
      }
      if (existing) {
        if (existing->desc == h) {
          r.status = VIPLE_VR_LINK_READY_OK;
          r.server_video_port = existing->server_video_port;
          r.server_audio_port = existing->server_audio_port;
        }
        out.push_back(r);
        continue;
      }
      if (n >= kMaxLinks) {
        out.push_back(r);
        continue;
      }

      try {
        auto L = std::make_unique<link_t>(m.io);
        L->id = h.id;
        L->desc = h;
        L->primary = h.client_addr == primary_client_addr;
        for (int i = 0; i < n; ++i) {
          L->primary = L->primary && !m.links[i]->primary;  // 主連線只有一條（同位址有好幾條時取最早的，例如 selftest）
        }
        // automatic：session 本身那條先送影像，其餘先探測
        L->probing.store(m.opt.video == options_t::video_e::automatic && !L->primary, std::memory_order_relaxed);
        L->client_video = udp::endpoint {to_v4(h.client_addr), h.client_video_port};
        L->client_audio = udp::endpoint {to_v4(h.client_addr), h.client_audio_port};
        // 綁在 client 配對到的 server 位址：不是本機位址就會 bind 失敗（拒絕這條連線）
        const auto local = to_v4(h.server_addr);
        for (auto *s : {&L->video_sock, &L->audio_sock}) {
          s->open(udp::v4());
          s->bind(udp::endpoint {local, 0});
          s->non_blocking(true);
          boost::system::error_code ec;
          s->set_option(asio::socket_base::send_buffer_size {kSockSndBuf}, ec);
        }
        L->server_video_port = L->video_sock.local_endpoint().port();
        L->server_audio_port = L->audio_sock.local_endpoint().port();

        r.status = VIPLE_VR_LINK_READY_OK;
        r.server_video_port = L->server_video_port;
        r.server_audio_port = L->server_audio_port;

        // 和單一路徑一致：client 要求 QoS 時，連線的 socket 也加入 qWAVE flow（Wi-Fi 上走影音的優先權佇列）
        if (m.opt.video_qos) {
          boost::asio::ip::address a = L->client_video.address();
          L->qos_v = platf::enable_socket_qos((uintptr_t) L->video_sock.native_handle(), a, L->client_video.port(), platf::qos_data_type_e::video, true);
        }
        if (m.opt.audio_qos) {
          boost::asio::ip::address a = L->client_audio.address();
          L->qos_a = platf::enable_socket_qos((uintptr_t) L->audio_sock.native_handle(), a, L->client_audio.port(), platf::qos_data_type_e::audio, true);
        }

        // 順序：先起送出執行緒（失敗就整條丟掉，還沒有人看得到它），再公開、最後才開始接收
        auto *p = L.get();
        p->tx = std::thread {[&m, p] {
          m.tx_loop(*p);
        }};
        m.links[n] = std::move(L);
        m.count.store(n + 1, std::memory_order_release);
        asio::post(m.io, [&m, p] {
          m.arm_video(*p);
          m.arm_audio(*p);
        });

        BOOST_LOG(info) << "[VIPLE-VR-LINK] link " << (int) h.id << " created: server " << local.to_string() << " video:" << p->server_video_port
                        << " audio:" << p->server_audio_port << " <-> client " << p->client_video.address().to_string() << " video:"
                        << h.client_video_port << " audio:" << h.client_audio_port << (p->primary ? " (session address)" : "");
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "[VIPLE-VR-LINK] link " << (int) h.id << " refused: " << e.what();
        r = ready_link_t {};
        r.id = h.id;
      }
      out.push_back(r);
    }
    return out;
  }

  bool hub_t::active() const {
    const auto &m = *impl_;
    if (m.stop.load(std::memory_order_relaxed)) {
      return false;
    }
    const int64_t now_ns = to_ns(clock::now());
    const int n = m.count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
      if (m.alive(*m.links[i], now_ns)) {
        return true;
      }
    }
    return false;
  }

  uint8_t hub_t::features() const {
    return impl_->features.load(std::memory_order_relaxed);
  }

  bool hub_t::fault_ctrl_blocked() const {
    return impl_->opt.fault.ctrl_blocked(impl_->fault_ms());
  }

  int hub_t::send_s2c(std::string_view tlv) {
    auto &m = *impl_;
    constexpr size_t kOverhead = sizeof(VIPLE_VR_LINK_HDR) + crypto::cipher::tag_size + 2;
    if (m.stop.load(std::memory_order_relaxed) || !(m.features.load(std::memory_order_relaxed) & VIPLE_VR_LINK_F_CTRL) ||
        tlv.empty() || kOverhead + tlv.size() >= VIPLE_VR_LINK_CTRL_MAX_LEN) {
      return 0;
    }

    // 資料報＝hdr＋tag＋密文（明文＝u16 ptype＋TLV）。同一則訊息在每條連線的 seq 相同，client 以它去重
    std::array<uint8_t, VIPLE_VR_LINK_CTRL_MAX_LEN> pkt {};
    std::array<uint8_t, VIPLE_VR_LINK_CTRL_MAX_LEN> plain {};
    plain[0] = (uint8_t) (VIPLE_VR_PTYPE_S2C & 0xFF);
    plain[1] = (uint8_t) (VIPLE_VR_PTYPE_S2C >> 8);
    std::memcpy(plain.data() + 2, tlv.data(), tlv.size());
    const size_t plain_len = 2 + tlv.size();

    VIPLE_VR_LINK_HDR hdr {};
    hdr.magic[0] = VIPLE_VR_LINK_MAGIC0;
    hdr.magic[1] = VIPLE_VR_LINK_MAGIC1;
    hdr.type = VIPLE_VR_LINK_T_S2C;
    {
      std::lock_guard lk {m.s2c_mtx};
      hdr.seq = ++m.s2c_seq;  // 整場遞增、不歸零：同一把 key 之下 IV 不會重複
      std::fill(m.s2c_iv.begin(), m.s2c_iv.end(), (uint8_t) 0);
      std::memcpy(m.s2c_iv.data(), &hdr.seq, sizeof(hdr.seq));
      m.s2c_iv[10] = 'H';  // Host originated
      m.s2c_iv[11] = 'L';  // Link datagram
      if (m.s2c_cipher.encrypt(std::string_view {(const char *) plain.data(), plain_len}, pkt.data() + sizeof(hdr),
                               pkt.data() + sizeof(hdr) + crypto::cipher::tag_size, &m.s2c_iv) != (int) plain_len) {
        return 0;
      }
    }
    const size_t total = sizeof(hdr) + crypto::cipher::tag_size + plain_len;

    const int64_t now_ns = to_ns(clock::now());
    const int n = m.count.load(std::memory_order_acquire);
    int sent = 0;
    for (int i = 0; i < n; ++i) {
      auto &L = *m.links[i];
      if (!m.alive(L, now_ns) || m.opt.fault.blocked(L.id, now_ns / 1'000'000)) {
        continue;
      }
      hdr.linkId = L.id;
      std::memcpy(pkt.data(), &hdr, sizeof(hdr));
      boost::system::error_code ec;
      {
        std::lock_guard sk {L.sock_mtx};
        L.video_sock.send_to(asio::buffer(pkt.data(), total), L.client_video, 0, ec);
      }
      if (ec) {
        L.s2c_drop.fetch_add(1, std::memory_order_relaxed);
      } else {
        L.s2c_tx.fetch_add(1, std::memory_order_relaxed);
        ++sent;
      }
    }
    return sent;
  }

  void hub_t::send_video(const uint8_t *pkts, size_t pkt_size, size_t count, const video_meta_t *meta) {
    auto &m = *impl_;
    if (m.stop.load(std::memory_order_relaxed) || count == 0 || pkt_size == 0) {
      return;
    }
    const auto now = clock::now();
    const int64_t now_ns = to_ns(now);
    const int n = m.count.load(std::memory_order_acquire);

    // §VR-LINK-REPAIR：先留一份（已加密的位元組），client 之後回報缺哪些就原樣補送
    if (meta && (m.features.load(std::memory_order_relaxed) & VIPLE_VR_LINK_F_REPAIR)) {
      m.pkt_bytes.store((uint32_t) pkt_size, std::memory_order_relaxed);
      std::lock_guard lk {m.store_mtx};
      m.store.put(*meta, pkts, pkt_size, count, now_ns);
    }

    const bool primary_alive = m.primary_usable(now_ns);

    const size_t bytes = pkt_size * count;

    // 影像位元率（automatic 的恢復門檻用）
    m.rate_bytes += bytes;
    if (m.rate_since == clock::time_point {}) {
      m.rate_since = now;
    } else if (now - m.rate_since >= 500ms) {
      const double secs = std::chrono::duration<double>(now - m.rate_since).count();
      m.video_mbps.store((uint32_t) (m.rate_bytes * 8 / secs / 1e6), std::memory_order_relaxed);
      m.rate_bytes = 0;
      m.rate_since = now;
    }

    // 保底：至少要有一條連線在送影像。可用的連線全都在探測或暫停時（例如正在送的那條剛斷、剩下的還沒量完），
    // 這一批就在每條可用的連線都送——原本的單一路徑這時是停的，不送就沒有畫面。
    bool any_carrier = false;
    for (int i = 0; i < n && !any_carrier; ++i) {
      const auto &L = *m.links[i];
      // 用 fresh（最近 0.7 s 有 PING）而不是 alive（2 s）：唯一在送的那條一沒聲音就立刻保底，不白等兩秒
      any_carrier = m.video_carrier(L, now_ns, primary_alive);
    }

    for (int i = 0; i < n; ++i) {
      auto &L = *m.links[i];
      if (!m.alive(L, now_ns)) {
        continue;
      }
      if (!any_carrier) {
        L.forced.fetch_add(count, std::memory_order_relaxed);
      } else if (primary_alive && !L.primary) {
        L.drop_policy.fetch_add(count, std::memory_order_relaxed);
        continue;
      } else if (L.probing.load(std::memory_order_relaxed)) {
        // 探測中：每秒只送一批（夠大的批次才量得準），其餘不送
        const int64_t interval = L.fast_probes.load(std::memory_order_relaxed) > 0 ? kProbeFastIntervalNs : kProbeIntervalNs;
        if (now_ns < L.probe_until_ns.load(std::memory_order_relaxed)) {
          // 這次探測還在進行：同一幀後面的批次也送
        } else if (count >= kProbeMinPkts && now_ns - L.last_probe_ns.load(std::memory_order_relaxed) >= interval) {
          L.probe_until_ns.store(now_ns + kProbeSpanNs, std::memory_order_relaxed);
          L.last_probe_ns.store(now_ns, std::memory_order_relaxed);
          L.small_since_ns.store(0, std::memory_order_relaxed);
          L.probes.fetch_add(1, std::memory_order_relaxed);
          if (L.fast_probes.load(std::memory_order_relaxed) > 0) {
            L.fast_probes.fetch_sub(1, std::memory_order_relaxed);
          }
        } else {
          // 位元率很低時（每幀不到 32 個封包）永遠等不到夠大的批次，client 也就量不出速率。這種流量很小，
          // 等了 kProbeNoBatchNs 還是沒有就直接開始送；真的送不動，飽和偵測會再把它退回來。
          bool start_now = false;
          if (count < kProbeMinPkts && now_ns - L.last_probe_ns.load(std::memory_order_relaxed) >= interval &&
              now_ns >= L.muted_until_ns.load(std::memory_order_relaxed)) {
            const int64_t since = L.small_since_ns.load(std::memory_order_relaxed);
            if (since == 0) {
              L.small_since_ns.store(now_ns, std::memory_order_relaxed);
            } else if (now_ns - since >= kProbeNoBatchNs) {
              start_now = true;
            }
          }
          if (!start_now) {
            L.drop_policy.fetch_add(count, std::memory_order_relaxed);
            continue;
          }
          L.small_since_ns.store(0, std::memory_order_relaxed);
          L.sat_reset.store(true, std::memory_order_relaxed);
          L.probing.store(false, std::memory_order_relaxed);
          L.promotions.fetch_add(1, std::memory_order_relaxed);
          BOOST_LOG(info) << "[VIPLE-VR-LINK] link " << (int) L.id << " could not be measured (video batches too small) - video starts on it";
        }
      } else if (now_ns < L.muted_until_ns.load(std::memory_order_relaxed)) {
        L.drop_muted.fetch_add(count, std::memory_order_relaxed);
        continue;
      }

      batch_t b;
      {
        std::lock_guard lk {L.mtx};
        while (!L.q.empty() && L.q_bytes + bytes > kQueueMaxBytes) {
          auto &old = L.q.front();
          L.drop_full.fetch_add(old.count, std::memory_order_relaxed);
          L.q_bytes -= old.data.size();
          if (L.pool.size() < 16) {
            L.pool.push_back(std::move(old.data));
          }
          L.q.pop_front();
        }
        if (!L.pool.empty()) {
          b.data = std::move(L.pool.back());
          L.pool.pop_back();
        }
      }
      b.data.assign(pkts, pkts + bytes);
      b.pkt_size = (uint32_t) pkt_size;
      b.count = (uint32_t) count;
      b.enq = now;
      {
        std::lock_guard lk {L.mtx};
        L.q_bytes += b.data.size();
        L.q.push_back(std::move(b));
      }
      L.cv.notify_one();
    }
  }

  hub_t::nack_result_t hub_t::on_nack(const uint8_t *body, size_t len) {
    auto &m = *impl_;
    nack_result_t res;
    nack_t nack;
    if (m.stop.load(std::memory_order_relaxed) || !(m.features.load(std::memory_order_relaxed) & VIPLE_VR_LINK_F_REPAIR) ||
        !parse_nack(body, len, nack)) {
      return res;
    }
    const auto now = clock::now();
    const int64_t now_ns = to_ns(now);

    // 補包和影像走同樣的連線（primary 模式下待命的連線不送）
    const bool primary_ok = m.primary_usable(now_ns);

    // 上一輪補的封包多久之內算「還在路上」：正在送影像的連線裡最短的往返時間（client 在 PING 回報的）＋3 ms
    int64_t gap_ns = kRepairGapMaxNs;
    {
      const int n = m.count.load(std::memory_order_acquire);
      for (int i = 0; i < n; ++i) {
        const auto &L = *m.links[i];
        const uint16_t rtt = L.cl_rtt_ms10.load(std::memory_order_relaxed);
        if (rtt != VIPLE_VR_LINK_RTT_UNKNOWN && m.video_carrier(L, now_ns, primary_ok)) {
          gap_ns = std::min<int64_t>(gap_ns, (int64_t) rtt * 100'000 + 3'000'000);
        }
      }
      gap_ns = std::clamp(gap_ns, kRepairGapMinNs, kRepairGapMaxNs);
    }

    batch_t rb;
    bool log_it = false;
    std::string log_detail;
    {
      std::lock_guard lk {m.store_mtx};
      ++m.rstats.nack;
      log_it = m.nack_logs++ < 12 || m.nack_logs % 50 == 0;  // 每場前 12 筆照印，之後每 50 筆一筆
      auto *b = m.store.find(nack.frame, nack.block);
      if (log_it && b) {
        unsigned have = 0;
        for (unsigned i = 0; i < b->total && nack.total == b->total; ++i) {
          have += bit_get(nack.have, i) ? 1 : 0;
        }
        log_detail = std::format("age={:.1f}ms have={}/{} (data {}) attempt={} round={}", (now_ns - b->first_ns) / 1e6, have, (unsigned) b->total,
                                 (unsigned) b->data, (unsigned) nack.attempt, (unsigned) b->rounds + 1);
      }
      if (!b) {
        res.gone = VIPLE_VR_GONE_UNKNOWN;
        ++m.rstats.gone_unknown;
      } else if (now_ns - b->first_ns > std::chrono::duration_cast<std::chrono::nanoseconds>(m.opt.video_max_age).count() * 3 / 2) {
        // 三個幀週期以前的 block（video_max_age 是兩個）：補了也來不及。client 的等待上限是「後面的封包到了之後一個
        // 幀週期」，不是從這個 block 送出起算——鏈路斷一小段、連續兩幀整個沒到時，它要等第三幀的封包到了才知道缺，
        // 這時第一幀已經超過兩個幀週期，但它還願意再等一個週期，補得回來（2026-10-07 S0：12 ms 的空檔剛好吞掉
        // 兩整幀的相位，上限是兩個週期時這兩幀都救不回）。空檔再長（三幀以上）就不補了，交給 refresh。
        res.gone = VIPLE_VR_GONE_EXPIRED;
        ++m.rstats.gone_expired;
        if (!log_it && (m.gone_logs++ < 30 || m.gone_logs % 20 == 0)) {  // 每場前 30 筆照印，之後每 20 筆一筆
          log_it = true;
          log_detail = std::format("age={:.1f}ms attempt={} rounds={}", (now_ns - b->first_ns) / 1e6, (unsigned) nack.attempt, (unsigned) b->rounds);
        }
      } else {
        // 上一輪補的還在路上：那些 shard 當成 client 會收到（這份回報是它等不及提早報的，位元圖還沒反映）
        const bool in_flight = b->last_round_ns != 0 && now_ns - b->last_round_ns < gap_ns;
        unsigned needed = 0;
        const auto plan = plan_repair(nack, b->data, b->total, b->stored, in_flight ? &b->resent : nullptr, &needed);
        if (plan.empty()) {
          ++m.rstats.nothing;  // client 其實已經夠還原（回報過期）、它缺的還沒送出，或缺的都剛補過還在路上
        } else if (b->rounds >= kRepairMaxRounds) {
          // 補滿輪數還缺：不再補，但也不回 GONE——前幾輪補的可能還在路上，client 自己有回報次數與等待時間的上限
          ++m.rstats.exhausted;
        } else {
          const uint32_t mbps = m.video_mbps.load(std::memory_order_relaxed);
          const double pps = mbps > 0 ? (double) mbps * 1e6 / 8.0 / (double) b->pkt_size : 15000.0;
          if (!m.budget.take((unsigned) plan.size(), pps, now_ns)) {
            res.gone = VIPLE_VR_GONE_BUDGET;
            ++m.rstats.gone_budget;
            if (!log_it && (m.gone_logs++ < 30 || m.gone_logs % 20 == 0)) {  // 和「太舊」共用同一個節流
              log_it = true;
              log_detail = std::format("age={:.1f}ms attempt={} rounds={} wanted={} pkts, budget left {:.0f}", (now_ns - b->first_ns) / 1e6,
                                       (unsigned) nack.attempt, (unsigned) b->rounds, plan.size(), m.budget.tokens);
            }
          } else {
            ++b->rounds;
            if (!in_flight) {
              b->resent.fill(0);  // 上一輪補的早該到了卻還缺：那些是真的又掉了，這一輪重新算
            }
            for (const uint8_t shard : plan) {
              bit_set(b->resent, shard);
            }
            b->last_round_ns = now_ns;
            rb.pkt_size = b->pkt_size;
            rb.count = (uint32_t) plan.size();
            rb.enq = now;
            rb.data.resize((size_t) rb.count * rb.pkt_size);
            for (size_t i = 0; i < plan.size(); ++i) {
              std::memcpy(rb.data.data() + i * rb.pkt_size, b->pkt(plan[i]), rb.pkt_size);
            }
            ++m.rstats.rounds;
            m.rstats.pkts += plan.size();
            res.resent = rb.count;
            res.needed = std::min<unsigned>(needed, rb.count);
          }
        }
      }
    }

    if (log_it) {
      BOOST_LOG(info) << "[VIPLE-VR-REPAIR] nack frame=" << nack.frame << " block=" << (int) nack.block << ' ' << (log_detail.empty() ? "(not in store)" : log_detail)
                      << " -> " << (res.gone ? std::format("GONE reason={}", (int) res.gone) : res.resent ? std::format("resend {} pkts", res.resent) : std::string {"nothing to send"});
    }
    if (res.gone != 0) {
      // 補不了：告訴 client 不必再等（它會照原本的方式放棄這一幀並回報 LOSS）
      VIPLE_VR_TLV_REPAIR_GONE g {};
      g.frame = nack.frame;
      g.block = nack.block;
      g.reason = res.gone;
      std::array<uint8_t, 2 + sizeof(g)> tlv;
      tlv[0] = VIPLE_VR_S2C_REPAIR_GONE;
      tlv[1] = (uint8_t) sizeof(g);
      std::memcpy(tlv.data() + 2, &g, sizeof(g));
      send_s2c(std::string_view {(const char *) tlv.data(), tlv.size()});
      return res;
    }
    if (res.resent == 0) {
      return res;
    }

    // 走正在送影像的連線（NACK 從哪條先到不代表那條的下行好）；沒有任何一條在送時（保底中）就每條可用的都送，
    // 和 send_video 的保底一致
    const int n = m.count.load(std::memory_order_acquire);
    bool any_carrying = false;
    for (int i = 0; i < n; ++i) {
      any_carrying = any_carrying || m.video_carrier(*m.links[i], now_ns, primary_ok);
    }
    for (int i = 0; i < n; ++i) {
      auto &L = *m.links[i];
      if (any_carrying ? !m.video_carrier(L, now_ns, primary_ok) : !m.alive(L, now_ns)) {
        continue;
      }
      batch_t copy;
      copy.pkt_size = rb.pkt_size;
      copy.count = rb.count;
      copy.enq = rb.enq;
      {
        std::lock_guard lk {L.mtx};
        if (!L.pool.empty()) {
          copy.data = std::move(L.pool.back());
          L.pool.pop_back();
        }
      }
      copy.data.assign(rb.data.begin(), rb.data.end());
      {
        std::lock_guard lk {L.mtx};
        L.rq.push_back(std::move(copy));
        L.rq_pending.store(true, std::memory_order_relaxed);
      }
      L.cv.notify_one();
    }
    return res;
  }

  std::string hub_t::take_repair_line() {
    auto &m = *impl_;
    if (!(m.features.load(std::memory_order_relaxed) & VIPLE_VR_LINK_F_REPAIR)) {
      return {};
    }
    impl_t::repair_stats_t c;
    {
      std::lock_guard lk {m.store_mtx};
      c = m.rstats;
    }
    const auto &p = m.rstats_prev;
    std::string s;
    if (c.nack != p.nack) {
      s = std::format("nack={} rounds={} pkts={} gone={} (expired={} budget={} unknown={}) nothingToSend={} exhausted={}",
                      c.nack - p.nack, c.rounds - p.rounds, c.pkts - p.pkts,
                      (c.gone_expired - p.gone_expired) + (c.gone_budget - p.gone_budget) + (c.gone_unknown - p.gone_unknown),
                      c.gone_expired - p.gone_expired, c.gone_budget - p.gone_budget, c.gone_unknown - p.gone_unknown,
                      c.nothing - p.nothing, c.exhausted - p.exhausted);
      const int n = m.count.load(std::memory_order_acquire);
      for (int i = 0; i < n; ++i) {
        auto &L = *m.links[i];
        const uint64_t tx = L.repair_tx.load(std::memory_order_relaxed);
        const uint64_t drop = L.repair_drop.load(std::memory_order_relaxed);
        s += std::format(" | L{} tx={} late={} queuedMaxMs={:.1f}", (int) L.id, tx - L.repair_tx_prev, drop - L.repair_drop_prev,
                         L.repair_wait_max_us.exchange(0, std::memory_order_relaxed) / 1000.0);
        L.repair_tx_prev = tx;
        L.repair_drop_prev = drop;
      }
    }
    m.rstats_prev = c;
    return s;
  }

  void hub_t::send_audio(const uint8_t *hdr, size_t hdr_len, const uint8_t *payload, size_t payload_len) {
    auto &m = *impl_;
    if (m.stop.load(std::memory_order_relaxed)) {
      return;
    }
    const auto now = clock::now();
    const int64_t now_ns = to_ns(now);
    const int n = m.count.load(std::memory_order_acquire);
    const std::array<asio::const_buffer, 2> bufs {asio::buffer(hdr, hdr_len), asio::buffer(payload, payload_len)};
    for (int i = 0; i < n; ++i) {
      auto &L = *m.links[i];
      if (!m.alive(L, now_ns) || m.opt.fault.blocked(L.id, now_ns / 1'000'000)) {
        continue;
      }
      boost::system::error_code ec;
      L.audio_sock.send_to(bufs, L.client_audio, 0, ec);
      (ec ? L.audio_drop : L.audio_pkts).fetch_add(1, std::memory_order_relaxed);
    }
  }

  std::string hub_t::take_stats_line() {
    auto &m = *impl_;
    const int n = m.count.load(std::memory_order_acquire);
    if (n == 0) {
      return {};
    }
    const auto now = clock::now();
    const int64_t now_ns = to_ns(now);
    const double secs = m.stats_prev == clock::time_point {} ? 10.0 : std::max(0.001, std::chrono::duration<double>(now - m.stats_prev).count());
    m.stats_prev = now;

    const uint64_t enet = enet_trk_first_.load(std::memory_order_relaxed);
    std::string s = std::format("trkFirst enet={}", enet - enet_trk_first_prev_);
    enet_trk_first_prev_ = enet;

    for (int i = 0; i < n; ++i) {
      auto &L = *m.links[i];
      link_t::prev_t c;
      c.tx_pkts = L.tx_pkts.load();
      c.tx_bytes = L.tx_bytes.load();
      c.drop_age = L.drop_age.load();
      c.drop_block = L.drop_block.load();
      c.drop_full = L.drop_full.load();
      c.drop_muted = L.drop_muted.load();
      c.drop_policy = L.drop_policy.load();
      c.drop_fault = L.drop_fault.load();
      c.tx_err = L.tx_err.load();
      c.audio_pkts = L.audio_pkts.load();
      c.audio_drop = L.audio_drop.load();
      c.rx_ping = L.rx_ping.load();
      c.rx_data = L.rx_data.load();
      c.rx_dup = L.rx_dup.load();
      c.rx_bad = L.rx_bad.load();
      c.trk_first = L.trk_first.load();
      c.rx_ctl = L.rx_ctl.load();
      c.s2c_tx = L.s2c_tx.load();
      c.s2c_drop = L.s2c_drop.load();
      c.cl_rx_pkts = L.cl_rx_pkts.load();
      c.cl_rx_used = L.cl_rx_used.load();
      const auto &p = L.prev;
      const uint16_t rtt = L.cl_rtt_ms10.load();
      const int64_t muted_ns = L.muted_until_ns.load() - now_ns;
      s += std::format(
        " | L{} {}{}{} tx={} ({:.1f} Mbps) dropAge={} dropBlock={} dropFull={} paused={} standby={} fault={} err={} sendMaxMs={:.1f} audio={}/{} mutes={}"
        " probes={} starts={} forced={} deliv={} rx ping={} trk={} dup={} bad={} trkFirst={} ctl={} s2c={}/{} client rx={} used={} rttMs={} gapMaxMs={} burstMbps={}",
        (int) L.id,
        m.alive(L, now_ns) ? "up" : "down",
        L.probing.load() ? "(probing)" : "",
        muted_ns > 0 ? std::format("(paused {:.0f}s)", muted_ns / 1e9) : "",
        c.tx_pkts - p.tx_pkts,
        (c.tx_bytes - p.tx_bytes) * 8 / secs / 1e6,
        c.drop_age - p.drop_age,
        c.drop_block - p.drop_block,
        c.drop_full - p.drop_full,
        c.drop_muted - p.drop_muted,
        c.drop_policy - p.drop_policy,
        c.drop_fault - p.drop_fault,
        c.tx_err - p.tx_err,
        L.block_max_us.exchange(0) / 1000.0,
        c.audio_pkts - p.audio_pkts,
        c.audio_drop - p.audio_drop,
        L.mutes.load(),
        L.probes.load(),
        L.promotions.load(),
        L.forced.load(),
        L.deliv_pct.load() < 0 ? std::string {"-"} : std::format("{}%", L.deliv_pct.load()),
        c.rx_ping - p.rx_ping,
        c.rx_data - p.rx_data,
        c.rx_dup - p.rx_dup,
        c.rx_bad - p.rx_bad,
        c.trk_first - p.trk_first,
        c.rx_ctl - p.rx_ctl,
        c.s2c_tx - p.s2c_tx,
        c.s2c_drop - p.s2c_drop,
        (uint32_t) (c.cl_rx_pkts - p.cl_rx_pkts),
        (uint32_t) (c.cl_rx_used - p.cl_rx_used),
        rtt == VIPLE_VR_LINK_RTT_UNKNOWN ? std::string {"-"} : std::format("{:.1f}", rtt / 10.0),
        L.cl_gap_max_ms.exchange(0),
        L.cl_burst_mbps.load()
      );
      L.prev = c;
    }
    return s;
  }
}  // namespace vr::multilink
