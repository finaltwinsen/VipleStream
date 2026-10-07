#pragma once

#include "Video.h"

typedef struct _RTPV_QUEUE_ENTRY {
    struct _RTPV_QUEUE_ENTRY* next;
    struct _RTPV_QUEUE_ENTRY* prev;
    PRTP_PACKET packet;
    uint64_t receiveTimeUs;
    uint64_t presentationTimeUs;
    uint32_t rtpTimestamp;
    int length;
    bool isParity;
} RTPV_QUEUE_ENTRY, *PRTPV_QUEUE_ENTRY;

typedef struct _RTPV_QUEUE_LIST {
    PRTPV_QUEUE_ENTRY head;
    PRTPV_QUEUE_ENTRY tail;
    uint32_t count;
} RTPV_QUEUE_LIST, *PRTPV_QUEUE_LIST;

// §K.17: Max new-frame packets to defer while waiting for old frame's last shard
#define RTPV_MAX_GRACE_PACKETS 4

// §VR-LINK-REPAIR：逐條連線記進度的格數（＝VIPLE_VR_LINK_MAX；RtpVideoQueue.c 有編譯期檢查）
#define RTPV_ML_LINKS 4

typedef struct _RTP_VIDEO_QUEUE {
    RTPV_QUEUE_LIST pendingFecBlockList;
    RTPV_QUEUE_LIST completedFecBlockList;

    uint64_t bufferFirstRecvTimeUs;
    uint32_t bufferLowestSequenceNumber;
    uint32_t bufferHighestSequenceNumber;
    uint32_t bufferFirstParitySequenceNumber;
    uint32_t bufferDataPackets;
    uint32_t bufferParityPackets;
    uint32_t receivedDataPackets;
    uint32_t receivedParityPackets;
    uint32_t receivedHighestSequenceNumber;
    uint32_t fecPercentage;
    uint32_t nextContiguousSequenceNumber;
    uint32_t missingPackets; // # of holes behind receivedHighestSequenceNumber
    bool useFastQueuePath;
    bool reportedLostFrame;

    uint32_t currentFrameNumber;

    bool multiFecCapable;
    uint8_t multiFecCurrentBlockNumber;
    uint8_t multiFecLastBlockNumber;

    uint64_t lastOosFramePresentationTimestamp;
    bool receivedOosData;
    // §VR-MULTILINK：同一幀的封包會從多條連線各來一份，亂序補包是常態——不做「依目前缺包數提早判定這一幀
    // 救不回來」的推測（另一條連線的那一份可能幾毫秒後就到）。由 VideoStream 每次加封包前設定。
    bool multiLink;
    // §VR-MULTILINK-HOLD：超前的封包（下一幀、或同一幀的下一個 FEC block）先排著，等目前這個 block 收齊再放行，
    // 最多等 mlHoldMaxUs（0＝不等）。說明見 RtpVideoQueue.c。mlHoldMaxUs 由 VideoStream 每次加封包前設定。
    uint32_t mlHoldMaxUs;
    int mlRxLink;                       // 這個封包是哪條連線收到的（VideoStream 每次加封包前設定；統計用）
    struct _RTPV_HELD_PACKET* mlHold;   // 環狀佇列，第一次用到才配置
    int mlHoldHead;
    int mlHoldCount;
    bool mlWaiting;                     // 佇列最前面那個封包正在等
    bool mlSweptValid;                  // 以下兩個＝上一次把環掃過一遍時佇列等的對象
    uint8_t mlSweptBlock;
    uint32_t mlSweptFrame;
    uint64_t mlWaitStartUs;
    uint64_t mlProgressUs;              // 最近一次「等的 block 在等待中收齊」的時間（§VR-LINK-REPAIR 用來延長環頭的期限）
    uint64_t mlStatLogUs;               // 以下是 10 s 統計
    uint32_t mlStatTick;
    uint32_t mlStatWaits;
    uint32_t mlStatRecovered;
    uint32_t mlStatExpired;
    uint32_t mlStatHeld;
    uint32_t mlStatMaxWaitUs;

    // §VR-LINK-REPAIR：正在等的那個 FEC block 缺 shard 時回報給 server（NACK），由它補送。說明見 RtpVideoQueue.c。
    // 前五個由 VideoStream 每次加封包前設定。
    bool mlRepair;                      // server 同意補包，而且有已確認的連線可以送回報
    bool mlHoldDual;                    // 至少兩條連線實際在送影像（另一條的那一份可能正要到）
    uint32_t mlRepairRtoUs;             // 回報之後這麼久還沒補齊就再報一次
    uint8_t mlCarryMask;                // 哪幾條連線實際在送影像（bit i＝mlRxLink 為 i 的那條）
    uint32_t mlLinkLagUs[RTPV_ML_LINKS];// 每條連線平常比最先送到的那條晚多久（VrMultiLink 量的）
    // 每條連線送到哪裡了：最新的（幀, block），與它在那個 block 裡送到的最大 RTP 序號。兩條以上在送時用來判斷
    // 「這條連線已經走過佇列在等的那個 block」——走過了還缺的，那條就不會再送來
    bool mlLinkValid[RTPV_ML_LINKS];
    uint8_t mlLinkBlock[RTPV_ML_LINKS];
    uint16_t mlLinkSeq[RTPV_ML_LINKS];
    uint32_t mlLinkFrame[RTPV_ML_LINKS];
    uint64_t mlLinkRxUs[RTPV_ML_LINKS]; // 這條連線最近一次送來它目前那個（幀, block）的封包的時間
    bool nkValid;                       // 以下的狀態屬於（nkFrame, nkBlock）；佇列等的對象換了就重來
    bool nkGone;                        // server 說補不了（或回報根本送不出去）：不必再等
    bool nkTailSeen;                    // 這個 block 的最後一個 shard 到過（這一輪該到的都到了）
    bool nkPolling;                     // RtpvPollHold 執行中＝接收真的閒下來了（「一段時間沒有新封包」的規則只在這時觸發）
    uint8_t nkBlock;
    uint8_t nkCount;                    // 已經回報幾次
    uint32_t nkFrame;
    uint64_t nkLastUs;                  // 上一次回報的時間
    uint64_t nkTailUs;                  // 最後一個 shard 到的時間
    uint64_t nkLastRxUs;                // 這個 block 最近一次收進封包的時間
    uint64_t nkFirstRxUs;               // 這個 block 第一個被採用的封包到達的時刻（0＝還沒有；放棄時的 log 用）
    uint64_t nkFirstPassUs;             // 第一條連線走過這個 block 的時間（兩條以上在送時用；0＝還沒有）
    uint8_t nkRefunds;                  // 這個 block 退還過幾次回報次數（回報是在鏈路空檔裡送的）
    int8_t nkLastRxLink;                // 這個 block 最近一個被採用的封包是哪條連線送來的（−1＝還沒有，或來自原本的 socket）
    uint64_t nkAnyRxUs;                 // 最近一次有任何影像封包到的時間（不分 block；不跟著對象重設）
    uint32_t nkLogCount;                // 逐筆 log 的節流（整場累計）
    uint32_t nkGiveUpLogCount;          // 放棄的 log 另外節流（少數事件，要看得到）
    uint32_t mlStatNack;                // 以下三個是 10 s 統計：回報次數、回報之後收齊的 block、server 回補不了的次數
    uint32_t mlStatRepaired;
    uint32_t mlStatGone;

    RTP_VIDEO_STATS stats; // the above values are short-lived, this tracks stats for the life of the queue

    // VipleStream §K.17: Grace period for late-arriving final shard.
    // At 180fps (5.55ms frame interval), BBR pacing spreads frame data
    // across the entire interval, so the last shard often arrives after
    // the next frame's first shard. Instead of immediately discarding
    // the old frame, we defer up to RTPV_MAX_GRACE_PACKETS new-frame
    // packets to give the missing shard a chance to arrive.
    struct {
        PRTP_PACKET packet;
        PRTPV_QUEUE_ENTRY entry;
        int length;
    } deferredPackets[RTPV_MAX_GRACE_PACKETS];
    int deferredCount;
    uint32_t deferredFrameNumber; // The new frame whose packets are deferred

    // §M01-B：佇列重置後還沒用真實封包建立序號基準。只用來印 re-anchor log，
    // 正確性不靠這個旗標（見 RtpvAddPacketInternal 的序號檢查）。
    bool seqBaselinePending;
    uint32_t seqRejectStreak;   // §M01-B：進行中 block 連續序號拒收數（診斷用）
    uint32_t seqJumpLogCount;   // §M01-B：序號半窗跳躍 log 節流

    // §M01-D：-101（ML_ERROR_NO_VIDEO_FRAME）診斷用，只記錄、不參與任何判斷。
    // VideoStream.c 回報 -101 時印出，用來分辨「host 還在送上一個 session 的
    // 畫面」與一般網路問題。佇列重置（RtpvInitializeQueue 的 memset）時一併歸零。
    //
    // 注意：幀在 data shard 收齊時就完成並推進 currentFrameNumber（reconstructFrame
    // 不等 parity），之後才到的同幀 parity 一定會被 §FRZ-B1 的 isBefore32 拒收
    // （落後量 lag=1）。所以 diagStalePkts 在開了 FEC 的正常串流裡也很大，
    // 不能當判別依據。判別要看 lag 超過 RTPV_DIAG_FAR_STALE_LAG 的 far 組：
    // 殘留 session 的幀號（例如 69411）先把佇列帶到高幀號，新 session 的
    // 低幀號 IDR／P-frame 之後全被判過期，lag ≈ 殘留 session 已跑的幀數
    // （數千到數萬）；新 session 的封包先到也一樣——殘留幀號較高會被當成
    // 新幀採納，之後新 session 的封包同樣落進 far 組，與到達順序無關。
    bool diagFirstFrameSeen;
    uint32_t diagFirstFrameIndex;     // 佇列收到的第一個 frameIndex（次要線索，受到達順序影響）
    uint32_t diagStalePkts;           // 所有 isBefore32 拒收的封包，大多是已完成幀的尾端 FEC parity，屬正常
    uint32_t diagFarStalePkts;        // lag > RTPV_DIAG_FAR_STALE_LAG 的拒收封包
    uint32_t diagFarStaleFrames;      // 同上，以幀計（連續同幀號只算一次）
    uint32_t diagFarStaleLastFrame;   // 最近一個 far 拒收的幀號（去重用）
    uint32_t diagFarStaleMinFrame;    // far 拒收的最小幀號
    uint32_t diagFarStaleMaxFrame;    // far 拒收的最大幀號
    uint32_t diagFarStaleMaxLag;      // far 拒收的最大落後量（currentFrameNumber - frameIndex）
} RTP_VIDEO_QUEUE, *PRTP_VIDEO_QUEUE;

// §M01-D：落後量（currentFrameNumber - frameIndex）超過這個值的拒收封包才算
// 「far」。正常尾端 parity 的 lag=1；多路徑亂序在 180 fps 下也只有十幾幀。
// 注意 bufferbloat 嚴重的慢路徑（秒級延遲）也可能產生 far，但 maxLag 只到
// 數百；殘留 session 的 maxLag 是它已跑的幀數，通常數千以上。
// VideoStream.c 解密前的落後丟棄也用同一個門檻。
#define RTPV_DIAG_FAR_STALE_LAG 64U

#define RTPF_RET_QUEUED    0
#define RTPF_RET_REJECTED  1
// §VR-MULTILINK-HOLD：封包先排著（所有權已交給佇列，和 QUEUED 一樣），還不知道會不會被採用
#define RTPF_RET_HELD      2

// §VR-MULTILINK-HOLD：沒有封包到的時候也要檢查等待的期限。RtpvHoldTimeoutMs 回傳最前面那個還能等幾毫秒
// （0＝沒有在等）；接收逾時後呼叫 RtpvPollHold 把到期的放行。
int RtpvHoldTimeoutMs(PRTP_VIDEO_QUEUE queue);
void RtpvPollHold(PRTP_VIDEO_QUEUE queue);
// §VR-LINK-REPAIR：server 回了 REPAIR_GONE（這個 block 補不了）。只在影像接收執行緒呼叫。
void RtpvRepairGone(PRTP_VIDEO_QUEUE queue, uint32_t frame, uint8_t block);

void RtpvInitializeQueue(PRTP_VIDEO_QUEUE queue);
void RtpvCleanupQueue(PRTP_VIDEO_QUEUE queue);
int RtpvAddPacket(PRTP_VIDEO_QUEUE queue, PRTP_PACKET packet, int length, PRTPV_QUEUE_ENTRY packetEntry);
uint32_t RtpvGetCurrentFrameNumber(PRTP_VIDEO_QUEUE queue);
void RtpvSubmitQueuedPackets(PRTP_VIDEO_QUEUE queue);
