/**
 * @file src/quic_server.h
 * @brief VipleStream MP-QUIC server: QUIC listener + multipath subflow
 *        management for streaming sessions.
 *
 * When MP-QUIC is negotiated, the server opens a QUIC listener on
 * config::stream.mpquic_port. Each streaming session gets a dedicated
 * picoquic connection. Video and audio are sent as QUIC DATAGRAMs
 * (RFC 9221); control uses a reliable QUIC stream (#0).
 *
 * The server drives multipath from its side by probing paths that the
 * client advertises through QUIC path validation (RFC 9443 §5).
 */
#pragma once

#ifdef VIPLE_MPQUIC

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/asio.hpp>

// picoquic uses anonymous-enum typedefs and tag-name typedefs that
// can't be forward-declared cleanly. Just include the full header.
#include <picoquic.h>

struct _reed_solomon;

namespace quic_server {

  struct SubflowStats {
    int pathId;
    float rttMs;
    float throughputMbps;
    float lossPercent;
    bool active;
    uint64_t bytesSent;
    uint64_t bytesRecv;
  };

  using RecvHandler = std::function<void(uint8_t flowType,
                                         const uint8_t *data, size_t len)>;

  class QuicSession {
  public:
    QuicSession(picoquic_cnx_t *cnx);
    ~QuicSession();

    QuicSession(const QuicSession &) = delete;
    QuicSession &operator=(const QuicSession &) = delete;

    bool sendDatagram(uint8_t flowType, const uint8_t *data, size_t len);

    // Scheduler-aware datagram send (picks best path based on strategy)
    bool sendDatagramScheduled(uint8_t flowType, const uint8_t *data,
                               size_t len, int scheduler);

    // §Q-REINJECT v1.5.172: 對所有 active path 同送同一個 datagram
    // （XLINK SIGCOMM 2021 reinjection 模式）。用於 PATH-SWITCH 後
    // 送 IDR frame，確保至少一條 path 送達。client depacketizer 用
    // sequence number 去重，先到的勝出。
    bool sendDatagramReinject(uint8_t flowType, const uint8_t *data,
                              size_t len);

    // §Q-REINJECT v1.5.172: 判定當前是否在 PATH-SWITCH 後 2 秒
    // reinjection 視窗內。stream.cpp 偵測 IDR frame 時，若此函式回
    // true 就用 sendDatagramReinject 送 IDR shards。
    bool isInReinjectionWindow() const;

    bool sendStream(const uint8_t *data, size_t len);

    void setRecvHandler(RecvHandler handler);

    // §Q-RECVHANDLER-FIX (review)：handler 是否已註冊。stream.cpp 原本
    // 用 process 級 static 旗標守註冊，導致第二條 session 起的新
    // QuicSession 永遠拿不到 handler（QUIC failover/IDR/FEC/ping 全
    // 靜默失效）。改由各 session 自身狀態回報，呼叫端每幀檢查重註冊。
    bool hasRecvHandler() const;

    // §Q-RECVHANDLER-FIX：IO thread 在 callback 內呼叫 handler，與
    // broadcast thread 的 setRecvHandler 賦值跨執行緒——透過此函式
    // 在鎖內取一份 copy 再於鎖外呼叫，避免 std::function 撕裂讀。
    void invokeRecvHandler(uint8_t flowType, const uint8_t *data, size_t len);

    std::vector<SubflowStats> getStats() const;

    bool isReady() const;

    // Drain all pending datagrams into picoquic — MUST be called from
    // the IO thread only (picoquic is single-threaded).
    void drainPendingToQuic();

  private:
    friend class QuicListener;

    // §Q-CNX-ATOMIC-FIX (review batch 2)：_cnx 僅限 IO 執行緒存取
    // （drainPendingToQuic / picoquicCallback / stop() join 之後）。
    // sender / stats 執行緒一律改讀 _ready——IO 執行緒在 callback_close
    // 寫 nullptr 後 picoquic 隨即 free cnx，跨執行緒讀是 TOCTOU UAF。
    picoquic_cnx_t *_cnx;

    // §Q-CNX-ATOMIC-FIX：連線是否 ready。IO 執行緒在 session 建立時
    // 設 true、偵測到 disconnecting / callback_close / stop() 設 false；
    // 其他執行緒只讀此 atomic，不得 dereference _cnx。
    std::atomic<bool> _ready{false};

    RecvHandler _recvHandler;
    mutable std::mutex _recvHandlerMutex; // §Q-RECVHANDLER-FIX：保護 _recvHandler 跨執行緒讀寫

    // §K.4 fix: video/audio threads enqueue into _pendingQueue;
    // the IO thread drains it via drainPendingToQuic().
    // All picoquic_queue_datagram_frame calls happen on the IO thread.
    struct PendingDgram {
      uint8_t flowType;
      int scheduler;
      bool isStream;          // true → reliable stream #0; false → datagram
      bool reinject;          // §Q-REINJECT v1.5.172: true → 對所有 active path
                              //   同送 datagram（XLINK reinjection 模式，用於
                              //   PATH-SWITCH 後送 IDR 提高送達率）
      std::vector<uint8_t> data;
    };
    std::mutex _pendingMutex;
    std::vector<PendingDgram> _pendingQueue;

    uint16_t _seqCounters[4] = {};

    // Picoquic does not expose a public "get number of paths" API, so we
    // track active path ids ourselves via picoquic_callback_path_available
    // / _suspended / _deleted. Path 0 is always present (the initial cnx
    // path), so we start at 1.
    mutable std::mutex _pathMutex;
    std::vector<uint64_t> _activePaths{0};

    // §Q-STATS-CACHE-FIX (review batch 2)：path quality 快取。
    // picoquic_get_path_quality 只能在 IO 執行緒呼叫（picoquic 單執行緒
    // 約束）；statsLoop 原本直接呼叫會與 ioLoop 並行走訪可能正被刪除
    // 的 path 物件。改由 IO 執行緒在 drainPendingToQuic 的 §K.11 限頻
    // 區塊內更新快取，getStats() 只讀快取。
    mutable std::mutex _statsCacheMutex;
    std::vector<SubflowStats> _statsCache;
    void updateStatsCache();  // 僅限 IO 執行緒呼叫

    // RR cursor for REDUNDANT scheduler (rotates the preferred path so
    // picoquic spreads load across subflows over consecutive frames).
    int _redundantRR = 0;

    // §Q-PATH-SWITCH 2026-05-27: track bestVideoPath transitions for
    // mid-stream diagnostic logging. -2 = uninitialized, -1 = fallback
    // to cnx-level queue (single path), 0+ = picoquic path index.
    int _lastVideoPath = -2;

    // §Q-PATH-SWITCH-STICKY 2026-05-27: bestVideoPath 一旦選定就 sticky，
    // 只在當前路徑不健康時才允許切換。v1.5.163 實測：dwell 時間策略
    // 只延遲震盪不防止震盪 → 改為無條件 sticky。
    // _lastPathSwitchTime 僅用於 diagnostic log。
    uint64_t _lastPathSwitchTime = 0;  // picoquic_current_time() µs

    // §Q-RECOVERY-IDR-FIX (review batch 2)：是否曾經選定過有效 video
    // path（bestVideoPath ≥ 0）。用於區分「初始 path 選定的 -1 → 0」
    // （不可觸發 IDR——見 §Q-IDR-INIT-FIX 啟動期 IDR 風暴教訓）與
    // 「全 path 失效後恢復的 -1 → M」（必須觸發 IDR + reinject 視窗，
    // 否則 client decoder 凍結，R.3 殭屍態）。僅 IO 執行緒
    // （drainPendingToQuic）讀寫，不需同步。
    bool _hadVideoPathBefore = false;

    // §Q-RECOVERY-IDR-FIX：M → -1 轉換的時刻（µs）。-1 → M 恢復時
    // 要求 -1 駐留 ≥1s 才觸發 IDR loop——亞秒級的全 path 抖動（單輪
    // RTO spike、nb_paths 縮放）期間 video 仍經 cnx-level queue 正常
    // 送出，不需要 IDR。僅 IO 執行緒讀寫。
    uint64_t _videoPathLostTime = 0;

    // §Q-PATH-SWITCH-IDR 2026-05-27: bestVideoPath 切換時自動要求 IDR。
    // drainPendingToQuic() 設 flag → stream.cpp video 送出迴圈消費。
    // §Q-STICKY-ESCAPE 2026-07-02: bool 改為 kind——
    //   0 = 無、1 = full（真 failover，10s IDR loop）、
    //   2 = light（escape 主動切往已驗證的更好路徑，2s 短視窗）。
    std::atomic<int> _pathSwitchIdrKind{0};

    // §Q-FAILOVER-CNXQ 2026-05-27: failover 期間改用 cnx-level datagram
    // queue（不用 per-path queue）。picoquic 對剛從 backup 升級的路徑
    // 有 scheduler 排程缺陷——per-path queue 的資料堆積但不被排程發送。
    // 改用 cnx-level queue 讓 picoquic 內部排程器自行路由到可用路徑。
    // Pass 3 / EMERGENCY-PROMOTE / 任何 real failover 切換設 true；
    // 現載流路徑 warm 且過 §Q-CNXQ-WARMUP 時間下限後清除。
    bool _failoverCnxQueue = false;
    // §Q-CNXQ-WARMUP 2026-07-02: cnx-level queue 的最短持續時刻。新
    // path 初始 cwin ~72KB 會騙過 64KB warm 門檻，需要時間下限。
    uint64_t _cnxQueueMinUntil = 0;

    // §Q-STICKY-ESCAPE 2026-07-02: sticky 選路品質逃生門的狀態。
    // 2026-07-01 事故：failover 到 VPN 後乙太恢復，sticky「不死不換」+
    // Pass 1 warm-cwin 門檻（閒置 path 永遠冷）讓視訊留在持續 loss 的
    // VPN 上 86 分鐘。每 500ms 對每條 path 取 nb_losses_found delta，
    // 以 unique_path_id 為 key（防 path 增刪 index 漂移）維護 6 視窗
    // lossy 位元圖。僅 IO 執行緒讀寫。
    struct EscSnap {
      bool used = false;
      uint64_t pathId = 0;
      uint64_t lossSnap = 0;
      uint8_t lossyBits = 0;   // bit0 = 最近視窗是否有 loss
      uint8_t samples = 0;     // 已觀測視窗數（新 path 需滿 6 才可信）
    };
    EscSnap _escSnaps[16] {};
    uint64_t _lastEscapeEval = 0;      // 上次評估時刻（500ms 節流）
    int _escapeStreak = 0;             // 連續滿足逃生條件的輪數
    uint64_t _escLastCandidateId = 0;  // streak 綁定的候選 path id
    uint64_t _lastEscapeSwitch = 0;    // escape 專屬 30s 冷卻
    uint64_t _escNoCandWarn = 0;       // 「無候選」log 節流
    bool _escapeIdrLight = false;      // 本輪切換使用 light IDR

    // §Q-ABR-FLOOR-ESCAPE (Fix C): stream.cpp ABR 執行緒設，IO 執行緒
    // 的 escape 評估消費。true = 放寬 hysteresis（streak 1、免 30s 冷
    // 卻；候選品質與 10s 駐留不放寬）。
    std::atomic<bool> _pathReevalRequested{false};
  public:
    // stream.cpp 調用：若 bestVideoPath 切換過則回非 0 並清除 flag。
    // 回傳值 = IDR kind（1=full 10s loop、2=light 2s 視窗）。
    int consumePathSwitchIdr() {
      return _pathSwitchIdrKind.exchange(0, std::memory_order_acq_rel);
    }

    // §Q-ABR-FLOOR-ESCAPE (Fix C): ABR 在 floor-stuck（低檔 + 持續
    // loss ≥30s）時呼叫，請求 MP-QUIC 層重評估視訊路徑。
    void requestPathReevaluation() {
      _pathReevalRequested.store(true, std::memory_order_release);
    }
  private:

    // §K.7 diag: datagram queue/send 計數器
    std::atomic<uint64_t> _dgramQueued{0};
    std::atomic<uint64_t> _dgramPushed{0};
    std::atomic<uint64_t> _dgramDroppedStale{0};
    std::atomic<uint64_t> _dgramPendingPeak{0};

    // §Q-STALE 2026-05-27: approximate picoquic video queue depth tracking.
    // 防止 picoquic datagram queue 無限堆積。server 以 120-156fps 編碼
    // 但 picoquic 只能送 ~60fps → 差額 FIFO 堆積 → 200K+ datagrams →
    // client 看到 2 分鐘前的畫面 → 滑鼠「慢半拍、越來越差」。
    // approxVideoQueueDepth 用 bytes_sent delta 估算 picoquic 已送量，
    // 超過 MAX 就丟棄多餘 video datagram 不入 picoquic queue。
    int64_t _approxVideoQueueDepth = 0;
    uint64_t _lastTotalBytesSent = 0;
    static constexpr int64_t MAX_VIDEO_QUEUE_DEPTH = 60; // ~4 frames × 14 dgrams/frame

    // §Q-PATH-SWITCH-FLUSH 2026-05-27: PATH-SWITCH 後寬限期計數器。
    // 非零期間 §Q-STALE 不丟 video datagram，確保 IDR 通過。
    int _pathSwitchGraceCycles = 0;

    // §Q-STALE-SAFETY-VALVE v1.5.181: video stall 偵測計時器。
    // 當 §Q-STALE 持續丟棄 video 超過 3 秒，表示陷入死亡螺旋
    // （沒有 PATH-SWITCH → grace 不重設 → 永遠丟棄）。
    // 安全閥重設 approxVQ + 恢復 grace 來打破循環。
    uint64_t _staleStallStart = 0;

    // §Q-GRACE-FIX 2026-05-27 (v1.5.172): per-failover-episode grace
    // 重設邏輯。原版每次 PATH-SWITCH 都重設 grace → 振盪期間 grace
    // 永不過期 → §Q-STALE 失效。改為「同 episode 內振盪不續命」。
    uint64_t _failoverEpisodeStartTime = 0;

    // §Q-REINJECT-WINDOW 2026-05-27 (v1.5.172 bug fix)：reinject 視窗
    // 只在「真正的 failover 切換」開（_lastVideoPath ≥ 0 → 新 path ≥ 0）。
    // 不能用 _lastPathSwitchTime，因為連線剛建立時的初始 path 選定
    // （-2 → -1 → 0）也會觸發 PATH-SWITCH 邏輯，誤把 reinject 開了
    // → IDR 推到所有 standby path 的 picoquic queue → 堆積塞爆。
    // §Q-PERF：atomic——IO 執行緒在 drain 內寫入，video 執行緒經
    // isInReinjectionWindow()（stream.cpp 每 frame）讀取，裸 uint64_t
    // 是跨執行緒 data race（與 _pathSwitchIdrNeeded 同型問題）。
    std::atomic<uint64_t> _reinjectWindowUntil{0};

    // §Q-PROMOTE-DEBOUNCE 2026-05-27: 防止 Pass 3 emergency-promote
    // 對同一 path 在短時間內重複呼叫。picoquic_set_path_status 是
    // async，期間 path 仍顯示 backup，會觸發 Pass 3 連續多次 promote。
    uint64_t _lastEmergencyPromoteTime = 0;
    uint64_t _lastEmergencyPromotedId = UINT64_MAX;

    // §K.10 diag: per-flow 計數器 (index = flowType: 0=unused, 1=video, 2=audio, 3=control)
    std::atomic<uint64_t> _dgramQueuedByFlow[4]{};
    std::atomic<uint64_t> _dgramFailedByFlow[4]{};
    std::atomic<uint64_t> _bytesByFlow[4]{};
    // 上次 stats snapshot（用於計算 delta 速率）
    uint64_t _prevBytesByFlow[4] = {};
    std::chrono::steady_clock::time_point _prevStatsTime = std::chrono::steady_clock::now();

    // §5b FEC: video datagram RS encoding (4 data + 2 parity)
    struct FecGroupState {
      std::vector<uint8_t> payloads[4];
      uint16_t seqs[4] = {};
      int count = 0;
      uint16_t baseSeq = 0;
    };
    FecGroupState _fecGroup;
    ::_reed_solomon *_fecRs = nullptr;
    void flushFecParity(int scheduler);
  };

  class QuicListener {
  public:
    QuicListener();
    ~QuicListener();

    QuicListener(const QuicListener &) = delete;
    QuicListener &operator=(const QuicListener &) = delete;

    /**
     * Start the QUIC listener.
     * @param port       UDP port to bind
     * @param certPath   PEM certificate path
     * @param keyPath    PEM private key path
     * @return true on success
     */
    bool start(uint16_t port,
               const std::string &certPath,
               const std::string &keyPath);

    void stop();

    bool isRunning() const { return _running.load(); }

    /**
     * Get the active session for a given client address, or nullptr.
     * Thread-safe.
     */
    std::shared_ptr<QuicSession> getSession(
        const boost::asio::ip::address &clientAddr);

    /**
     * Called when a new QUIC session is accepted (after handshake).
     */
    using SessionCallback = std::function<void(std::shared_ptr<QuicSession>)>;
    void setSessionCallback(SessionCallback cb);

    // Log aggregate stats for all sessions (called periodically)
    void logStats();

  private:
    void ioLoop();

    static int picoquicCallback(picoquic_cnx_t *cnx,
        uint64_t stream_id, uint8_t *bytes, size_t length,
        picoquic_call_back_event_t fin_or_event,
        void *callback_ctx, void *stream_ctx);

    picoquic_quic_t *_quic = nullptr;
    std::thread _ioThread;
    std::atomic<bool> _running{false};

    std::mutex _sessionMutex;
    std::unordered_map<std::string, std::shared_ptr<QuicSession>> _sessions;

    SessionCallback _sessionCallback;
    uint16_t _port = 0;

    // Periodic stats logging
    std::thread _statsThread;
    std::atomic<bool> _statsRunning{false};
    void statsLoop();
  };

  // Global listener instance (created by stream.cpp when MP-QUIC is active)
  inline QuicListener *g_listener = nullptr;

}  // namespace quic_server

#endif  // VIPLE_MPQUIC
