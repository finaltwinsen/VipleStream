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

void RtpvInitializeQueue(PRTP_VIDEO_QUEUE queue);
void RtpvCleanupQueue(PRTP_VIDEO_QUEUE queue);
int RtpvAddPacket(PRTP_VIDEO_QUEUE queue, PRTP_PACKET packet, int length, PRTPV_QUEUE_ENTRY packetEntry);
uint32_t RtpvGetCurrentFrameNumber(PRTP_VIDEO_QUEUE queue);
void RtpvSubmitQueuedPackets(PRTP_VIDEO_QUEUE queue);
