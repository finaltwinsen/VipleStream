/**
 * @file src/quic_server.cpp
 * @brief VipleStream MP-QUIC server implementation.
 */
#ifdef VIPLE_MPQUIC

#include "quic_server.h"
#include "config.h"
#include "logging.h"

#include <picoquic.h>
#include <picoquic_utils.h>
#include <filesystem>
#include <iomanip>
#include <string>

// OpenSSL for ECDSA cert generation
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
// Congestion algorithm symbols live in their own headers
#include <picoquic_bbr.h>
#include <picoquic_cubic.h>
#include <picoquic_newreno.h>
// picotls minicrypto for ECDSA P-256 signing bypass
// (picotls-openssl signing under MinGW/UCRT64 + OpenSSL 3.x silently
//  fails for ALL key types — the CertificateVerify never gets signed.
//  Workaround: create picoquic context with NULL cert/key, then manually
//  load cert via ptls_load_certificates and key via minicrypto.)
#include <picoquic_internal.h>
#include <picotls.h>
#include <picotls/minicrypto.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <mswsock.h>   // LPFN_WSARECVMSG, WSAID_WSARECVMSG
  #pragma comment(lib, "ws2_32.lib")
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #define SOCKET int
  #define INVALID_SOCKET (-1)
  #define closesocket(fd) (::close(fd))
#endif

#include <algorithm>
#include <cstring>
#include <unordered_map>

// §K.4: 需要存取全域 dispose 函式指標，在載入 minicrypto 金鑰後
// 清除它，避免 OpenSSL dispose 對 minicrypto 結構做錯誤 cast。
#include <picoquic_crypto_provider_api.h>

extern "C" {
#include "rswrapper.h"
}

using namespace std::chrono_literals;

// §5b FEC constants for video datagrams
static constexpr int FEC_DATA_SHARDS   = 4;
static constexpr int FEC_PARITY_SHARDS = 2;
static constexpr int FEC_TOTAL_SHARDS  = 6;
static constexpr int FEC_SHARD_MAX     = 1500;
static constexpr uint8_t FEC_PARITY_FLAG = 0x80;

// Datagram frame header (must match moonlight-common-c QuicTransport.h)
#pragma pack(push, 1)
struct QuicDgramHeader {
  uint8_t flowType;
  uint8_t reserved;
  uint16_t seq;   // big-endian
};
#pragma pack(pop)

static constexpr size_t DGRAM_HDR_SIZE = sizeof(QuicDgramHeader);

// Flow type constants (must match QuicTransport.h)
static constexpr uint8_t FLOW_VIDEO   = 0x01;
static constexpr uint8_t FLOW_AUDIO   = 0x02;
static constexpr uint8_t FLOW_CONTROL = 0x03;

// SignCertProxy 已移除 — §K.3 診斷完畢、§K.4 握手成功。

namespace quic_server {

  // ── QuicSession ──────────────────────────────────────────

  QuicSession::QuicSession(picoquic_cnx_t *cnx) : _cnx(cnx) {
    _fecRs = reed_solomon_new(FEC_DATA_SHARDS, FEC_PARITY_SHARDS);
  }

  QuicSession::~QuicSession() {
    if (_fecRs) {
      reed_solomon_release(_fecRs);
      _fecRs = nullptr;
    }
  }

  bool QuicSession::isReady() const {
    // §Q-CNX-ATOMIC-FIX (review batch 2)：sender/stats 執行緒不碰 _cnx，
    // 只讀 IO 執行緒維護的 atomic（舊版讀 _cnx + picoquic_get_cnx_state
    // 與 callback_close 的 _cnx=nullptr / picoquic free 是 TOCTOU UAF）。
    return _ready.load(std::memory_order_acquire);
  }

  // Scheduler constants (must match QuicTransport.h)
  static constexpr int SCHED_AUTO      = 0;
  static constexpr int SCHED_MIN_RTT   = 1;
  static constexpr int SCHED_AGGREGATE = 2;
  static constexpr int SCHED_REDUNDANT = 3;
  static constexpr int SCHED_ECF       = 4;

  bool QuicSession::sendDatagram(uint8_t flowType,
                                  const uint8_t *data, size_t len) {
    if (!isReady())
      return false;

    // §K.4 執行緒安全修正：video/audio 執行緒只負責組裝封包並
    // 推入 _pendingQueue，IO 執行緒在 drainPendingToQuic() 中
    // 統一呼叫 picoquic_queue_datagram_frame()。
    std::vector<uint8_t> frame(DGRAM_HDR_SIZE + len);
    QuicDgramHeader hdr{};
    hdr.flowType = flowType;
    hdr.reserved = 0;
    hdr.seq = htons(_seqCounters[flowType]++);
    std::memcpy(frame.data(), &hdr, DGRAM_HDR_SIZE);
    std::memcpy(frame.data() + DGRAM_HDR_SIZE, data, len);

    {
      std::lock_guard<std::mutex> lock(_pendingMutex);
      _pendingQueue.push_back({flowType, SCHED_AUTO, false, false, std::move(frame)});
    }
    return true;
  }

  bool QuicSession::sendDatagramScheduled(uint8_t flowType,
                                           const uint8_t *data, size_t len,
                                           int scheduler) {
    if (!isReady()) {
      static int notReadyCount = 0;
      if (notReadyCount++ < 5) {
        // §Q-CNX-ATOMIC-FIX：不得在 sender 執行緒讀 _cnx（cnxState 移除）
        BOOST_LOG(warning) << "[VIPLE-MPQUIC] §K.7 sendDatagramScheduled: NOT READY"
                           << " flow=" << (int)flowType << " len=" << len;
      }
      return false;
    }

    // §K.4 執行緒安全修正：解析 AUTO → 具體排程策略後，連同封包
    // 資料一起推入 _pendingQueue。路徑品質查詢與 path_status 設定
    // 都延遲到 IO 執行緒的 drainPendingToQuic() 中執行。
    if (scheduler == SCHED_AUTO) {
      switch (flowType) {
      case FLOW_VIDEO:   scheduler = SCHED_ECF; break;
      case FLOW_AUDIO:   scheduler = SCHED_REDUNDANT; break;
      case FLOW_CONTROL: scheduler = SCHED_MIN_RTT; break;
      default:           scheduler = SCHED_MIN_RTT; break;
      }
    }

    // §5b FEC: buffer video datagrams in groups of 4 and produce 2 parity shards
    if (flowType == FLOW_VIDEO && _fecRs) {
      auto &g = _fecGroup;
      int si = g.count;

      g.payloads[si].assign(data, data + len);
      uint16_t seq = _seqCounters[flowType]++;
      if (si == 0) g.baseSeq = seq;
      g.seqs[si] = seq;

      std::vector<uint8_t> frame(DGRAM_HDR_SIZE + len);
      QuicDgramHeader hdr{};
      hdr.flowType = flowType;
      hdr.reserved = (uint8_t)(si + 1);   // fecInfo: data shard 1-4
      hdr.seq = htons(seq);
      std::memcpy(frame.data(), &hdr, DGRAM_HDR_SIZE);
      std::memcpy(frame.data() + DGRAM_HDR_SIZE, data, len);

      {
        std::lock_guard<std::mutex> lock(_pendingMutex);
        _pendingQueue.push_back({flowType, scheduler, false, false, std::move(frame)});
      }

      if (++g.count == FEC_DATA_SHARDS) {
        flushFecParity(scheduler);
        static bool fec_once = false;
        if (!fec_once) {
          BOOST_LOG(info) << "[VIPLE-MPQUIC] §5b FEC: first group encoded OK"
                          << " baseSeq=" << g.baseSeq
                          << " data=" << FEC_DATA_SHARDS
                          << " parity=" << FEC_PARITY_SHARDS;
          fec_once = true;
        }
        g.count = 0;
      }

      _dgramPushed++;
      return true;
    }

    // Non-video or FEC unavailable: original path
    std::vector<uint8_t> frame(DGRAM_HDR_SIZE + len);
    QuicDgramHeader hdr{};
    hdr.flowType = flowType;
    hdr.reserved = 0;
    hdr.seq = htons(_seqCounters[flowType]++);
    std::memcpy(frame.data(), &hdr, DGRAM_HDR_SIZE);
    std::memcpy(frame.data() + DGRAM_HDR_SIZE, data, len);

    {
      std::lock_guard<std::mutex> lock(_pendingMutex);
      _pendingQueue.push_back({flowType, scheduler, false, false, std::move(frame)});
    }
    _dgramPushed++;
    return true;
  }

  bool QuicSession::sendStream(const uint8_t *data, size_t len) {
    if (!isReady())
      return false;

    // §K.4 執行緒安全：stream 寫入也推入 pending queue，由 IO 執行緒處理
    std::vector<uint8_t> buf(data, data + len);
    {
      std::lock_guard<std::mutex> lock(_pendingMutex);
      _pendingQueue.push_back({FLOW_CONTROL, 0, true, false, std::move(buf)});
    }
    return true;
  }

  // §Q-REINJECT v1.5.172: 判定當前是否在 reinject 視窗內。
  // 僅在「真正的 failover 切換」開啟視窗（_lastVideoPath ≥ 0 → 新 path ≥ 0），
  // 初始 path 選定（-2 → -1 → 0）不開，否則 reinject 會把 IDR 推到
  // standby path 的 picoquic queue → 堆積塞爆。
  bool QuicSession::isInReinjectionWindow() const {
    uint64_t until = _reinjectWindowUntil.load(std::memory_order_acquire);
    if (until == 0) return false;
    return picoquic_current_time() < until;
  }

  // §Q-REINJECT v1.5.172: XLINK SIGCOMM 2021 reinjection 模式。
  // 把 datagram 推到所有 non-demoted path 的 per-path queue，client
  // 端 depacketizer 用 sequence number 去重，先到的勝出。
  //
  // 用途：PATH-SWITCH 後送 IDR frame——IDR 透過 datagram 在切換縫隙
  // 容易遺失，多路同送確保至少一條 path 送達。
  bool QuicSession::sendDatagramReinject(uint8_t flowType,
                                         const uint8_t *data, size_t len) {
    if (!isReady())
      return false;

    // §Q-REINJECT-FECGAP-FIX (review batch 2)：reinject 遞增共用的
    // _seqCounters[FLOW_VIDEO] 但繞過 _fecGroup。若當下 group 已有
    // 1..3 個 data shard，burst 後續入組的 shard seq 會跳號——client
    // 以 baseSeq = seq - shardIndex 推回 group，跳號使同一 group 被拆
    // 成兩個 bucket：前半配上 parity 會以「已被 burst 占用的 seq」
    // 重建出錯位封包（靠 jitter buffer 去重吸收），後半永遠湊不滿 →
    // failover 後 2 秒（最需要 FEC 時）跨 burst 的 group 必定無法恢復。
    // 修法：先放棄未滿的 group（不發 parity；已送出的 data shard 照常
    // 交付、僅失去 FEC 保護），讓下一個 data shard 從 si=0、新 baseSeq
    // 重新開組，保證 group 內 4 個 data shard seq 連續。
    // 執行緒歸屬：sendDatagramReinject 與 sendDatagramScheduled(VIDEO)
    // 都只在 video broadcast 執行緒呼叫（stream.cpp 同一送出迴圈），
    // _fecGroup 為該執行緒所限，不需加鎖。
    if (flowType == FLOW_VIDEO) {
      _fecGroup.count = 0;
    }

    // 組好 frame（同 sendDatagram 的 header 格式）。Reinject 期間
    // 多份 datagram 共用同一個 sequence number，由 client 端去重。
    std::vector<uint8_t> frame(DGRAM_HDR_SIZE + len);
    QuicDgramHeader hdr{};
    hdr.flowType = flowType;
    hdr.reserved = 0;
    hdr.seq = htons(_seqCounters[flowType]++);
    std::memcpy(frame.data(), &hdr, DGRAM_HDR_SIZE);
    std::memcpy(frame.data() + DGRAM_HDR_SIZE, data, len);

    {
      std::lock_guard<std::mutex> lock(_pendingMutex);
      // reinject=true → drain loop 看到此 flag 會 fan-out 到所有 active path
      _pendingQueue.push_back({flowType, SCHED_AUTO, false, true, std::move(frame)});
    }
    _dgramPushed++;
    _dgramQueuedByFlow[flowType < 4 ? flowType : 0]++;
    return true;
  }

  void QuicSession::flushFecParity(int scheduler) {
    auto &g = _fecGroup;

    size_t maxLen = 0;
    for (int i = 0; i < FEC_DATA_SHARDS; i++) {
      if (g.payloads[i].size() > maxLen)
        maxLen = g.payloads[i].size();
    }
    if (maxLen == 0 || maxLen > FEC_SHARD_MAX)
      return;

    uint8_t shardBufs[FEC_TOTAL_SHARDS][FEC_SHARD_MAX];
    uint8_t *shards[FEC_TOTAL_SHARDS];
    std::memset(shardBufs, 0, sizeof(shardBufs));

    for (int i = 0; i < FEC_TOTAL_SHARDS; i++)
      shards[i] = shardBufs[i];
    for (int i = 0; i < FEC_DATA_SHARDS; i++)
      std::memcpy(shardBufs[i], g.payloads[i].data(), g.payloads[i].size());

    reed_solomon_encode(_fecRs, shards, FEC_TOTAL_SHARDS, (int)maxLen);

    // Parity payload: [len0:2 BE][len1:2 BE][len2:2 BE][len3:2 BE][parity: maxLen]
    static constexpr size_t META_SIZE = FEC_DATA_SHARDS * sizeof(uint16_t);

    for (int p = 0; p < FEC_PARITY_SHARDS; p++) {
      size_t payloadLen = META_SIZE + maxLen;
      std::vector<uint8_t> frame(DGRAM_HDR_SIZE + payloadLen);

      QuicDgramHeader hdr{};
      hdr.flowType = FLOW_VIDEO;
      hdr.reserved = FEC_PARITY_FLAG | (uint8_t)(p + 1);  // 0x81, 0x82
      hdr.seq = htons(g.baseSeq);
      std::memcpy(frame.data(), &hdr, DGRAM_HDR_SIZE);

      uint8_t *meta = frame.data() + DGRAM_HDR_SIZE;
      for (int i = 0; i < FEC_DATA_SHARDS; i++) {
        uint16_t pl = htons((uint16_t)g.payloads[i].size());
        std::memcpy(meta + i * 2, &pl, 2);
      }
      std::memcpy(meta + META_SIZE, shards[FEC_DATA_SHARDS + p], maxLen);

      {
        std::lock_guard<std::mutex> lock(_pendingMutex);
        _pendingQueue.push_back({FLOW_VIDEO, scheduler, false, false, std::move(frame)});
      }
    }
  }

  void QuicSession::drainPendingToQuic() {
    // §K.4：從 _pendingQueue 取出所有待發項目，在 IO 執行緒上
    // 統一呼叫 picoquic API（picoquic 是單執行緒設計，所有
    // API 呼叫必須在同一執行緒）。
    //
    // 先 swap 再處理——持鎖時間極短（一次 vector swap），不會
    // 阻塞 video/audio 的高頻 enqueue。
    std::vector<PendingDgram> batch;
    {
      std::lock_guard<std::mutex> lock(_pendingMutex);
      batch.swap(_pendingQueue);
    }

    if (batch.empty() || !_cnx)
      return;

    // §K.13 UAF guard 2026-05-26 — 對齊 PC 端 moonlight-qt §K.13 fix。
    // drainPendingToQuic 在 server send-side thread 跑，跟 picoquic ioLoop
    // 是 cross-thread。如果 cnx 已進 disconnecting/disconnected（client
    // 突然斷線觸發 picoquic 內部 mass path-deletion），下面直接 access
    // _cnx->path[i]->... 跟 picoquic 內部 cleanup race，可能命中 transient
    // freed picoquic_path_t。client 端 v1.5.106 dmp 已實證在
    // sender.c:826 picoquic_predict_packet_header_length 命中此 NULL deref。
    // 在 drain 入口先 bail 一次，避免進入後續路徑 mutation 區。
    if (picoquic_get_cnx_state(_cnx) >= picoquic_state_disconnecting) {
      // §Q-CNX-ATOMIC-FIX (review batch 2)：舊版 isReady 直接讀 cnx state，
      // disconnecting 當下 sender 自然停止；改 atomic 後在此補回同等
      // 語意，避免 close callback 到達前 _pendingQueue 持續堆積。
      _ready.store(false, std::memory_order_release);
      return;
    }

    // §Q-STALE 2026-05-27: 用 bytes_sent delta 估算 picoquic 已送出多少
    // datagram，更新 approxVideoQueueDepth。server 編碼 120-156fps 但
    // picoquic 每秒只送 ~60fps → 差額堆積 → client 看到舊畫面 →
    // 滑鼠延遲。這裡減去已送出量，batch loop 裡會加回新入隊量。
    {
      uint64_t currentBytesSent = 0;
      for (int i = 0; i < _cnx->nb_paths; i++) {
        if (_cnx->path[i]) currentBytesSent += _cnx->path[i]->bytes_sent;
      }
      int64_t delta = (int64_t)(currentBytesSent - _lastTotalBytesSent);
      _lastTotalBytesSent = currentBytesSent;
      // 估算已發送的 datagram 數（含 QUIC header overhead ~21 bytes）
      if (delta > 0) {
        _approxVideoQueueDepth -= delta / 1420;
      }
      if (_approxVideoQueueDepth < 0) _approxVideoQueueDepth = 0;
    }

    // bestVideoPath 在這裡預宣告（初始 -1），§K.11 diagnostic block
    // 需要引用它；實際值在下方 Pass 1/2/3 選擇後更新。
    int bestVideoPath = -1;

    // §K.11 DIAG: 監控 picoquic datagram queue 深度和 cwnd 狀態。
    // §K.11-PERF 2026-05-27: 修正效能死亡螺旋——v1.5.159 發現 queue
    // 長到 87K 時每次 drain 都遍歷 linked list 計算深度（O(87K)），
    // 加上 `queueDepth > 50` 讓 BOOST_LOG 每次 drain 都執行，
    // I/O overhead 拖慢 ioLoop → queue 繼續增長 → 正反饋螺旋。
    // 量化：87K nodes × ~100ns/cache-miss ≈ 8.7ms/次 × 180fps =
    // 1.57 秒 CPU/秒，單核飽和。修正後降到 ~0.3% CPU。
    // 修正：queue 深度遍歷 + log 全部移入 500-drain 頻率限制內。
    {
      static int diagCounter = 0;
      if (++diagCounter >= 500) {
        diagCounter = 0;
        int queueDepth = 0;
        picoquic_misc_frame_header_t* dg = _cnx->first_datagram;
        while (dg != NULL) {
          queueDepth++;
          dg = dg->next_misc_frame;
        }
        // §5d: per-path queue 深度也計入
        for (int pi = 0; pi < _cnx->nb_paths; pi++) {
          if (_cnx->path[pi] != nullptr) {
            dg = _cnx->path[pi]->first_datagram;
            while (dg != NULL) {
              queueDepth++;
              dg = dg->next_misc_frame;
            }
          }
        }
        if (queueDepth > 0) {
          _dgramPendingPeak = std::max(_dgramPendingPeak.load(), (uint64_t)queueDepth);
        }
        // §Q-STALE: 用實際遍歷值校正估計量，防止 bytes_sent 估算偏移
        _approxVideoQueueDepth = queueDepth;
        // 用 _lastVideoPath（上一次 drain cycle 的結果），因為此處
        // 新一輪的 bestVideoPath 還沒計算（Pass 1/2/3 在 §K.11 之後）。
        int diagPathIdx = (_lastVideoPath >= 0) ? _lastVideoPath : 0;
        uint64_t cwnd = 0, inFlight = 0;
        if (diagPathIdx < _cnx->nb_paths && _cnx->path[diagPathIdx] != nullptr) {
          cwnd = _cnx->path[diagPathIdx]->cwin;
          inFlight = _cnx->path[diagPathIdx]->bytes_in_transit;
        }
        uint64_t nbLosses = 0, nbRetransmit = 0, totalBytesLost = 0;
        if (diagPathIdx < _cnx->nb_paths && _cnx->path[diagPathIdx] != nullptr) {
          nbLosses = _cnx->path[diagPathIdx]->nb_losses_found;
          nbRetransmit = _cnx->path[diagPathIdx]->nb_retransmit;
          totalBytesLost = _cnx->path[diagPathIdx]->total_bytes_lost;
        }
        BOOST_LOG(info) << "[VIPLE-MPQUIC] §K.11 drain: pending=" << queueDepth
                        << " approxVQ=" << _approxVideoQueueDepth
                        << " batch=" << batch.size()
                        << " path=" << diagPathIdx
                        << " cwnd=" << cwnd
                        << " inFlight=" << inFlight
                        << " headroom=" << (int64_t)(cwnd - inFlight)
                        << " losses=" << nbLosses
                        << " retx=" << nbRetransmit
                        << " lostB=" << totalBytesLost
                        << " stale=" << _dgramDroppedStale.load()
                        << (_failoverCnxQueue ? " [FAILOVER-CNXQ]" : "");
        // §K.16: cwnd 警戒（已被 500-drain 限頻保護，額外 5 秒限頻避免連續告警）
        bool cwndAlert = (cwnd > 0 && cwnd < 200000);
        if (cwndAlert) {
          static auto lastCwndWarn = std::chrono::steady_clock::now() - std::chrono::seconds(10);
          auto now = std::chrono::steady_clock::now();
          if (now - lastCwndWarn >= std::chrono::seconds(5)) {
            lastCwndWarn = now;
            BOOST_LOG(warning) << "[VIPLE-MPQUIC] §K.16 cwnd LOW: " << cwnd
                << " (floor=" << (48 * (diagPathIdx < _cnx->nb_paths && _cnx->path[diagPathIdx]
                       ? _cnx->path[diagPathIdx]->send_mtu : 1500))
                << ") pending=" << queueDepth;
          }
        }
        // §Q-STATS-CACHE-FIX (review batch 2)：在 IO 執行緒更新 path
        // quality 快取。500 次 drain ≈ 0.5-1 秒一次，statsLoop 每 5 秒
        // 讀一次綽綽有餘；放限頻區塊內避免高頻 picoquic 走訪
        // （§K.11-PERF 教訓）。
        updateStatsCache();
      }
    }

    // §5d: 找 MIN_RTT 路徑，video datagram 路由到該 path 的 per-path queue。
    // 單路徑時 bestVideoPath=-1，fallback 到 cnx-level queue（向下相容）。
    // §5d.fix: 排除 backup/demoted 路徑 — backup path 上的 per-path queue
    //   不會被 picoquic 排程器發送，datagram 會堆積永遠送不出去。
    //
    // §Q-CWND-WARM 2026-05-27: cwnd-aware 兩階段選擇
    //   Pass 1: 在 cwin >= MIN_WARM_CWND 的「warm」path 中挑 min RTT
    //   Pass 2: 若都 cold，挑 cwin 最大的（最少受 BBR slow-start 限制）
    //
    // 動機：v1.5.143 死亡螺旋根因之一是新探到 / 剛 EMERGENCY-PROMOTE
    // 的 path cwin 從 ~15KB 起步，BBR pacing rate 也低。若立刻被選為
    // primary 灌入 30Mbps video，bytes_in_transit 撐爆 cwin → 大量丟包
    // → BBR 降速 → 死亡螺旋。MIN_WARM_CWND=64KB 大約是 4 個 IW 後的
    // 容量（slow-start 1-2 RTT 後達到，BBR 從 startup 進 ProbeBW 前的
    // 典型值）。
    constexpr uint64_t MIN_WARM_CWND = 64 * 1024;
    if (_cnx->nb_paths > 1) {
      // §Q-PATH-FRESH 2026-05-27：picoquic 的 path_is_demoted 在 client
      // 端 interface 死掉時不會立即更新（要等 server 端累積夠多 RTO 才
      // 標 demoted）。中間幾秒 cwin 仍顯示 healthy 但 nb_retransmit > 0，
      // bestVideoPath 還 stuck 在死 path 上 → 大量 video datagram 排進
      // 沒人收的 per-path queue。
      //
      // 加 `nb_retransmit == 0` 過濾：path 進 RTO 狀態（連續沒收到 ACK）
      // 時直接踢出 best 選擇，讓資料流向其他活路徑。nb_retransmit 收到
      // ACK 後會自動歸零，正常 transient loss 不會誤判。
      //
      // Pass 1: warm paths, min RTT, no active retransmit
      uint64_t minRtt = UINT64_MAX;
      for (int i = 0; i < _cnx->nb_paths; i++) {
        if (_cnx->path[i] != nullptr &&
            !_cnx->path[i]->path_is_demoted &&
            !_cnx->path[i]->path_is_backup &&
            _cnx->path[i]->nb_retransmit == 0 &&
            _cnx->path[i]->cwin >= MIN_WARM_CWND &&
            _cnx->path[i]->smoothed_rtt < minRtt) {
          minRtt = _cnx->path[i]->smoothed_rtt;
          bestVideoPath = i;
        }
      }
      // §Q-FAILOVER-CNXQ 清除點已移到選路之後（2026-07-02 review 補強）：
      // 原本「Pass 1 找到任何 warm 路徑就清除」在 §Q-STICKY-ESCAPE 切到
      // 冷路徑後會被還 warm 的舊路徑立刻清掉，冷路徑落回有排程缺陷的
      // per-path queue。改為「目前載視訊的路徑本身 warm 且健康」才回歸。
      // Pass 2: fallback — no warm path; pick largest cwin among
      // ACK-fresh paths to ride out the cold-start phase.
      if (bestVideoPath < 0) {
        uint64_t maxCwin = 0;
        for (int i = 0; i < _cnx->nb_paths; i++) {
          if (_cnx->path[i] != nullptr &&
              !_cnx->path[i]->path_is_demoted &&
              !_cnx->path[i]->path_is_backup &&
              _cnx->path[i]->nb_retransmit == 0 &&
              _cnx->path[i]->cwin > maxCwin) {
            maxCwin = _cnx->path[i]->cwin;
            bestVideoPath = i;
          }
        }
        // 降速時打一次警告（throttled），方便診斷死亡螺旋前兆
        static uint64_t lastColdWarn = 0;
        uint64_t nowTime = picoquic_current_time();
        if (bestVideoPath >= 0 && nowTime - lastColdWarn > 5000000) {
          lastColdWarn = nowTime;
          BOOST_LOG(warning) << "[VIPLE-MPQUIC] §Q-CWND-WARM: no warm path "
                             << "(all cwin < " << MIN_WARM_CWND
                             << ") — using path " << bestVideoPath
                             << " cwin=" << _cnx->path[bestVideoPath]->cwin
                             << " bytes_in_transit="
                             << _cnx->path[bestVideoPath]->bytes_in_transit;
        }
      }
      // §Q-FAILOVER-PROMOTE 2026-05-27：Pass 1/2 都找不到
      // 非 backup 且 ACK-fresh 的路徑 → 所有 active path 都在 RTO。
      // 緊急掃描 backup path，挑 RTT 最小的 promote 為 available，
      // 讓 video datagram 能繼續送出。不 promote 的話 picoquic 排程器
      // 不會對 backup path 送任何 datagram。
      if (bestVideoPath < 0) {
        uint64_t bestRtt = UINT64_MAX;
        int bestBackupIdx = -1;
        uint64_t bestBackupPathId = 0;
        for (int i = 0; i < _cnx->nb_paths; i++) {
          if (_cnx->path[i] != nullptr &&
              !_cnx->path[i]->path_is_demoted &&
              _cnx->path[i]->path_is_backup &&
              _cnx->path[i]->smoothed_rtt < bestRtt) {
            bestRtt = _cnx->path[i]->smoothed_rtt;
            bestBackupIdx = i;
            bestBackupPathId = _cnx->path[i]->unique_path_id;
          }
        }
        if (bestBackupIdx >= 0) {
          // §Q-PROMOTE-DEBOUNCE 2026-05-27 (v1.5.171 實測修正)：
          // picoquic_set_path_status 是 async（PATH_STATUS frame 傳給
          // client，client 回 ACK 後才生效）。期間 path 仍顯示 backup，
          // Pass 3 重複觸發 promote → 同一 path 在 20ms 內 promote 7 次
          // → 每次 _failoverCnxQueue = true → cnx-queue 模式切來切去
          // → datagram 路由錯誤。500ms debounce：promote 後在此期間
          // 不再嘗試對同一 path promote。
          uint64_t nowUs = picoquic_current_time();
          constexpr uint64_t PROMOTE_DEBOUNCE_US = 500000;  // 500ms
          bool inDebounce = (bestBackupPathId == _lastEmergencyPromotedId) &&
                            (nowUs - _lastEmergencyPromoteTime < PROMOTE_DEBOUNCE_US);

          if (!inDebounce) {
            picoquic_set_path_status(_cnx, bestBackupPathId,
                                     picoquic_path_status_available);
            _lastEmergencyPromotedId = bestBackupPathId;
            _lastEmergencyPromoteTime = nowUs;
            _failoverCnxQueue = true;  // §Q-FAILOVER-CNXQ: per-path queue 對剛升級路徑有排程缺陷
            BOOST_LOG(info)
                << "[VIPLE-MPQUIC] §Q-FAILOVER-PROMOTE: "
                << "no healthy non-backup path — promoting backup path "
                << bestBackupPathId << " (idx=" << bestBackupIdx
                << ", rtt=" << (bestRtt / 1000) << "ms) to available"
                << " [switching to cnx-level queue]";
          }
          // 不管 debounce 與否，bestVideoPath 都指向這個 backup，
          // 讓 video 持續往這條路徑送（即使 picoquic 內部還沒完全
          // 升級它，cnx-level queue 的排程器會盡力嘗試）
          bestVideoPath = bestBackupIdx;
        }
      }
    }

    // §Q-FAILOVER-CNXQ resume 2026-07-02（review 補強）：目前載視訊的
    // 路徑本身 warm 且健康 → 回歸 per-path queue。放在選路 Pass 之後、
    // escape override 之前：此時 _lastVideoPath 仍指向「上一輪實際載
    // 視訊」的路徑（escape/failover 本輪才會改寫它）。
    // §Q-CNXQ-WARMUP：另須過 3 秒時間下限——新 path 初始 cwin ~72KB
    // 本來就 ≥64KB，純 cwin 門檻會立即誤判 warm。
    if (_failoverCnxQueue && _lastVideoPath >= 0 &&
        picoquic_current_time() >= _cnxQueueMinUntil &&
        _lastVideoPath < _cnx->nb_paths && _cnx->path[_lastVideoPath] &&
        !_cnx->path[_lastVideoPath]->path_is_demoted &&
        !_cnx->path[_lastVideoPath]->path_is_backup &&
        _cnx->path[_lastVideoPath]->nb_retransmit == 0 &&
        _cnx->path[_lastVideoPath]->cwin >= MIN_WARM_CWND) {
      _failoverCnxQueue = false;
      BOOST_LOG(info) << "[VIPLE-MPQUIC] §Q-FAILOVER-CNXQ: current video path "
                      << _lastVideoPath << " warm — resuming per-path queue";
    }

    // §Q-STICKY-ESCAPE 2026-07-02: sticky 選路的品質逃生門——評估段。
    // 2026-07-01 事故：failover 到 VPN 後乙太網路恢復，但 sticky
    // 「不死不換」+ Pass 1 warm-cwin 門檻（閒置 path 永遠冷）讓視訊
    // 留在持續 loss 的 VPN 上 86 分鐘，ABR 被壓死在 ~11Mbps。
    // 機制：每 500ms 對每條 path 取 nb_losses_found delta（含 FEC 已
    // 修復的 loss——正是「FEC 修掉但 ABR 持續看到 missing」的路徑品質
    // 訊號），以 unique_path_id 為 key 維護 6 視窗 lossy 位元圖。
    // 當前路徑近 6 視窗 ≥4 有 loss，且存在「6 視窗零 loss + RTT ≤ 當前
    // 一半且差 ≥1ms + 已驗證非 backup」的候選 → 連續 3 輪 + 距上次
    // PATH-SWITCH ≥10s + escape 冷卻 30s → 記下候選，sticky 之後顯式
    // override。Fix C（ABR floor-stuck）訊號放寬 streak/冷卻，但不放寬
    // 候選品質與 10s 駐留。
    int escapeCandidate = -1;
    if (_cnx->nb_paths > 1) {
      uint64_t nowEsc = picoquic_current_time();
      if (nowEsc - _lastEscapeEval >= 500000) {
        _lastEscapeEval = nowEsc;
        bool reeval = _pathReevalRequested.exchange(false, std::memory_order_acq_rel);

        // 1) 更新 per-path loss 視窗（unique_path_id 為 key，防 index 漂移）
        bool seen[16] = {};
        for (int i = 0; i < _cnx->nb_paths; i++) {
          auto* p = _cnx->path[i];
          if (p == nullptr) continue;
          int k = -1, freeK = -1;
          for (int s = 0; s < 16; s++) {
            if (_escSnaps[s].used && _escSnaps[s].pathId == p->unique_path_id) {
              k = s;
              break;
            }
            if (!_escSnaps[s].used && freeK < 0) freeK = s;
          }
          if (k < 0) {
            if (freeK < 0) continue;  // 槽滿（>16 條 path 不現實）
            _escSnaps[freeK] = EscSnap{};
            _escSnaps[freeK].used = true;
            _escSnaps[freeK].pathId = p->unique_path_id;
            _escSnaps[freeK].lossSnap = p->nb_losses_found;  // 首見只建 baseline
            seen[freeK] = true;
            continue;
          }
          seen[k] = true;
          uint64_t delta = p->nb_losses_found - _escSnaps[k].lossSnap;
          _escSnaps[k].lossSnap = p->nb_losses_found;
          _escSnaps[k].lossyBits =
              (uint8_t)((_escSnaps[k].lossyBits << 1) | (delta ? 1u : 0u));
          if (_escSnaps[k].samples < 0xFF) _escSnaps[k].samples++;
        }
        for (int s = 0; s < 16; s++) {
          if (_escSnaps[s].used && !seen[s]) _escSnaps[s] = EscSnap{};  // path 已消失
        }

        // 近 6 視窗的 lossy 視窗數；-1 = 無資料或觀測不足（不可信）
        auto lossyWin = [this](uint64_t pathId) -> int {
          for (int s = 0; s < 16; s++) {
            if (_escSnaps[s].used && _escSnaps[s].pathId == pathId) {
              if (_escSnaps[s].samples < 6) return -1;
              int c = 0;
              for (int b = 0; b < 6; b++) c += (_escSnaps[s].lossyBits >> b) & 1;
              return c;
            }
          }
          return -1;
        };

        // 2) 逃生評估
        picoquic_path_t* cur =
            (_lastVideoPath >= 0 && _lastVideoPath < _cnx->nb_paths)
                ? _cnx->path[_lastVideoPath] : nullptr;
        int curLossy = cur ? lossyWin(cur->unique_path_id) : -1;
        // 一般模式：≥4/6 視窗有 loss；ABR floor-stuck 模式：≥1 即可
        bool distress = (cur != nullptr) && curLossy >= (reeval ? 1 : 4);
        if (distress) {
          int best = -1;
          uint64_t bestRtt = UINT64_MAX;
          for (int i = 0; i < _cnx->nb_paths; i++) {
            auto* p = _cnx->path[i];
            if (i == _lastVideoPath || p == nullptr) continue;
            if (p->path_is_demoted || p->path_is_backup) continue;
            if (!p->first_tuple->challenge_verified) continue;
            if (p->nb_retransmit != 0) continue;
            if (lossyWin(p->unique_path_id) != 0) continue;  // 6 視窗零 loss
            // RTT ≤ 當前一半且絕對差 ≥1ms（sub-ms 雜訊不觸發）
            if (p->smoothed_rtt * 2 > cur->smoothed_rtt) continue;
            if (cur->smoothed_rtt - p->smoothed_rtt < 1000) continue;
            // 故意不要求 warm cwin：閒置候選 cwin 永遠冷（雞生蛋——
            // 要 warm 才被選、要被選才會 warm）。切換後 LAN 亞秒級
            // slow-start + light IDR 吸收瞬間凹陷。
            if (p->smoothed_rtt < bestRtt) {
              bestRtt = p->smoothed_rtt;
              best = i;
            }
          }
          if (best >= 0) {
            uint64_t bestId = _cnx->path[best]->unique_path_id;
            if (bestId == _escLastCandidateId) {
              _escapeStreak++;
            } else {
              _escapeStreak = 1;
              _escLastCandidateId = bestId;
            }
            bool streakOk = _escapeStreak >= (reeval ? 1 : 3);
            bool dwellOk = (_lastPathSwitchTime == 0) ||
                           (nowEsc - _lastPathSwitchTime >= 10000000);
            bool coolOk = reeval || _lastEscapeSwitch == 0 ||
                          (nowEsc - _lastEscapeSwitch >= 30000000);
            if (streakOk && dwellOk && coolOk) {
              escapeCandidate = best;
              BOOST_LOG(info)
                  << "[VIPLE-MPQUIC] §Q-STICKY-ESCAPE: leaving path "
                  << _lastVideoPath << " (rtt=" << (cur->smoothed_rtt / 1000)
                  << "ms, lossy " << curLossy << "/6 win) -> path " << best
                  << " (rtt=" << (bestRtt / 1000)
                  << "ms, 0 loss/6 win) streak=" << _escapeStreak
                  << (reeval ? " [abr-distress]" : "");
            }
          } else {
            _escapeStreak = 0;
            _escLastCandidateId = 0;
            if (reeval && nowEsc - _escNoCandWarn >= 30000000) {
              _escNoCandWarn = nowEsc;
              BOOST_LOG(info)
                  << "[VIPLE-MPQUIC] §Q-STICKY-ESCAPE: re-eval requested "
                  << "(ABR floor-stuck) but no better candidate path — "
                  << "staying on path " << _lastVideoPath;
            }
          }
        } else {
          _escapeStreak = 0;
          _escLastCandidateId = 0;
        }
      }
    }

    // §Q-PATH-SWITCH-STICKY 2026-05-27: bestVideoPath 一旦選定就不換，
    // 除非當前路徑不健康。v1.5.163 實測發現：原本的 3 秒 dwell 時間
    // 只是延遲震盪，dwell 過期後同樣的 RTT 比較再次觸發切換 → IDR
    // 風暴（33K frame drops / 86 秒後 QUIC 連線崩潰）。
    //
    // 新策略「sticky unless dead」：
    //   - 當前路徑健康 → 無條件維持，不管 Pass 1/2/3 選了什麼
    //   - 當前路徑不健康 → 放行 Pass 1/2/3 的結果（failover）
    //
    // §Q-STICKY-LOCK 2026-05-27 (v1.5.171 實測修正)：原版用
    // `nb_retransmit > 0` 判斷 path 死亡太敏感，剛從 backup 升級的
    // path 帶著累積的 retransmit 計數，sticky 立刻誤判「dead」放行
    // 切換 → bestVideoPath 25ms 內 0→1→2→1→0 振盪 → IDR storm →
    // encoder 還沒產出 IDR 就被下次切換覆蓋 → client 永遠收不到 IDR。
    //
    // 改：PATH-SWITCH 後 1.5 秒內無條件 stick，不檢查 nb_retransmit。
    // 期間若 path 真的死了，picoquic 會設 path_is_demoted（仍允許
    // 切換）。1.5 秒給新 path 足夠時間 ACK 累積 + cwnd warm-up +
    // encoder 產出 IDR。
    {
      uint64_t nowUs = picoquic_current_time();
      constexpr uint64_t STICKY_LOCK_US = 1500000;  // 1.5 秒

      if (bestVideoPath != _lastVideoPath && _lastVideoPath >= 0) {
        bool inStickyLock = (_lastPathSwitchTime > 0) &&
                            (nowUs - _lastPathSwitchTime < STICKY_LOCK_US);

        bool currentPathDead = false;
        if (_lastVideoPath < _cnx->nb_paths && _cnx->path[_lastVideoPath]) {
          auto* cur = _cnx->path[_lastVideoPath];
          if (inStickyLock) {
            // 鎖定期：只看 picoquic 明確標記的死亡（demoted）
            // 和 backup（user 手動切 backup）。不看 nb_retransmit 和
            // cwin（剛升級的 path 這兩個值都不穩）。
            currentPathDead = cur->path_is_demoted || cur->path_is_backup;
          } else {
            // 鎖定期過：完整健康檢查
            currentPathDead = cur->path_is_demoted ||
                              cur->nb_retransmit > 0 ||
                              cur->cwin == 0 ||
                              cur->path_is_backup;
          }
        } else {
          currentPathDead = true;  // path 不存在
        }

        if (!currentPathDead) {
          // 當前路徑健康 → sticky，不切換
          bestVideoPath = _lastVideoPath;
        }
      }
    }

    // §Q-STICKY-ESCAPE：顯式 override。必須放在 sticky 之後——候選
    // path 的 cwin 是冷的（閒置不會 warm），Pass 1 的 warm 門檻選不到
    // 它、Pass 2 又偏好大 cwin 的當前路徑，sticky 也會把結果黏回去；
    // 唯一出路是評估通過後直接指定。
    if (escapeCandidate >= 0 && escapeCandidate != bestVideoPath) {
      bestVideoPath = escapeCandidate;
      _lastEscapeSwitch = picoquic_current_time();
      _escapeStreak = 0;
      _escLastCandidateId = 0;
      _escapeIdrLight = true;  // isRealFailover 區塊消費 → light IDR
      // 剛切上去的路徑 cwin 冷，per-path queue 對這種路徑有排程缺陷
      // （§Q-FAILOVER-CNXQ 教訓，該 path 剛被 client 從 backup 升回）
      // → 先走 cnx-level queue，Pass 1 偵測到 warm 後自動清除。
      _failoverCnxQueue = true;
    }

    // §Q-PATH-SWITCH: 記錄 bestVideoPath 切換事件
    if (bestVideoPath != _lastVideoPath) {
      uint64_t prevPathId = (_lastVideoPath >= 0 && _lastVideoPath < _cnx->nb_paths
                             && _cnx->path[_lastVideoPath])
          ? _cnx->path[_lastVideoPath]->unique_path_id : UINT64_MAX;
      // §Q-REVIEW 2026-05-29：補 < nb_paths 上界檢查（對齊上面 _lastVideoPath
      // 的防禦寫法）。bestVideoPath 實務上由 i<nb_paths 迴圈選出必在界內，
      // 但這裡缺上界與隔壁不一致；補齊避免任何路徑刪除 race 下 OOB 讀。
      bool bestValid = (bestVideoPath >= 0 && bestVideoPath < _cnx->nb_paths
                        && _cnx->path[bestVideoPath]);
      uint64_t newPathId = bestValid ? _cnx->path[bestVideoPath]->unique_path_id : UINT64_MAX;
      uint64_t newRtt    = bestValid ? _cnx->path[bestVideoPath]->smoothed_rtt : 0;
      uint64_t newCwin   = bestValid ? _cnx->path[bestVideoPath]->cwin : 0;
      BOOST_LOG(info) << "[VIPLE-MPQUIC] §Q-PATH-SWITCH: bestVideoPath "
                      << _lastVideoPath << "(id=" << prevPathId << ") -> "
                      << bestVideoPath << "(id=" << newPathId
                      << ", rtt=" << (newRtt / 1000) << "ms"
                      << ", cwin=" << newCwin << "B)";
      // §Q-PATH-SWITCH-IDR：路徑切換時自動要求 IDR，因為 client 端
      // 的 ENet 控制通道可能已死，無法自行請求 IDR。
      // §Q-IDR-INIT-FIX 2026-06-05：搬到下方 isRealFailover 區塊。
      // 原本在這裡無條件 store(true)，連線建立時的初始 path 選定
      // （-2 → -1 → 0）也會觸發 → §Q-FAILOVER-IDR-LOOP 在每次串流
      // 開頭跑滿 10 秒、每 500ms 一個 IDR → cooldown 過濾後每 1.5s
      // 一個多餘 IDR → 啟動期畫面以 1.5s 週期在模糊↔清晰間震盪
      // 6-7 遍。§Q-REINJECT-WINDOW 早就修過同一個誤觸發（v1.5.172），
      // 這顆 flag 漏掉了。

      // §Q-PATH-SWITCH-FLUSH 2026-05-27: 路徑切換時重設 approxVQ 和
      // 設定寬限期。failover 期間舊路徑死亡 → bytes_sent 不增長 →
      // approxVQ 只增不減 → 所有後續 video datagram（包含 IDR）都被
      // §Q-STALE 丟棄 → client 永遠收不到 IDR → 影像凍結。
      //
      // 寬限期（2000 次 drain ≈ 11-17 秒）確保 §Q-FAILOVER-IDR-LOOP
      // 的 10 秒 IDR 重送視窗內所有 IDR 不被 §Q-STALE 擋掉。
      // v1.5.177：從 500 提高到 2000，配合 IDR loop 10s 視窗。
      //
      // §Q-GRACE-FIX 2026-05-27（v1.5.172）：但只在「新的 failover
      // episode」才重設 grace。原版每次 PATH-SWITCH 都重設 → 振盪
      // 期間 grace 永不過期 → §Q-STALE 失效 → 死路徑 queue 堆積。
      // 改：若距離上次 episode 開始 ≥ 5 秒才算新 episode 才重設。
      uint64_t nowUsForEpisode = picoquic_current_time();
      constexpr uint64_t FAILOVER_EPISODE_GAP_US = 5000000;  // 5 秒
      bool newEpisode = (nowUsForEpisode - _failoverEpisodeStartTime) > FAILOVER_EPISODE_GAP_US;

      // §Q-REINJECT-WINDOW 2026-05-27 (v1.5.172 bug fix)：reinject 視窗
      // **只在真正的 failover 切換時開啟**（從一個有效 path 切到另一個
      // 有效 path）。初始連線時 _lastVideoPath = -2/-1，不該開 reinject，
      // 否則 IDR 會被推到 standby path 塞爆 picoquic queue。
      // §Q-RECOVERY-IDR-FIX (review batch 2)：補上「N → -1 → M」恢復
      // 轉換。整 IP 封鎖情境：所有 path 失效 → bestVideoPath=-1 →
      // _lastVideoPath 被寫成 -1；解除封鎖後恢復是 -1 → M，舊閘門判為
      // 非 failover → 不發 IDR、不開 reinject 視窗 → client decoder
      // 凍結（R.3 殭屍態）。_hadVideoPathBefore 在首次選到有效 path
      // 之前恆為 false，因此初始 -2 → -1 → 0 序列仍然不會觸發
      // （行為與 §Q-IDR-INIT-FIX 一致）。
      //
      // 駐留門檻（adversarial review 補強）：bestVideoPath==-1 不等於
      // 視訊停止——-1 是 cnx-level queue fallback，video 照常送出。
      // 瞬時抖動（lossy 鏈路上單輪 RTO spike 把唯一 path 踢出、或
      // client 關第二介面 nb_paths 縮到 1 再恢復）也會走 M→-1→M，
      // 若無門檻，每次都重啟 10s IDR loop → 不穩定鏈路上 IDR 風暴
      // 常駐。真正的 R.3 殭屍態（整 IP 封鎖）至少持續數秒，要求
      // -1 駐留 ≥1s 才視為「恢復」，亞秒級抖動不觸發。
      constexpr uint64_t RECOVERY_MIN_DOWN_US = 1000000;  // 1 秒
      bool recoveredFromTotalLoss =
          _lastVideoPath == -1 && _hadVideoPathBefore &&
          _videoPathLostTime != 0 &&
          (nowUsForEpisode - _videoPathLostTime) >= RECOVERY_MIN_DOWN_US;
      bool isRealFailover =
          (_lastVideoPath >= 0 || recoveredFromTotalLoss) &&
          bestVideoPath >= 0;

      if (newEpisode) {
        _failoverEpisodeStartTime = nowUsForEpisode;
        _approxVideoQueueDepth = 0;
        _pathSwitchGraceCycles = 2000;  // v1.5.177: 配合 §Q-FAILOVER-IDR-LOOP 10s 視窗
        _dgramDroppedStale.store(0);  // 重置計數，方便診斷新 episode baseline
        BOOST_LOG(info) << "[VIPLE-MPQUIC] §Q-GRACE-FIX: new failover episode started — resetting grace + approxVQ";
      } else if (isRealFailover) {
        BOOST_LOG(info) << "[VIPLE-MPQUIC] §Q-GRACE-FIX: same episode (PATH-SWITCH oscillation) — keeping existing grace=" << _pathSwitchGraceCycles;
      }

      if (isRealFailover) {
        // §Q-IDR-INIT-FIX：只在真正的 failover 切換才要求 IDR loop。
        // 初始 path 選定（-2 → -1 → 0）不觸發（見上方註解）。
        // §Q-STICKY-ESCAPE：escape 主動切往「已驗證、低 RTT、零 loss」
        // 的更好路徑 → light kind（2s 短視窗），不需要完整 10s 風暴。
        _pathSwitchIdrKind.store(_escapeIdrLight ? 2 : 1,
                                 std::memory_order_release);

        // §Q-CNXQ-WARMUP 2026-07-02（round 4 IDR 風暴根因之一）：任何
        // real failover 切換後先走 cnx-level queue 至少 3 秒。切換目標
        // 常是「剛從 backup 升級」的路徑——per-path queue 對這種路徑有
        // 排程缺陷（§Q-FAILOVER-CNXQ 教訓：資料堆積不被發送），且新
        // path 初始 cwin ~72KB 會騙過 64KB warm 門檻讓清除條件立即成立
        // → 資料送不出去 → client 3s stall → 判死降 backup → 切回 →
        // 再升級 → 每 ~10s 一輪 PATH-SWITCH + 完整 IDR loop 的風暴。
        // cnx-level queue 由 picoquic 內部排程器路由（無此缺陷），3 秒
        // 足夠讓目標路徑真正開始收發。
        _failoverCnxQueue = true;
        _cnxQueueMinUntil = nowUsForEpisode + 3000000;  // 3s 下限

        // 開 2 秒 reinject 視窗（XLINK 模式）
        constexpr uint64_t REINJECT_WINDOW_US = 2000000;
        _reinjectWindowUntil.store(nowUsForEpisode + REINJECT_WINDOW_US,
                                   std::memory_order_release);
        BOOST_LOG(info) << "[VIPLE-MPQUIC] §Q-REINJECT-WINDOW: opened (2s) for failover switch";

        // §Q-RECOVERY-IDR-FIX (adversarial review 補強)：同 episode 內的
        // real failover（如 5 秒內的 -1→M 恢復）不會走 newEpisode 的
        // grace 重設——若前段 grace 已耗盡且 approxVQ 堆高 + cwnd 滿，
        // 這次要送的 IDR 會被 §Q-STALE 丟掉，修復的「恢復必發 IDR」
        // 保證失效。給一個最低 grace 下限確保 IDR 通過（300 次 drain
        // ≈ 2-3 秒，足夠 IDR loop 前幾發；不像 newEpisode 的 2000 那麼
        // 寬，避免振盪期間 §Q-STALE 失效的老問題）。
        if (_pathSwitchGraceCycles < 300) {
          _pathSwitchGraceCycles = 300;
        }
      }

      // §Q-RECOVERY-IDR-FIX：必須在 isRealFailover 判定之後才設旗標，
      // 初始 -1 → 0 轉換當下旗標仍為 false，不會誤觸發啟動期 IDR。
      if (bestVideoPath >= 0) {
        _hadVideoPathBefore = true;
        _videoPathLostTime = 0;  // 恢復 → 清除 -1 進入時刻
      }
      else if (_lastVideoPath >= 0) {
        // M → -1：記錄全 path 失效的起點，供 -1 → M 恢復時計算駐留時間
        _videoPathLostTime = nowUsForEpisode;
      }

      _lastVideoPath = bestVideoPath;
      _lastPathSwitchTime = nowUsForEpisode;
      _escapeIdrLight = false;  // §Q-STICKY-ESCAPE: kind 已消費，清除
    }

    // §Q-PATH-SWITCH-FLUSH: 每次 drain 遞減寬限計數
    if (_pathSwitchGraceCycles > 0)
      _pathSwitchGraceCycles--;

    // §Q-STALE-HEADROOM-GATE v1.5.199：只在 picoquic 真的塞住（cwnd 滿）時
    // 才讓 §Q-STALE 丟 video。實測（8 Mbps/1080p30）§Q-STALE 在 cwnd 健康
    // （headroom ~250K）、pending=0 下仍誤判 _approxVideoQueueDepth 爆量而
    // 丟光 video → 死亡螺旋（丟 video → bytes_sent 只剩 audio → 估計值扣得
    // 極慢卡死）。真正的 backpressure 訊號是 cwin - bytes_in_transit <= 0。
    // O(1) 計算，不做昂貴的 datagram queue 遍歷（§K.11-PERF 教訓）。
    int64_t videoPathHeadroom = INT64_MAX; // 預設未知 → 視為有空間（不丟）
    {
      int hp = (bestVideoPath >= 0) ? bestVideoPath : 0;
      if (hp < _cnx->nb_paths && _cnx->path[hp] != nullptr) {
        videoPathHeadroom = (int64_t)_cnx->path[hp]->cwin
                          - (int64_t)_cnx->path[hp]->bytes_in_transit;
      }
    }
    bool quicBackpressured = (videoPathHeadroom <= 0);

    // §K.9 LAN MTU boost
    // §Q-PERF：提出 per-datagram 迴圈——舊位置對 batch 中每個 datagram
    // 都遍歷全部 path 重設（14 dgrams/frame × 60fps × nb_paths ≈ 每秒
    // 數千次冗餘寫入）。每次 drain 檢查一次就足夠。
    for (int i = 0; i < _cnx->nb_paths; i++) {
      if (_cnx->path[i] != nullptr && _cnx->path[i]->send_mtu < 1500) {
        _cnx->path[i]->send_mtu = 1500;
      }
    }

    for (auto &dg : batch) {
      if (dg.isStream) {
        picoquic_add_to_stream(_cnx, 0, dg.data.data(), dg.data.size(), 0);
        continue;
      }

      uint8_t ft = dg.flowType < 4 ? dg.flowType : 0;

      // §Q-STALE-SAFETY-VALVE v1.5.181：如果 video 連續 3 秒處於
      // approxVQ > MAX 且 grace=0 的狀態（表示 §Q-STALE 持續丟棄所有
      // video），重設 approxVideoQueueDepth + 恢復短暫 grace。
      // 防止 WiFi 短暫 stall → §Q-STALE 死亡螺旋 → video 永久停止。
      // 因果鏈：所有 non-backup path 暫時無法送 → bestVideoPath=-1 →
      // 沒有 PATH-SWITCH → grace 不重設 → §Q-STALE 永不解除。
      if (ft == FLOW_VIDEO && _pathSwitchGraceCycles == 0 && quicBackpressured &&
          _approxVideoQueueDepth > MAX_VIDEO_QUEUE_DEPTH) {
        uint64_t nowStale = picoquic_current_time();
        if (_staleStallStart == 0) {
          _staleStallStart = nowStale;
        } else if (nowStale - _staleStallStart > 3000000) {  // 3 秒
          _approxVideoQueueDepth = 0;
          _pathSwitchGraceCycles = 500;  // ~3 秒 grace
          _staleStallStart = 0;
          BOOST_LOG(warning) << "[VIPLE-MPQUIC] §Q-STALE-SAFETY-VALVE: "
                             << "video stalled 3s — resetting approxVQ + grace "
                             << "(dgramDroppedStale=" << _dgramDroppedStale << ")";
          // 不 continue — 讓這個 datagram 通過（grace 剛恢復）
        } else {
          // 仍在 3 秒內 → 照常丟棄
          _dgramDroppedStale++;
          continue;
        }
      } else if (ft == FLOW_VIDEO) {
        // §Q-STALE-VALVE-FIX (review)：只在「video flow 非 stale」時重置
        // 計時器。原本無條件 else 會被每個 audio datagram（~5-10ms 一個、
        // 不受 §Q-STALE 丟棄）清零 → stall 計時永遠累積不到 3 秒 → 安全閥
        // 在 audio 開啟的所有實際場景中是死的（與 R.3 殭屍態症狀相符）。
        _staleStallStart = 0;  // video 非 stale 狀態 → 重置計時器
      }

      // §Q-STALE: video datagram 佇列深度超限 → 丟棄，不入 picoquic queue。
      // picoquic 的 datagram linked list 是 FIFO，如果 server 編碼速率
      // 高於 picoquic 送出速率（180fps 編碼 vs ~60fps 送出），差額堆積
      // 導致 client 看到越來越舊的畫面。丟棄多餘 video 讓 client 始終
      // 看到最新 frame。Audio 和 control 不丟。
      //
      // §Q-PATH-SWITCH-FLUSH: PATH-SWITCH 後寬限期內不丟 video datagram。
      // 理由：PATH-SWITCH 觸發 IDR，但 IDR frame 的 datagram 和普通
      // video datagram 格式相同（都是 FLOW_VIDEO）。若 approxVQ 仍高
      // （舊路徑堆積的 stale P-frame 從 encoder pipeline 持續入隊），
      // IDR datagram 會被丟棄 → client 永遠收不到 IDR → 影像凍結。
      if (ft == FLOW_VIDEO && _pathSwitchGraceCycles == 0 && quicBackpressured &&
          _approxVideoQueueDepth > MAX_VIDEO_QUEUE_DEPTH) {
        _dgramDroppedStale++;
        continue;
      }

      int qret;

      // §Q-REINJECT v1.5.172 修正版（v1.5.176）: redundancy-on-best-path 模式。
      //
      // 原本 fan-out 到所有 path 的模式失敗：path_is_demoted 在 path 物理
      // 死亡後好幾秒才被 picoquic 設定。期間 datagram 被推到 dead path
      // queue 永遠送不出 → client jitter buffer timeout 丟整個 IDR frame。
      //
      // 改：把 IDR shard 推到 **當前 bestVideoPath** 3 份（同 shard 重複）。
      // bestVideoPath 是 Pass 1/2/3 剛剛選出的健康 path，picoquic 確認可用。
      // 3 份 redundancy 對應 IDR 可能在 path-switch 縫隙部分丟失。client
      // 用 sequence number 去重（jitter buffer 既有機制）。
      if (dg.reinject) {
        qret = -1;
        int sentCount = 0;
        int targetPath = (bestVideoPath >= 0) ? bestVideoPath : -1;
        constexpr int REINJECT_COPIES = 3;
        if (targetPath >= 0 && _cnx->path[targetPath] != nullptr) {
          for (int rep = 0; rep < REINJECT_COPIES; rep++) {
            int r = picoquic_queue_datagram_frame_on_path(
                _cnx, targetPath, dg.data.size(), dg.data.data());
            if (r == 0) {
              sentCount++;
              qret = 0;
            }
          }
        }
        // bestVideoPath 不可用 → fallback cnx-level queue（推 1 份就好）
        if (qret != 0) {
          qret = picoquic_queue_datagram_frame(_cnx, dg.data.size(), dg.data.data());
          if (qret == 0) sentCount = 1;
        }
        // §Q-REINJECT 診斷（節流避免 log flood）
        static int reinjectLogCount = 0;
        if (reinjectLogCount++ < 50) {
          BOOST_LOG(info) << "[VIPLE-MPQUIC] §Q-REINJECT: flow=" << (int)ft
                          << " len=" << dg.data.size()
                          << " copies=" << sentCount
                          << " path=" << targetPath
                          << (targetPath < 0 ? " [cnx-fallback]" : "");
        }
      } else if (ft == FLOW_VIDEO && bestVideoPath >= 0 && !_failoverCnxQueue) {
        // §5d: video → per-path queue (MIN_RTT path)；其餘 → cnx-level queue
        // §Q-FAILOVER-CNXQ: failover 期間改用 cnx-level queue，因為
        // picoquic per-path queue 對剛從 backup 升級的路徑有排程缺陷
        // （資料堆積在 path->first_datagram 但排程器不發送）。
        qret = picoquic_queue_datagram_frame_on_path(
            _cnx, bestVideoPath, dg.data.size(), dg.data.data());
      } else {
        qret = picoquic_queue_datagram_frame(_cnx, dg.data.size(), dg.data.data());
      }
      if (qret != 0) {
        _dgramFailedByFlow[ft]++;
        static int errCount = 0;
        if (errCount++ < 20) {
          size_t curMtu = (_cnx->nb_paths > 0 && _cnx->path[0])
                            ? _cnx->path[0]->send_mtu : 0;
          BOOST_LOG(error) << "[VIPLE-MPQUIC] §K.8 picoquic_queue_datagram_frame FAILED: ret="
                           << qret << " len=" << dg.data.size()
                           << " mtu=" << curMtu
                           << " headroom=" << (int64_t)(curMtu - dg.data.size() - 21 - 8)
                           << " flow=" << (int)dg.flowType
                           << " cnxState=" << (int)picoquic_get_cnx_state(_cnx);
        }
      } else {
        _dgramQueued++;
        _dgramQueuedByFlow[ft]++;
        _bytesByFlow[ft] += dg.data.size();
        // §Q-STALE: 追蹤 approximate queue depth
        if (ft == FLOW_VIDEO) _approxVideoQueueDepth++;
        // §K.8 diag: 首筆 datagram 成功送出時記錄一次
        if (_dgramQueued == 1) {
          size_t curMtu = (_cnx->nb_paths > 0 && _cnx->path[0])
                            ? _cnx->path[0]->send_mtu : 0;
          BOOST_LOG(info) << "[VIPLE-MPQUIC] §K.8 FIRST datagram queued OK!"
                          << " len=" << dg.data.size()
                          << " mtu=" << curMtu
                          << " flow=" << (int)dg.flowType;
        }
      }
    }
  }

  void QuicSession::setRecvHandler(RecvHandler handler) {
    std::lock_guard<std::mutex> lk(_recvHandlerMutex);
    _recvHandler = std::move(handler);
  }

  bool QuicSession::hasRecvHandler() const {
    std::lock_guard<std::mutex> lk(_recvHandlerMutex);
    return (bool)_recvHandler;
  }

  // §Q-RECVHANDLER-FIX：鎖內 copy handler、鎖外呼叫（避免持鎖時
  // 跑使用者 callback 造成鎖序問題，亦避免 std::function 撕裂讀）。
  void QuicSession::invokeRecvHandler(uint8_t flowType, const uint8_t *data, size_t len) {
    RecvHandler h;
    {
      std::lock_guard<std::mutex> lk(_recvHandlerMutex);
      h = _recvHandler;
    }
    if (h)
      h(flowType, data, len);
  }

  // §Q-STATS-CACHE-FIX (review batch 2)：僅限 IO 執行緒呼叫
  // （drainPendingToQuic §K.11 限頻區塊）。在 IO 執行緒上安全走訪
  // picoquic path 物件，結果入快取。
  void QuicSession::updateStatsCache() {
    if (!_cnx)
      return;

    std::vector<uint64_t> paths;
    {
      std::lock_guard<std::mutex> lock(_pathMutex);
      paths = _activePaths;
    }

    std::vector<SubflowStats> fresh;
    for (uint64_t pathId : paths) {
      picoquic_path_quality_t pq{};
      if (picoquic_get_path_quality(_cnx, pathId, &pq) != 0) {
        continue;
      }
      SubflowStats s{};
      s.pathId = (int)pathId;
      s.rttMs = (float)pq.rtt / 1000.0f;
      s.throughputMbps = (float)((double)pq.pacing_rate * 8.0 / 1e6);
      s.lossPercent = (pq.sent > 0)
          ? (float)((double)pq.lost / (double)pq.sent * 100.0)
          : 0.0f;
      s.active = true;
      s.bytesSent = pq.bytes_sent;
      s.bytesRecv = pq.bytes_received;
      fresh.push_back(s);
    }

    std::lock_guard<std::mutex> lock(_statsCacheMutex);
    _statsCache.swap(fresh);
  }

  std::vector<SubflowStats> QuicSession::getStats() const {
    // §Q-STATS-CACHE-FIX：只讀 IO 執行緒維護的快取，不碰 picoquic API
    // （舊版在 statsLoop 執行緒呼叫 picoquic_get_path_quality，與 ioLoop
    // 並行走訪可能正被刪除的 path 物件——違反 picoquic 單執行緒約束）。
    std::lock_guard<std::mutex> lock(_statsCacheMutex);
    return _statsCache;
  }

  // ── QuicListener ─────────────────────────────────────────

  QuicListener::QuicListener() = default;

  QuicListener::~QuicListener() {
    stop();
  }

  bool QuicListener::start(uint16_t port,
                           const std::string &certPath,
                           const std::string &keyPath) {
    if (_running.load())
      return false;

    // picotls-openssl under MinGW/UCRT64 + OpenSSL 3.x silently fails
    // to sign CertificateVerify with Sunshine's RSA 2048 cert — the
    // server sends ACK-only Initial packets and the handshake stalls.
    // Work around: generate a dedicated ECDSA P-256 cert/key for QUIC.
    // This is only used for the QUIC transport layer; the HTTPS/RTSP
    // pairing system still uses the original RSA cert.
    namespace fs = std::filesystem;
    auto configDir = fs::path(certPath).parent_path();
    std::string ecCert = (configDir / "quic_cert_ec.pem").string();
    std::string ecKey  = (configDir / "quic_key_ec.pem").string();

    if (!fs::exists(ecCert) || !fs::exists(ecKey)) {
      BOOST_LOG(info) << "[VIPLE-MPQUIC] Generating ECDSA P-256 cert for QUIC...";

      EVP_PKEY *pkey = EVP_EC_gen("P-256");
      if (!pkey) {
        BOOST_LOG(error) << "[VIPLE-MPQUIC] EVP_EC_gen failed";
        return false;
      }

      X509 *x = X509_new();
      X509_set_version(x, 2);
      ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
      X509_gmtime_adj(X509_get_notBefore(x), 0);
      X509_gmtime_adj(X509_get_notAfter(x), 20L * 365 * 24 * 3600);
      X509_set_pubkey(x, pkey);

      auto *name = X509_get_subject_name(x);
      X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
          (const unsigned char *)"VipleStream QUIC", -1, -1, 0);
      X509_set_issuer_name(x, name);
      X509_sign(x, pkey, EVP_sha256());

      // Write key
      FILE *fk = fopen(ecKey.c_str(), "wb");
      if (fk) { PEM_write_PrivateKey(fk, pkey, NULL, NULL, 0, NULL, NULL); fclose(fk); }
      // Write cert
      FILE *fc = fopen(ecCert.c_str(), "wb");
      if (fc) { PEM_write_X509(fc, x); fclose(fc); }

      X509_free(x);
      EVP_PKEY_free(pkey);
      BOOST_LOG(info) << "[VIPLE-MPQUIC] ECDSA cert generated: " << ecCert;
    }

    BOOST_LOG(info) << "[VIPLE-MPQUIC] Using ECDSA cert=" << ecCert
                    << " key=" << ecKey
                    << " certExists=" << fs::exists(ecCert)
                    << " keyExists=" << fs::exists(ecKey);

    // §Q Phase 5 (5f): persistent ticket encryption key so 0-RTT tickets
    // issued to clients survive a Sunshine restart. Without this picoquic
    // generates a fresh random key per process, invalidating every
    // client's cached ticket the moment the server is restarted.
    static uint8_t s_ticketKey[64];
    static size_t s_ticketKeyLen = 0;
    {
      std::string ticketKeyFile = (configDir / "quic_ticket_key.bin").string();
      if (s_ticketKeyLen == 0) {
        FILE *fk = fopen(ticketKeyFile.c_str(), "rb");
        if (fk) {
          size_t n = fread(s_ticketKey, 1, sizeof(s_ticketKey), fk);
          fclose(fk);
          if (n == sizeof(s_ticketKey)) {
            s_ticketKeyLen = n;
            BOOST_LOG(info) << "[VIPLE-MPQUIC] Ticket key loaded: " << ticketKeyFile;
          } else {
            BOOST_LOG(warning) << "[VIPLE-MPQUIC] Ticket key size mismatch ("
                               << n << " bytes); regenerating";
          }
        }
      }
      if (s_ticketKeyLen == 0) {
        if (RAND_bytes(s_ticketKey, sizeof(s_ticketKey)) == 1) {
          s_ticketKeyLen = sizeof(s_ticketKey);
          FILE *fw = fopen(ticketKeyFile.c_str(), "wb");
          if (fw) {
            fwrite(s_ticketKey, 1, sizeof(s_ticketKey), fw);
            fclose(fw);
            BOOST_LOG(info) << "[VIPLE-MPQUIC] Ticket key generated: " << ticketKeyFile;
          } else {
            BOOST_LOG(warning) << "[VIPLE-MPQUIC] Failed to write " << ticketKeyFile
                               << " — 0-RTT will not survive restart";
          }
        } else {
          BOOST_LOG(warning) << "[VIPLE-MPQUIC] RAND_bytes failed; "
                             << "falling back to picoquic-internal random key";
        }
      }
    }

    uint64_t currentTime = picoquic_current_time();

    // CRITICAL: pass NULL cert/key to picoquic_create().
    // picotls-openssl's set_private_key_from_file (the global provider)
    // silently fails ALL signing under MinGW/UCRT64 + OpenSSL 3.x — both
    // RSA and ECDSA produce ACK-only Initial packets, handshake never
    // completes.  By passing NULL, we skip the broken provider path entirely.
    // Cert + key are loaded manually below via picotls core + minicrypto.
    _quic = picoquic_create(
        128,                       // max_nb_connections
        nullptr,                   // cert — loaded manually below
        nullptr,                   // key  — loaded manually below
        nullptr,                   // cert_root
        "viplestream",             // default ALPN
        QuicListener::picoquicCallback,
        this,
        nullptr, nullptr, nullptr, // cnx_id_cb, reset_seed
        currentTime,
        nullptr,                   // simulated_time
        nullptr,                   // ticket_file_name (server doesn't load)
        s_ticketKeyLen > 0 ? s_ticketKey : nullptr,
        s_ticketKeyLen);           // ticket_encryption_key (§5f persistent)

    if (!_quic) {
      BOOST_LOG(error) << "[VIPLE-MPQUIC] Failed to create QUIC server context";
      return false;
    }

    // ── Manually load cert + key via picotls core + minicrypto ──
    // This is the heart of the MinGW/OpenSSL 3.x signing workaround.
    // picoquic_create(NULL, NULL) created the ptls_context but left
    // certificates and sign_certificate empty. We populate them here
    // using APIs that bypass the broken picotls-openssl provider:
    //
    //   ptls_load_certificates()           — picotls core (PEM→DER, no provider)
    //   ptls_minicrypto_load_private_key() — minicrypto native ECDSA P-256 signing
    //
    // OpenSSL cipher suites + key exchange remain active for AES-GCM
    // performance — only the signing path is rerouted through minicrypto.
    {
      auto *tlsCtx = reinterpret_cast<ptls_context_t *>(_quic->tls_master_ctx);
      int certRet = ptls_load_certificates(tlsCtx, ecCert.c_str());
      if (certRet != 0) {
        BOOST_LOG(error) << "[VIPLE-MPQUIC] ptls_load_certificates failed: " << certRet;
        picoquic_free(_quic);
        _quic = nullptr;
        return false;
      }
      BOOST_LOG(info) << "[VIPLE-MPQUIC] Loaded " << tlsCtx->certificates.count
                      << " cert(s) via picotls core";

      int keyRet = ptls_minicrypto_load_private_key(tlsCtx, ecKey.c_str());
      if (keyRet != 0) {
        BOOST_LOG(error) << "[VIPLE-MPQUIC] ptls_minicrypto_load_private_key failed: "
                         << keyRet << " (is the key ECDSA P-256 PEM?)";
        picoquic_free(_quic);
        _quic = nullptr;
        return false;
      }
      BOOST_LOG(info) << "[VIPLE-MPQUIC] Private key loaded via minicrypto "
                      << "(bypasses broken picotls-openssl signing)";

      // 簡要確認 sign_certificate 已就位
      if (!tlsCtx->sign_certificate || !tlsCtx->sign_certificate->cb) {
        BOOST_LOG(error) << "[VIPLE-MPQUIC] sign_certificate is NULL after "
                         << "key load — handshake will fail!";
      }

      // §K.4 修正 EVP_PKEY_free crash：
      // picoquic 的 OpenSSL provider 在初始化時註冊了全域 dispose
      // 函式 (picoquic_openssl_dispose_sign_certificate)。該函式
      // 假設 sign_certificate 是 ptls_openssl_sign_certificate_t，
      // 會從特定 offset 取出 EVP_PKEY* 呼叫 EVP_PKEY_free。
      // 但我們用 minicrypto 載入金鑰，sign_certificate 結構完全
      // 不同 → cast 後取到垃圾指標 → ACCESS_VIOLATION。
      // 修正：清除全域 dispose 指標，讓 picoquic_master_tlscontext_free
      // 只做 free(ctx->sign_certificate) 而不呼叫 OpenSSL cleanup。
      // minicrypto 的 sign_certificate 沒有需要特別 dispose 的
      // 內部資源（picoquic_clear_minicrypto 是空函式）。
      picoquic_dispose_sign_certificate_fn = nullptr;
      BOOST_LOG(info) << "[VIPLE-MPQUIC] Cleared OpenSSL dispose_sign_certificate_fn "
                      << "(minicrypto key doesn't need OpenSSL cleanup)";
    }

    // CRITICAL: picoquic_create(NULL, NULL) marks the context as client-only
    // (enforce_client_only=1) because "no cert = client".  Now that we've
    // manually loaded cert + key above, clear the flag so the server
    // accepts incoming Initial packets instead of replying SERVER_BUSY.
    picoquic_enforce_client_only(_quic, 0);

    // Enable DATAGRAM by advertising a non-zero max_datagram_frame_size
    // transport parameter (picoquic has no separate "enable datagram"
    // option — it's purely TP-driven). Multipath enabled globally.
    picoquic_set_default_tp_value(_quic,
        picoquic_tp_max_datagram_frame_size, 65535);
    picoquic_set_default_multipath_option(_quic, 1);

    // §M.2 (2026-06-16) — 設定 QUIC idle timeout。未設定時 picoquic 預設
    // 無 idle timeout，client 異常斷線後 server 端 QUIC session 可存活
    // 30-120+ 秒（等 retx 全超時），配合 §Q-SERVER-GRACE 會無限延長 ping
    // timeout → session 不會被 stop → idle watchdog 永遠 refresh → server
    // 永遠 BUSY。30 秒對正常串流無影響（持續有 video datagram 流量），
    // 只有 client 完全消失時才觸發。
    picoquic_set_default_idle_timeout(_quic, 30000);  // 30s in ms

    // §Q-MP-FIX 2026-05-24: 必須啟用 path callbacks，否則
    // path_available / path_suspended / path_deleted 不會觸發。
    picoquic_enable_path_callbacks_default(_quic, 1);

    // §K.7 修正：開啟 PMTU 探測 (basic, opportunistic) 並提高 max MTU
    // 上限至 1500，讓 picoquic 在 LAN 環境下探測到完整 Ethernet MTU。
    // 注意：required policy 在某些情況會破壞 video thread 啟動時序，
    // 改用 basic + mtu_max 讓 picoquic 在正常流量下自動探測。
    picoquic_set_default_pmtud_policy(_quic, picoquic_pmtud_basic);
    picoquic_set_mtu_max(_quic, 1500);
    BOOST_LOG(info) << "[VIPLE-MPQUIC] §K.7 PMTU policy=basic, mtu_max=1500";

    // Apply configured congestion control algorithm (BBR default)
    switch (config::stream.mpquic_congestion) {
      case 0:
        picoquic_set_default_congestion_algorithm(_quic, picoquic_newreno_algorithm);
        BOOST_LOG(info) << "[VIPLE-MPQUIC] Congestion: NewReno";
        break;
      case 2:
        picoquic_set_default_congestion_algorithm(_quic, picoquic_cubic_algorithm);
        BOOST_LOG(info) << "[VIPLE-MPQUIC] Congestion: Cubic";
        break;
      default:
        picoquic_set_default_congestion_algorithm(_quic, picoquic_bbr_algorithm);
        BOOST_LOG(info) << "[VIPLE-MPQUIC] Congestion: BBR";
        break;
    }

    _port = port;
    _running.store(true);
    _ioThread = std::thread(&QuicListener::ioLoop, this);

    // Start periodic stats logging (every 30s)
    _statsRunning.store(true);
    _statsThread = std::thread(&QuicListener::statsLoop, this);

    BOOST_LOG(info) << "[VIPLE-MPQUIC] Server listening on port " << port;
    return true;
  }

  void QuicListener::stop() {
    if (!_running.load())
      return;

    _running.store(false);
    _statsRunning.store(false);

    if (_ioThread.joinable())
      _ioThread.join();
    if (_statsThread.joinable())
      _statsThread.join();

    {
      std::lock_guard<std::mutex> lock(_sessionMutex);
      // §Q-CNX-ATOMIC-FIX (review batch 2)：listener 停止時明確標記所有
      // session 不可用，外部仍持有 shared_ptr 的執行緒（stream.cpp 的
      // quicVideoSession 等）不再往 _pendingQueue 入隊。ioThread 已
      // join，此處等效於 IO 執行緒，可安全清 _cnx（picoquic_free 在
      // 後面才執行）。
      for (auto &[addr, session] : _sessions) {
        if (session) {
          session->_ready.store(false, std::memory_order_release);
          session->_cnx = nullptr;
        }
      }
      _sessions.clear();
    }

    if (_quic) {
      picoquic_free(_quic);
      _quic = nullptr;
    }

    BOOST_LOG(info) << "[VIPLE-MPQUIC] Server stopped";
  }

  // §K.5 helper: 正規化地址字串 — 去掉 IPv6-mapped-IPv4 前綴
  // 讓 "::ffff:192.168.51.5" 和 "192.168.51.5" 可以比對
  static std::string normalizeAddrStr(const std::string &addr) {
    static const std::string v4mapped = "::ffff:";
    if (addr.size() > v4mapped.size() &&
        addr.compare(0, v4mapped.size(), v4mapped) == 0) {
      return addr.substr(v4mapped.size());
    }
    return addr;
  }

  std::shared_ptr<QuicSession> QuicListener::getSession(
      const boost::asio::ip::address &clientAddr) {
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto key = normalizeAddrStr(clientAddr.to_string());
    auto it = _sessions.find(key);
    if (it != _sessions.end())
      return it->second;
    return nullptr;
  }

  void QuicListener::setSessionCallback(SessionCallback cb) {
    _sessionCallback = std::move(cb);
  }

  void QuicListener::ioLoop() {
    uint8_t recvBuf[2048];
    uint8_t sendBuf[2048];

    // Bind dual-stack UDP socket
    struct sockaddr_in6 listenAddr{};
    listenAddr.sin6_family = AF_INET6;
    listenAddr.sin6_port = htons(_port);
    listenAddr.sin6_addr = in6addr_any;

    SOCKET sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
      BOOST_LOG(error) << "[VIPLE-MPQUIC] Failed to create server socket";
      return;
    }

    int v6only = 0;
    setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY,
               (const char *)&v6only, sizeof(v6only));

    // §Q-MP-PKTINFO: 啟用 PKTINFO 讓 WSARecvMsg 回報封包到達的
    // 實際本地地址。沒有這個，addr_to 永遠是 in6addr_any，
    // picoquic 無法區分不同本地介面的 PATH_CHALLENGE，multipath
    // 的 path validation 會完全失敗。
    {
      int yes = 1;
      setsockopt(sock, IPPROTO_IPV6, IPV6_PKTINFO,
                 (const char *)&yes, sizeof(yes));
      setsockopt(sock, IPPROTO_IP, IP_PKTINFO,
                 (const char *)&yes, sizeof(yes));
    }

    if (bind(sock, (struct sockaddr *)&listenAddr,
             sizeof(listenAddr)) != 0) {
      BOOST_LOG(error) << "[VIPLE-MPQUIC] Failed to bind server socket on port "
                       << _port;
      closesocket(sock);
      return;
    }

#ifdef _WIN32
    // §Q-MP-PKTINFO: 動態載入 WSARecvMsg（Windows 要求透過
    // WSAIoctl 取得函式指標，無法直接呼叫）。
    LPFN_WSARECVMSG WSARecvMsgFn = nullptr;
    {
      GUID wsaRecvMsgGuid = WSAID_WSARECVMSG;
      DWORD dwBytesReturned = 0;
      WSAIoctl(sock, SIO_GET_EXTENSION_FUNCTION_POINTER,
               &wsaRecvMsgGuid, sizeof(wsaRecvMsgGuid),
               &WSARecvMsgFn, sizeof(WSARecvMsgFn),
               &dwBytesReturned, nullptr, nullptr);
      if (!WSARecvMsgFn) {
        BOOST_LOG(warning) << "[VIPLE-MPQUIC] WSARecvMsg not available; "
                           << "multipath PATH_CHALLENGE will use fallback addr_to";
      }
    }
#endif

    uint64_t totalRecv = 0, totalSent = 0;
    uint64_t loopIter = 0;
    auto loopStart = std::chrono::steady_clock::now();
    BOOST_LOG(info) << "[VIPLE-MPQUIC] ioLoop started, _running=" << _running.load();

    while (_running.load()) {
      loopIter++;
      struct sockaddr_storage peerAddr{};
      socklen_t peerLen = sizeof(peerAddr);
      uint64_t currentTime = picoquic_current_time();

      // Poll for incoming packets (1ms timeout)
      fd_set readSet;
      struct timeval tv{};
      tv.tv_usec = 1000;
      FD_ZERO(&readSet);
      FD_SET(sock, &readSet);

      int selRet = select((int)(sock + 1), &readSet, nullptr, nullptr, &tv);
      if (selRet > 0) {
        // §Q-MP-PKTINFO: 用 WSARecvMsg 取得封包到達的實際本地
        // 地址，讓 picoquic 正確辨識 multipath PATH_CHALLENGE。
        int recvLen = 0;
        struct sockaddr_storage localDest{};
        int recvIfIndex = 0;

#ifdef _WIN32
        if (WSARecvMsgFn) {
          WSABUF dataBuf;
          dataBuf.buf = (char *)recvBuf;
          dataBuf.len = sizeof(recvBuf);

          char controlBuf[256];
          WSAMSG wsaMsg{};
          wsaMsg.name = (LPSOCKADDR)&peerAddr;
          wsaMsg.namelen = (INT)sizeof(peerAddr);
          wsaMsg.lpBuffers = &dataBuf;
          wsaMsg.dwBufferCount = 1;
          wsaMsg.Control.buf = controlBuf;
          wsaMsg.Control.len = sizeof(controlBuf);
          wsaMsg.dwFlags = 0;

          DWORD bytesRecv = 0;
          int wsaRet = WSARecvMsgFn(sock, &wsaMsg, &bytesRecv,
                                     nullptr, nullptr);
          if (wsaRet == 0 && bytesRecv > 0) {
            recvLen = (int)bytesRecv;
            peerLen = wsaMsg.namelen;

            // 從 control message 提取到達地址
            for (WSACMSGHDR *cmsg = WSA_CMSG_FIRSTHDR(&wsaMsg);
                 cmsg != nullptr;
                 cmsg = WSA_CMSG_NXTHDR(&wsaMsg, cmsg)) {
              if (cmsg->cmsg_level == IPPROTO_IPV6 &&
                  cmsg->cmsg_type == IPV6_PKTINFO) {
                auto *info = (struct in6_pktinfo *)WSA_CMSG_DATA(cmsg);
                auto *sa6 = (struct sockaddr_in6 *)&localDest;
                sa6->sin6_family = AF_INET6;
                sa6->sin6_addr = info->ipi6_addr;
                sa6->sin6_port = htons(_port);
                recvIfIndex = (int)info->ipi6_ifindex;
                break;
              }
              if (cmsg->cmsg_level == IPPROTO_IP &&
                  cmsg->cmsg_type == IP_PKTINFO) {
                auto *info = (struct in_pktinfo *)WSA_CMSG_DATA(cmsg);
                // 收到 IPv4 封包 → 轉成 IPv4-mapped IPv6 以匹配
                // 我們的 AF_INET6 dual-stack 語意
                auto *sa6 = (struct sockaddr_in6 *)&localDest;
                memset(sa6, 0, sizeof(*sa6));
                sa6->sin6_family = AF_INET6;
                sa6->sin6_port = htons(_port);
                // ::ffff:a.b.c.d
                sa6->sin6_addr.s6_addr[10] = 0xFF;
                sa6->sin6_addr.s6_addr[11] = 0xFF;
                memcpy(&sa6->sin6_addr.s6_addr[12],
                       &info->ipi_addr, 4);
                recvIfIndex = (int)info->ipi_ifindex;
                break;
              }
            }
          }
        } else
#endif
        {
          // fallback: WSARecvMsg 不可用時用 recvfrom（addr_to
          // 退回 listenAddr，multipath 會失敗但單路正常）
          recvLen = recvfrom(sock, (char *)recvBuf, sizeof(recvBuf), 0,
                             (struct sockaddr *)&peerAddr, &peerLen);
          memcpy(&localDest, &listenAddr, sizeof(listenAddr));
        }

        if (recvLen > 0) {
          totalRecv++;

          int pktRet = picoquic_incoming_packet(
              _quic, recvBuf, (size_t)recvLen,
              (struct sockaddr *)&peerAddr,
              (struct sockaddr *)&localDest,
              recvIfIndex, 0, currentTime);
          if (pktRet != 0) {
            BOOST_LOG(error) << "[VIPLE-MPQUIC] picoquic_incoming_packet failed: "
                             << pktRet;
          }
          // 簡短 log 前幾封封包（不存取 picoquic 內部欄位）
          if (totalRecv <= 5) {
            BOOST_LOG(info) << "[VIPLE-MPQUIC] Recv #" << totalRecv
                            << " len=" << recvLen;
          }
        }
      }

      // §K.4：在 prepare_next_packet 之前，把各 session 的
      // pending datagram/stream 資料 drain 進 picoquic 的內部佇列。
      // 這確保所有 picoquic API 呼叫都在 IO 執行緒上。
      {
        std::lock_guard<std::mutex> lock(_sessionMutex);
        for (auto &[addr, session] : _sessions) {
          if (session && session->isReady()) {
            session->drainPendingToQuic();
          }
        }
      }

      // Process outgoing packets.
      // picoquic_prepare_next_packet takes the QUIC context (not a cnx)
      // and walks the wake queue internally, returning the cnx it sent
      // for via p_last_cnx.
      int pktsSentThisCycle = 0;
      while (true) {
        struct sockaddr_storage destAddr{};
        struct sockaddr_storage fromAddr{};
        int ifIndex = 0;
        size_t sendLen = 0;
        picoquic_connection_id_t logCid{};
        picoquic_cnx_t *lastCnx = nullptr;

        int ret = picoquic_prepare_next_packet(
            _quic, picoquic_current_time(),
            sendBuf, sizeof(sendBuf), &sendLen,
            &destAddr, &fromAddr,
            &ifIndex, &logCid, &lastCnx);

        if (ret != 0) {
          BOOST_LOG(error) << "[VIPLE-MPQUIC] picoquic_prepare_next_packet failed: ret=" << ret;
          break;
        }
        if (sendLen == 0)
          break;

        // The listen socket is AF_INET6 (dual-stack).  If picoquic hands
        // back an AF_INET dest (it can, depending on how it stored the
        // peer addr), we must wrap it in an IPv4-mapped IPv6 address so
        // sendto() on the v6 socket succeeds.
        struct sockaddr_storage actualDest{};
        socklen_t destLen;
        if (destAddr.ss_family == AF_INET) {
          auto *v4 = (struct sockaddr_in *)&destAddr;
          auto *v6 = (struct sockaddr_in6 *)&actualDest;
          memset(v6, 0, sizeof(*v6));
          v6->sin6_family = AF_INET6;
          v6->sin6_port = v4->sin_port;
          // Build ::ffff:x.x.x.x mapped address
          memset(&v6->sin6_addr.s6_addr[10], 0xff, 2);
          memcpy(&v6->sin6_addr.s6_addr[12], &v4->sin_addr, 4);
          destLen = sizeof(sockaddr_in6);
        } else {
          memcpy(&actualDest, &destAddr, sizeof(destAddr));
          destLen = sizeof(sockaddr_in6);
        }
        int sendResult = sendto(sock, (const char *)sendBuf, (int)sendLen, 0,
               (struct sockaddr *)&actualDest, destLen);
        if (sendResult > 0) {
          totalSent++;
          pktsSentThisCycle++;
          if (totalSent <= 3) {
            BOOST_LOG(info) << "[VIPLE-MPQUIC] Sent pkt #" << totalSent
                            << " len=" << sendLen;
          }
        } else {
          int err = WSAGetLastError();
          BOOST_LOG(error) << "[VIPLE-MPQUIC] sendto failed pkt #"
                           << (totalSent + 1) << " len=" << sendLen
                           << " err=" << err
                           << " destFamily=" << destAddr.ss_family;
        }
      }
    }

    // §K.7 diag: 在 IO loop 結束時印出各 session 的 datagram 統計
    {
      std::lock_guard<std::mutex> lock(_sessionMutex);
      for (auto &[addr, session] : _sessions) {
        if (session) {
          BOOST_LOG(info) << "[VIPLE-MPQUIC] §K.7 session=" << addr
                          << " dgramPushed=" << session->_dgramPushed.load()
                          << " dgramQueued=" << session->_dgramQueued.load()
                          << " pendingPeak=" << session->_dgramPendingPeak.load()
                          << " ready=" << session->isReady();
        }
      }
    }

    auto loopEnd = std::chrono::steady_clock::now();
    auto loopMs = std::chrono::duration_cast<std::chrono::milliseconds>(loopEnd - loopStart).count();
    BOOST_LOG(info) << "[VIPLE-MPQUIC] ioLoop exit: recv=" << totalRecv
                    << " sent=" << totalSent
                    << " iterations=" << loopIter
                    << " durationMs=" << loopMs
                    << " _running=" << _running.load();

    closesocket(sock);
  }

  int QuicListener::picoquicCallback(picoquic_cnx_t *cnx,
      uint64_t stream_id, uint8_t *bytes, size_t length,
      picoquic_call_back_event_t fin_or_event,
      void *callback_ctx, void *stream_ctx) {
    auto *listener = static_cast<QuicListener *>(callback_ctx);
    (void)stream_ctx;

    auto event = fin_or_event;

    // Helper: look up the session bound to this cnx.
    auto findSession = [&](picoquic_cnx_t *c) -> std::shared_ptr<QuicSession> {
      std::lock_guard<std::mutex> lock(listener->_sessionMutex);
      for (auto &[key, sess] : listener->_sessions) {
        if (sess->_cnx == c) return sess;
      }
      return nullptr;
    };

    switch (event) {
    case picoquic_callback_ready: {
      BOOST_LOG(info) << "[VIPLE-MPQUIC] Client connection ready";

      // §K.8 修正：handshake 完成後立即提升 path send_mtu 到 1500。
      // 根因：picoquic_queue_datagram_frame() 在 frames.c:5329 檢查
      //   length + 21 + cnxid_length > send_mtu
      // 初始 MTU = 1252 (IPv4)，但 video datagram ~1428 bytes →
      //   1428 + 21 + 8 = 1457 > 1252 → DATAGRAM_TOO_LONG (error 1083)
      // PMTUD basic 模式需要時間探測到 1500，但 video 在 handshake
      // 後立即開始發送。LAN Ethernet MTU = 1500，安全直設。
      if (cnx->nb_paths > 0 && cnx->path[0] != nullptr) {
        size_t oldMtu = cnx->path[0]->send_mtu;
        cnx->path[0]->send_mtu = 1500;
        BOOST_LOG(info) << "[VIPLE-MPQUIC] §K.8 path[0] send_mtu boosted: "
                        << oldMtu << " → 1500";

        // VipleStream §K.14: 重設 BBR cwnd 和 lower bounds。
        // 握手期間 CRYPTO frame 遺失（LAN 上常見 RACK 假陽性）
        // 會使 BBR 的 cwnd 從初始 15360 崩塌到 ~5891。
        // 第一個 video IDR frame 需要 ~50KB (36 datagrams)，
        // cwnd 不足會造成大部分 datagram 無法送出。
        // 在此重設讓 BBR slow-start 從乾淨狀態重新爬升。
        uint64_t oldCwnd = cnx->path[0]->cwin;
        if (cnx->congestion_alg == picoquic_bbr_algorithm) {
          picoquic_bbr_reset_after_handshake(cnx);
          BOOST_LOG(info) << "[VIPLE-MPQUIC] §K.14 BBR reset after handshake: cwnd "
                          << oldCwnd << " → " << cnx->path[0]->cwin
                          << " (inflight_lo/bw_lo → UINT64_MAX)";
        }
      }

      auto session = std::make_shared<QuicSession>(cnx);
      // §Q-CNX-ATOMIC-FIX (review batch 2)：IO 執行緒標記 ready；sender
      // 執行緒（video/audio/stats）由此判定，不再 dereference _cnx。
      session->_ready.store(true, std::memory_order_release);
      picoquic_set_callback(cnx, QuicListener::picoquicCallback, listener);

      // §K.6: 我們使用 push 模型 (picoquic_queue_datagram_frame)
      // 不需要 pull 模型 (prepare_datagram callback)。
      // 移除 picoquic_mark_datagram_ready(cnx, 1) — 之前的設定
      // 導致 picoquic 每次 prepare_next_packet 都觸發
      // prepare_datagram callback (event=16)，每秒數百次，
      // 全部落入 default: 分支寫 info log，拖累 IO 迴圈效能。

      // Store session keyed by peer address. picoquic_get_peer_addr writes
      // a sockaddr* through the out-pointer — it does NOT memcpy into a
      // storage buffer.
      struct sockaddr *peerAddrPtr = nullptr;
      picoquic_get_peer_addr(cnx, &peerAddrPtr);

      if (peerAddrPtr) {
        std::lock_guard<std::mutex> lock(listener->_sessionMutex);
        char addrStr[INET6_ADDRSTRLEN] = {};
        if (peerAddrPtr->sa_family == AF_INET) {
          inet_ntop(AF_INET,
                    &reinterpret_cast<struct sockaddr_in *>(peerAddrPtr)->sin_addr,
                    addrStr, sizeof(addrStr));
        } else if (peerAddrPtr->sa_family == AF_INET6) {
          inet_ntop(AF_INET6,
                    &reinterpret_cast<struct sockaddr_in6 *>(peerAddrPtr)->sin6_addr,
                    addrStr, sizeof(addrStr));
        }
        auto normalizedKey = normalizeAddrStr(addrStr);
        BOOST_LOG(info) << "[VIPLE-MPQUIC] Session stored: key=\""
                        << normalizedKey << "\" (raw=\"" << addrStr << "\")";
        // §Q-CNX-ATOMIC-FIX (review batch 2)：同 IP 重連會覆蓋 map
        // entry，被擠出的舊 session 之後在 callback_close 以 _cnx==cnx
        // 走訪 map 找不到自己（已不在 map）→ _ready 殘留 true、_cnx
        // 懸空。在覆蓋前明確標記舊 session 不可用（IO 執行緒上安全）。
        auto oldIt = listener->_sessions.find(normalizedKey);
        if (oldIt != listener->_sessions.end() && oldIt->second &&
            oldIt->second != session) {
          oldIt->second->_ready.store(false, std::memory_order_release);
          oldIt->second->_cnx = nullptr;
        }
        listener->_sessions[normalizedKey] = session;
      }

      if (listener->_sessionCallback)
        listener->_sessionCallback(session);
      break;
    }

    case picoquic_callback_path_available: {
      // §MP-PRIMARY 2026-05-24: 新路徑驗證通過後追蹤但維持 backup。
      // 只有 handshake 路徑（path 0）是 available。non-primary 路徑
      // 不參與 datagram 排程，僅供 failover 使用。如果不設 backup，
      // picoquic 預設把驗證通過的路徑視為 available，scheduler 會
      // 把 video datagram 分散到多條 RTT 不同的路徑，depacketizer
      // 因時序差異而組裝失敗 → 畫面靜止。
      auto session = findSession(cnx);
      if (session) {
        std::lock_guard<std::mutex> lock(session->_pathMutex);
        if (std::find(session->_activePaths.begin(),
                      session->_activePaths.end(), stream_id)
            == session->_activePaths.end()) {
          session->_activePaths.push_back(stream_id);
        }
      }
      picoquic_set_path_status(cnx, stream_id, picoquic_path_status_backup);
      BOOST_LOG(info) << "[VIPLE-MPQUIC] Path available: id=" << stream_id
                      << " (set to backup, failover-ready)";

      // §Q-SERVER-PATH-DIAG v1.5.196 Fix O：path event 快照
      if (cnx) {
        for (int pi = 0; pi < cnx->nb_paths; pi++) {
          if (cnx->path[pi] != nullptr) {
            BOOST_LOG(info) << "[VIPLE-MPQUIC] §Q-SERVER-PATH-DIAG: "
                            << "path[" << pi << "] id="
                            << cnx->path[pi]->unique_path_id
                            << " backup=" << cnx->path[pi]->path_is_backup
                            << " demoted=" << cnx->path[pi]->path_is_demoted
                            << " rtt=" << (cnx->path[pi]->smoothed_rtt / 1000)
                            << "ms";
          }
        }
      }
      break;
    }

    case picoquic_callback_path_suspended:
    case picoquic_callback_path_deleted: {
      auto session = findSession(cnx);
      if (session) {
        std::lock_guard<std::mutex> lock(session->_pathMutex);
        auto &v = session->_activePaths;
        v.erase(std::remove(v.begin(), v.end(), stream_id), v.end());
      }
      BOOST_LOG(info) << "[VIPLE-MPQUIC] Path "
                      << (event == picoquic_callback_path_deleted ? "deleted" : "suspended")
                      << ": id=" << stream_id;

      // §Q-SERVER-PATH-DIAG v1.5.196 Fix O：path event 時印出 server
      // 端所有存活路徑的狀態快照，幫助診斷 failover 期間 server 的
      // 排程行為（例如：server 是否把 WiFi 設為 available？）
      if (cnx) {
        for (int pi = 0; pi < cnx->nb_paths; pi++) {
          if (cnx->path[pi] != nullptr) {
            BOOST_LOG(info) << "[VIPLE-MPQUIC] §Q-SERVER-PATH-DIAG: "
                            << "path[" << pi << "] id="
                            << cnx->path[pi]->unique_path_id
                            << " backup=" << cnx->path[pi]->path_is_backup
                            << " demoted=" << cnx->path[pi]->path_is_demoted
                            << " rtt=" << (cnx->path[pi]->smoothed_rtt / 1000)
                            << "ms";
          }
        }
      }

      // §Q-MP-EMERGENCY-PROMOTE 2026-05-27（server 對稱版）：
      // server 預設把所有新探到的路徑設成 backup（§MP-PRIMARY 規則，
      // 避免 video datagram 跨 path 排程造成 depacketizer 組裝失敗）。
      // 當原 primary（通常是 path 0 / Ethernet）被 picoquic 刪除後，
      // bestVideoPath 掃描會跳過所有 backup path → 回傳 -1 → video
      // 退到 cnx-level queue。client 端雖然透過 PATH_AVAILABLE 框架
      // 通報它認為某路徑現在 available，但 server 本地的 path_is_backup
      // 旗標不會自動翻轉，picoquic 內建排程器照樣不選那條路。
      //
      // 修正：path 被刪後掃描剩餘路徑，若所有 surviving path 都 backup，
      // 挑 smoothed_rtt 最小的那條 promote 成 available。維持單一 primary
      // 的設計，但補上失效時的 failover。
      if (cnx && cnx->nb_paths > 0) {
        bool anyNonBackup = false;
        for (int i = 0; i < cnx->nb_paths; i++) {
          if (cnx->path[i] != nullptr &&
              !cnx->path[i]->path_is_demoted &&
              !cnx->path[i]->path_is_backup) {
            anyNonBackup = true;
            break;
          }
        }
        if (!anyNonBackup) {
          int bestIdx = -1;
          uint64_t bestRtt = UINT64_MAX;
          uint64_t bestPathId = 0;
          for (int i = 0; i < cnx->nb_paths; i++) {
            if (cnx->path[i] != nullptr &&
                !cnx->path[i]->path_is_demoted &&
                cnx->path[i]->path_is_backup &&
                cnx->path[i]->smoothed_rtt < bestRtt) {
              bestRtt = cnx->path[i]->smoothed_rtt;
              bestIdx = i;
              bestPathId = cnx->path[i]->unique_path_id;
            }
          }
          if (bestIdx >= 0) {
            picoquic_set_path_status(cnx, bestPathId,
                                     picoquic_path_status_available);
            // §Q-FAILOVER-CNXQ: 同 Pass 3，升級路徑需用 cnx-level queue
            if (session) {
              session->_failoverCnxQueue = true;
            }
            BOOST_LOG(info) << "[VIPLE-MPQUIC] §Q-MP-EMERGENCY-PROMOTE: "
                            << "primary path " << stream_id
                            << " killed, no other non-backup paths — "
                            << "promoting backup path " << bestPathId
                            << " (idx=" << bestIdx
                            << ", rtt=" << (bestRtt / 1000) << "ms) "
                            << "to available";
          }
        }
      }
      break;
    }

    case picoquic_callback_datagram: {
      if (length < DGRAM_HDR_SIZE)
        break;
      auto *hdr = reinterpret_cast<const QuicDgramHeader *>(bytes);
      auto session = findSession(cnx);
      if (session) {
        // §Q-INPUT-DIAG v1.5.195 Fix N: server 端 QUIC input 接收計數
        if (hdr->flowType == 0x04) {
          static std::atomic<int> inputReceived{0};
          static auto lastDiag = std::chrono::steady_clock::now();
          int count = ++inputReceived;
          auto now = std::chrono::steady_clock::now();
          if (now - lastDiag > std::chrono::seconds(5) && count > 0) {
            BOOST_LOG(info) << "[VIPLE-MPQUIC] §Q-INPUT-DIAG: server "
                            << "received " << count
                            << " QUIC input datagrams (last 5s)";
            inputReceived = 0;
            lastDiag = now;
          }
        }
        // §Q-RECVHANDLER-FIX：經 invokeRecvHandler 鎖內 copy + 鎖外呼叫
        session->invokeRecvHandler(
            hdr->flowType,
            bytes + DGRAM_HDR_SIZE,
            length - DGRAM_HDR_SIZE);
      }
      break;
    }

    case picoquic_callback_stream_data:
    case picoquic_callback_stream_fin: {
      if (stream_id != 0)
        break;
      auto session = findSession(cnx);
      if (session) {
        session->invokeRecvHandler(FLOW_CONTROL, bytes, length); // §Q-RECVHANDLER-FIX
      }
      break;
    }

    case picoquic_callback_close:
    case picoquic_callback_application_close: {
      // Log detailed close reason for debugging TLS / handshake failures
      auto state = picoquic_get_cnx_state(cnx);
      uint64_t localErr = picoquic_get_local_error(cnx);
      uint64_t remoteErr = picoquic_get_remote_error(cnx);
      BOOST_LOG(info) << "[VIPLE-MPQUIC] Client connection closed"
                      << " state=" << (int)state
                      << " localErr=0x" << std::hex << localErr
                      << " remoteErr=0x" << remoteErr << std::dec;

      std::lock_guard<std::mutex> lock(listener->_sessionMutex);
      for (auto it = listener->_sessions.begin();
           it != listener->_sessions.end(); ++it) {
        if (it->second->_cnx == cnx) {
          // §Q-CNX-ATOMIC-FIX：先清 ready 再清 _cnx——sender 執行緒只讀
          // _ready，store 之後不會再進入任何 send 路徑。
          it->second->_ready.store(false, std::memory_order_release);
          it->second->_cnx = nullptr;
          listener->_sessions.erase(it);
          break;
        }
      }
      break;
    }

    case picoquic_callback_almost_ready:
      BOOST_LOG(info) << "[VIPLE-MPQUIC] Callback: almost_ready (TLS handshake complete)";
      break;

    // §K.6: 正確處理 datagram 相關 callback，不落入 default log。
    // 我們使用 push 模型 (queue_datagram_frame)，pull 模型的
    // prepare_datagram 只需回傳 0（無資料）。
    case picoquic_callback_prepare_datagram:
      // picoquic polling 要不要送 datagram — 我們用 push 模型，
      // 不提供資料。回傳 0 讓 picoquic 停止 polling。
      // 注意：picoquic 收到 length=0 會自動清除 datagram_ready flag。
      break;

    case picoquic_callback_datagram_acked:
    case picoquic_callback_datagram_lost:
    case picoquic_callback_datagram_spurious:
      // Datagram 送達/遺失/誤判通知 — 目前不需處理（datagram
      // 本身是 fire-and-forget，retransmit 由上層 FEC 處理）。
      break;

    default: {
      // §K.6: 降級為 debug，避免未知 event 灌爆 log。
      // 只在首次出現時印一次 info 提醒有未處理的 event。
      static std::unordered_map<int, bool> logged_events;
      auto evt_id = (int)event;
      if (logged_events.find(evt_id) == logged_events.end()) {
        logged_events[evt_id] = true;
        BOOST_LOG(info) << "[VIPLE-MPQUIC] Unhandled callback event="
                        << evt_id << " stream=" << stream_id
                        << " len=" << length
                        << " (further occurrences suppressed)";
      }
      break;
    }
    }

    return 0;
  }

  void QuicListener::logStats() {
    // §Q-STATS-LOCK-FIX (review batch 2)：鎖內只 snapshot session 清單，
    // 組字串 + BOOST_LOG I/O 移到鎖外。原本整段持 _sessionMutex，
    // ioLoop 的 callback（findSession 等）要拿同一把鎖，stats 期間會
    // 阻塞 IO 執行緒。shared_ptr 保證 session 在鎖外仍存活。
    std::vector<std::pair<std::string, std::shared_ptr<QuicSession>>> snapshot;
    {
      std::lock_guard<std::mutex> lock(_sessionMutex);
      if (_sessions.empty())
        return;
      snapshot.reserve(_sessions.size());
      for (auto &[addr, session] : _sessions) {
        snapshot.emplace_back(addr, session);
      }
    }

    static const char *flowNames[] = {"?", "VIDEO", "AUDIO", "CTRL"};

    for (auto &[addr, session] : snapshot) {
      if (!session || !session->isReady())
        continue;

      // §K.10 per-flow 累計 + delta 速率
      auto now = std::chrono::steady_clock::now();
      double dtSec = std::chrono::duration<double>(now - session->_prevStatsTime).count();
      if (dtSec < 0.1) dtSec = 0.1; // 防除零

      std::string flowLine;
      for (int f = 1; f <= 3; f++) {
        uint64_t queued = session->_dgramQueuedByFlow[f].load();
        uint64_t failed = session->_dgramFailedByFlow[f].load();
        uint64_t bytes  = session->_bytesByFlow[f].load();
        uint64_t prevBytes = session->_prevBytesByFlow[f];
        double rateMbps = (double)(bytes - prevBytes) * 8.0 / dtSec / 1e6;

        if (f > 1) flowLine += " | ";
        flowLine += flowNames[f];
        flowLine += ": q=" + std::to_string(queued);
        if (failed > 0) flowLine += " FAIL=" + std::to_string(failed);
        flowLine += " " + std::to_string((int)(rateMbps * 10) / 10.0).substr(0,5) + "Mbps";

        session->_prevBytesByFlow[f] = bytes;
      }
      session->_prevStatsTime = now;

      uint64_t peak = session->_dgramPendingPeak.load();
      BOOST_LOG(info) << "[VIPLE-MPQUIC] §K.10 " << addr << " | " << flowLine
                      << " | pendingPeak=" << peak;

      // Per-path 品質明細
      auto stats = session->getStats();
      if (!stats.empty()) {
        std::string pathLine;
        for (size_t i = 0; i < stats.size(); i++) {
          if (i > 0) pathLine += " | ";
          pathLine += "p" + std::to_string(stats[i].pathId)
                    + " RTT=" + std::to_string((int)stats[i].rttMs) + "ms"
                    + " " + std::to_string((int)stats[i].throughputMbps) + "Mbps"
                    + " loss=" + std::to_string((int)(stats[i].lossPercent * 10) / 10.0).substr(0,4) + "%"
                    + " tx=" + std::to_string(stats[i].bytesSent / 1024) + "KB"
                    + " rx=" + std::to_string(stats[i].bytesRecv / 1024) + "KB";
        }
        BOOST_LOG(info) << "[VIPLE-MPQUIC] §K.10 " << addr << " paths: " << pathLine;
      }
    }
  }

  void QuicListener::statsLoop() {
    while (_statsRunning.load()) {
      // §K.10: 5 秒一次（之前 30 秒太慢，診斷時看不到即時變化）
      for (int i = 0; i < 50 && _statsRunning.load(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (_statsRunning.load()) {
        logStats();
      }
    }
  }

}  // namespace quic_server

#endif  // VIPLE_MPQUIC
