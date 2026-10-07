#include "Limelight-internal.h"
#include "rswrapper.h"
#include "VrMultiLink.h"

#if defined(LC_DEBUG) && !defined(LC_FUZZING)
// This enables FEC validation mode with a synthetic drop
// and recovered packet checks vs the original input. It
// is on by default for debug builds.
#define FEC_VALIDATION_MODE
#define FEC_VERBOSE
#endif

// VipleStream: Reduced from 5 minutes to 60 seconds.
// VPN connections (Tailscale, WireGuard) have frequent OOS packets;
// 5-minute cooldown effectively disables speculative RFI for the entire session.
#define SPECULATIVE_RFI_COOLDOWN_PERIOD_US 60000000

// RTP packets use a 90 KHz presentation timestamp clock
#define PTS_DIVISOR 90

void RtpvInitializeQueue(PRTP_VIDEO_QUEUE queue) {
    reed_solomon_init();
    memset(queue, 0, sizeof(*queue));

    queue->currentFrameNumber = 1;
    queue->multiFecCapable = APP_VERSION_AT_LEAST(7, 1, 431);

    // §M01-B：memset 出來的 nextContiguousSequenceNumber=0 不代表「沒有基準」。
    // 舊版會把 isBefore16(seq,0) 為真（seq ≥ 32768）的封包全部判成過期，
    // 一直卡到 16-bit 序號繞回。基準改由第一個開新 FEC block 的封包重建。
    queue->seqBaselinePending = true;
}

static void purgeListEntries(PRTPV_QUEUE_LIST list) {
    while (list->head != NULL) {
        PRTPV_QUEUE_ENTRY entry = list->head;
        list->head = entry->next;
        free(entry->packet);
    }

    list->tail = NULL;
    list->count = 0;
}

// §VR-MULTILINK-HOLD：等待佇列的一格（說明在下面 mlAddPacket 前）
#define RTPV_ML_HOLD_MAX 2048

#if RTPV_ML_LINKS < VIPLE_VR_LINK_MAX
#error RTPV_ML_LINKS must cover VIPLE_VR_LINK_MAX
#endif

typedef struct _RTPV_HELD_PACKET {
    PRTP_PACKET packet;
    PRTPV_QUEUE_ENTRY entry;
    int length;
    int link;
    uint64_t arrivalUs;
} RTPV_HELD_PACKET;

static void mlFreeHold(PRTP_VIDEO_QUEUE queue) {
    while (queue->mlHoldCount > 0) {
        free(queue->mlHold[queue->mlHoldHead].packet);
        queue->mlHoldHead = (queue->mlHoldHead + 1) % RTPV_ML_HOLD_MAX;
        queue->mlHoldCount--;
    }
    free(queue->mlHold);
    queue->mlHold = NULL;
    queue->mlHoldHead = 0;
    queue->mlWaiting = false;
    queue->mlSweptValid = false;
}

void RtpvCleanupQueue(PRTP_VIDEO_QUEUE queue) {
    purgeListEntries(&queue->pendingFecBlockList);
    purgeListEntries(&queue->completedFecBlockList);

    // §K.17: Free any deferred packets from grace period
    for (int i = 0; i < queue->deferredCount; i++) {
        free(queue->deferredPackets[i].packet);
    }
    queue->deferredCount = 0;

    // §VR-MULTILINK-HOLD：還在等的封包一併釋放
    mlFreeHold(queue);
}

static void insertEntryIntoList(PRTPV_QUEUE_LIST list, PRTPV_QUEUE_ENTRY entry) {
    LC_ASSERT(entry->prev == NULL);
    LC_ASSERT(entry->next == NULL);

    if (list->head == NULL) {
        LC_ASSERT(list->count == 0);
        LC_ASSERT(list->tail == NULL);
        list->head = list->tail = entry;
    }
    else {
        LC_ASSERT(list->count != 0);
        PRTPV_QUEUE_ENTRY oldTail = list->tail;
        entry->prev = oldTail;
        LC_ASSERT(oldTail->next == NULL);
        oldTail->next = entry;
        list->tail = entry;
    }

    list->count++;
}

static void removeEntryFromList(PRTPV_QUEUE_LIST list, PRTPV_QUEUE_ENTRY entry) {
    LC_ASSERT(entry != NULL);
    LC_ASSERT(list->count != 0);
    LC_ASSERT(list->head != NULL);
    LC_ASSERT(list->tail != NULL);

    if (list->head == entry) {
        list->head = entry->next;
    }
    if (list->tail == entry) {
        list->tail = entry->prev;
    }

    if (entry->prev != NULL) {
        LC_ASSERT(entry->prev->next == entry);
        entry->prev->next = entry->next;
    }
    if (entry->next != NULL) {
        LC_ASSERT(entry->next->prev == entry);
        entry->next->prev = entry->prev;
    }

    entry->next = NULL;
    entry->prev = NULL;

    list->count--;
}

static void reportFinalFrameFecStatus(PRTP_VIDEO_QUEUE queue) {
    SS_FRAME_FEC_STATUS fecStatus;

    fecStatus.frameIndex = BE32(queue->currentFrameNumber);
    fecStatus.highestReceivedSequenceNumber = BE16(queue->receivedHighestSequenceNumber);
    fecStatus.nextContiguousSequenceNumber = BE16(queue->nextContiguousSequenceNumber);
    fecStatus.missingPacketsBeforeHighestReceived = BE16(queue->missingPackets);
    fecStatus.totalDataPackets = BE16(queue->bufferDataPackets);
    fecStatus.totalParityPackets = BE16(queue->bufferParityPackets);
    fecStatus.receivedDataPackets = BE16(queue->receivedDataPackets);
    fecStatus.receivedParityPackets = BE16(queue->receivedParityPackets);
    fecStatus.fecPercentage = (uint8_t)queue->fecPercentage;
    fecStatus.multiFecBlockIndex = (uint8_t)queue->multiFecCurrentBlockNumber;
    fecStatus.multiFecBlockCount = (uint8_t)(queue->multiFecLastBlockNumber + 1);

    connectionSendFrameFecStatus(&fecStatus);
}

// newEntry is contained within the packet buffer so we free the whole entry by freeing entry->packet
static bool queuePacket(PRTP_VIDEO_QUEUE queue, PRTPV_QUEUE_ENTRY newEntry, PRTP_PACKET packet, int length, bool isParity, bool isFecRecovery) {
    PRTPV_QUEUE_ENTRY entry;
    bool outOfSequence;

    LC_ASSERT(!(isFecRecovery && isParity));
    LC_ASSERT(!isBefore16(packet->sequenceNumber, queue->nextContiguousSequenceNumber));

    // If the packet is in order, we can take the fast path and avoid having
    // to loop through the whole list. If we get an out of order or missing
    // packet, the fast path will stop working and we'll use the loop instead.
    //
    // NB: It's not enough to just check next contiguous sequence number because
    // it's possible that we hit the OOS path earlier which doesn't update the
    // next contiguous sequence number. If that happens, we need to use the slow
    // path for this entire frame to avoid possibly mishandling a duplicate packet.
    if (queue->useFastQueuePath && packet->sequenceNumber == queue->nextContiguousSequenceNumber) {
        queue->nextContiguousSequenceNumber = U16(packet->sequenceNumber + 1);
        outOfSequence = false;
    }
    else {
        outOfSequence = false;

        // Check for duplicates
        entry = queue->pendingFecBlockList.head;
        while (entry != NULL) {
            if (packet->sequenceNumber == entry->packet->sequenceNumber) {
                return false;
            }
            else if (isBefore16(packet->sequenceNumber, entry->packet->sequenceNumber)) {
                outOfSequence = true;
            }

            entry = entry->next;
        }

        // If we make it here, we cannot use the fast queue path for this frame because
        // we're about to queue a non-duplicate packet out of order. This will not update
        // nextContiguousSequenceNumber which the fast path relies on.
        queue->useFastQueuePath = false;
    }

    newEntry->packet = packet;
    newEntry->length = length;
    newEntry->isParity = isParity;
    newEntry->prev = NULL;
    newEntry->next = NULL;
    newEntry->presentationTimeUs = ((uint64_t)packet->timestamp * 1000) / PTS_DIVISOR;
    newEntry->rtpTimestamp = packet->timestamp;

    // FEC recovery packets are synthesized by us, so don't use them to determine OOS data
    if (!isFecRecovery) {
        if (outOfSequence) {
            // This packet was received after a higher sequence number packet, so note that we
            // received an out of order packet to disable our speculative RFI recovery logic.
            queue->lastOosFramePresentationTimestamp = newEntry->presentationTimeUs;
            if (!queue->receivedOosData) {
                Limelog("Leaving speculative RFI mode after OOS video data at frame %u\n",
                        queue->currentFrameNumber);
                queue->receivedOosData = true;
            }
        }
        else if (queue->receivedOosData && newEntry->presentationTimeUs > queue->lastOosFramePresentationTimestamp + SPECULATIVE_RFI_COOLDOWN_PERIOD_US) {
            Limelog("Entering speculative RFI mode after sequenced video data at frame %u\n",
                    queue->currentFrameNumber);
            queue->receivedOosData = false;
        }
    }

    insertEntryIntoList(&queue->pendingFecBlockList, newEntry);

    return true;
}

#define PACKET_RECOVERY_FAILURE()                     \
    ret = -1;                                         \
    Limelog("FEC recovery returned corrupt packet %d" \
            " (frame %d)", rtpPacket->sequenceNumber, \
            queue->currentFrameNumber);               \
    free(packets[i]);                                 \
    continue

// Returns 0 if the frame is completely constructed
static int reconstructFrame(PRTP_VIDEO_QUEUE queue) {
    unsigned int totalPackets = queue->bufferDataPackets + queue->bufferParityPackets;
    unsigned int neededPackets = queue->bufferDataPackets;
    int ret;

    LC_ASSERT(totalPackets == U16(queue->bufferHighestSequenceNumber - queue->bufferLowestSequenceNumber) + 1U);

#ifdef FEC_VALIDATION_MODE
    // We'll need an extra packet to run in FEC validation mode, because we will
    // be "dropping" one below and recovering it using parity. However, some frames
    // are so large that FEC is disabled entirely, so don't wait for parity on those.
    neededPackets += queue->fecPercentage ? 1 : 0;
#endif

    LC_ASSERT(totalPackets - neededPackets <= queue->bufferParityPackets);

    if (queue->pendingFecBlockList.count < neededPackets) {
        // If we've never received OOS data from this host, we can predict whether this frame will be recoverable
        // based on the packets we've received (or not) so far. If the number of missing shards exceeds the total
        // needed shards, there is no hope of recovering the data. The only way we could recover this frame is by
        // receiving OOS data, which is unlikely because we've not seen any recently from this host.
        if (!queue->reportedLostFrame && !queue->receivedOosData && !queue->multiLink) {
            // NB: We use totalPackets - neededPackets instead of just bufferParityPackets here because we require
            // one extra parity shard for recovery if we're in FEC validation mode.
            if (queue->missingPackets > totalPackets - neededPackets) {
                notifyFrameLost(queue->currentFrameNumber, true);
                queue->reportedLostFrame = true;
            }
            else {
                // Assert that there are enough remaining packets to possibly recover this frame.
                LC_ASSERT(neededPackets - queue->pendingFecBlockList.count <= U16(queue->bufferHighestSequenceNumber - queue->receivedHighestSequenceNumber));
            }
        }

        // Not enough data to recover yet
        return -1;
    }

    // If we make it here and reported a lost frame, we lied to the host. This can happen if we happen to get
    // unlucky and this particular frame happens to be the one with OOS data, but it should almost never happen.
    LC_ASSERT(queue->missingPackets <= queue->bufferParityPackets);
    LC_ASSERT(!queue->reportedLostFrame || queue->receivedOosData);
    if (queue->reportedLostFrame && !queue->receivedOosData) {
        // If it turns out that we lied to the host, stop further speculative RFI requests for a while.
        queue->receivedOosData = true;
        queue->lastOosFramePresentationTimestamp = queue->pendingFecBlockList.head->presentationTimeUs;
        Limelog("Leaving speculative RFI mode due to incorrect loss prediction of frame %u\n", queue->currentFrameNumber);
    }

#ifdef FEC_VALIDATION_MODE
    // If FEC is disabled or unsupported for this frame, we must bail early here.
    if ((queue->fecPercentage == 0 || AppVersionQuad[0] < 5) &&
            queue->receivedDataPackets == queue->bufferDataPackets) {
#else
    if (queue->receivedDataPackets == queue->bufferDataPackets) {
#endif
        // We've received a full frame with no need for FEC.
        return 0;
    }

    if (AppVersionQuad[0] < 5) {
        // Our FEC recovery code doesn't work properly until Gen 5
        Limelog("FEC recovery not supported on Gen %d servers\n",
                AppVersionQuad[0]);
        return -1;
    }

    reed_solomon* rs = NULL;
    unsigned char** packets = calloc(totalPackets, sizeof(unsigned char*));
    unsigned char* marks = calloc(totalPackets, sizeof(unsigned char));
    if (packets == NULL || marks == NULL) {
        ret = -2;
        goto cleanup;
    }

    rs = reed_solomon_new(queue->bufferDataPackets, queue->bufferParityPackets);

    // This could happen in an OOM condition, but it could also mean the FEC data
    // that we fed to reed_solomon_new() is bogus, so we'll assert to get a better look.
    LC_ASSERT(rs != NULL);
    if (rs == NULL) {
        ret = -3;
        goto cleanup;
    }

    memset(marks, 1, sizeof(char) * (totalPackets));

    int receiveSize = StreamConfig.packetSize + MAX_RTP_HEADER_SIZE;
    int packetBufferSize = receiveSize + sizeof(RTPV_QUEUE_ENTRY);

#ifdef FEC_VALIDATION_MODE
    // Choose a packet to drop
    unsigned int dropIndex = rand() % queue->bufferDataPackets;
    PRTP_PACKET droppedRtpPacket = NULL;
    int droppedRtpPacketLength = 0;
#endif

    PRTPV_QUEUE_ENTRY entry = queue->pendingFecBlockList.head;
    while (entry != NULL) {
        unsigned int index = U16(entry->packet->sequenceNumber - queue->bufferLowestSequenceNumber);

#ifdef FEC_VALIDATION_MODE
        if (index == dropIndex) {
            // If this was the drop choice, remember the original contents
            // and "drop" it.
            droppedRtpPacket = entry->packet;
            droppedRtpPacketLength = entry->length;
            entry = entry->next;
            continue;
        }
#endif

        // We should never have duplicate packets enqueued
        LC_ASSERT(packets[index] == NULL);
        LC_ASSERT(marks[index] != 0);

        packets[index] = (unsigned char*) entry->packet;
        marks[index] = 0;

        //Set padding to zero
        if (entry->length < receiveSize) {
            memset(&packets[index][entry->length], 0, receiveSize - entry->length);
        }

        entry = entry->next;
    }

    unsigned int i;
    for (i = 0; i < totalPackets; i++) {
        if (marks[i]) {
            packets[i] = malloc(packetBufferSize);
            if (packets[i] == NULL) {
                ret = -4;
                goto cleanup_packets;
            }
        }
    }

    ret = reed_solomon_decode(rs, packets, marks, totalPackets, receiveSize);

    // We should always provide enough parity to recover the missing data successfully.
    // If this fails, something is probably wrong with our FEC state.
    LC_ASSERT(ret == 0);

    if (queue->bufferDataPackets != queue->receivedDataPackets) {
#ifdef FEC_VERBOSE
        Limelog("Recovered %d video data shards from frame %d\n",
                queue->bufferDataPackets - queue->receivedDataPackets,
                queue->currentFrameNumber);
#endif

        // Report the final FEC status if we needed to perform a recovery
        reportFinalFrameFecStatus(queue);
    }

cleanup_packets:
    for (i = 0; i < totalPackets; i++) {
        if (marks[i]) {
            // Only submit frame data, not FEC packets
            if (ret == 0 && i < queue->bufferDataPackets) {
                PRTPV_QUEUE_ENTRY queueEntry = (PRTPV_QUEUE_ENTRY)&packets[i][receiveSize];
                PRTP_PACKET rtpPacket = (PRTP_PACKET) packets[i];
                rtpPacket->sequenceNumber = U16(i + queue->bufferLowestSequenceNumber);
                rtpPacket->header = queue->pendingFecBlockList.head->packet->header;
                rtpPacket->timestamp = queue->pendingFecBlockList.head->packet->timestamp;
                rtpPacket->ssrc = queue->pendingFecBlockList.head->packet->ssrc;

                int dataOffset = sizeof(*rtpPacket);
                if (rtpPacket->header & FLAG_EXTENSION) {
                    dataOffset += 4; // 2 additional fields
                }

                PNV_VIDEO_PACKET nvPacket = (PNV_VIDEO_PACKET)(((char*)rtpPacket) + dataOffset);
                nvPacket->frameIndex = queue->currentFrameNumber;
                nvPacket->multiFecBlocks =
                        ((queue->multiFecLastBlockNumber << 2) | queue->multiFecCurrentBlockNumber) << 4;
                // TODO: nvPacket->multiFecFlags?

#ifdef FEC_VALIDATION_MODE
                if (i == dropIndex && droppedRtpPacket != NULL) {
                    // Check the packet contents if this was our known drop
                    PNV_VIDEO_PACKET droppedNvPacket = (PNV_VIDEO_PACKET)(((char*)droppedRtpPacket) + dataOffset);
                    int droppedDataLength = droppedRtpPacketLength - dataOffset - sizeof(*nvPacket);
                    int recoveredDataLength = StreamConfig.packetSize - sizeof(*nvPacket);
                    int j;
                    int recoveryErrors = 0;

                    LC_ASSERT_VT(droppedDataLength <= recoveredDataLength);
                    LC_ASSERT_VT(droppedDataLength == recoveredDataLength || (nvPacket->flags & FLAG_EOF));

                    // Check all NV_VIDEO_PACKET fields except FEC stuff which differs in the recovered packet
                    LC_ASSERT_VT(nvPacket->flags == droppedNvPacket->flags);
                    LC_ASSERT_VT(nvPacket->extraFlags == droppedNvPacket->extraFlags);
                    LC_ASSERT_VT(nvPacket->frameIndex == droppedNvPacket->frameIndex);
                    LC_ASSERT_VT(nvPacket->streamPacketIndex == droppedNvPacket->streamPacketIndex);
                    LC_ASSERT_VT(!queue->multiFecCapable || nvPacket->multiFecBlocks == droppedNvPacket->multiFecBlocks);

                    // Check the data itself - use memcmp() and only loop if an error is detected
                    if (memcmp(nvPacket + 1, droppedNvPacket + 1, droppedDataLength)) {
                        unsigned char* actualData = (unsigned char*)(nvPacket + 1);
                        unsigned char* expectedData = (unsigned char*)(droppedNvPacket + 1);
                        for (j = 0; j < droppedDataLength; j++) {
                            if (actualData[j] != expectedData[j]) {
                                Limelog("Recovery error at %d: expected 0x%02x, actual 0x%02x\n",
                                        j, expectedData[j], actualData[j]);
                                recoveryErrors++;
                            }
                        }
                    }

                    // If this packet is at the end of the frame, the remaining data should be zeros.
                    for (j = droppedDataLength; j < recoveredDataLength; j++) {
                        unsigned char* actualData = (unsigned char*)(nvPacket + 1);
                        if (actualData[j] != 0) {
                            Limelog("Recovery error at %d: expected 0x00, actual 0x%02x\n",
                                    j, actualData[j]);
                            recoveryErrors++;
                        }
                    }

                    LC_ASSERT_VT(recoveryErrors == 0);

                    // This drop was fake, so we don't want to actually submit it to the depacketizer.
                    // It will get confused because it's already seen this packet before.
                    free(packets[i]);
                    continue;
                }
#endif

                // Do some rudamentary checks to see that the recovered packet is sane.
                // In some cases (4K 30 FPS 80 Mbps), we seem to get some odd failures
                // here in rare cases where FEC recovery is required. I'm unsure if it
                // is our bug, NVIDIA's, or something else, but we don't want the corrupt
                // packet to by ingested by our depacketizer (or worse, the decoder).
                if (i == 0 && !(nvPacket->flags & FLAG_SOF)) {
                    PACKET_RECOVERY_FAILURE();
                }
                if (i == queue->bufferDataPackets - 1 && !(nvPacket->flags & FLAG_EOF)) {
                    PACKET_RECOVERY_FAILURE();
                }
                if (i > 0 && i < queue->bufferDataPackets - 1 && !(nvPacket->flags & FLAG_CONTAINS_PIC_DATA)) {
                    PACKET_RECOVERY_FAILURE();
                }
                if (nvPacket->flags & ~(FLAG_SOF | FLAG_EOF | FLAG_CONTAINS_PIC_DATA)) {
                    PACKET_RECOVERY_FAILURE();
                }

                // FEC recovered frames may have extra zero padding at the end. This is
                // fine per H.264 Annex B which states trailing zero bytes must be
                // discarded by decoders. It's not safe to strip all zero padding because
                // it may be a legitimate part of the H.264 bytestream.

                LC_ASSERT(isBefore16(rtpPacket->sequenceNumber, queue->bufferFirstParitySequenceNumber));
                queuePacket(queue, queueEntry, rtpPacket, StreamConfig.packetSize + dataOffset, false, true);
            } else if (packets[i] != NULL) {
                free(packets[i]);
            }
        }
    }

cleanup:
    reed_solomon_release(rs);

    if (packets != NULL)
        free(packets);

    if (marks != NULL)
        free(marks);

    return ret;
}

static void stageCompleteFecBlock(PRTP_VIDEO_QUEUE queue) {
    unsigned int nextSeqNum = queue->bufferLowestSequenceNumber;

    while (queue->pendingFecBlockList.count > 0) {
        PRTPV_QUEUE_ENTRY entry = queue->pendingFecBlockList.head;

        unsigned int lowestRtpSequenceNumber = entry->packet->sequenceNumber;

        do {
            // We should never encounter a packet that's lower than our next seq num
            LC_ASSERT(!isBefore16(entry->packet->sequenceNumber, nextSeqNum));

            // Never return parity packets
            if (entry->isParity) {
                PRTPV_QUEUE_ENTRY parityEntry = entry;

                // Skip this entry
                entry = parityEntry->next;

                // Remove this entry
                removeEntryFromList(&queue->pendingFecBlockList, parityEntry);

                // Free the entry and packet
                free(parityEntry->packet);

                continue;
            }

            // Check for the next packet in sequence. This will be O(1) for non-reordered packet streams.
            if (entry->packet->sequenceNumber == nextSeqNum) {
                removeEntryFromList(&queue->pendingFecBlockList, entry);

                // To avoid having to sample the system time for each packet, we cheat
                // and use the first packet's receive time for all packets. This ends up
                // actually being better for the measurements that the depacketizer does,
                // since it properly handles out of order packets.
                LC_ASSERT(queue->bufferFirstRecvTimeUs != 0);
                entry->receiveTimeUs = queue->bufferFirstRecvTimeUs;

                // Move this packet to the completed FEC block list
                insertEntryIntoList(&queue->completedFecBlockList, entry);
                break;
            }
            else if (isBefore16(entry->packet->sequenceNumber, lowestRtpSequenceNumber)) {
                lowestRtpSequenceNumber = entry->packet->sequenceNumber;
            }

            entry = entry->next;
        } while (entry != NULL);

        if (entry == NULL) {
            // Start at the lowest we found last enumeration
            nextSeqNum = lowestRtpSequenceNumber;
        }
        else {
            // We found this packet so move on to the next one in sequence
            nextSeqNum = U16(nextSeqNum + 1);
        }
    }
}

static void submitCompletedFrame(PRTP_VIDEO_QUEUE queue) {
    while (queue->completedFecBlockList.count > 0) {
        PRTPV_QUEUE_ENTRY entry = queue->completedFecBlockList.head;

        // Parity packets should have been removed by stageCompleteFecBlock()
        LC_ASSERT(!entry->isParity);

        // Submit this packet for decoding. It will own freeing the entry now.
        removeEntryFromList(&queue->completedFecBlockList, entry);
        queueRtpPacket(entry);
    }
}

uint32_t RtpvGetCurrentFrameNumber(PRTP_VIDEO_QUEUE queue) {
    return queue->currentFrameNumber;
}

// §K.17: Internal implementation of RtpvAddPacket, used for recursive processing
// of deferred packets during grace period resolution.
static int RtpvAddPacketInternal(PRTP_VIDEO_QUEUE queue, PRTP_PACKET packet, int length, PRTPV_QUEUE_ENTRY packetEntry);

// §VR-MULTILINK-HOLD ─────────────────────────────────────────────────────────
// 多連線時每個封包會從每條連線各來一份，但各條連線的延遲不一樣。原本的做法是「看到下一幀（或同一幀的下一個
// FEC block）的封包就放棄還沒收齊的這一個」：領先那條連線剛好掉了一段時，落後那條稍後送到的同一段全被當成過期，
// 兩條合起來明明收得齊，這一幀還是掉了。
// 這裡改成：超前的封包照到達順序排著，目前這個 block 收齊就依序放行；最前面那個等超過 mlHoldMaxUs（VideoStream
// 設成一個幀週期）或排滿了，才照原本的方式放棄。期限只在有封包到的時候檢查——影像是連續的，下一個封包
// 幾毫秒內就會來。多連線時取代 §K.17 的寬限期（那個只處理差一個 shard、最多 4 個封包）。
//
// 佇列「正在等」的永遠是 (currentFrameNumber, multiFecCurrentBlockNumber)：一幀收齊後 currentFrameNumber 已經加一，
// 一個 block 收齊後 block 號已經加一。排著的封包還沒進過 RtpvAddPacketInternal，NV 標頭仍是線上的位元組序。
// 讀出封包屬於哪一幀的哪個 FEC block（不改封包內容）；太短讀不出來回 false
static bool mlPeek(PRTP_VIDEO_QUEUE queue, PRTP_PACKET packet, int length, uint32_t* frame, uint8_t* block) {
    int dataOffset = sizeof(*packet);
    PNV_VIDEO_PACKET nv;

    if (packet->header & FLAG_EXTENSION) {
        dataOffset += 4;
    }
    if (length < dataOffset + (int)sizeof(NV_VIDEO_PACKET)) {
        return false;
    }
    nv = (PNV_VIDEO_PACKET)(((char*)packet) + dataOffset);
    *frame = LE32(nv->frameIndex);
    *block = queue->multiFecCapable ? (uint8_t)((nv->multiFecBlocks >> 4) & 0x3) : 0;
    return true;
}

static bool mlIsAhead(PRTP_VIDEO_QUEUE queue, PRTP_PACKET packet, int length) {
    uint32_t frame;
    uint8_t block;

    if (!mlPeek(queue, packet, length, &frame, &block)) {
        return false;  // 太短：交給內部函式拒收
    }
    if (queue->currentFrameNumber == 0) {
        return false;  // §FRZ-RESYNC-SENTINEL：重置後沒有基準幀號，下一個封包直接採納
    }
    if (frame != queue->currentFrameNumber) {
        return isBefore32(queue->currentFrameNumber, frame);
    }
    return block > queue->multiFecCurrentBlockNumber;
}

// §VR-LINK-REPAIR ────────────────────────────────────────────────────────────
// 佇列正在等的那個 FEC block 缺 shard、FEC 也補不回來時，回報給 server：「我在等哪個（幀, block）、手上有哪些 shard」。
// 補哪些、走哪條連線、補不補，全由 server 決定——它留著原封包，也看得到每條鏈路的狀況；頭盔這端只描述現況，
// 每一份回報都是完整的狀態，掉了或重複都無妨。補回來的封包和第一次送的位元組完全相同，走的就是平常的接收路徑。
//
// 什麼時候回報（第一份）——只有一條連線在送影像時：
//   - 這個 block 的最後一個 shard 到了卻還不夠（中間掉了一段）：立刻；
//   - 尾端掉了：這個 block 3 ms 沒有新封包（只在接收真的閒下來時才判定——手上還有封包在處理的時候，同一批裡排在
//     後面的可能就是這個 block 的）；
//   - 下一幀的封包都到了（跨幀等待開始）：立刻。
// 兩條以上在送時，缺的那一段多半會由另一條連線的那一份補上，太早回報只是讓 server 白送一輪（2026-10-07 S0：
// 落後 8 ms 的那條還沒送到就回報，補包全是多餘的，又被 ABR 算成丟包，位元率掉到三分之一）。所以看每條連線各自
// 送到哪裡了：
//   - 每條在送的連線都「走過」這個 block（送到了它的最後一個 shard，或更後面的封包）還是不夠：立刻——該來的
//     都來過了；
//   - 只有一部分走過：從第一條走過起算，等「還沒走過的那幾條平常比已經走過的晚多久，再多八分之一＋1 ms」
//     （VrMultiLink 逐條量的；至少 2 ms、最多一個幀週期）——那條要是斷了就不會再來，不能一直等它。還沒走過的若是
//     平常比較快的那條，它的那一份早該到了，只等下限。還沒走過的連線正在送這個 block（2 ms 內還有它的封包）時
//     不算逾時，等它送完：量到的時間差只看每一幀的第一個封包，速率低的那條頭差不多同時到、尾卻晚好幾毫秒；
//   - 都還沒走過，但這個 block 已經「3 ms＋其餘連線裡最慢那條的時間差」沒有新封包（送來最後一個封包的那條不算：
//     正在送這個 block 的若就是最慢的那條，其他連線的那一份早該到了）；
//   - 有後面的封包在排隊時，不管上面算出來多晚，最晚在「環頭的期限減一個重報間隔」回報——再晚補包就趕不上了
//     （整個 block 都沒到時只有這一條和「每條都走過」用得上）。代價：一幀有兩個以上的 block、或落後量接近幀週期時，
//     可能在落後那條送到之前多補一輪。
// 之後每隔 mlRepairRtoUs 還沒補齊就再報一次最新的狀態，最多 RTPV_NACK_MAX 次。
// 鏈路整個斷一小段時，空檔裡送出的回報多半落空——回報本身沒到，或 server 補的那一批掉在空檔裡。這幾次不該算：
// 鏈路一恢復就退還、馬上再報（每個 block 最多退 RTPV_NACK_REFUNDS 次）。否則三次都在空檔裡用完，恢復之後明明補得
// 回來卻沒得報（2026-10-07 S0：每 500 ms 斷 12 ms，30 秒掉 5 幀）。「落空」的判斷有兩種，見 nkNoteArrival。
#define RTPV_NACK_MAX        3
#define RTPV_NACK_REFUNDS    2
#define RTPV_NACK_IDLE_US    3000
#define RTPV_NACK_DUAL_US    2000   // 兩條以上在送時，等較慢那條的時間下限

// 回報的狀態跟著佇列等的對象走：對象換了（收齊或被放棄）就重來
static void nkSync(PRTP_VIDEO_QUEUE queue) {
    if (!queue->nkValid || queue->nkFrame != queue->currentFrameNumber ||
            queue->nkBlock != queue->multiFecCurrentBlockNumber) {
        queue->nkValid = true;
        queue->nkFrame = queue->currentFrameNumber;
        queue->nkBlock = queue->multiFecCurrentBlockNumber;
        queue->nkCount = 0;
        queue->nkGone = false;
        queue->nkTailSeen = false;
        queue->nkLastUs = 0;
        queue->nkTailUs = 0;
        queue->nkLastRxUs = 0;
        queue->nkFirstPassUs = 0;
        queue->nkRefunds = 0;
        queue->nkLastRxLink = -1;
        queue->nkFirstRxUs = 0;
    }
}

// 有影像封包到（哪一條連線、哪一個 block 都算；ahead＝它是後面的封包，不是正在等的這個 block 的）。
// 上一份回報落空了就退還那一次、馬上再報。兩種情況算落空：
//   - 回報送出之後一路沒有任何封包，而現在第一個到的是後面的封包（不是補包）：鏈路剛才是斷的，回報或它換來的補包
//     掉在空檔裡。不看靜默了多久——真實鏈路上空檔前送出的封包會晚幾毫秒才到，量到的靜默比空檔本身短（頭盔實測：
//     12 ms 的空檔，只看「靜默滿一個幀週期」約有 1.5% 沒退還，三次回報全在空檔裡用完）。
//   - 隔了一個幀週期以上才又有封包（不管是哪個 block 的），而上一份回報是在那之前不久、或空檔裡送的。
static void nkNoteArrival(PRTP_VIDEO_QUEUE queue, uint64_t nowUs, bool ahead) {
    const uint64_t prev = queue->nkAnyRxUs;
    bool lost;

    queue->nkAnyRxUs = nowUs;
    if (prev == 0 || !queue->nkValid || queue->nkFrame != queue->currentFrameNumber ||
            queue->nkBlock != queue->multiFecCurrentBlockNumber || queue->nkCount == 0 || queue->nkGone ||
            queue->nkRefunds >= RTPV_NACK_REFUNDS || queue->nkLastUs == 0) {
        return;
    }
    lost = ahead && queue->nkLastUs > prev;
    if (!lost && queue->mlHoldMaxUs != 0 && nowUs - prev >= queue->mlHoldMaxUs) {
        lost = queue->nkLastUs + queue->mlRepairRtoUs >= prev;
    }
    if (lost) {
        queue->nkCount--;
        queue->nkRefunds++;
        queue->nkLastUs = 0;  // 還有回報過（nkCount > 0）：下一次立刻到期；歸零了就照第一份回報的規則
    }
}

// 佇列剛收下一個封包（seq＝它的 RTP 序號，rxUs＝它到達的時刻）：是目前等的這個 block 的，就記下時刻；
// 這個 block 的最後一個 shard 到了也記。從等待環交出去的封包同樣要記（rxUs 是它當初到達的時刻）
static void nkNoteAccepted(PRTP_VIDEO_QUEUE queue, uint16_t seq, uint64_t rxUs, int link) {
    if (!queue->mlRepair || queue->pendingFecBlockList.count == 0) {
        return;  // count＝0：這個封包讓 block 收齊了，佇列已經改等下一個
    }
    if (U16(seq - queue->bufferLowestSequenceNumber) >
            U16(queue->bufferHighestSequenceNumber - queue->bufferLowestSequenceNumber)) {
        return;  // 不是這個 block 的
    }
    nkSync(queue);
    if (queue->nkFirstRxUs == 0 || rxUs < queue->nkFirstRxUs) {
        queue->nkFirstRxUs = rxUs;
    }
    if (rxUs > queue->nkLastRxUs) {
        queue->nkLastRxUs = rxUs;
        queue->nkLastRxLink = (link >= 0 && link < RTPV_ML_LINKS) ? (int8_t)link : -1;
    }
    if (seq == (uint16_t)queue->bufferHighestSequenceNumber && !queue->nkTailSeen) {
        queue->nkTailSeen = true;
        queue->nkTailUs = rxUs;
    }
}

// 記下這條連線送到哪裡了。每個進來的封包都記，不管它之後被採用、排隊還是被拒收
static void mlNoteLink(PRTP_VIDEO_QUEUE queue, PRTP_PACKET packet, int length, uint64_t nowUs) {
    const int l = queue->mlRxLink;
    uint32_t frame;
    uint8_t block;

    if (l < 0 || l >= RTPV_ML_LINKS || !mlPeek(queue, packet, length, &frame, &block)) {
        return;
    }
    if (!queue->mlLinkValid[l] || isBefore32(queue->mlLinkFrame[l], frame) ||
            (queue->mlLinkFrame[l] == frame && block > queue->mlLinkBlock[l])) {
        queue->mlLinkValid[l] = true;
        queue->mlLinkFrame[l] = frame;
        queue->mlLinkBlock[l] = block;
        queue->mlLinkSeq[l] = packet->sequenceNumber;
        queue->mlLinkRxUs[l] = nowUs;
    }
    else if (queue->mlLinkFrame[l] == frame && queue->mlLinkBlock[l] == block) {
        if (isBefore16(queue->mlLinkSeq[l], packet->sequenceNumber)) {
            queue->mlLinkSeq[l] = packet->sequenceNumber;
        }
        queue->mlLinkRxUs[l] = nowUs;
    }
}

// 哪幾條連線已經走過佇列正在等的這個 block：送到了它的最後一個 shard，或更後面的封包（bit i＝第 i 條）。
// 同一條連線上的封包照順序到，走過了還缺的，那條就不會再送來（補包除外）
static unsigned nkPassedMask(PRTP_VIDEO_QUEUE queue) {
    unsigned mask = 0;
    int l;

    if (queue->currentFrameNumber == 0) {
        return 0;  // §FRZ-RESYNC-SENTINEL：沒有基準幀號
    }
    for (l = 0; l < RTPV_ML_LINKS; l++) {
        bool passed;

        if (!queue->mlLinkValid[l]) {
            continue;
        }
        if (queue->mlLinkFrame[l] != queue->currentFrameNumber) {
            passed = isBefore32(queue->currentFrameNumber, queue->mlLinkFrame[l]);
        }
        else if (queue->mlLinkBlock[l] != queue->multiFecCurrentBlockNumber) {
            passed = queue->mlLinkBlock[l] > queue->multiFecCurrentBlockNumber;
        }
        else {
            // 還在同一個 block：要知道它的最後一個 shard 是幾號（至少收過這個 block 的一個封包）
            passed = queue->pendingFecBlockList.count != 0 &&
                     !isBefore16(queue->mlLinkSeq[l], (uint16_t)queue->bufferHighestSequenceNumber);
        }
        if (passed) {
            mask |= 1u << l;
        }
    }
    return mask;
}

// 每條在送影像的連線都走過了：除了補包，沒有別的可等
static bool nkAllPassed(PRTP_VIDEO_QUEUE queue) {
    const unsigned carry = queue->mlCarryMask;
    return carry != 0 && (nkPassedMask(queue) & carry) == carry;
}

// 兩條以上在送、還有連線沒走過這個 block：最多再等它多久。passed＝已經走過的（bit i＝第 i 條）。
// 等的是「還沒走過的那幾條平常比已經走過的晚多久」再多八分之一＋1 ms，至少 RTPV_NACK_DUAL_US、最多一個幀週期。
// 沒有任何一條走過時（passed＝0）就是在送的連線裡最慢那條的時間差。
static uint32_t nkLagWaitUs(PRTP_VIDEO_QUEUE queue, unsigned passed) {
    const unsigned carry = queue->mlCarryMask;
    uint32_t pendingMax = 0;
    uint32_t passedMin = UINT32_MAX;
    uint32_t lag;
    int l;

    for (l = 0; l < RTPV_ML_LINKS; l++) {
        if (!(carry & (1u << l))) {
            continue;
        }
        if (passed & (1u << l)) {
            if (queue->mlLinkLagUs[l] < passedMin) {
                passedMin = queue->mlLinkLagUs[l];
            }
        }
        else if (queue->mlLinkLagUs[l] > pendingMax) {
            pendingMax = queue->mlLinkLagUs[l];
        }
    }
    if (passedMin == UINT32_MAX) {
        lag = pendingMax;
    }
    else {
        lag = pendingMax > passedMin ? pendingMax - passedMin : 0;
    }
    lag += lag / 8 + 1000;
    if (lag < RTPV_NACK_DUAL_US) {
        lag = RTPV_NACK_DUAL_US;
    }
    if (queue->mlHoldMaxUs != 0 && lag > queue->mlHoldMaxUs) {
        lag = queue->mlHoldMaxUs;
    }
    return lag;
}

// 「這個 block 一段時間沒有新封包」這條規則要多等多久：其餘還沒走過的連線裡最慢那條的時間差（同樣再多八分之一＋1 ms、
// 夾在下限與一個幀週期之間）。送來最後一個封包的那條不算——等的是「別條連線的那一份」，它自己晚多久無關；
// 正在送這個 block 的若就是平常最慢的那條，其他連線的那一份早該到了，只等下限。
// 不用相減（nkLagWaitUs 的算法）：兩條輪流領先時兩條的估計值差不多，相減會變成 0。
static uint32_t nkIdleLagWaitUs(PRTP_VIDEO_QUEUE queue, unsigned passed) {
    const unsigned carry = queue->mlCarryMask;
    uint32_t lag = 0;
    int l;

    for (l = 0; l < RTPV_ML_LINKS; l++) {
        if ((carry & ~passed & (1u << l)) && l != queue->nkLastRxLink && queue->mlLinkLagUs[l] > lag) {
            lag = queue->mlLinkLagUs[l];
        }
    }
    lag += lag / 8 + 1000;
    if (lag < RTPV_NACK_DUAL_US) {
        lag = RTPV_NACK_DUAL_US;
    }
    if (queue->mlHoldMaxUs != 0 && lag > queue->mlHoldMaxUs) {
        lag = queue->mlHoldMaxUs;
    }
    return lag;
}

// 有超前的封包在排隊（下一幀已經到了），而且環已經對目前的對象掃過。沒掃過的時候，排著的可能就是新對象自己的
// 封包——拿它當「下一幀到了」會對一個其實快收齊的 block 送出「什麼都沒收到」的回報
static bool nkAheadWaiting(PRTP_VIDEO_QUEUE queue) {
    return queue->mlHoldCount > 0 && queue->mlSweptValid && queue->mlSweptFrame == queue->currentFrameNumber &&
           queue->mlSweptBlock == queue->multiFecCurrentBlockNumber;
}

// 環頭那個封包最晚等到什麼時候（不看補包的狀況）：到達後一個幀週期。
// 可以補包時，等待期間前面的 block 收齊過（補包救回來的，或另一條連線補上的）就從那一刻起再給一個幀週期，但從到達
// 起算不超過兩個幀週期：鏈路斷一小段、連續兩幀都沒到時，第一幀救回來已經用掉大半的時間，照原本的期限第二幀一定
// 趕不上（實機：每 0.5 s 斷 12 ms，等待最長 9.0 ms、上限 11.1 ms，只差一點）。沒有進展時期限不變。
static uint64_t mlHeadLimitUs(PRTP_VIDEO_QUEUE queue, const RTPV_HELD_PACKET* h) {
    uint64_t limit = h->arrivalUs + queue->mlHoldMaxUs;

    if (queue->mlRepair && queue->mlProgressUs > h->arrivalUs) {
        const uint64_t ext = queue->mlProgressUs + queue->mlHoldMaxUs;
        const uint64_t cap = h->arrivalUs + 2ull * queue->mlHoldMaxUs;
        limit = ext < cap ? ext : cap;
    }
    return limit;
}

// 下一次該回報的時刻（0＝不會再回報）。idleOk＝「一段時間沒有新封包」的規則算不算數：手上還有封包在處理時不算
// （同一批暫存裡排在後面的可能就是這個 block 的；從環裡交出去的封包記的又是它當初到達的時刻），要等接收真的
// 閒下來（RtpvPollHold）才判定。RtpvHoldTimeoutMs 算下一次醒來的時間時要算進去。
static uint64_t nkNextDueUs(PRTP_VIDEO_QUEUE queue, uint64_t nowUs, bool idleOk) {
    bool ahead;

    if (!queue->mlRepair) {
        return 0;
    }
    nkSync(queue);
    if (queue->nkGone || queue->nkCount >= RTPV_NACK_MAX) {
        return 0;
    }
    ahead = nkAheadWaiting(queue);
    if (queue->pendingFecBlockList.count == 0 && !ahead) {
        return 0;  // 一個封包都還沒到，也沒有後面的封包：這個 block 可能根本還沒送
    }
    if (queue->nkCount != 0) {
        return queue->nkLastUs + queue->mlRepairRtoUs;
    }
    if (queue->mlHoldDual) {
        // 兩條以上在送：看每條連線各自送到哪裡了（見上面的說明）
        const unsigned carry = queue->mlCarryMask;
        const unsigned passed = nkPassedMask(queue) & (carry != 0 ? carry : ~0u);
        uint64_t due = 0;
        int l;

        if (carry != 0 && passed == carry) {
            return nowUs;
        }
        // 有超前的封包在排隊＝送它來的那條已經走過了（它若是從原本的 socket 來的，上面的遮罩看不到）
        if ((passed != 0 || ahead) && queue->nkFirstPassUs == 0) {
            queue->nkFirstPassUs = nowUs;
        }
        if (queue->nkFirstPassUs != 0) {
            due = queue->nkFirstPassUs + nkLagWaitUs(queue, passed);
        }
        if (idleOk && queue->nkLastRxUs != 0) {
            const uint64_t idle = queue->nkLastRxUs + RTPV_NACK_IDLE_US + nkIdleLagWaitUs(queue, passed);
            if (due == 0 || idle < due) {
                due = idle;
            }
        }
        if (ahead && queue->mlHoldMaxUs != 0) {
            // 有後面的封包在排隊：環頭最晚等到 mlHeadLimitUs()。回報再晚於「期限減一個重報間隔」，補包就趕不上了
            // ——整個 block 都沒到時（沒有尾端、也沒有「沒有新封包」可判斷），上面的時間差是從環頭到達才起算的。
            // 這是刻意的取捨（寧可多補一輪，不賭落後那條）：環頭是下一幀、落後量明顯小於幀週期時，落後那條的那一份在
            // 這個時刻之前就該開始到了，不會多報；環頭是同一幀的下一個 block，或落後量接近／超過幀週期時，這個時刻會
            // 早於落後那條開始送這個 block——它之後照常送到的話，這一輪補包就是多的（server 記成補包量、延後 FEC 回降，
            // 不算丟包）。不能改成等到「落後那條該到的時刻」才報：那時離期限已經不到一個重報間隔，它沒來（斷了但還在
            // carryMask 的遲滯期、兩條都掉了這個 block，或落後量比等待上限還長）這一幀就救不回來——實測那樣改會多掉幀。
            // ahead 為真時 nkFirstPassUs 一定設過。
            const uint64_t limit = mlHeadLimitUs(queue, &queue->mlHold[queue->mlHoldHead]);
            uint64_t latest = limit > queue->mlRepairRtoUs ? limit - queue->mlRepairRtoUs : 0;
            if (latest < queue->nkFirstPassUs + RTPV_NACK_DUAL_US) {
                latest = queue->nkFirstPassUs + RTPV_NACK_DUAL_US;
            }
            if (due == 0 || due > latest) {
                due = latest;
            }
        }
        if (due != 0) {
            // 還沒走過的連線正在送這個 block：不算逾時，等到它最後一個封包之後 RTPV_NACK_DUAL_US
            for (l = 0; l < RTPV_ML_LINKS; l++) {
                if ((carry & ~passed & (1u << l)) && queue->mlLinkValid[l] &&
                        queue->mlLinkFrame[l] == queue->currentFrameNumber &&
                        queue->mlLinkBlock[l] == queue->multiFecCurrentBlockNumber &&
                        queue->mlLinkRxUs[l] + RTPV_NACK_DUAL_US > due) {
                    due = queue->mlLinkRxUs[l] + RTPV_NACK_DUAL_US;
                }
            }
            // 延長有上限：從第一條走過起算一個幀週期（再晚回報，補包也趕不上等待的期限）
            if (queue->nkFirstPassUs != 0 && queue->mlHoldMaxUs != 0 && due > queue->nkFirstPassUs + queue->mlHoldMaxUs) {
                due = queue->nkFirstPassUs + queue->mlHoldMaxUs;
            }
        }
        return due;
    }
    if (ahead) {
        return nowUs;
    }
    if (queue->nkTailSeen) {
        return queue->nkTailUs;
    }
    return (idleOk && queue->nkLastRxUs != 0) ? queue->nkLastRxUs + RTPV_NACK_IDLE_US : 0;
}

static void nkSend(PRTP_VIDEO_QUEUE queue, uint64_t nowUs) {
    uint8_t tlv[2 + sizeof(VIPLE_VR_TLV_NACK) + 32];
    VIPLE_VR_TLV_NACK n;
    int bytes = 0;

    memset(&n, 0, sizeof(n));
    memset(tlv, 0, sizeof(tlv));
    n.frame = queue->currentFrameNumber;
    n.block = (uint8_t)queue->multiFecCurrentBlockNumber;
    n.attempt = (uint8_t)(queue->nkCount + 1);
    if (queue->pendingFecBlockList.count != 0) {
        uint32_t total = queue->bufferDataPackets + queue->bufferParityPackets;
        uint32_t recv = queue->receivedDataPackets + queue->receivedParityPackets;
        PRTPV_QUEUE_ENTRY e;

        if (total == 0 || total > 255) {
            // 不做 FEC 的超大幀（一個 block 可以到上千個 shard）：位元圖描述不了，不回報、也不等補包。
            // nkCount 不動：沒回報過就是沒回報過，之後收齊了不能算成「補包救回來的」
            queue->nkGone = true;
            return;
        }
        n.total = (uint8_t)total;
        n.need = (uint8_t)(queue->bufferDataPackets > recv ? queue->bufferDataPackets - recv : 0);
        for (e = queue->pendingFecBlockList.head; e != NULL; e = e->next) {
            uint32_t idx = U16(e->packet->sequenceNumber - queue->bufferLowestSequenceNumber);
            if (idx < total) {
                tlv[2 + sizeof(n) + (idx >> 3)] |= (uint8_t)(1u << (idx & 7));
            }
        }
        bytes = (int)((total + 7) / 8);
    }
    // 沒收到任何封包時 total＝0、不帶位元圖：server 用它自己的紀錄
    tlv[0] = VIPLE_VR_C2S_NACK;
    tlv[1] = (uint8_t)(sizeof(n) + bytes);
    memcpy(&tlv[2], &n, sizeof(n));

    queue->nkCount++;
    queue->nkLastUs = nowUs;
    if (queue->mlStatNack++ == 0 && queue->mlStatWaits == 0) {
        queue->mlStatLogUs = nowUs;
    }
    if (vrmlSendCtrl(tlv, 2 + (int)sizeof(n) + bytes) <= 0) {
        queue->nkGone = true;  // 沒有任何一條連線送得出去：等也沒有用
    }
    // 每場前 12 筆照印，之後每 50 筆一筆
    if (queue->nkLogCount++ < 12 || (queue->nkLogCount % 50) == 0) {
        const char* why;
        if (n.attempt > 1) {
            why = " (retry)";
        }
        else if (queue->mlHoldDual) {
            why = nkAllPassed(queue) ? " (every link passed)" : " (slower link overdue)";
        }
        else {
            why = queue->mlHoldCount > 0 ? " (next frame waiting)" : (queue->nkTailSeen ? " (tail seen)" : " (idle)");
        }
        Limelog("[VIPLE-VR-REPAIR] nack frame=%u block=%u have=%u/%u (data %u) attempt=%u%s%s\n",
                n.frame, (unsigned)n.block,
                queue->pendingFecBlockList.count != 0 ? queue->receivedDataPackets + queue->receivedParityPackets : 0,
                (unsigned)n.total, queue->pendingFecBlockList.count != 0 ? queue->bufferDataPackets : 0, (unsigned)n.attempt,
                why, queue->nkGone ? " SEND FAILED" : "");
    }
}

// 等不到補包、要放棄目前這個 block 了：留一筆 log（有自己的節流）。「first packet … ms ago」是這個 block 第一個被
// 採用的封包到達的時刻（在等待環裡排過的算它當初到的時候）——不能用 bufferFirstRecvTimeUs，那是佇列開這個 block
// 的時刻，排過隊的封包會被記成掃環的時候，數字固定偏短
static void nkLogGiveUp(PRTP_VIDEO_QUEUE queue, uint64_t nowUs, const char* why) {
    // 每場前 30 筆照印，之後每 20 筆一筆
    if (queue->mlRepair && queue->nkValid && queue->nkCount + queue->nkRefunds > 0 &&
            queue->nkFrame == queue->currentFrameNumber && queue->nkBlock == queue->multiFecCurrentBlockNumber &&
            (queue->nkGiveUpLogCount++ < 30 || (queue->nkGiveUpLogCount % 20) == 0)) {
        const bool any = queue->pendingFecBlockList.count != 0;
        Limelog("[VIPLE-VR-REPAIR] gave up frame=%u block=%u (%s): nacks=%u refunds=%u, %.1f ms since the last one (rto %.1f ms), "
                "have=%u/%u, first packet %.1f ms ago, gone=%d\n",
                queue->nkFrame, (unsigned)queue->nkBlock, why, (unsigned)queue->nkCount, (unsigned)queue->nkRefunds,
                queue->nkLastUs != 0 ? (nowUs - queue->nkLastUs) / 1000.0 : -1.0, queue->mlRepairRtoUs / 1000.0,
                any ? queue->receivedDataPackets + queue->receivedParityPackets : 0, any ? queue->bufferDataPackets : 0,
                queue->nkFirstRxUs != 0 && nowUs >= queue->nkFirstRxUs ? (nowUs - queue->nkFirstRxUs) / 1000.0 : -1.0,
                queue->nkGone ? 1 : 0);
    }
}

// 該回報了就回報
static void nkEvaluate(PRTP_VIDEO_QUEUE queue, uint64_t nowUs) {
    uint64_t due = nkNextDueUs(queue, nowUs, queue->nkPolling);
    if (due != 0 && nowUs >= due) {
        nkSend(queue, nowUs);
    }
}

// 把一個排過隊的封包交給佇列：被採用才算這條連線的「採用數」，被拒收（另一條先到了、或已經過期）就釋放
static void mlSubmitHeld(PRTP_VIDEO_QUEUE queue, const RTPV_HELD_PACKET* h) {
    const uint16_t seq = h->packet->sequenceNumber;  // 內部函式可能把封包交出去，之後不能再碰它

    if (RtpvAddPacketInternal(queue, h->packet, h->length, h->entry) == RTPF_RET_QUEUED) {
        vrmlNoteVideoUsed(h->link);
        // §VR-LINK-REPAIR：從環裡交出去的也是「這個 block 收到的封包」。不記的話，掃完之後還缺 shard 的新對象
        // 沒有任何回報的計時，要等再下一幀的封包到了才會報
        nkNoteAccepted(queue, seq, h->arrivalUs, h->link);
    }
    else {
        free(h->packet);
    }
}

// 等不到了（到期或排滿）：放行環裡「(幀, block) 最小」那個對象最早到的封包——不一定是最前面那一格。
// 最前面的可能是領先那條送來的更後面的幀；直接放行它，中間那一幀（落後那條已經送到、排在後面）就整個被跳過。
// 放行之後佇列改等這個最小的對象，其餘同對象的封包由 mlSweep 接著交出去。
static void mlSubmitLowest(PRTP_VIDEO_QUEUE queue) {
    const int n = queue->mlHoldCount;
    RTPV_HELD_PACKET h;
    uint32_t bestFrame = 0;
    uint8_t bestBlock = 0;
    int best = 0;
    int i;

    if (mlPeek(queue, queue->mlHold[queue->mlHoldHead].packet, queue->mlHold[queue->mlHoldHead].length, &bestFrame, &bestBlock)) {
        for (i = 1; i < n; i++) {
            const RTPV_HELD_PACKET* e = &queue->mlHold[(queue->mlHoldHead + i) % RTPV_ML_HOLD_MAX];
            uint32_t frame;
            uint8_t block;

            if (!mlPeek(queue, e->packet, e->length, &frame, &block)) {
                best = i;  // 讀不出來的：交出去讓內部函式拒收
                break;
            }
            if (isBefore32(frame, bestFrame) || (frame == bestFrame && block < bestBlock)) {
                best = i;
                bestFrame = frame;
                bestBlock = block;
            }
        }
    }

    h = queue->mlHold[(queue->mlHoldHead + best) % RTPV_ML_HOLD_MAX];
    for (i = best; i > 0; i--) {
        queue->mlHold[(queue->mlHoldHead + i) % RTPV_ML_HOLD_MAX] = queue->mlHold[(queue->mlHoldHead + i - 1) % RTPV_ML_HOLD_MAX];
    }
    queue->mlHoldHead = (queue->mlHoldHead + 1) % RTPV_ML_HOLD_MAX;
    queue->mlHoldCount--;
    mlSubmitHeld(queue, &h);
}

// 佇列等的對象換了（上一個 block 收齊或被放棄）：環裡排在後面的封包可能就有屬於新對象的——例如上一幀等到逾時，
// 這一幀有兩個 FEC block，領先那條的 block 1 排在前面、落後那條送來的 block 0 排在後面。只看最前面那個的話，
// 要等它到期才輪到後面的，而它一放行佇列就把 block 0 放棄了。所以對象一換就把整個環掃一遍，把不再超前的
// 照到達順序交出去，只留下仍然超前的；掃的過程中對象又前進就再掃一遍。
static void mlSweep(PRTP_VIDEO_QUEUE queue) {
    bool again;

    do {
        const uint32_t frame = queue->currentFrameNumber;
        const uint8_t block = queue->multiFecCurrentBlockNumber;
        const int n = queue->mlHoldCount;
        int kept = 0;
        int i;

        for (i = 0; i < n; i++) {
            RTPV_HELD_PACKET h = queue->mlHold[(queue->mlHoldHead + i) % RTPV_ML_HOLD_MAX];

            if (mlIsAhead(queue, h.packet, h.length)) {
                queue->mlHold[(queue->mlHoldHead + kept) % RTPV_ML_HOLD_MAX] = h;
                kept++;
            }
            else {
                mlSubmitHeld(queue, &h);
            }
        }
        queue->mlHoldCount = kept;
        again = queue->currentFrameNumber != frame || queue->multiFecCurrentBlockNumber != block;
    } while (again && queue->mlHoldCount > 0);

    queue->mlSweptValid = true;
    queue->mlSweptFrame = queue->currentFrameNumber;
    queue->mlSweptBlock = queue->multiFecCurrentBlockNumber;
}

// 最前面那個封包最晚等到什麼時候。上限是到達後 mlHoldMaxUs（一個幀週期；等待中有進展時見 mlHeadLimitUs）。
// 只有一條連線在送影像時，等的不是另一條連線的那一份、而是補包：server 說補不了，或該報的都報了也等過一輪，
// 就不必等到上限。兩條以上在送、可以補包時也一樣——只要每條都已經走過這個 block（另一條的那一份不會再來了）。
// 不能補包時維持原樣：等滿上限。（VideoStream 只在「兩條在送」或「可以補包」時才把 mlHoldMaxUs 設成非 0。）
static uint64_t mlHeadDeadlineUs(PRTP_VIDEO_QUEUE queue, const RTPV_HELD_PACKET* h) {
    uint64_t limit = mlHeadLimitUs(queue, h);

    if (!queue->mlHoldDual || (queue->mlRepair && nkAllPassed(queue))) {
        if (!queue->mlRepair) {
            return h->arrivalUs;  // 沒有東西可等
        }
        nkSync(queue);
        if (queue->nkGone) {
            // 兩條以上在送、之前至少補過一輪：「補不了」是從快的那條先到的，慢的那條上前一輪的補包可能還在路上
            // （只有一條時它們走同一條連線、一定排在 GONE 前面）。等到它該到的時候再放行
            // （退還過的次數也算報過；nkLastUs＝0 是剛退還、還沒再報）
            if (queue->mlHoldDual && queue->nkCount + queue->nkRefunds > 1 && queue->nkLastUs != 0) {
                const uint64_t t = queue->nkLastUs + nkLagWaitUs(queue, 0);
                return t < limit ? t : limit;
            }
            return h->arrivalUs;
        }
        if (queue->nkCount >= RTPV_NACK_MAX && queue->nkLastUs + queue->mlRepairRtoUs < limit) {
            limit = queue->nkLastUs + queue->mlRepairRtoUs;
        }
    }
    return limit;
}

// 把排著的封包能放行的都放行。mustPop＝排滿了，最前面那個不管等多久都要處理掉
static void mlDrain(PRTP_VIDEO_QUEUE queue, uint64_t nowUs, bool mustPop) {
    while (queue->mlHoldCount > 0) {
        RTPV_HELD_PACKET* h;

        // 對象換了：先結算上一次等待，再把環裡屬於新對象的挑出來（見 mlSweep）。最前面那個就算已經到期也要先掃——
        // 它一放行，新對象就被放棄了
        if (!queue->mlSweptValid || queue->mlSweptFrame != queue->currentFrameNumber ||
            queue->mlSweptBlock != queue->multiFecCurrentBlockNumber) {
            if (queue->mlWaiting) {
                // 等的那個 block 收齊了（逾時放棄的在下面已經把 mlWaiting 清掉）
                uint64_t waited = nowUs - queue->mlWaitStartUs;
                queue->mlWaiting = false;
                queue->mlProgressUs = nowUs;
                queue->mlStatRecovered++;
                if (waited > queue->mlStatMaxWaitUs) {
                    queue->mlStatMaxWaitUs = (uint32_t)(waited > UINT32_MAX ? UINT32_MAX : waited);
                }
            }
            mlSweep(queue);
            if (queue->mlHoldCount < RTPV_ML_HOLD_MAX) {
                mustPop = false;
            }
            continue;
        }

        // 掃過之後，環裡剩下的對目前的對象而言都是超前的
        h = &queue->mlHold[queue->mlHoldHead];
        if (!mustPop && nowUs < mlHeadDeadlineUs(queue, h)) {
            if (!queue->mlWaiting) {
                queue->mlWaiting = true;
                queue->mlWaitStartUs = h->arrivalUs;
                if (queue->mlStatWaits++ == 0 && queue->mlStatNack == 0) {
                    queue->mlStatLogUs = nowUs;
                }
            }
            // §VR-LINK-REPAIR：下一幀都到了、這個 block 還沒收齊——該回報了就現在報（環剛掃過）
            nkEvaluate(queue, nowUs);
            if (nowUs < mlHeadDeadlineUs(queue, h)) {
                return;
            }
            // 回報送不出去（沒有可用的連線）：沒有東西可等了，往下走放棄的路
        }
        // 等不到（或排滿了）：放行一個封包，內部函式會照原本的方式放棄目前的 block
        if (queue->mlWaiting) {
            queue->mlWaiting = false;
            queue->mlStatExpired++;
        }
        nkLogGiveUp(queue, nowUs, mustPop ? "hold full" : "wait expired");
        mustPop = false;
        mlSubmitLowest(queue);
    }
}

static void mlLogStats(PRTP_VIDEO_QUEUE queue, uint64_t nowUs) {
    if ((queue->mlStatWaits == 0 && queue->mlStatNack == 0) || nowUs - queue->mlStatLogUs < 10000000) {
        return;
    }
    Limelog("[VIPLE-VR-LINK] hold 10s: waits=%u recovered=%u expired=%u heldPkts=%u maxWaitMs=%.1f limitMs=%.1f"
            " | nack=%u repaired=%u gone=%u\n",
            queue->mlStatWaits, queue->mlStatRecovered, queue->mlStatExpired, queue->mlStatHeld,
            queue->mlStatMaxWaitUs / 1000.0, queue->mlHoldMaxUs / 1000.0,
            queue->mlStatNack, queue->mlStatRepaired, queue->mlStatGone);
    queue->mlStatWaits = queue->mlStatRecovered = queue->mlStatExpired = queue->mlStatHeld = queue->mlStatMaxWaitUs = 0;
    queue->mlStatNack = queue->mlStatRepaired = queue->mlStatGone = 0;
}

static int mlAddPacket(PRTP_VIDEO_QUEUE queue, PRTP_PACKET packet, int length, PRTPV_QUEUE_ENTRY packetEntry) {
    uint64_t nowUs;

    const bool ahead = mlIsAhead(queue, packet, length);

    if (queue->mlRepair) {
        const uint64_t t = PltGetMicroseconds();
        nkNoteArrival(queue, t, ahead);
        mlNoteLink(queue, packet, length, t);
    }
    if (!ahead) {
        // 正在等的那個 block 的封包（或過期、重複的）：照常處理，再看排著的能不能放行
        const uint32_t prevFrame = queue->currentFrameNumber;
        const uint8_t prevBlock = (uint8_t)queue->multiFecCurrentBlockNumber;
        const uint16_t seq = packet->sequenceNumber;  // 內部函式可能把封包交出去，之後不能再碰它
        const bool hadNack = queue->mlRepair && queue->nkValid && queue->nkCount > 0 &&
                             queue->nkFrame == prevFrame && queue->nkBlock == prevBlock;
        int ret = RtpvAddPacketInternal(queue, packet, length, packetEntry);

        if (queue->mlRepair) {
            // §VR-LINK-REPAIR
            nowUs = PltGetMicroseconds();
            if (queue->currentFrameNumber != prevFrame || queue->multiFecCurrentBlockNumber != prevBlock) {
                if (hadNack) {
                    queue->mlStatRepaired++;  // 回報之後收齊了（非超前的封包不會讓佇列放棄 block，對象前進＝收齊）
                }
            }
            if (ret == RTPF_RET_QUEUED) {
                nkNoteAccepted(queue, seq, nowUs, queue->mlRxLink);
            }
            // 先掃環、再評估回報。對象剛換的時候，環裡排的可能就是新對象自己的封包；順序反過來會在它們交出去之前
            // 就送出「什麼都沒收到」的回報，server 把整個 block 重送一次（還被 ABR 記成丟包）
            if (queue->mlHoldCount > 0) {
                mlDrain(queue, nowUs, false);
            }
            nkEvaluate(queue, nowUs);
            if ((queue->mlStatWaits | queue->mlStatNack) != 0 && (queue->mlHoldCount > 0 || (++queue->mlStatTick & 0xFF) == 0)) {
                mlLogStats(queue, nowUs);
            }
            return ret;
        }
        if (queue->mlHoldCount > 0) {
            nowUs = PltGetMicroseconds();
            mlDrain(queue, nowUs, false);
            mlLogStats(queue, nowUs);
        }
        else if (queue->mlStatWaits != 0 && (++queue->mlStatTick & 0xFF) == 0) {
            mlLogStats(queue, PltGetMicroseconds());
        }
        return ret;
    }

    if (queue->mlHold == NULL) {
        queue->mlHold = malloc(sizeof(RTPV_HELD_PACKET) * RTPV_ML_HOLD_MAX);
        if (queue->mlHold == NULL) {
            return RtpvAddPacketInternal(queue, packet, length, packetEntry);
        }
        queue->mlHoldHead = 0;
        queue->mlHoldCount = 0;
    }

    nowUs = PltGetMicroseconds();
    if (queue->mlHoldCount == RTPV_ML_HOLD_MAX) {
        mlDrain(queue, nowUs, true);
        if (!mlIsAhead(queue, packet, length)) {
            // 騰位置的時候對象前進了，這個封包已經不算超前：直接處理（排進去的話要等對象再換一次才輪得到它）
            int ret = RtpvAddPacketInternal(queue, packet, length, packetEntry);
            if (queue->mlHoldCount > 0) {
                mlDrain(queue, nowUs, false);
            }
            mlLogStats(queue, nowUs);
            return ret;
        }
    }
    {
        RTPV_HELD_PACKET* slot = &queue->mlHold[(queue->mlHoldHead + queue->mlHoldCount) % RTPV_ML_HOLD_MAX];
        slot->packet = packet;
        slot->entry = packetEntry;
        slot->length = length;
        slot->link = queue->mlRxLink;
        slot->arrivalUs = nowUs;
        queue->mlHoldCount++;
        queue->mlStatHeld++;
    }
    mlDrain(queue, nowUs, false);
    mlLogStats(queue, nowUs);
    return RTPF_RET_HELD;
}

// 下一個要處理的期限（等待到期、或該回報缺包了）還有幾毫秒；0＝沒有。每個期限到了之後 RtpvPollHold 一定會讓狀態
// 前進（放行、回報次數加一），不會一直回 1。
int RtpvHoldTimeoutMs(PRTP_VIDEO_QUEUE queue) {
    uint64_t nowUs = PltGetMicroseconds();
    uint64_t due = 0;

    if (queue->mlHoldCount > 0) {
        due = mlHeadDeadlineUs(queue, &queue->mlHold[queue->mlHoldHead]);
        if (due == 0) {
            due = 1;
        }
    }
    if (queue->mlRepair) {
        uint64_t nk = nkNextDueUs(queue, nowUs, true);
        if (nk != 0 && (due == 0 || nk < due)) {
            due = nk;
        }
    }
    if (due == 0) {
        return 0;
    }
    if (due <= nowUs) {
        return 1;
    }
    return (int)((due - nowUs + 999) / 1000);
}

void RtpvPollHold(PRTP_VIDEO_QUEUE queue) {
    if (queue->mlHoldCount > 0 || queue->mlRepair) {
        uint64_t nowUs = PltGetMicroseconds();
        queue->nkPolling = true;  // 接收逾時才會走到這裡：socket 與暫存都是空的
        if (queue->mlHoldCount > 0) {
            mlDrain(queue, nowUs, false);  // 先放行到期的（對象可能因此換掉），再看要不要回報
        }
        nkEvaluate(queue, nowUs);
        queue->nkPolling = false;
        mlLogStats(queue, nowUs);
    }
}

void RtpvRepairGone(PRTP_VIDEO_QUEUE queue, uint32_t frame, uint8_t block) {
    if (!queue->mlRepair) {
        return;
    }
    nkSync(queue);
    if (queue->nkFrame == frame && queue->nkBlock == block && !queue->nkGone) {
        queue->nkGone = true;
        queue->mlStatGone++;
        if (queue->mlHoldCount > 0) {
            mlDrain(queue, PltGetMicroseconds(), false);  // 沒有別的可等了（見 mlHeadDeadlineUs）：照原本的方式放棄
        }
    }
}

int RtpvAddPacket(PRTP_VIDEO_QUEUE queue, PRTP_PACKET packet, int length, PRTPV_QUEUE_ENTRY packetEntry) {
    // §VR-MULTILINK-HOLD：多連線走自己的等待邏輯（§K.17 還有延後中的封包時先讓它收尾）
    if (queue->deferredCount == 0) {
        if (queue->multiLink && queue->mlHoldMaxUs != 0) {
            return mlAddPacket(queue, packet, length, packetEntry);
        }
        if (queue->mlHoldCount > 0) {
            // 等待中途關掉（只剩一條連線在送影像）：排著的全部放行，順序和到期時一樣（先掃、再放行最小的對象）
            const uint32_t savedHoldUs = queue->mlHoldMaxUs;
            queue->mlHoldMaxUs = 0;
            mlDrain(queue, PltGetMicroseconds(), false);
            queue->mlHoldMaxUs = savedHoldUs;
        }
        queue->mlWaiting = false;
    }

    // §K.17: Handle deferred packets from grace period.
    // When we detect a new frame but the old frame is close to complete (missing ≤ 1
    // shard for FEC recovery), we defer up to RTPV_MAX_GRACE_PACKETS new-frame packets
    // to give the missing shard a chance to arrive. This is critical at 180fps where
    // BBR pacing spreads shards across the entire 5.55ms frame interval.
    if (queue->deferredCount > 0) {
        int dataOffset = sizeof(*packet);
        if (packet->header & FLAG_EXTENSION) {
            dataOffset += 4;
        }
        if (length < dataOffset + (int)sizeof(NV_VIDEO_PACKET)) {
            // Too short to peek — can't be a valid video packet.
            // Let RtpvAddPacketInternal handle rejection.
            return RtpvAddPacketInternal(queue, packet, length, packetEntry);
        }
        PNV_VIDEO_PACKET peekNv = (PNV_VIDEO_PACKET)(((char*)packet) + dataOffset);
        uint32_t peekFrame = LE32(peekNv->frameIndex);

        if (peekFrame == queue->currentFrameNumber) {
            // The missing shard arrived! Process it for the old frame first.
            int savedCount = queue->deferredCount;
            struct {
                PRTP_PACKET packet;
                PRTPV_QUEUE_ENTRY entry;
                int length;
            } saved[RTPV_MAX_GRACE_PACKETS];
            memcpy(saved, queue->deferredPackets, sizeof(saved[0]) * savedCount);
            queue->deferredCount = 0;

            Limelog("§K.17 grace: frame %u recovered — late shard arrived after %d deferred packet(s)\n",
                    queue->currentFrameNumber, savedCount);

            // Process the old frame's missing shard
            int ret = RtpvAddPacketInternal(queue, packet, length, packetEntry);

            // Now process all deferred packets (they belong to the next frame)
            for (int i = 0; i < savedCount; i++) {
                int dret = RtpvAddPacketInternal(queue, saved[i].packet, saved[i].length, saved[i].entry);
                if (dret != RTPF_RET_QUEUED) {
                    free(saved[i].packet);
                }
            }

            return ret;
        }
        else {
            // Not the missing shard. Could be more new-frame packets or even
            // a frame beyond the deferred one.
            if (peekFrame == queue->deferredFrameNumber &&
                queue->deferredCount < RTPV_MAX_GRACE_PACKETS) {
                // Same new frame, still within grace budget — defer this one too
                queue->deferredPackets[queue->deferredCount].packet = packet;
                queue->deferredPackets[queue->deferredCount].entry = packetEntry;
                queue->deferredPackets[queue->deferredCount].length = length;
                queue->deferredCount++;
                return RTPF_RET_QUEUED;
            }

            // Grace period exhausted (budget full or different frame entirely).
            // Give up on the old frame and process all deferred + this packet.
            int savedCount = queue->deferredCount;
            struct {
                PRTP_PACKET packet;
                PRTPV_QUEUE_ENTRY entry;
                int length;
            } saved[RTPV_MAX_GRACE_PACKETS];
            memcpy(saved, queue->deferredPackets, sizeof(saved[0]) * savedCount);
            queue->deferredCount = 0;

            Limelog("§K.17 grace: frame %u NOT recovered after %d deferred packet(s), giving up\n",
                    queue->currentFrameNumber, savedCount);

            // Process deferred packets first (triggers old frame discard + new frame init)
            for (int i = 0; i < savedCount; i++) {
                int dret = RtpvAddPacketInternal(queue, saved[i].packet, saved[i].length, saved[i].entry);
                if (dret != RTPF_RET_QUEUED) {
                    free(saved[i].packet);
                }
            }

            // Fall through to process this packet normally
        }
    }

    // §K.17: Check if we should INITIATE grace period for the current frame.
    // This must be done here (in the wrapper, before RtpvAddPacketInternal)
    // because the internal function does in-place LE32 conversion on packet
    // fields. Doing the grace check after conversion would cause double
    // conversion when the deferred packet is later re-processed.
    if (queue->deferredCount == 0 &&
        queue->pendingFecBlockList.count != 0) {
        int peekOffset = sizeof(*packet);
        if (packet->header & FLAG_EXTENSION) {
            peekOffset += 4;
        }
        if (length >= peekOffset + (int)sizeof(NV_VIDEO_PACKET)) {
            PNV_VIDEO_PACKET peekNv = (PNV_VIDEO_PACKET)(((char*)packet) + peekOffset);
            uint32_t peekFrame = LE32(peekNv->frameIndex);

            // §FRZ-B1: frameIndex 是 32-bit 單調遞增，必須用 isBefore32。
            // isBefore16 在差距 >32768 幀時會迴繞誤判（毒化事故根因）。
            if (peekFrame != queue->currentFrameNumber &&
                !isBefore32(peekFrame, queue->currentFrameNumber)) {
                // New frame arriving while old frame is still pending.
                // Check if old frame is close to FEC-recoverable (deficit ≤ 1).
                uint32_t totalReceived = queue->receivedDataPackets + queue->receivedParityPackets;
                uint32_t totalNeeded = queue->bufferDataPackets;
                if (totalReceived + 1 >= totalNeeded && totalReceived < totalNeeded) {
                    // Only 1 more packet (data or parity) needed for FEC recovery.
                    queue->deferredPackets[0].packet = packet;
                    queue->deferredPackets[0].entry = packetEntry;
                    queue->deferredPackets[0].length = length;
                    queue->deferredCount = 1;
                    queue->deferredFrameNumber = peekFrame;
                    return RTPF_RET_QUEUED;
                }
            }
        }
    }

    return RtpvAddPacketInternal(queue, packet, length, packetEntry);
}

static int RtpvAddPacketInternal(PRTP_VIDEO_QUEUE queue, PRTP_PACKET packet, int length, PRTPV_QUEUE_ENTRY packetEntry) {
    // §M01-B：上游在這裡就用 nextContiguousSequenceNumber 拒收「落後」封包，
    // 已移到下方幀號／FEC block 檢查之後，只套用在要併入進行中 block 的封包。

    // FLAG_EXTENSION is required for all supported versions of GFE.
    LC_ASSERT_VT(packet->header & FLAG_EXTENSION);

    int dataOffset = sizeof(*packet);
    if (packet->header & FLAG_EXTENSION) {
        dataOffset += 4; // 2 additional fields
    }

    if (length < dataOffset + (int)sizeof(NV_VIDEO_PACKET)) {
        // Reject packets that are too small to fit a NV_VIDEO_PACKET header
        return RTPF_RET_REJECTED;
    }

    PNV_VIDEO_PACKET nvPacket = (PNV_VIDEO_PACKET)(((char*)packet) + dataOffset);

    nvPacket->streamPacketIndex = LE32(nvPacket->streamPacketIndex);
    nvPacket->frameIndex = LE32(nvPacket->frameIndex);
    nvPacket->fecInfo = LE32(nvPacket->fecInfo);

    // For legacy servers, we'll fixup the reserved data so that it looks like
    // it's a single FEC frame from a multi-FEC capable server. This allows us
    // to make our parsing logic simpler.
    if (!queue->multiFecCapable) {
        nvPacket->multiFecFlags = 0x10;
        nvPacket->multiFecBlocks = 0x00;
    }

    // §M01-D：-101 診斷，記下佇列收到的第一個幀號（只記錄，不影響判斷；
    // 受封包到達順序影響，只當次要線索，主要判別看 far 組）
    if (!queue->diagFirstFrameSeen) {
        queue->diagFirstFrameSeen = true;
        queue->diagFirstFrameIndex = nvPacket->frameIndex;
    }

#ifndef LC_FUZZING
    // §FRZ-B1: 原本用 isBefore16 —— frameIndex 是 32-bit 單調遞增，差距
    // >32768 幀的舊 datagram（path failback 時 server 端佇列殘留排出）會被
    // 迴繞誤判成「未來幀」接受，倒帶 currentFrameNumber 後所有新鮮幀反被
    // 判過期 REJECTED → 解碼器斷糧、畫面永久凍結。isBefore32 根治。
    // §M01-D：這道防線不可放寬（放寬會重開 §FRZ-B1 凍結）；這裡只加計數。
    if (isBefore32(nvPacket->frameIndex, queue->currentFrameNumber)) {
        // §M01-D：-101 診斷計數（只記錄，拒收行為不變）。lag ≤ 門檻的多半是
        // 已完成幀的尾端 FEC parity 或亂序，屬正常，只累加總數；超過門檻的
        // 才進 far 組（見 RtpVideoQueue.h 的說明）。
        {
            // isBefore32 成立保證 lag 介於 1 到 2^31 之間
            uint32_t lag = U32(queue->currentFrameNumber - nvPacket->frameIndex);

            queue->diagStalePkts++;
            if (lag > RTPV_DIAG_FAR_STALE_LAG) {
                queue->diagFarStalePkts++;
                if (lag > queue->diagFarStaleMaxLag) {
                    queue->diagFarStaleMaxLag = lag;
                }
                if (queue->diagFarStaleFrames == 0) {
                    queue->diagFarStaleFrames = 1;
                    queue->diagFarStaleLastFrame = nvPacket->frameIndex;
                    queue->diagFarStaleMinFrame = nvPacket->frameIndex;
                    queue->diagFarStaleMaxFrame = nvPacket->frameIndex;
                }
                else if (nvPacket->frameIndex != queue->diagFarStaleLastFrame) {
                    queue->diagFarStaleFrames++;
                    queue->diagFarStaleLastFrame = nvPacket->frameIndex;
                    if (nvPacket->frameIndex < queue->diagFarStaleMinFrame) {
                        queue->diagFarStaleMinFrame = nvPacket->frameIndex;
                    }
                    if (nvPacket->frameIndex > queue->diagFarStaleMaxFrame) {
                        queue->diagFarStaleMaxFrame = nvPacket->frameIndex;
                    }
                }
            }
        }

        // Reject frames behind our current frame number
        return RTPF_RET_REJECTED;
    }
#endif

    uint32_t fecIndex = (nvPacket->fecInfo & 0x3FF000) >> 12;
    uint8_t fecCurrentBlockNumber = (nvPacket->multiFecBlocks >> 4) & 0x3;

    if (nvPacket->frameIndex == queue->currentFrameNumber && fecCurrentBlockNumber < queue->multiFecCurrentBlockNumber) {
        // Reject FEC blocks behind our current block number
        return RTPF_RET_REJECTED;
    }

    // §M01-B：序號「落後」檢查只保護要併入進行中 FEC block 的封包。
    // 會開新 block 的封包（新幀、新 block、佇列為空）在下方初始化區會用
    // 自己的序號重建 nextContiguousSequenceNumber，舊基準對它們沒有保護作用。
    // 基準失效時，舊寫法會把所有新鮮封包判成過期，一路卡到序號繞回。
    // 基準失效的情況有三種：watchdog reset 後的 memset 0、stale 或 reinject
    // 封包成了基準、斷線期間 server 序號前進 ≥ 32768。
    // 合法流量裡，新 block 的序號一定在前半窗，所以行為和上游一樣。
    if (queue->pendingFecBlockList.count != 0 &&
        nvPacket->frameIndex == queue->currentFrameNumber &&
        fecCurrentBlockNumber == queue->multiFecCurrentBlockNumber &&
        isBefore16(packet->sequenceNumber, queue->nextContiguousSequenceNumber)) {
        queue->seqRejectStreak++;
        if (queue->seqRejectStreak == 1024 || (queue->seqRejectStreak % 16384) == 0) {
            Limelog("[VIPLE-VIDEO] §M01-B: %u consecutive in-block packets behind nextContig=%u (seq=%u frame=%u)\n",
                    queue->seqRejectStreak, queue->nextContiguousSequenceNumber,
                    (unsigned)packet->sequenceNumber, nvPacket->frameIndex);
        }
        return RTPF_RET_REJECTED;
    }
    queue->seqRejectStreak = 0;

    // Reinitialize the queue if it's empty after a frame delivery or
    // if we can't finish a frame before receiving the next one.
    if (queue->pendingFecBlockList.count == 0 || queue->currentFrameNumber != nvPacket->frameIndex ||
            queue->multiFecCurrentBlockNumber != fecCurrentBlockNumber) {
        if (queue->pendingFecBlockList.count != 0) {
            // Report the final status of the FEC queue before dropping this frame
            reportFinalFrameFecStatus(queue);

            if (queue->multiFecLastBlockNumber != 0) {
                Limelog("Unrecoverable frame %d (block %d of %d): %d+%d=%d received < %d needed\n",
                        queue->currentFrameNumber, queue->multiFecCurrentBlockNumber+1,
                        queue->multiFecLastBlockNumber+1,
                        queue->receivedDataPackets,
                        queue->receivedParityPackets,
                        queue->pendingFecBlockList.count,
                        queue->bufferDataPackets);

                // If we just missed a block of this frame rather than the whole thing,
                // we must manually advance the queue to the next frame. Parsing this
                // frame further is not possible.
                if (queue->currentFrameNumber == nvPacket->frameIndex) {
                    // Discard any unsubmitted buffers from the previous frame
                    purgeListEntries(&queue->pendingFecBlockList);
                    purgeListEntries(&queue->completedFecBlockList);

                    // Notify the host of the loss of this frame
                    if (!queue->reportedLostFrame) {
                        notifyFrameLost(queue->currentFrameNumber, false);
                        queue->reportedLostFrame = true;
                    }

                    queue->currentFrameNumber++;
                    queue->multiFecCurrentBlockNumber = 0;
                    return RTPF_RET_REJECTED;
                }
            }
            else {
                Limelog("Unrecoverable frame %d: %d+%d=%d received < %d needed\n",
                        queue->currentFrameNumber, queue->receivedDataPackets,
                        queue->receivedParityPackets,
                        queue->pendingFecBlockList.count,
                        queue->bufferDataPackets);
            }
        }

        // We must either start on the current FEC block number for the current frame,
        // or block 0 of a new frame.
        uint8_t expectedFecBlockNumber = (queue->currentFrameNumber == nvPacket->frameIndex ? queue->multiFecCurrentBlockNumber : 0);
        if (fecCurrentBlockNumber != expectedFecBlockNumber) {
            // Report the final status of the FEC queue before dropping this frame
            reportFinalFrameFecStatus(queue);

            Limelog("Unrecoverable frame %d: lost FEC blocks %d to %d\n",
                    nvPacket->frameIndex,
                    expectedFecBlockNumber + 1,
                    fecCurrentBlockNumber);

            // Discard any unsubmitted buffers from the previous frame
            purgeListEntries(&queue->pendingFecBlockList);
            purgeListEntries(&queue->completedFecBlockList);

            // Notify the host of the loss of this frame
            if (!queue->reportedLostFrame) {
                notifyFrameLost(queue->currentFrameNumber, false);
                queue->reportedLostFrame = true;
            }

            // We dropped a block of this frame, so we must skip to the next one.
            queue->currentFrameNumber = nvPacket->frameIndex + 1;
            queue->multiFecCurrentBlockNumber = 0;
            return RTPF_RET_REJECTED;
        }

        // Discard any pending buffers from the previous FEC block
        purgeListEntries(&queue->pendingFecBlockList);

        // Discard any completed FEC blocks from the previous frame
        if (queue->currentFrameNumber != nvPacket->frameIndex) {
            purgeListEntries(&queue->completedFecBlockList);
        }

        // If the frame numbers are not contiguous, the network dropped an entire frame.
        // The check here looks weird, but that's because we increment the frame number
        // after successfully processing a frame.
        if (queue->currentFrameNumber != nvPacket->frameIndex) {
            LC_ASSERT_VT(queue->currentFrameNumber < nvPacket->frameIndex);

            // If the frame immediately preceding this one was lost, we may have already
            // reported it using our speculative RFI logic. Don't report it again.
            if (queue->currentFrameNumber + 1 != nvPacket->frameIndex || !queue->reportedLostFrame) {
                // NB: We only have to notify for the most recent lost frame, since
                // the depacketizer will report the RFI range starting at the last
                // frame it saw.
                notifyFrameLost(nvPacket->frameIndex - 1, false);
            }

            // §FRZ-GAP-FEC 2026-07-16：整幀消失也要向 server 回報 FEC status。
            // 舊行為只發 RFI——server 端 §Q-STALE 以整幀為單位丟棄時（窄路徑
            // 壅塞），client 收到的幀序跳號但手上沒有部分幀，07-15 事故 60s
            // 內 ~3000 幀消失只產生 16 份 FEC status，ABR 對 99% 丟失全盲、
            // ping-tick 誤判零丟包持續回升。這裡以「上一幀 shard 數 × 消失
            // 幀數」估算 missing 補發一份 synthetic status（0x5502 既有格式，
            // wire 不變；server 只讀 missingPacketsBeforeHighestReceived）。
            // 附帶效益：刷新 server 端 lastFecStatusTime → ping-tick 自動讓位。
            {
                uint32_t gap = nvPacket->frameIndex - queue->currentFrameNumber;
                if (queue->currentFrameNumber == 0) {
                    // §FRZ-RESYNC-SENTINEL 2026-07-20：watchdog 完整 reset
                    // 後的哨兵——無基準幀號，靜默採納本幀，不合成 gap 回報
                    //（07-20 事故：reset-to-1 讓每次 watchdog fire 都合成
                    // gap=4096 假回報，server AIMD 被釘死）。
                    gap = 0;
                }
                else if (queue->currentFrameNumber + 1 == nvPacket->frameIndex && queue->reportedLostFrame) {
                    gap = 0;  // 唯一消失的幀已由 speculative 路徑回報過
                }
                else if (queue->reportedLostFrame) {
                    gap--;    // 扣掉已回報的那一幀，其餘照算
                }
                if (gap > 4096) {
                    gap = 4096;  // frameIndex 毒化防護（§FRZ-B3 同類教訓）
                }
                if (gap > 0) {
                    // 此刻 bufferDataPackets/bufferParityPackets 仍是上一幀的
                    // 值（下方才重設為本幀），是消失幀發送量最貼近的估計。
                    uint32_t shardsPerFrame = (uint32_t)queue->bufferDataPackets + queue->bufferParityPackets;
                    uint64_t estMissing;
                    SS_FRAME_FEC_STATUS fecStatus;

                    if (shardsPerFrame == 0) {
                        shardsPerFrame = 10;  // 首幀前無歷史資料時的保守估值
                    }
                    estMissing = (uint64_t)gap * shardsPerFrame;

                    memset(&fecStatus, 0, sizeof(fecStatus));
                    fecStatus.frameIndex = BE32(nvPacket->frameIndex - 1);
                    fecStatus.missingPacketsBeforeHighestReceived =
                        BE16((uint16_t)(estMissing > UINT16_MAX ? UINT16_MAX : estMissing));
                    fecStatus.totalDataPackets = BE16(queue->bufferDataPackets);
                    fecStatus.totalParityPackets = BE16(queue->bufferParityPackets);
                    fecStatus.fecPercentage = (uint8_t)queue->fecPercentage;
                    fecStatus.multiFecBlockCount = 1;
                    connectionSendFrameFecStatus(&fecStatus);
                }
            }
        }

        queue->currentFrameNumber = nvPacket->frameIndex;

        // Tell the control stream logic about this frame, even if we don't end up
        // being able to reconstruct a full frame from it.
        connectionSawFrame(queue->currentFrameNumber);

        queue->bufferFirstRecvTimeUs = PltGetMicroseconds();

        // §M01-B：這裡用本封包的序號重建基準。舊基準若在本封包「前半窗之外」，
        // 代表基準已失效（見上方 §M01-B 序號檢查的說明）；只印 log，不改行為。
        if (queue->seqBaselinePending) {
            queue->seqBaselinePending = false;
            Limelog("[VIPLE-VIDEO] §M01-B: RTP seq baseline re-anchored after queue reset (seq=%u lowest=%u frame=%u)\n",
                    (unsigned)packet->sequenceNumber, (unsigned)U16(packet->sequenceNumber - fecIndex),
                    nvPacket->frameIndex);
        }
        else if (isBefore16(packet->sequenceNumber, queue->nextContiguousSequenceNumber)) {
            // 舊寫法在函式開頭就會拒收這個封包，一直卡到序號繞回
            if (queue->seqJumpLogCount++ < 10 || (queue->seqJumpLogCount % 100) == 0) {
                Limelog("[VIPLE-VIDEO] §M01-B: seq jumped half-window (seq=%u nextContig=%u frame=%u) — re-anchored\n",
                        (unsigned)packet->sequenceNumber, queue->nextContiguousSequenceNumber,
                        nvPacket->frameIndex);
            }
        }

        queue->bufferLowestSequenceNumber = U16(packet->sequenceNumber - fecIndex);
        queue->nextContiguousSequenceNumber = queue->bufferLowestSequenceNumber;
        queue->receivedDataPackets = 0;
        queue->receivedParityPackets = 0;
        queue->receivedHighestSequenceNumber = 0;
        queue->missingPackets = 0;
        queue->useFastQueuePath = true;
        queue->reportedLostFrame = false;
        queue->bufferDataPackets = (nvPacket->fecInfo & 0xFFC00000) >> 22;
        queue->fecPercentage = (nvPacket->fecInfo & 0xFF0) >> 4;
        queue->bufferParityPackets = (queue->bufferDataPackets * queue->fecPercentage + 99) / 100;
        queue->bufferFirstParitySequenceNumber = U16(queue->bufferLowestSequenceNumber + queue->bufferDataPackets);
        queue->bufferHighestSequenceNumber = U16(queue->bufferFirstParitySequenceNumber + queue->bufferParityPackets - 1);
        queue->multiFecCurrentBlockNumber = fecCurrentBlockNumber;
        queue->multiFecLastBlockNumber = (nvPacket->multiFecBlocks >> 6) & 0x3;

        queue->stats.packetCountVideo += queue->bufferDataPackets;
        queue->stats.packetCountFec += queue->bufferParityPackets;
    }

    // Reject packets above our FEC queue valid sequence number range
    if (isBefore16(queue->bufferHighestSequenceNumber, packet->sequenceNumber)) {
        return RTPF_RET_REJECTED;
    }

    LC_ASSERT_VT(!queue->fecPercentage || U16(packet->sequenceNumber - fecIndex) == queue->bufferLowestSequenceNumber);
    LC_ASSERT_VT((nvPacket->fecInfo & 0xFF0) >> 4 == queue->fecPercentage);
    LC_ASSERT_VT((nvPacket->fecInfo & 0xFFC00000) >> 22 == queue->bufferDataPackets);

    // Verify that the legacy non-multi-FEC compatibility code works
    LC_ASSERT_VT(queue->multiFecCapable || fecCurrentBlockNumber == 0);
    LC_ASSERT_VT(queue->multiFecCapable || queue->multiFecLastBlockNumber == 0);

    // Multi-block FEC details must remain the same within a single frame
    LC_ASSERT_VT(fecCurrentBlockNumber == queue->multiFecCurrentBlockNumber);
    LC_ASSERT_VT(((nvPacket->multiFecBlocks >> 6) & 0x3) == queue->multiFecLastBlockNumber);

    LC_ASSERT_VT((nvPacket->flags & FLAG_EOF) || length - dataOffset == StreamConfig.packetSize);
    if (!queuePacket(queue, packetEntry, packet, length, !isBefore16(packet->sequenceNumber, queue->bufferFirstParitySequenceNumber), false)) {
        return RTPF_RET_REJECTED;
    }
    else {
        // Update total missing packet count
        if (queue->pendingFecBlockList.count == 1) {
            // Initialize counts and highest seqnum on the first packet
            LC_ASSERT(queue->missingPackets == 0);
            LC_ASSERT(queue->receivedHighestSequenceNumber == 0);
            queue->missingPackets += U16(packet->sequenceNumber - queue->bufferLowestSequenceNumber);
            queue->receivedHighestSequenceNumber = packet->sequenceNumber;
        }
        else if (isBefore16(queue->receivedHighestSequenceNumber, packet->sequenceNumber)) {
            // If we receive a packet above the highest sequence number,
            // adjust our missing packets count based on that new sequence number.
            queue->missingPackets += U16(packet->sequenceNumber - queue->receivedHighestSequenceNumber - 1);
            queue->receivedHighestSequenceNumber = packet->sequenceNumber;
        }
        else {
            // If we receive a packet behind the highest sequence number, but
            // queuePacket() accepted it, we must have received a missing packet.
            LC_ASSERT(queue->missingPackets > 0);
            queue->missingPackets--;
        }

        // We explicitly assert less-than because we know we received at least one packet (this one)
        LC_ASSERT(queue->missingPackets < queue->bufferDataPackets + queue->bufferParityPackets);

        if (isBefore16(packet->sequenceNumber, queue->bufferFirstParitySequenceNumber)) {
            queue->receivedDataPackets++;
            LC_ASSERT(queue->receivedDataPackets <= queue->bufferDataPackets);
        }
        else {
            queue->receivedParityPackets++;
            LC_ASSERT(queue->receivedParityPackets <= queue->bufferParityPackets);
        }

        // Try to submit this frame. If we haven't received enough packets,
        // this will fail and we'll keep waiting.
        if (reconstructFrame(queue) == 0) {
            // Stage the complete FEC block for use once reassembly is complete
            stageCompleteFecBlock(queue);

            // stageCompleteFecBlock() should have consumed all pending FEC data
            LC_ASSERT(queue->pendingFecBlockList.head == NULL);
            LC_ASSERT(queue->pendingFecBlockList.tail == NULL);
            LC_ASSERT(queue->pendingFecBlockList.count == 0);

            // If we're not yet at the last FEC block for this frame, move on to the next block.
            // Otherwise, the frame is complete and we can move on to the next frame.
            if (queue->multiFecCurrentBlockNumber < queue->multiFecLastBlockNumber) {
                // Move on to the next FEC block for this frame
                queue->multiFecCurrentBlockNumber++;
            }
            else {
                // Submit all FEC blocks to the depacketizer
                submitCompletedFrame(queue);

                // submitCompletedFrame() should have consumed all completed FEC data
                LC_ASSERT(queue->completedFecBlockList.head == NULL);
                LC_ASSERT(queue->completedFecBlockList.tail == NULL);
                LC_ASSERT(queue->completedFecBlockList.count == 0);

                // Continue to the next frame
                queue->currentFrameNumber++;
                queue->multiFecCurrentBlockNumber = 0;
            }
        }

        return RTPF_RET_QUEUED;
    }
}

