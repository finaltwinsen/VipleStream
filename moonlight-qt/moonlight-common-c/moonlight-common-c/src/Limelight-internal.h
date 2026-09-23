#pragma once

#include "Platform.h"
#include "Limelight.h"
#include "PlatformSockets.h"
#include "PlatformThreads.h"
#include "PlatformCrypto.h"
#include "Video.h"
#include "Input.h"
#include "RtpAudioQueue.h"
#include "RtpVideoQueue.h"
#include "ByteBuffer.h"

#include <enet/enet.h>

#ifdef VIPLE_MPQUIC
// §M01-A：QUIC 接收 ring（AudioStream.c 的 quicAudioRing、VideoStream.c 的
// quicVideoRing）共用的記憶體屏障。兩個 ring 都是 SPSC：生產端是 QuicIO
// 執行緒（recv callback），消費端是 AudioRecv／VideoRecv 執行緒。
// 舊版只把 head/tail 宣告成 volatile，volatile 不保證 slot 的 data/len 與
// head/tail 之間的可見順序：
//   - 硬體：ARM64（弱記憶體序）上消費端可能先看到前進後的 head，卻讀到
//     slot 上一輪殘留的 len／data（2026-09-23 Pixel 5 音訊 blockSize 348
//     SIGABRT 的根因）；x86/x64 是 TSO，硬體不做這種重排。
//   - 編譯器：len[]、data[] 是一般存取，gcc/clang 在 x64 上也可以把 len
//     的 store 移到 volatile head 的 store 之後。
// 屏障同時擋住這兩種重排。用法（兩個 ring 一致）：
//   生產端：讀 tail 判斷滿不滿 → ACQUIRE → 寫 slot（data、len）→ RELEASE → 寫 head
//   消費端：讀 head 判斷空不空 → ACQUIRE → 讀 slot（len、data）→ RELEASE → 寫 tail
// Windows 的 MemoryBarrier()（Platform.h 已引入 Windows.h）在 x64 是
// lock-prefixed 指令、ARM64 是 dmb ish；其他平台用 GCC/Clang 內建的
// __atomic_thread_fence（ARM64 編成 dmb ishld／dmb ish，x86 只是編譯器屏障）。
#if defined(_WIN32)
#define QUIC_RING_ACQUIRE_FENCE() MemoryBarrier()
#define QUIC_RING_RELEASE_FENCE() MemoryBarrier()
#else
#define QUIC_RING_ACQUIRE_FENCE() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define QUIC_RING_RELEASE_FENCE() __atomic_thread_fence(__ATOMIC_RELEASE)
#endif
#endif

// Common globals
extern char* RemoteAddrString;
extern struct sockaddr_storage RemoteAddr;
extern struct sockaddr_storage LocalAddr;
extern SOCKADDR_LEN AddrLen;
extern int AppVersionQuad[4];
extern STREAM_CONFIGURATION StreamConfig;
extern CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
extern DECODER_RENDERER_CALLBACKS VideoCallbacks;
extern AUDIO_RENDERER_CALLBACKS AudioCallbacks;
extern int NegotiatedVideoFormat;
extern volatile bool ConnectionInterrupted;
extern bool HighQualitySurroundSupported;
extern bool HighQualitySurroundEnabled;
extern OPUS_MULTISTREAM_CONFIGURATION NormalQualityOpusConfig;
extern OPUS_MULTISTREAM_CONFIGURATION HighQualityOpusConfig;
extern int AudioPacketDuration;
extern bool AudioEncryptionEnabled;
extern bool ReferenceFrameInvalidationSupported;

extern uint16_t RtspPortNumber;
extern uint16_t ControlPortNumber;
extern uint16_t AudioPortNumber;
extern uint16_t VideoPortNumber;
extern uint16_t OverrideVideoPort;
extern uint16_t OverrideAudioPort;
extern uint16_t OverrideControlPort;

extern SS_PING AudioPingPayload;
extern SS_PING VideoPingPayload;
extern uint32_t ControlConnectData;

extern uint32_t SunshineFeatureFlags;

// Encryption flags shared by Sunshine and Moonlight in RTSP
#define SS_ENC_CONTROL_V2 0x01
#define SS_ENC_VIDEO 0x02
#define SS_ENC_AUDIO 0x04

extern uint32_t EncryptionFeaturesSupported;
extern uint32_t EncryptionFeaturesRequested;
extern uint32_t EncryptionFeaturesEnabled;

// ENet channel ID values
#define CTRL_CHANNEL_GENERIC      0x00
#define CTRL_CHANNEL_URGENT       0x01 // IDR, LTR ACK and RFI
#define CTRL_CHANNEL_KEYBOARD     0x02
#define CTRL_CHANNEL_MOUSE        0x03
#define CTRL_CHANNEL_PEN          0x04
#define CTRL_CHANNEL_TOUCH        0x05
#define CTRL_CHANNEL_UTF8         0x06
#define CTRL_CHANNEL_GAMEPAD_BASE 0x10 // 0x10 to 0x1F by controller index
#define CTRL_CHANNEL_SENSOR_BASE  0x20 // 0x20 to 0x2F by controller index
#define CTRL_CHANNEL_COUNT        0x30

#ifndef UINT24_MAX
#define UINT24_MAX 0xFFFFFF
#endif

#define U16(x) ((unsigned short) ((x) & UINT16_MAX))
#define U24(x) ((unsigned int) ((x) & UINT24_MAX))
#define U32(x) ((unsigned int) ((x) & UINT32_MAX))

#define isBefore16(x, y) (U16((x) - (y)) > (UINT16_MAX/2))
#define isBefore24(x, y) (U24((x) - (y)) > (UINT24_MAX/2))
#define isBefore32(x, y) (U32((x) - (y)) > (UINT32_MAX/2))

#define APP_VERSION_AT_LEAST(a, b, c)                                                       \
    ((AppVersionQuad[0] > (a)) ||                                                           \
     (AppVersionQuad[0] == (a) && AppVersionQuad[1] > (b)) ||                               \
     (AppVersionQuad[0] == (a) && AppVersionQuad[1] == (b) && AppVersionQuad[2] >= (c)))

#define IS_SUNSHINE() (AppVersionQuad[3] < 0)

// Client feature flags for x-ml-general.featureFlags SDP attribute
#define ML_FF_FEC_STATUS 0x01 // Client sends SS_FRAME_FEC_STATUS for frame losses
#define ML_FF_SESSION_ID_V1 0x02 // Client supports X-SS-Ping-Payload and X-SS-Connect-Data

#define UDP_RECV_POLL_TIMEOUT_MS 100

// At this value or above, we will request high quality audio unless CAPABILITY_SLOW_OPUS_DECODER
// is set on the audio renderer.
#define HIGH_AUDIO_BITRATE_THRESHOLD 15000

// Below this value, we will request 20 ms audio frames to reduce bandwidth if the audio
// renderer sets CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION.
#define LOW_AUDIO_BITRATE_TRESHOLD 5000

// Internal macro for checking the magic byte of the audio configuration value
#define MAGIC_BYTE_FROM_AUDIO_CONFIG(x) ((x) & 0xFF)

int serviceEnetHost(ENetHost* client, ENetEvent* event, enet_uint32 timeoutMs);
int gracefullyDisconnectEnetPeer(ENetHost* host, ENetPeer* peer, enet_uint32 lingerTimeoutMs);
int extractVersionQuadFromString(const char* string, int* quad);
bool isReferenceFrameInvalidationSupportedByDecoder(void);
bool isReferenceFrameInvalidationEnabled(void);
void* extendBuffer(void* ptr, size_t newSize);

void fixupMissingCallbacks(PDECODER_RENDERER_CALLBACKS* drCallbacks, PAUDIO_RENDERER_CALLBACKS* arCallbacks,
    PCONNECTION_LISTENER_CALLBACKS* clCallbacks);
void setRecorderCallbacks(PDECODER_RENDERER_CALLBACKS drCallbacks, PAUDIO_RENDERER_CALLBACKS arCallbacks);

char* getSdpPayloadForStreamConfig(int rtspClientVersion, int* length);

int initializeControlStream(void);
int startControlStream(void);
int stopControlStream(void);
void destroyControlStream(void);
void connectionDetectedFrameLoss(uint32_t startFrame, uint32_t endFrame);
void connectionReceivedCompleteFrame(uint32_t frameIndex, bool frameIsLTR);
void connectionSawFrame(uint32_t frameIndex);
void connectionSendFrameFecStatus(PSS_FRAME_FEC_STATUS fecStatus);
int sendInputPacketOnControlStream(unsigned char* data, int length, uint8_t channelId, uint32_t flags, bool moreData);
void flushInputOnControlStream(void);
bool isControlDataInTransit(void);
#ifdef VIPLE_MPQUIC
bool isEnetConnected(void);
// §M01-C：ENet 控制平面的 QUIC fallback 是否可用（failover 進行中 或 QUIC
// 傳輸活著）；InputStream 用它決定送失敗時要不要壓制 connectionTerminated。
bool isQuicControlFallbackAvailable(void);
#endif

int performRtspHandshake(PSERVER_INFORMATION serverInfo);

void initializeVideoDepacketizer(int pktSize);
void destroyVideoDepacketizer(void);
void queueRtpPacket(PRTPV_QUEUE_ENTRY queueEntry);
void stopVideoDepacketizer(void);
void requestDecoderRefresh(void);
void notifyFrameLost(unsigned int frameNumber, bool speculative);
// §FRZ-WATCHDOG: VideoStream 斷糧 watchdog 用（皆 VideoRecv 執行緒）
uint64_t videoDepacketizerLastFrameTimeUs(void);
void videoDepacketizerRequestResync(void);

void initializeVideoStream(void);
void destroyVideoStream(void);
void notifyKeyFrameReceived(void);
int startVideoStream(void* rendererContext, int drFlags);
void stopVideoStream(void);

int initializeAudioStream(void);
int notifyAudioPortNegotiationComplete(void);
void destroyAudioStream(void);
int startAudioStream(void* audioContext, int arFlags);
void stopAudioStream(void);

int initializeInputStream(void);
void destroyInputStream(void);
int startInputStream(void);
int stopInputStream(void);
