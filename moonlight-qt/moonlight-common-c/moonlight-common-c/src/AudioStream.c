#include "Limelight-internal.h"

#ifdef VIPLE_MPQUIC
#include "QuicTransport.h"
#endif

#include "VrMultiLink.h"

static SOCKET rtpSocket = INVALID_SOCKET;

static LINKED_BLOCKING_QUEUE packetQueue;
static RTP_AUDIO_QUEUE rtpAudioQueue;

static PLT_THREAD udpPingThread;
static PLT_THREAD receiveThread;
static PLT_THREAD decoderThread;

static PPLT_CRYPTO_CONTEXT audioDecryptionCtx;
static uint32_t avRiKeyId;

static unsigned short lastSeq;

static bool pingThreadStarted;
static bool receivedDataFromPeer;
static uint64_t firstReceiveTime;

#ifdef LC_DEBUG
#define INVALID_OPUS_HEADER 0x00
static uint8_t opusHeaderByte;
#endif

#define MAX_PACKET_SIZE 1400

#ifdef VIPLE_MPQUIC
// QUIC receive ring buffer for audio datagrams
#define QUIC_AUDIO_RING_SIZE 512

static struct {
    unsigned char data[QUIC_AUDIO_RING_SIZE][MAX_PACKET_SIZE];
    int           len[QUIC_AUDIO_RING_SIZE];
    volatile int  head;
    volatile int  tail;
} quicAudioRing;

static void quicAudioRecvCallback(unsigned char flowType,
                                   const unsigned char* data, int dataLen,
                                   void* context) {
    (void)context;
    if (flowType != QUIC_FLOW_AUDIO)
        return;
    if (dataLen > MAX_PACKET_SIZE)
        return;

    // §M01-A：SPSC ring 的發佈順序（屏障巨集與原理見 Limelight-internal.h）。
    // 舊版只靠 volatile head/tail，ARM64 上消費端可能看到前進後的 head，卻
    // 讀到同一個 slot 上一輪（512 個 datagram 前）殘留的 len。2026-09-23
    // 20:06:43 Pixel 5 的實例：標頭完全正確的資料封包（pt=97）配上前一輪
    // FEC 封包的長度 360（12+12+336）→ blockSize 348≠336 → debug 版在
    // RtpAudioQueue.c 的 block size 檢查 SIGABRT；release 版則是靜默停用
    // 整場音訊 FEC。
    int head = quicAudioRing.head;
    int next = (head + 1) % QUIC_AUDIO_RING_SIZE;
    if (next == quicAudioRing.tail)
        return;

    // §M01-A：確認 tail 已前進（消費端讀完這個 slot）之後才覆寫 slot
    QUIC_RING_ACQUIRE_FENCE();
    memcpy(quicAudioRing.data[head], data, dataLen);
    quicAudioRing.len[head] = dataLen;
    // §M01-A：data/len 必須先於 head 對消費端可見
    QUIC_RING_RELEASE_FENCE();
    quicAudioRing.head = next;
}

static int quicAudioRecv(char* buf, int bufLen) {
    int tail = quicAudioRing.tail;
    if (tail == quicAudioRing.head)
        return 0;

    // §M01-A：看到 head 前進之後才讀這個 slot 的 len/data
    QUIC_RING_ACQUIRE_FENCE();
    int len = quicAudioRing.len[tail];
    if (len > bufLen)
        len = bufLen;
    memcpy(buf, quicAudioRing.data[tail], len);
    // §M01-A：讀完 slot 才把它還給生產端
    QUIC_RING_RELEASE_FENCE();
    quicAudioRing.tail = (tail + 1) % QUIC_AUDIO_RING_SIZE;
    return len;
}

static bool useQuicAudio = false;
#endif

typedef struct _QUEUE_AUDIO_PACKET_HEADER {
    LINKED_BLOCKING_QUEUE_ENTRY lentry;
    int size;
} QUEUED_AUDIO_PACKET_HEADER, *PQUEUED_AUDIO_PACKET_HEADER;

typedef struct _QUEUED_AUDIO_PACKET {
    QUEUED_AUDIO_PACKET_HEADER header;
    char data[MAX_PACKET_SIZE];
} QUEUED_AUDIO_PACKET, *PQUEUED_AUDIO_PACKET;

static void AudioPingThreadProc(void* context) {
    char legacyPingData[] = { 0x50, 0x49, 0x4E, 0x47 };
    LC_SOCKADDR saddr;

    LC_ASSERT(AudioPortNumber != 0);

    memcpy(&saddr, &RemoteAddr, sizeof(saddr));
    SET_PORT(&saddr, AudioPortNumber);

    // We do not check for errors here. Socket errors will be handled
    // on the read-side in ReceiveThreadProc(). This avoids potential
    // issues related to receiving ICMP port unreachable messages due
    // to sending a packet prior to the host PC binding to that port.
    int pingCount = 0;
    while (!PltIsThreadInterrupted(&udpPingThread)) {
        if (AudioPingPayload.payload[0] != 0) {
            pingCount++;
            AudioPingPayload.sequenceNumber = BE32(pingCount);

            sendto(rtpSocket, (char*)&AudioPingPayload, sizeof(AudioPingPayload), 0, (struct sockaddr*)&saddr, AddrLen);
        }
        else {
            sendto(rtpSocket, legacyPingData, sizeof(legacyPingData), 0, (struct sockaddr*)&saddr, AddrLen);
        }

        PltSleepMsInterruptible(&udpPingThread, 500);
    }
}

// Initialize the audio stream and start
int initializeAudioStream(void) {
    LbqInitializeLinkedBlockingQueue(&packetQueue, 30);
    RtpaInitializeQueue(&rtpAudioQueue);
    lastSeq = 0;
    receivedDataFromPeer = false;
    pingThreadStarted = false;
    firstReceiveTime = 0;

#ifdef VIPLE_MPQUIC
    // §Q-AUDIO-RING: 防禦性清空 — 排除 process 存活跨 session 時
    // ring buffer 殘留前 session datagram 的所有可能性。
    // startAudioStream() 也有 reset，但 initializeAudioStream() 會先
    // 設定新的 avRiKeyId；若在 init→start 之間有遲到 callback 寫入，
    // start 的 reset 會丟掉它，但 defense-in-depth 在此也清一次更安全。
    if (quicAudioRing.head != quicAudioRing.tail) {
        Limelog("[VIPLE-MPQUIC] AudioStream init: flushing %d stale QUIC audio packets\n",
                (quicAudioRing.head - quicAudioRing.tail + QUIC_AUDIO_RING_SIZE) % QUIC_AUDIO_RING_SIZE);
    }
    quicAudioRing.head = 0;
    quicAudioRing.tail = 0;
    useQuicAudio = false;
#endif

    audioDecryptionCtx = PltCreateCryptoContext();
#ifdef LC_DEBUG
    opusHeaderByte = INVALID_OPUS_HEADER;
#endif

    // Copy and byte-swap the AV RI key ID used for the audio encryption IV
    memcpy(&avRiKeyId, StreamConfig.remoteInputAesIv, sizeof(avRiKeyId));
    avRiKeyId = BE32(avRiKeyId);

    return 0;
}

// This is called when the RTSP SETUP message is parsed and the audio port
// number is parsed out of it. Alternatively, it's also called if parsing fails
// and will use the well known audio port instead.
int notifyAudioPortNegotiationComplete(void) {
    LC_ASSERT(!pingThreadStarted);
    LC_ASSERT(AudioPortNumber != 0);

    // For GFE 3.22 compatibility, we must start the audio ping thread before the RTSP handshake.
    // It will not reply to our RTSP PLAY request until the audio ping has been received.
    rtpSocket = bindUdpSocket(RemoteAddr.ss_family, &LocalAddr, AddrLen, 0, SOCK_QOS_TYPE_AUDIO);
    if (rtpSocket == INVALID_SOCKET) {
        return LastSocketFail();
    }

    // We may receive audio before our threads are started, but that's okay. We'll
    // drop the first 1 second of audio packets to catch up with the backlog.
    int err = PltCreateThread("AudioPing", AudioPingThreadProc, NULL, &udpPingThread);
    if (err != 0) {
        return err;
    }

    pingThreadStarted = true;
    return 0;
}

static void freePacketList(PLINKED_BLOCKING_QUEUE_ENTRY entry) {
    PLINKED_BLOCKING_QUEUE_ENTRY nextEntry;

    while (entry != NULL) {
        nextEntry = entry->flink;

        // The entry is stored within the data allocation
        free(entry->data);

        entry = nextEntry;
    }
}

// Tear down the audio stream once we're done with it
void destroyAudioStream(void) {
    if (rtpSocket != INVALID_SOCKET) {
        if (pingThreadStarted) {
            PltInterruptThread(&udpPingThread);
            PltJoinThread(&udpPingThread);
        }

        closeSocket(rtpSocket);
        rtpSocket = INVALID_SOCKET;
    }

    PltDestroyCryptoContext(audioDecryptionCtx);
    freePacketList(LbqDestroyLinkedBlockingQueue(&packetQueue));
    RtpaCleanupQueue(&rtpAudioQueue);
}

static bool queuePacketToLbq(PQUEUED_AUDIO_PACKET* packet) {
    int err;

    do {
        err = LbqOfferQueueItem(&packetQueue, *packet, &(*packet)->header.lentry);
        if (err == LBQ_SUCCESS) {
            // The LBQ owns the buffer now
            *packet = NULL;
        }
        else if (err == LBQ_BOUND_EXCEEDED) {
            void* oldestPacket;

            Limelog("Audio packet queue overflow\n");

            // §AUD-OVFL-POP1 2026-07-16：舊行為是 LbqFlushQueueItems 整隊
            // 傾倒——30 包 = 150ms 音訊一次全丟（07-15 事故 259 次 overflow
            // ≈ 39 秒音訊被整段拋棄）。改為只彈出最舊 1 包再重試：上游
            // （jitter buffer flush / FEC 批次吐出）突發灌入時，每包只
            // 犧牲 5ms 最舊音訊，保留其餘連續性。
            if (LbqPollQueueElement(&packetQueue, &oldestPacket) == LBQ_SUCCESS) {
                free(oldestPacket);
            }
        }
    } while (err == LBQ_BOUND_EXCEEDED);

    return err == LBQ_SUCCESS;
}

#ifdef LC_DEBUG
// §M01-A：debug 版的 Opus TOC byte 一致性檢查。上游用 LC_ASSERT_VT，但檢查
// 的是 host 送來（或 FEC 重建）的資料，封包毀損、解密錯誤時 debug 版會直接
// abort；release 版根本不檢查。改成每秒最多一筆 log、不中止。
// 第一包的 TOC 是 0x00 時不當基準（等下一包再取），避免把毀損值鎖成基準。
// 注意：host 是 Sunshine 時後續比對一律略過（見下方），實質上只檢查第一包。
static void checkOpusHeaderByte(uint8_t tocByte, uint16_t sequenceNumber) {
    static uint64_t lastOpusHeaderLogMs;
    uint64_t nowMs;

    if (opusHeaderByte == INVALID_OPUS_HEADER) {
        if (tocByte != INVALID_OPUS_HEADER) {
            opusHeaderByte = tocByte;
            return;
        }

        nowMs = PltGetMillis();
        if (lastOpusHeaderLogMs == 0 || nowMs - lastOpusHeaderLogMs >= 1000) {
            Limelog("[VIPLE-AUDIO] §M01-A first Opus TOC byte is 0x00 (seq=%u), baseline deferred\n",
                    sequenceNumber);
            lastOpusHeaderLogMs = nowMs;
        }
        return;
    }

    // Opus header should stay constant for the entire stream.
    // If it doesn't, it may indicate that the RtpAudioQueue
    // incorrectly recovered a data shard or the decryption
    // of the audio packet failed. Sunshine violates this for
    // surround sound in some cases, so just ignore it.
    if (tocByte == opusHeaderByte || IS_SUNSHINE()) {
        return;
    }

    nowMs = PltGetMillis();
    if (lastOpusHeaderLogMs == 0 || nowMs - lastOpusHeaderLogMs >= 1000) {
        Limelog("[VIPLE-AUDIO] §M01-A unexpected Opus TOC byte 0x%02x (expected 0x%02x, seq=%u)\n",
                tocByte, opusHeaderByte, sequenceNumber);
        lastOpusHeaderLogMs = nowMs;
    }
}
#endif

static void decodeInputData(PQUEUED_AUDIO_PACKET packet) {
    // If the packet size is zero, this is a placeholder for a missing
    // packet. Trigger packet loss concealment logic in libopus by
    // invoking the decoder with a NULL buffer.
    if (packet->header.size == 0) {
        AudioCallbacks.decodeAndPlaySample(NULL, 0);
        return;
    }

    PRTP_PACKET rtp = (PRTP_PACKET)&packet->data[0];
    if (lastSeq != 0 && (unsigned short)(lastSeq + 1) != rtp->sequenceNumber) {
        Limelog("Network dropped audio data (expected %d, but received %d)\n", lastSeq + 1, rtp->sequenceNumber);
    }

    lastSeq = rtp->sequenceNumber;

    if (AudioEncryptionEnabled) {
        // We must have room for the AES padding which may be written to the buffer
        unsigned char decryptedOpusData[ROUND_TO_PKCS7_PADDED_LEN(MAX_PACKET_SIZE)];
        unsigned char iv[16] = { 0 };
        int dataLength = packet->header.size - sizeof(*rtp);

        LC_ASSERT(dataLength <= MAX_PACKET_SIZE);

        // The IV is the avkeyid (equivalent to the rikeyid) +
        // the RTP sequence number, in big endian.
        uint32_t ivSeq = BE32(avRiKeyId + rtp->sequenceNumber);

        memcpy(iv, &ivSeq, sizeof(ivSeq));

        if (!PltDecryptMessage(audioDecryptionCtx, ALGORITHM_AES_CBC, CIPHER_FLAG_RESET_IV | CIPHER_FLAG_FINISH,
                               (unsigned char*)StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                               iv, sizeof(iv),
                               NULL, 0,
                               (unsigned char*)(rtp + 1), dataLength,
                               decryptedOpusData, &dataLength)) {
            // §M01-A：解密失敗來自網路輸入（封包毀損、長度錯誤、跨 session
            // 殘留封包用的是舊金鑰）。release 版本來就只是丟掉這包；上游在
            // 這裡還有 LC_ASSERT_VT(false)，debug 版會直接 abort，拿掉。
            Limelog("Failed to decrypt audio packet (sequence number: %u)\n", rtp->sequenceNumber);
            return;
        }

#ifdef LC_DEBUG
        checkOpusHeaderByte(decryptedOpusData[0], rtp->sequenceNumber);
#endif

        AudioCallbacks.decodeAndPlaySample((char*)decryptedOpusData, dataLength);
    }
    else {
#ifdef LC_DEBUG
        checkOpusHeaderByte(((uint8_t*)(rtp + 1))[0], rtp->sequenceNumber);
#endif

        AudioCallbacks.decodeAndPlaySample((char*)(rtp + 1), packet->header.size - sizeof(*rtp));
    }
}

static void AudioReceiveThreadProc(void* context) {
    PRTP_PACKET rtp;
    PQUEUED_AUDIO_PACKET packet;
    int queueStatus;
    bool useSelect;
    uint32_t packetsToDrop;
    int waitingForAudioMs;

    // §VR-MULTILINK：0＝單一路徑，以下行為不變
    const int mlLinks = vrmlLinkCount();

    packet = NULL;
    packetsToDrop = 500 / AudioPacketDuration;

    if (setNonFatalRecvTimeoutMs(rtpSocket, UDP_RECV_POLL_TIMEOUT_MS) < 0) {
        // SO_RCVTIMEO failed, so use select() to wait
        useSelect = true;
    }
    else {
        // SO_RCVTIMEO timeout set for recv()
        useSelect = false;
    }

    waitingForAudioMs = 0;
    while (!PltIsThreadInterrupted(&receiveThread)) {
        if (packet == NULL) {
            packet = (PQUEUED_AUDIO_PACKET)malloc(sizeof(*packet));
            if (packet == NULL) {
                Limelog("Audio Receive: malloc() failed\n");
                ListenerCallbacks.connectionTerminated(-1);
                break;
            }
        }

#ifdef VIPLE_MPQUIC
        if (useQuicAudio) {
            packet->header.size = quicAudioRecv((char*)&packet->data[0], MAX_PACKET_SIZE);
            if (packet->header.size == 0) {
                PltSleepMs(1);
                continue;
            }
        }
        else
#endif
        if (mlLinks > 0) {
            // 同時等原本的 socket 與每條連線的音訊 socket；重複的封包由 RtpaAddPacket 丟掉
            packet->header.size = vrmlRecvAudio(rtpSocket, &packet->data[0], MAX_PACKET_SIZE);
        }
        else {
            packet->header.size = recvUdpSocket(rtpSocket, &packet->data[0], MAX_PACKET_SIZE, useSelect);
        }
        if (packet->header.size < 0) {
            Limelog("Audio Receive: recvUdpSocket() failed: %d\n", (int)LastSocketError());
            ListenerCallbacks.connectionTerminated(LastSocketFail());
            break;
        }
        else if (packet->header.size == 0) {
            // Receive timed out; try again

            if (!receivedDataFromPeer) {
                waitingForAudioMs += UDP_RECV_POLL_TIMEOUT_MS;
            }
            else {
                // If we hit this path, there are no queued audio packets on the host PC,
                // so we don't need to drop anything.
                packetsToDrop = 0;
            }
            continue;
        }

        if (packet->header.size < (int)sizeof(RTP_PACKET)) {
            // Runt packet
            continue;
        }

        rtp = (PRTP_PACKET)&packet->data[0];

        if (!receivedDataFromPeer) {
            receivedDataFromPeer = true;
            Limelog("Received first audio packet after %d ms\n", waitingForAudioMs);

            if (firstReceiveTime != 0) {
                // XXX firstReceiveTime is never set here...
                // We're already dropping 500ms of audio so this probably doesn't matter
                packetsToDrop += (uint32_t)(PltGetMillis() - firstReceiveTime) / AudioPacketDuration;
            }

            Limelog("Initial audio resync period: %d milliseconds\n", packetsToDrop * AudioPacketDuration);
        }

        // GFE accumulates audio samples before we are ready to receive them, so
        // we will drop the ones that arrived before the receive thread was ready.
        if (packetsToDrop > 0) {
            // Only count actual audio data (not FEC) in the packets to drop calculation
            if (rtp->packetType == 97) {
                packetsToDrop--;
            }
            continue;
        }

        // Convert fields to host byte-order
        rtp->sequenceNumber = BE16(rtp->sequenceNumber);
        rtp->timestamp = BE32(rtp->timestamp);
        rtp->ssrc = BE32(rtp->ssrc);

        queueStatus = RtpaAddPacket(&rtpAudioQueue, (PRTP_PACKET)&packet->data[0], (uint16_t)packet->header.size);
        if (RTPQ_HANDLE_NOW(queueStatus)) {
            if ((AudioCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
                if (!queuePacketToLbq(&packet)) {
                    // An exit signal was received
                    break;
                }
                else {
                    // Ownership should have been taken by the LBQ
                    LC_ASSERT(packet == NULL);
                }
            }
            else {
                decodeInputData(packet);
            }
        }
        else {
            if (RTPQ_PACKET_CONSUMED(queueStatus)) {
                // The queue consumed our packet, so we must allocate a new one
                packet = NULL;
            }

            if (RTPQ_PACKET_READY(queueStatus)) {
                // If packets are ready, pull them and send them to the decoder
                uint16_t length;
                PQUEUED_AUDIO_PACKET queuedPacket;
                while ((queuedPacket = (PQUEUED_AUDIO_PACKET)RtpaGetQueuedPacket(&rtpAudioQueue, sizeof(QUEUED_AUDIO_PACKET_HEADER), &length)) != NULL) {
                    // Populate header data (not preserved in queued packets)
                    queuedPacket->header.size = length;

                    if ((AudioCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
                        if (!queuePacketToLbq(&queuedPacket)) {
                            // An exit signal was received
                            free(queuedPacket);
                            break;
                        }
                        else {
                            // Ownership should have been taken by the LBQ
                            LC_ASSERT(queuedPacket == NULL);
                        }
                    }
                    else {
                        decodeInputData(queuedPacket);
                        free(queuedPacket);
                    }
                }

                // Break on exit
                if (queuedPacket != NULL) {
                    break;
                }
            }
        }
    }

    if (packet != NULL) {
        free(packet);
    }
}

static void AudioDecoderThreadProc(void* context) {
    int err;
    PQUEUED_AUDIO_PACKET packet;

    while (!PltIsThreadInterrupted(&decoderThread)) {
        err = LbqWaitForQueueElement(&packetQueue, (void**)&packet);
        if (err != LBQ_SUCCESS) {
            // An exit signal was received
            return;
        }

        decodeInputData(packet);

        free(packet);
    }
}

void stopAudioStream(void) {
    if (!receivedDataFromPeer) {
        Limelog("No audio traffic was ever received from the host!\n");
    }

    AudioCallbacks.stop();

    PltInterruptThread(&receiveThread);
    if ((AudioCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
        // Signal threads waiting on the LBQ
        LbqSignalQueueShutdown(&packetQueue);
        PltInterruptThread(&decoderThread);
    }

    PltJoinThread(&receiveThread);
    if ((AudioCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
        PltJoinThread(&decoderThread);
    }

#ifdef VIPLE_MPQUIC
    // §Q-AUDIO-RING: session 結束後立即取消 QUIC audio callback，
    // 防止 QUIC I/O 執行緒在 session 間隙繼續向 ring 寫入遲到封包。
    // 下一次 startAudioStream() 會重新註冊。
    //
    // §M01-A 已知（良性）：取消 callback 不是同步的，QuicIO 執行緒可能此刻
    // 仍在 quicAudioRecvCallback 裡，在下面 reset 之後又把 head 寫回 next。
    // 下一場 initializeAudioStream() 的 flush 會把它清掉；另案見 docs/TODO.md。
    if (useQuicAudio) {
        quicSetRecvCallbackForFlow(QUIC_FLOW_AUDIO, NULL, NULL);
    }
    quicAudioRing.head = 0;
    quicAudioRing.tail = 0;
    useQuicAudio = false;
#endif

    AudioCallbacks.cleanup();
}

int startAudioStream(void* audioContext, int arFlags) {
    int err;
    OPUS_MULTISTREAM_CONFIGURATION chosenConfig;

    if (HighQualitySurroundEnabled) {
        LC_ASSERT(HighQualitySurroundSupported);
        LC_ASSERT(HighQualityOpusConfig.channelCount != 0);
        LC_ASSERT(HighQualityOpusConfig.streams != 0);
        chosenConfig = HighQualityOpusConfig;
    }
    else {
        LC_ASSERT(NormalQualityOpusConfig.channelCount != 0);
        LC_ASSERT(NormalQualityOpusConfig.streams != 0);
        chosenConfig = NormalQualityOpusConfig;
    }

    chosenConfig.samplesPerFrame = 48 * AudioPacketDuration;

    err = AudioCallbacks.init(StreamConfig.audioConfiguration, &chosenConfig, audioContext, arFlags);
    if (err != 0) {
        return err;
    }

    AudioCallbacks.start();

#ifdef VIPLE_MPQUIC
    useQuicAudio = (StreamConfig.useQuicTransport && quicIsConnected());
    if (useQuicAudio) {
        quicAudioRing.head = 0;
        quicAudioRing.tail = 0;
        quicSetRecvCallbackForFlow(QUIC_FLOW_AUDIO, quicAudioRecvCallback, NULL);
        Limelog("[VIPLE-MPQUIC] Audio using QUIC datagram transport\n");
    }
#endif

    err = PltCreateThread("AudioRecv", AudioReceiveThreadProc, NULL, &receiveThread);
    if (err != 0) {
        AudioCallbacks.stop();
        closeSocket(rtpSocket);
        AudioCallbacks.cleanup();
        return err;
    }

    if ((AudioCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
        err = PltCreateThread("AudioDec", AudioDecoderThreadProc, NULL, &decoderThread);
        if (err != 0) {
            AudioCallbacks.stop();
            PltInterruptThread(&receiveThread);
            PltJoinThread(&receiveThread);
            closeSocket(rtpSocket);
            AudioCallbacks.cleanup();
            return err;
        }
    }

    return 0;
}

int LiGetPendingAudioFrames(void) {
    return LbqGetItemCount(&packetQueue);
}

int LiGetPendingAudioDuration(void) {
    return LiGetPendingAudioFrames() * AudioPacketDuration;
}

const RTP_AUDIO_STATS* LiGetRTPAudioStats(void) {
    return &rtpAudioQueue.stats;
}
