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
    return _cnx &&
           picoquic_get_cnx_state(_cnx) == picoquic_state_ready;
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
      _pendingQueue.push_back({flowType, SCHED_AUTO, false, std::move(frame)});
    }
    return true;
  }

  bool QuicSession::sendDatagramScheduled(uint8_t flowType,
                                           const uint8_t *data, size_t len,
                                           int scheduler) {
    if (!isReady()) {
      static int notReadyCount = 0;
      if (notReadyCount++ < 5) {
        BOOST_LOG(warning) << "[VIPLE-MPQUIC] §K.7 sendDatagramScheduled: NOT READY"
                           << " flow=" << (int)flowType << " len=" << len
                           << " cnxState=" << (_cnx ? (int)picoquic_get_cnx_state(_cnx) : -1);
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
        _pendingQueue.push_back({flowType, scheduler, false, std::move(frame)});
      }

      if (++g.count == FEC_DATA_SHARDS) {
        flushFecParity(scheduler);
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
      _pendingQueue.push_back({flowType, scheduler, false, std::move(frame)});
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
      _pendingQueue.push_back({FLOW_CONTROL, 0, true, std::move(buf)});
    }
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
        _pendingQueue.push_back({FLOW_VIDEO, scheduler, false, std::move(frame)});
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
      return;
    }

    // §K.11 DIAG: 監控 picoquic datagram queue 深度和 cwnd 狀態。
    // v1.5.74 的 aggressive flush 在 slow-start 階段砍掉了 IDR
    // frame 的 shards（cwnd ~14KB 只能送 ~10 shards，剩下的被
    // 下一輪 flush 清掉），造成黑畫面。改為純診斷——不 drop。
    {
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
      uint64_t cwnd = 0, inFlight = 0;
      if (_cnx->nb_paths > 0 && _cnx->path[0] != nullptr) {
        cwnd = _cnx->path[0]->cwin;
        inFlight = _cnx->path[0]->bytes_in_transit;
      }
      // 每 500 筆 batch 印一次（約每秒），或 queue > 50 時每次印，
      // 或 cwnd 低於 IDR frame × 2 警戒值（~200KB）時每次印
      // §K.16: BBRMinPipeCwnd = 128，地板 ~185KB
      static int diagCounter = 0;
      bool cwndAlert = (cwnd > 0 && cwnd < 200000);
      if (++diagCounter >= 500 || queueDepth > 50 || cwndAlert) {
        diagCounter = 0;
        // §K.12: 增加 BBR 狀態診斷——recovery / pto / loss 計數
        uint64_t nbLosses = 0, nbRetransmit = 0, totalBytesLost = 0;
        if (_cnx->nb_paths > 0 && _cnx->path[0] != nullptr) {
          nbLosses = _cnx->path[0]->nb_losses_found;
          nbRetransmit = _cnx->path[0]->nb_retransmit;
          totalBytesLost = _cnx->path[0]->total_bytes_lost;
        }
        BOOST_LOG(info) << "[VIPLE-MPQUIC] §K.11 drain: pending=" << queueDepth
                        << " batch=" << batch.size()
                        << " cwnd=" << cwnd
                        << " inFlight=" << inFlight
                        << " headroom=" << (int64_t)(cwnd - inFlight)
                        << " losses=" << nbLosses
                        << " retx=" << nbRetransmit
                        << " lostB=" << totalBytesLost;
        if (cwndAlert) {
          BOOST_LOG(warning) << "[VIPLE-MPQUIC] §K.16 cwnd LOW: " << cwnd
                             << " (floor=" << (48 * (_cnx->nb_paths > 0 && _cnx->path[0]
                                ? _cnx->path[0]->send_mtu : 1500))
                             << ") pending=" << queueDepth;
        }
      }
    }

    // §5d: 找 MIN_RTT 路徑，video datagram 路由到該 path 的 per-path queue。
    // 單路徑時 bestVideoPath=-1，fallback 到 cnx-level queue（向下相容）。
    int bestVideoPath = -1;
    if (_cnx->nb_paths > 1) {
      uint64_t minRtt = UINT64_MAX;
      for (int i = 0; i < _cnx->nb_paths; i++) {
        if (_cnx->path[i] != nullptr && !_cnx->path[i]->path_is_demoted &&
            _cnx->path[i]->smoothed_rtt < minRtt) {
          minRtt = _cnx->path[i]->smoothed_rtt;
          bestVideoPath = i;
        }
      }
    }

    for (auto &dg : batch) {
      if (dg.isStream) {
        picoquic_add_to_stream(_cnx, 0, dg.data.data(), dg.data.size(), 0);
        continue;
      }

      // §K.9 LAN MTU boost
      for (int i = 0; i < _cnx->nb_paths; i++) {
        if (_cnx->path[i] != nullptr && _cnx->path[i]->send_mtu < 1500) {
          _cnx->path[i]->send_mtu = 1500;
        }
      }

      uint8_t ft = dg.flowType < 4 ? dg.flowType : 0;
      int qret;
      // §5d: video → per-path queue (MIN_RTT path)；其餘 → cnx-level queue
      if (ft == FLOW_VIDEO && bestVideoPath >= 0) {
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
    _recvHandler = std::move(handler);
  }

  std::vector<SubflowStats> QuicSession::getStats() const {
    std::vector<SubflowStats> stats;
    if (!_cnx)
      return stats;

    std::vector<uint64_t> paths;
    {
      std::lock_guard<std::mutex> lock(_pathMutex);
      paths = _activePaths;
    }

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
      stats.push_back(s);
    }

    return stats;
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
      break;
    }

    case picoquic_callback_datagram: {
      if (length < DGRAM_HDR_SIZE)
        break;
      auto *hdr = reinterpret_cast<const QuicDgramHeader *>(bytes);
      auto session = findSession(cnx);
      if (session && session->_recvHandler) {
        session->_recvHandler(
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
      if (session && session->_recvHandler) {
        session->_recvHandler(FLOW_CONTROL, bytes, length);
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
    std::lock_guard<std::mutex> lock(_sessionMutex);
    if (_sessions.empty())
      return;

    static const char *flowNames[] = {"?", "VIDEO", "AUDIO", "CTRL"};

    for (auto &[addr, session] : _sessions) {
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
