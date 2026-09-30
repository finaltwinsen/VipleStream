// VipleVr.h — VipleStream 2.0 VR 協定的單一定義來源（docs/vr_protocol.md §4）。
//
// 這個檔案在三份 common-c 必須 byte-identical：
//   Q  moonlight-qt/moonlight-common-c/moonlight-common-c/src/VipleVr.h
//   A  moonlight-android/app/src/main/jni/moonlight-core/moonlight-common-c/src/VipleVr.h
//   S3 Sunshine 使用的 common-c 副本（由 check_commonc_sync.ps1 檢查）
// client（C）與 server（C++，MSVC／MinGW／GCC）都 include 它，所以只能用
// C99 與 C++ 共通的寫法：不用 _Static_assert、不用 designated initializer。
//
// 線上格式一律 little-endian。VR 只在 little-endian 平台上啟用（x86、arm64），
// 結構直接以 #pragma pack(1) 的版面上線；大端平台編得過，但不得協商 VR。
//
// 相容原則：VR 只在雙方明確協商（client /launch 帶 vr=1，而且 server 回應
// <VipleStreamVRSession>）之後才啟用。沒協商的 session 永遠不送、不收這裡的
// 任何 ptype，也不會看到 24 B 的 0x81 VR header。

#ifndef VIPLE_VR_H
#define VIPLE_VR_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// C 與 C++ 通用的編譯期檢查（typedef 陣列長度為負就編不過）。
#define VIPLE_VR_STATIC_ASSERT(cond, name) \
    typedef char viple_vr_static_assert_##name[(cond) ? 1 : -1]

// MSVC 的 C 模式只認 __inline。
#if defined(_MSC_VER) && !defined(__cplusplus)
#define VIPLE_VR_INLINE static __inline
#else
#define VIPLE_VR_INLINE static inline
#endif

// ── 版本與能力協商（§4.2）──────────────────────────────────────────

#define VIPLE_VR_PROTO_VERSION 1

// /serverinfo 的 <VipleStreamVR>（十進位 bitmask）
#define VIPLE_VR_SERVER_CAP_PROTO_V1        0x01
#define VIPLE_VR_SERVER_CAP_PCVR            0x02  // 只有 Windows server 會設
#define VIPLE_VR_SERVER_CAP_HAPTICS         0x04
#define VIPLE_VR_SERVER_CAP_GAZE_UPLINK     0x08
#define VIPLE_VR_SERVER_CAP_QP_FOVEATION    0x10
#define VIPLE_VR_SERVER_CAP_RECOVERY_INTRA  0x20  // encoder 支援 intra refresh

// /launch 的 vrCaps（十六進位）
#define VIPLE_VR_CLIENT_CAP_RECOVERY_INTRA  0x01
#define VIPLE_VR_CLIENT_CAP_TRACK_THREAD    0x02
#define VIPLE_VR_CLIENT_CAP_GAZE            0x04
#define VIPLE_VR_CLIENT_CAP_SKELETON        0x08

// /launch 的 vrCodecs（client 能解的 codec）
#define VIPLE_VR_CODEC_H264  0x01
#define VIPLE_VR_CODEC_HEVC  0x02
#define VIPLE_VR_CODEC_AV1   0x04

// launch 失敗時回應的錯誤碼：沿用 Sunshine 慣例，放在 XML root 的 status_code（非 200）
// 與 status_message（以這些字串開頭，後接 ": 說明"）；HTTP 狀態本身仍是 200。
#define VIPLE_VR_ERR_BUSY                   "VR_BUSY"
#define VIPLE_VR_ERR_TRANSPORT_UNSUPPORTED  "VR_TRANSPORT_UNSUPPORTED"
#define VIPLE_VR_ERR_CODEC_LIMIT            "VR_CODEC_LIMIT"
#define VIPLE_VR_ERR_VRLINK_ACTIVE          "VRLINK_ACTIVE"
#define VIPLE_VR_ERR_BAD_PARAMS             "VR_BAD_PARAMS"
#define VIPLE_VR_ERR_DISABLED               "VR_DISABLED"
#define VIPLE_VR_ERR_NEEDS_VR_CLIENT        "VR_NEEDS_VR_CLIENT"

// /launch 參數的合理範圍（server 解碼時檢查，超出就回 VR_BAD_PARAMS）
#define VIPLE_VR_EYE_DIM_MIN     256
#define VIPLE_VR_EYE_DIM_MAX     4096
#define VIPLE_VR_HZ_MIN          60
#define VIPLE_VR_HZ_MAX          144
#define VIPLE_VR_FOV_TAN_MAX     100000   // tan×10000，約 84.3°
#define VIPLE_VR_IPD_MAX         8000     // mm×100
#define VIPLE_VR_EYE_TO_HEAD_COUNT 14     // 每眼 pos(mm×100)×3＋quat(×10000)×4
#define VIPLE_VR_OVERSCAN_MAX    200      // deg×10

// STREAM_CONFIGURATION.vrFlags／common-c 全域 VrFlags：由 client 在收到
// <VipleStreamVRSession> 之後設定。0 代表一般 session，行為與桌面完全相同。
#define VIPLE_VR_SF_ENABLED         0x01  // 這是 VR session（0x81 帶 24 B VR header）
#define VIPLE_VR_SF_RECOVERY_INTRA  0x02  // recovery=intra：掉幀送 LOSS，不等 IDR

// ── 控制通道（§4.1、§4.3）──────────────────────────────────────────

#define VIPLE_VR_PTYPE_TRACKING  0x5506  // C→S，232 B
#define VIPLE_VR_PTYPE_C2S       0x5507  // C→S，TLV
#define VIPLE_VR_PTYPE_S2C       0x5508  // S→C，TLV
// 0x5509–0x550F 保留給 VR，不得挪作他用。

// ENet channel：VR tracking 與即時 TLV 走這條，旗標 UNSEQUENCED。
// 一般控制訊息不得使用這條 channel。
#define VIPLE_VR_CTRL_CHANNEL  0x07

// 控制訊息 payload 上限：common-c 加密路徑以 256 B stack buffer 組裝，
// 扣掉 4 B V2 header 後必須「嚴格小於」252（§F2 runtime 檢查）。
#define VIPLE_VR_MAX_CTRL_PAYLOAD  251

// ENet throttle（F5）：VR session 在首次連線與 §Q-ENET-RECONNECT 之後都要套用。
#define VIPLE_VR_ENET_THROTTLE_INTERVAL      5000
#define VIPLE_VR_ENET_THROTTLE_ACCELERATION  2
#define VIPLE_VR_ENET_THROTTLE_DECELERATION  0

// tracking 送出頻率 = 顯示 Hz × 這個倍數
#define VIPLE_VR_TRACKING_RATE_MUL  2

// ── 0x5506 TRACKING（232 B）────────────────────────────────────────

#define VIPLE_VR_TRACKING_VERSION 1

// VIPLE_VR_TRACKING.flags
#define VIPLE_VR_TRK_HMD           0x01
#define VIPLE_VR_TRK_LEFT          0x02
#define VIPLE_VR_TRK_RIGHT         0x04
#define VIPLE_VR_TRK_GAZE          0x08
#define VIPLE_VR_TRK_PRESENCE      0x10
#define VIPLE_VR_TRK_PHASE_SAMPLE  0x20  // 依 LATCH 回授多送的相位對齊樣本

// pose 陣列索引
#define VIPLE_VR_POSE_HMD    0
#define VIPLE_VR_POSE_LEFT   1
#define VIPLE_VR_POSE_RIGHT  2
#define VIPLE_VR_POSE_COUNT  3

// VIPLE_VR_CONTROLLER_INPUT.buttons／touches（同一個 bit 代表同一個鍵）
#define VIPLE_VR_BTN_SYSTEM            0x0001
#define VIPLE_VR_BTN_MENU              0x0002
#define VIPLE_VR_BTN_A                 0x0004  // 左手為 X
#define VIPLE_VR_BTN_B                 0x0008  // 左手為 Y
#define VIPLE_VR_BTN_THUMBSTICK        0x0010
#define VIPLE_VR_BTN_TRIGGER           0x0020
#define VIPLE_VR_BTN_GRIP              0x0040
#define VIPLE_VR_BTN_TRACKPAD          0x0080
#define VIPLE_VR_BTN_THUMBREST         0x0100
// 0x0200–0x8000 保留；pressCtr 以同樣的 bit 序每鍵 2 bit（鍵 i 在 bit 2i..2i+1）

// VIPLE_VR_CONTROLLER_INPUT.flags
#define VIPLE_VR_CTRL_ACTIVE   0x01
#define VIPLE_VR_CTRL_FOCUSED  0x02

#pragma pack(push, 1)

typedef struct _VIPLE_VR_POSE {
    float pos[3];     // 公尺，client 參考空間
    float rot[4];     // 四元數 x, y, z, w
    float linVel[3];  // m/s
    float angVel[3];  // rad/s
} VIPLE_VR_POSE;

typedef struct _VIPLE_VR_CONTROLLER_INPUT {
    uint32_t buttons;
    uint32_t touches;
    uint32_t pressCtr;  // 每鍵 2 bit 的按壓計數，補償 unsequenced 丟包漏掉的短按
    uint16_t trigger;   // 0–65535
    uint16_t grip;
    int16_t  stickX;
    int16_t  stickY;
    uint8_t  battery;   // 0–100，255 = 未知
    uint8_t  flags;     // VIPLE_VR_CTRL_*
    uint16_t reserved;
} VIPLE_VR_CONTROLLER_INPUT;

typedef struct _VIPLE_VR_GAZE {
    uint16_t yawF16;    // IEEE half，弧度
    uint16_t pitchF16;
    uint8_t  confidence;
    uint8_t  flags;
    uint16_t reserved;
} VIPLE_VR_GAZE;

typedef struct _VIPLE_VR_TRACKING {
    uint8_t  version;       // VIPLE_VR_TRACKING_VERSION
    uint8_t  flags;         // VIPLE_VR_TRK_*
    uint16_t spaceEpoch;    // recenter 時遞增
    uint32_t sampleId;      // 單調遞增，0 保留為「無」
    uint64_t sampleTimeNs;  // client 單調時鐘
    uint32_t predictNs;     // 目標顯示時間 − sampleTime
    VIPLE_VR_POSE pose[VIPLE_VR_POSE_COUNT];
    VIPLE_VR_CONTROLLER_INPUT input[2];
    VIPLE_VR_GAZE gaze;
} VIPLE_VR_TRACKING;

// ── 0x81 VR frame header（24 B，只在 VR session）──────────────────
//
// 一般 session 的 Sunshine 送 8 B 的 0x01 short header；VR session 改送 24 B 的
// 0x81 header（host appversion 固定 7.1.431，client 對 0x81 一律跳 24 B，
// 不變式 2）。前 6 B 的語意與 short header 相同。

#define VIPLE_VR_FRAME_HEADER_TYPE  0x81
#define VIPLE_VR_FRAME_HEADER_SIZE  24

// VIPLE_VR_FRAME_HEADER.vrFlags
#define VIPLE_VR_FF_POSE_VALID     0x01
#define VIPLE_VR_FF_ECHO_MATCHED   0x02  // echoSampleId 是真的收到的樣本
#define VIPLE_VR_FF_POSE_FALLBACK  0x04  // 沒有對應樣本，pose 是外插或沿用
#define VIPLE_VR_FF_REPEATED       0x08  // 重複幀（遊戲沒有新畫面）
#define VIPLE_VR_FF_POS_RELATIVE   0x10  // renderPos 是相對 echo 樣本的位移
#define VIPLE_VR_FF_DERIVED_POSE   0x20
#define VIPLE_VR_FF_FOVEATED       0x40
#define VIPLE_VR_FF_REFRESH_DONE   0x80  // intra-refresh wave 的最後一幀

// renderPos 的單位與範圍
#define VIPLE_VR_POS_UNITS_PER_M  10000.0f  // 0.1 mm
#define VIPLE_VR_POS_MAX_M        3.2767f

typedef struct _VIPLE_VR_FRAME_HEADER {
    uint8_t  headerType;              // VIPLE_VR_FRAME_HEADER_TYPE
    uint16_t frameProcessingLatency;  // 1/10 ms
    uint8_t  frameType;               // 同 short header
    uint16_t lastPayloadLen;
    uint8_t  vrFlags;                 // VIPLE_VR_FF_*
    uint8_t  layoutEpoch;
    uint32_t echoSampleId;
    uint8_t  renderRot[6];            // smallest-three，48 bit little-endian
    int16_t  renderPos[3];            // 0.1 mm
} VIPLE_VR_FRAME_HEADER;

// ── TLV 容器：0x5507 VR_C2S、0x5508 VR_S2C（§4.5）──────────────────
//
// 格式：{u8 subtype, u8 len, payload[len]}，可串接多筆，總長 ≤ 251 B。
// 不認得的 subtype 依 len 跳過；len 超出剩餘長度就丟棄整則訊息。

// C→S subtype
#define VIPLE_VR_C2S_CONFIG            0x01  // reliable
#define VIPLE_VR_C2S_RECENTER          0x02  // reliable
#define VIPLE_VR_C2S_PRESENCE          0x03  // reliable
#define VIPLE_VR_C2S_CONTROLLER_STATE  0x04  // reliable
#define VIPLE_VR_C2S_REFRESH_CHANGED   0x05  // reliable，觸發重開 session
#define VIPLE_VR_C2S_CLIENT_TIMING     0x06  // VR channel unsequenced，1 Hz
#define VIPLE_VR_C2S_FRAME_FEEDBACK    0x07  // VR channel unsequenced（GA）
#define VIPLE_VR_C2S_LATCH             0x08  // VR channel unsequenced，10 Hz
#define VIPLE_VR_C2S_LOSS              0x09  // VR channel unsequenced，送 2 次
#define VIPLE_VR_C2S_RESERVED_MIC      0x7F

// S→C subtype
#define VIPLE_VR_S2C_HAPTIC            0x01  // VR channel unsequenced
#define VIPLE_VR_S2C_STATE             0x02  // reliable
#define VIPLE_VR_S2C_STATS             0x03  // VR channel unsequenced，1 Hz
#define VIPLE_VR_S2C_CONFIG_ACK        0x04  // reliable
#define VIPLE_VR_S2C_LAYOUT            0x05  // reliable
#define VIPLE_VR_S2C_REFRESH_START     0x06  // VR channel unsequenced，送 2 次

// LOSS.reason
#define VIPLE_VR_LOSS_NETWORK       1  // depacketizer 偵測到掉幀
#define VIPLE_VR_LOSS_DECODE_ERROR  2  // decoder 回報錯誤
#define VIPLE_VR_LOSS_RESEND        3  // 2×RTT+20 ms 內沒收到 REFRESH_START，重送

// REFRESH_START.reason
#define VIPLE_VR_REFRESH_LOSS    1
#define VIPLE_VR_REFRESH_SAFETY  2  // 週期性安全網
#define VIPLE_VR_REFRESH_IDR     3  // encoder 不支援 IR，改送 IDR

// STATE.state
#define VIPLE_VR_STATE_IDLE          0
#define VIPLE_VR_STATE_ORCHESTRATING 1
#define VIPLE_VR_STATE_HMD_ACTIVE    2
#define VIPLE_VR_STATE_STUB_ECHO     3  // M1a：沒有 driver，桌面擷取＋tracking 回聲
#define VIPLE_VR_STATE_ERROR         0xFF

// STATE.code
#define VIPLE_VR_STATE_CODE_NONE                 0
#define VIPLE_VR_STATE_CODE_VRLINK_ACTIVE        1
#define VIPLE_VR_STATE_CODE_SAFE_MODE            2
#define VIPLE_VR_STATE_CODE_ABI_MISMATCH_RESTART 3
// M1b V5（S1-15、M1b 設計 §D.3）：編排器的 STATE code（ERROR 以外另註）
#define VIPLE_VR_STATE_CODE_DEPLOY_REFUSED          4
#define VIPLE_VR_STATE_CODE_STEAMVR_NOT_INSTALLED   5
#define VIPLE_VR_STATE_CODE_NO_USER_SESSION        6
#define VIPLE_VR_STATE_CODE_HMD_TIMEOUT            7
#define VIPLE_VR_STATE_CODE_OTHER_HMD_ACTIVE       8
#define VIPLE_VR_STATE_CODE_VR_APP_RUNNING         9
#define VIPLE_VR_STATE_CODE_STEAMVR_LAUNCH_FAILED  10
#define VIPLE_VR_STATE_CODE_REGISTER_FAILED        11
#define VIPLE_VR_STATE_CODE_GUARD_FAILED           12
#define VIPLE_VR_STATE_CODE_IPC_UNAVAILABLE        13
#define VIPLE_VR_STATE_CODE_DRIVER_LOST            14  // ORCHESTRATING 期間是進度提示；重連失敗改 ERROR
#define VIPLE_VR_STATE_CODE_STEAMVR_INCOMPATIBLE   15
#define VIPLE_VR_STATE_CODE_OPENXR_RUNTIME_OTHER   16  // 警告：隨 HMD_ACTIVE 送，不是 ERROR
#define VIPLE_VR_STATE_CODE_STEAM_NOT_LOGGED_IN    17
#define VIPLE_VR_STATE_CODE_DRIVER_DISABLED_BY_USER 18
#define VIPLE_VR_STATE_CODE_STEAMVR_RESTART_REQUIRED 19
#define VIPLE_VR_STATE_CODE_ADAPTER_MISMATCH       20
#define VIPLE_VR_STATE_CODE_DRIVER_DEGRADED        21

typedef struct _VIPLE_VR_TLV_LOSS {
    uint32_t firstLost;  // 幀號（含）
    uint32_t lastLost;   // 幀號（含）
    uint8_t  reason;     // VIPLE_VR_LOSS_*
} VIPLE_VR_TLV_LOSS;

typedef struct _VIPLE_VR_TLV_LATCH {
    uint32_t frameId;
    int32_t  slackUs;          // 正值：幀比 latch 早到
    uint32_t displayPeriodNs;
} VIPLE_VR_TLV_LATCH;

// M4a R3：1 Hz client 時序統計（C→S，VR channel unsequenced）。欄位只往後加；server 依 len
// 能讀多少算多少。cumulative 欄位是 session 開始以來的累計值（同 STATS，掉一筆不漏算）；
// *P50／*P95 是最近 1 s 區間的分位數。mtp* 以 0.1 ms 為單位（MTP_content＝顯示該幀的
// predictedDisplayTime − echoSampleId 的 sampleTime，全部在 client 時鐘內算）。
typedef struct _VIPLE_VR_TLV_CLIENT_TIMING {
    uint32_t framesPresented;  // cumulative：送進 projection layer 的串流幀
    uint32_t xrMissed;         // cumulative：XR frame loop 漏幀
    uint32_t metaMiss;         // cumulative：查不到 0x81 meta 而丟棄的幀
    uint32_t displayPeriodNs;
    uint16_t decodeP50Us;
    uint16_t decodeP95Us;
    uint16_t renderP50Us;      // latch 前的影像 render（map＋render）
    uint16_t renderP95Us;
    int16_t  slackP50Us;       // 與 LATCH.slackUs 同義（正值＝幀比 latch 早到）
    int16_t  slackP05Us;       // 最差的 5%（最晚到）
    uint16_t mtpP50_100us;
    uint16_t mtpP95_100us;
} VIPLE_VR_TLV_CLIENT_TIMING;

typedef struct _VIPLE_VR_TLV_REFRESH_START {
    uint32_t startFrame;  // wave 第一幀的幀號
    uint8_t  frameCnt;    // wave 長度；最後一幀帶 VIPLE_VR_FF_REFRESH_DONE
    uint8_t  reason;      // VIPLE_VR_REFRESH_*
} VIPLE_VR_TLV_REFRESH_START;

typedef struct _VIPLE_VR_TLV_STATE {
    uint8_t  state;     // VIPLE_VR_STATE_*
    uint8_t  progress;  // 0–100
    uint16_t code;      // VIPLE_VR_STATE_CODE_*
} VIPLE_VR_TLV_STATE;

typedef struct _VIPLE_VR_TLV_HAPTIC {
    uint8_t  device;      // VIPLE_VR_POSE_LEFT／RIGHT
    uint8_t  reserved[3];
    uint32_t durationUs;
    float    frequencyHz;
    float    amplitude;   // 0–1
    uint32_t eventId;
} VIPLE_VR_TLV_HAPTIC;

// 1 Hz server 統計。欄位只往後加；client 依 len 讀得到多少算多少。
// 計數欄位一律是 session 開始以來的**累計值**（STATS 走 UNSEQUENCED，掉一筆也不會
// 漏算），client 以相鄰兩筆的差算區間值；clk* 是當下的量測值。
typedef struct _VIPLE_VR_TLV_STATS {
    uint32_t poseRx;          // 收到的 tracking 樣本數
    uint32_t poseOutOfOrder;  // sampleId 倒退或重複
    uint32_t poseGap;         // sampleId 跳號累計（估丟失）
    uint32_t framesTagged;    // 帶 echo 的 VR 幀數
    uint32_t framesFallback;  // 沒有可用樣本的 VR 幀數
    uint32_t lossRx;          // 收到的 LOSS 筆數
    uint32_t refreshWaves;    // 啟動的 refresh wave 數
    int32_t  clkOffsetUs;     // M1b：server 時鐘 − client 時鐘
    uint32_t clkOffsetJitterUs;
    uint32_t staleCount;
} VIPLE_VR_TLV_STATS;

#pragma pack(pop)

VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_POSE) == 52, pose_52);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_CONTROLLER_INPUT) == 24, input_24);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_GAZE) == 8, gaze_8);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TRACKING) == 232, tracking_232);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_FRAME_HEADER) == VIPLE_VR_FRAME_HEADER_SIZE, frame_header_24);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TLV_LOSS) == 9, tlv_loss_9);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TLV_LATCH) == 12, tlv_latch_12);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TLV_REFRESH_START) == 6, tlv_refresh_6);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TLV_CLIENT_TIMING) == 32, tlv_client_timing_32);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TLV_STATE) == 4, tlv_state_4);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TLV_HAPTIC) == 20, tlv_haptic_20);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TLV_STATS) == 40, tlv_stats_40);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TRACKING) <= VIPLE_VR_MAX_CTRL_PAYLOAD, tracking_fits);

// ── 解碼後的每幀 VR metadata（client 內部，不上線）─────────────────

typedef struct _VIPLE_VR_FRAME_META {
    uint8_t  present;       // 1：這一幀帶了 0x81 VR header
    uint8_t  vrFlags;
    uint8_t  layoutEpoch;
    uint8_t  reserved;
    uint32_t echoSampleId;
    float    renderRot[4];  // x, y, z, w
    float    renderPos[3];  // 公尺（POS_RELATIVE 時是位移）
} VIPLE_VR_FRAME_META;

// ── 共用小工具 ───────────────────────────────────────────────────

// smallest-three 四元數壓縮：2 bit 最大分量索引＋3×15 bit，共 47 bit，
// 以 48 bit little-endian 存放。最大分量取正號（q 與 −q 是同一個旋轉）。
// 往返誤差最差約 0.007°（20 萬組隨機四元數實測）。
#define VIPLE_VR_QUAT_COMPONENT_MAX 0.70710678f
#define VIPLE_VR_QUAT_STEPS         32767.0f

VIPLE_VR_INLINE void VipleVrPackQuat48(const float q[4], uint8_t out[6]) {
    int largest = 0;
    float maxAbs = q[0] < 0 ? -q[0] : q[0];
    float sign;
    uint64_t bits;
    int i, n;
    for (i = 1; i < 4; i++) {
        float a = q[i] < 0 ? -q[i] : q[i];
        if (a > maxAbs) {
            maxAbs = a;
            largest = i;
        }
    }
    sign = q[largest] < 0 ? -1.0f : 1.0f;
    bits = (uint64_t)largest;
    for (i = 0, n = 0; i < 4; i++) {
        float v;
        long quant;
        if (i == largest) {
            continue;
        }
        v = q[i] * sign;
        if (v > VIPLE_VR_QUAT_COMPONENT_MAX) v = VIPLE_VR_QUAT_COMPONENT_MAX;
        if (v < -VIPLE_VR_QUAT_COMPONENT_MAX) v = -VIPLE_VR_QUAT_COMPONENT_MAX;
        // [-max, max] → [0, 32767]
        quant = (long)((v / VIPLE_VR_QUAT_COMPONENT_MAX + 1.0f) * 0.5f * VIPLE_VR_QUAT_STEPS + 0.5f);
        if (quant < 0) quant = 0;
        if (quant > 32767) quant = 32767;
        bits |= ((uint64_t)quant) << (2 + 15 * n);
        n++;
    }
    for (i = 0; i < 6; i++) {
        out[i] = (uint8_t)(bits >> (8 * i));
    }
}

VIPLE_VR_INLINE void VipleVrUnpackQuat48(const uint8_t in[6], float q[4]) {
    uint64_t bits = 0;
    int largest, i, n;
    float sumSq = 0.0f;
    for (i = 0; i < 6; i++) {
        bits |= ((uint64_t)in[i]) << (8 * i);
    }
    largest = (int)(bits & 0x3);
    for (i = 0, n = 0; i < 4; i++) {
        long quant;
        float v;
        if (i == largest) {
            continue;
        }
        quant = (long)((bits >> (2 + 15 * n)) & 0x7FFF);
        v = ((float)quant / VIPLE_VR_QUAT_STEPS * 2.0f - 1.0f) * VIPLE_VR_QUAT_COMPONENT_MAX;
        q[i] = v;
        sumSq += v * v;
        n++;
    }
    // 最大分量由單位長度還原；數值誤差讓 sumSq 略大於 1 時取 0。
    q[largest] = sumSq < 1.0f ? sqrtf(1.0f - sumSq) : 0.0f;
}

#ifdef __cplusplus
}
#endif

#endif // VIPLE_VR_H
