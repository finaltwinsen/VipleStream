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
} RTP_VIDEO_QUEUE, *PRTP_VIDEO_QUEUE;

#define RTPF_RET_QUEUED    0
#define RTPF_RET_REJECTED  1

void RtpvInitializeQueue(PRTP_VIDEO_QUEUE queue);
void RtpvCleanupQueue(PRTP_VIDEO_QUEUE queue);
int RtpvAddPacket(PRTP_VIDEO_QUEUE queue, PRTP_PACKET packet, int length, PRTPV_QUEUE_ENTRY packetEntry);
uint32_t RtpvGetCurrentFrameNumber(PRTP_VIDEO_QUEUE queue);
void RtpvSubmitQueuedPackets(PRTP_VIDEO_QUEUE queue);
