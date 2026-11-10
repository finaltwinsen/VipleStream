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
#define VIPLE_VR_SERVER_CAP_GAZE_UPLINK     0x08  // §VR-GAZE：支援上行注視點資料
#define VIPLE_VR_SERVER_CAP_QP_FOVEATION    0x10
#define VIPLE_VR_SERVER_CAP_RECOVERY_INTRA  0x20  // encoder 支援 intra refresh
#define VIPLE_VR_SERVER_CAP_MULTILINK       0x40  // §VR-MULTILINK：支援多連線
#define VIPLE_VR_SERVER_CAP_GAZE_FOVEA      0x80  // §VR-GAZE-FOVEA：支援 0x82 幀標頭（逐幀動態注視點中心）
#define VIPLE_VR_SERVER_CAP_MIC             0x100 // §VR-MIC：收頭盔麥克風（C2S MIC），寫進 host 的虛擬麥克風（<VipleStreamVR> 是十進位整數，放得下）

// /launch 的 vrCaps（十六進位）
#define VIPLE_VR_CLIENT_CAP_RECOVERY_INTRA  0x01
#define VIPLE_VR_CLIENT_CAP_TRACK_THREAD    0x02
#define VIPLE_VR_CLIENT_CAP_GAZE            0x04  // 上行送出 VIPLE_VR_GAZE
#define VIPLE_VR_CLIENT_CAP_SKELETON        0x08
#define VIPLE_VR_CLIENT_CAP_MULTILINK       0x10  // §VR-MULTILINK
#define VIPLE_VR_CLIENT_CAP_GAZE_FOVEA      0x20  // §VR-GAZE-FOVEA：能接收 0x82 幀標頭並還原動態中心
#define VIPLE_VR_CLIENT_CAP_FOVEA_INSET     0x40  // §VR-FOVEA-INSET：懂分段注視點曲線（協商回應的 foveaInset=）
// §VR-FOVEA-TILES（2026-11-10）：懂「每眼兩塊影像」的注視點排法（協商回應的 foveaTiles=／foveaRender=），
// 也代表收得到 0x82 幀標頭（不論有沒有眼動追蹤，注視點那塊的位置都逐幀帶）
#define VIPLE_VR_CLIENT_CAP_FOVEA_TILES     0x80

// /launch 的 vrCodecs（client 能解的 codec）
#define VIPLE_VR_CODEC_H264  0x01
#define VIPLE_VR_CODEC_HEVC  0x02
#define VIPLE_VR_CODEC_AV1   0x04
// §PYROWAVE-VR（2026-10-28）：PyroWave 小波編碼（每幀 intra，LOSS 不需要 refresh）；server 要 pyrowave::is_available()
#define VIPLE_VR_CODEC_PYROWAVE 0x08

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

// §VR-PLAYAREA（2026-10-30）：真實遊玩區與坐姿零點範圍
#define VIPLE_VR_PLAYAREA_MIN_MM        500      // 寬深下限 0.5 m（mm）
#define VIPLE_VR_PLAYAREA_MAX_MM        20000    // 寬深上限 20 m（mm）
#define VIPLE_VR_SEATED_POS_MIN_MM      (-20000) // 坐姿水平 x/z 範圍 ±20 m（mm）
#define VIPLE_VR_SEATED_POS_MAX_MM      20000
#define VIPLE_VR_SEATED_HEIGHT_MIN_MM   0        // 坐姿高度 y 範圍 0～3 m（mm）
#define VIPLE_VR_SEATED_HEIGHT_MAX_MM   3000
#define VIPLE_VR_SEATED_YAW_MDEG_MIN    (-180000) // 坐姿 yaw 範圍 ±180°（毫度，deg×1000）
#define VIPLE_VR_SEATED_YAW_MDEG_MAX    180000
#define VIPLE_VR_PLAYAREA_DEFAULT_W_MM  10000    // 預設遊玩區 10 m × 10 m
#define VIPLE_VR_PLAYAREA_DEFAULT_D_MM  10000
#define VIPLE_VR_SEATED_DEFAULT_X_MM    0        // 預設坐姿零點 (0, 1.2, 0) m
#define VIPLE_VR_SEATED_DEFAULT_Y_MM    1200
#define VIPLE_VR_SEATED_DEFAULT_Z_MM    0
#define VIPLE_VR_SEATED_DEFAULT_YAW_MDEG 0

// STREAM_CONFIGURATION.vrFlags／common-c 全域 VrFlags：由 client 在收到
// <VipleStreamVRSession> 之後設定。0 代表一般 session，行為與桌面完全相同。
#define VIPLE_VR_SF_ENABLED         0x01  // 這是 VR session（0x81 帶 24 B VR header）
#define VIPLE_VR_SF_RECOVERY_INTRA  0x02  // recovery=intra：掉幀送 LOSS，不等 IDR
#define VIPLE_VR_SF_MULTILINK       0x04  // §VR-MULTILINK：server 在 <VipleStreamVRSession> 回了 multilink=1
#define VIPLE_VR_SF_MULTILINK_SELFTEST 0x08  // dev：同一張網卡再開一條連線，用單一網卡驗複製與去重
#define VIPLE_VR_SF_REPAIR_RTT      0x10  // §VR-REPAIR-RTT：client 願意用（只在本機用：決定 LINK_HELLO 帶不帶 VIPLE_VR_LINK_F_REPAIR_RTT）

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

// tracking 送出頻率 = 顯示 Hz × 這個倍數。2026-10-29 §VR-TRACK-RATE：2 → 4（144 Hz 時 576 Hz）。相位鎖定下仍有約 14% 的格
// 用到前一筆樣本（多老一個取樣間隔），間隔減半讓那些格少老一半；上行約 0.42 → 0.84 Mbps。server 不依賴這個倍數
#define VIPLE_VR_TRACKING_RATE_MUL  4

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
// §VR-FRAME-CTRL：Steam Frame 手把每手四個面鍵＋肩鍵。A／B／C／D 依 SteamVR frame_controller 的鏡像對應：
// 右手 a／b／x／y，左手 dpad_down／dpad_right／dpad_left／dpad_up（Touch 的左手只有 A＝X、B＝Y）
#define VIPLE_VR_BTN_C                 0x0200  // Frame：右 x、左 dpad_left
#define VIPLE_VR_BTN_D                 0x0400  // Frame：右 y、左 dpad_up
#define VIPLE_VR_BTN_BUMPER            0x0800  // Frame：shoulder（SteamVR 的 /input/bumper）
// 0x1000–0x8000 保留；pressCtr 以同樣的 bit 序每鍵 2 bit（鍵 i 在 bit 2i..2i+1）

// VIPLE_VR_CONTROLLER_INPUT.flags
#define VIPLE_VR_CTRL_ACTIVE   0x01
#define VIPLE_VR_CTRL_FOCUSED  0x02

// VIPLE_VR_CONTROLLER_INPUT.profile（M4a 收尾）：client 目前的 OpenXR interaction profile，server 的
// SteamVR driver 依此選控制器外觀（render model）。原本是 reserved 的低位元組，舊 client 送 0＝未知。
#define VIPLE_VR_CTRL_PROFILE_UNKNOWN  0
#define VIPLE_VR_CTRL_PROFILE_TOUCH    1  // /interaction_profiles/oculus/touch_controller（Quest 系列）
#define VIPLE_VR_CTRL_PROFILE_INDEX    2  // /interaction_profiles/valve/index_controller
#define VIPLE_VR_CTRL_PROFILE_FRAME    3  // /interaction_profiles/valve/frame_controller_valve（Steam Frame）
#define VIPLE_VR_CTRL_PROFILE_OTHER    4  // 其他（khr/simple 等）

// §VR-WHEEL（2026-11-05）：input[0].reserved＝方向盤模擬。client 的設定與快捷鍵（兩支搖桿同時按住 1.5 秒）決定，
// server 在 Xbox 手把模式時依兩手的相對位置算方向盤角度、取代左搖桿 X。舊 client 送 0＝關；舊 server 不看這個位元組。
#define VIPLE_VR_WHEEL_ON          0x80  // 方向盤模式開
#define VIPLE_VR_WHEEL_RANGE_MASK  0x7F  // 轉向範圍（左打到底到右打到底，單位 10 度；0＝預設 180 度）
#define VIPLE_VR_WHEEL_RANGE_DEFAULT_DEG 180

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
    uint8_t  profile;   // VIPLE_VR_CTRL_PROFILE_*（M4a 收尾；原 reserved 的低位元組）
    uint8_t  reserved;  // input[0]（左手）：§VR-WHEEL 方向盤設定（VIPLE_VR_WHEEL_*）；input[1]：保留 0
} VIPLE_VR_CONTROLLER_INPUT;

// VIPLE_VR_GAZE.flags
#define VIPLE_VR_GAZE_F_SAMPLE_TIME   0x01  // sampleAgeMs 欄位有效（非 0 即有意義）
#define VIPLE_VR_GAZE_F_PREDICTED     0x02  // 樣本為 runtime 預測值（非夾值）
#define VIPLE_VR_GAZE_F_BINOCULAR     0x04  // 雙眼注視點一致（非單眼回退）

typedef struct _VIPLE_VR_GAZE {
    uint16_t yawF16;    // IEEE half，弧度，相對頭部（正 = 往右）
    uint16_t pitchF16;  // IEEE half，弧度，相對頭部（正 = 往上）
    uint8_t  confidence;
    uint8_t  flags;     // VIPLE_VR_GAZE_F_*
    uint16_t sampleAgeMs;   // 樣本年齡（0–65535 ms，以取樣時刻與傳送時刻計算；flags 有 SAMPLE_TIME 才有效；原 reserved）
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
#define VIPLE_VR_FRAME_HEADER_TYPE_GAZE 0x82
#define VIPLE_VR_FRAME_HEADER_GAZE_SIZE 32

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

typedef struct _VIPLE_VR_FRAME_HEADER_GAZE {
    uint8_t  headerType;              // VIPLE_VR_FRAME_HEADER_TYPE_GAZE (0x82)
    uint16_t frameProcessingLatency;  // 1/10 ms
    uint8_t  frameType;               // 同 short header
    uint16_t lastPayloadLen;
    uint8_t  vrFlags;                 // VIPLE_VR_FF_*（注視點時含 VIPLE_VR_FF_FOVEATED）
    uint8_t  layoutEpoch;
    uint32_t echoSampleId;
    uint8_t  renderRot[6];            // smallest-three，48 bit little-endian
    int16_t  renderPos[3];            // 0.1 mm
    // 這一幀的注視點中心（0–65535 對應 [0.0, 1.0]，y 由上緣算）。
    // §VR-FOVEA-TILES：協商出 foveaTiles 時這四欄改放「注視點那塊的左上角在算圖影像裡的像素座標」（x、y；分母＝
    // foveaRender 的寬、高），整數才能讓兩端算出完全相同的裁切範圍。client 的 VIPLE_VR_FRAME_META.foveaCentre 仍是
    // 欄位值／65535，乘回 65535 四捨五入就是原值
    uint16_t foveaCxL;
    uint16_t foveaCyL;
    uint16_t foveaCxR;
    uint16_t foveaCyR;
} VIPLE_VR_FRAME_HEADER_GAZE;

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
#define VIPLE_VR_C2S_FRAME_FEEDBACK    0x07  // VR channel unsequenced，有新收齊的幀才送，最快 100 ms（§ABR-ADAPT）
#define VIPLE_VR_C2S_LATCH             0x08  // VR channel unsequenced，10 Hz
#define VIPLE_VR_C2S_LOSS              0x09  // VR channel unsequenced，送 2 次
#define VIPLE_VR_C2S_LINK_HELLO        0x0A  // reliable，§VR-MULTILINK
#define VIPLE_VR_C2S_NACK              0x0B  // §VR-LINK-REPAIR：只走連線的 DATA（不走 ENet）
#define VIPLE_VR_C2S_HOTSPOT_INFO      0x0C  // reliable，§VR-HOTSPOT-AUTO
#define VIPLE_VR_C2S_MIC               0x0D  // §VR-MIC：VR channel unsequenced，每 20 ms 一則：{u16 seq（LE）, Opus 封包}（48 kHz 單聲道）
#define VIPLE_VR_C2S_RATE              0x0E  // reliable，§VR-RATE-AUTO：要求 host 每幾張送一張（VIPLE_VR_TLV_RATE）
#define VIPLE_VR_C2S_NET_SWITCH        0x0F  // reliable（另在連線上各送一份），§VR-NETSEL-HOLD：頭盔要切網路了（VIPLE_VR_TLV_NET_SWITCH）
#define VIPLE_VR_C2S_RESERVED_MIC      0x7F  // 舊保留值（未使用；新版改用 0x0D）

// S→C subtype
#define VIPLE_VR_S2C_HAPTIC            0x01  // VR channel unsequenced
#define VIPLE_VR_S2C_STATE             0x02  // reliable
#define VIPLE_VR_S2C_STATS             0x03  // VR channel unsequenced，1 Hz
#define VIPLE_VR_S2C_CONFIG_ACK        0x04  // reliable
#define VIPLE_VR_S2C_LAYOUT            0x05  // reliable
#define VIPLE_VR_S2C_REFRESH_START     0x06  // VR channel unsequenced，送 2 次
#define VIPLE_VR_S2C_LINK_READY        0x07  // reliable，§VR-MULTILINK
#define VIPLE_VR_S2C_REPAIR_GONE       0x08  // §VR-LINK-REPAIR：只走連線的 T_S2C
#define VIPLE_VR_S2C_LINK_ADDRS        0x09  // reliable，§VR-MULTILINK-HOTADD：server 目前可廣告的位址完整快照
#define VIPLE_VR_S2C_RATE              0x0B  // reliable，§VR-RATE-AUTO：host 已套用的分頻（VIPLE_VR_TLV_RATE_APPLIED）
#define VIPLE_VR_S2C_NOTICE            0x0A  // reliable，§VR-PADMODE：要在頭盔裡短暫顯示的提示，payload {u8 code}（VIPLE_VR_NOTICE_*）

// NOTICE.code（不認得的代碼略過）
#define VIPLE_VR_NOTICE_PAD_ON_AUTO      1  // 這款遊戲不使用 VR 手把，已切成 Xbox 手把模式
#define VIPLE_VR_NOTICE_PAD_ON_MANUAL    2  // 手動切成 Xbox 手把模式（兩支選單鍵長按 2 秒）
#define VIPLE_VR_NOTICE_PAD_OFF_MANUAL   3  // 手動切回 VR 手把
#define VIPLE_VR_NOTICE_PAD_UNAVAILABLE  4  // 要切成手把模式，但 host 建不了虛擬手把（沒有 ViGEmBus）
#define VIPLE_VR_NOTICE_MIC_UNAVAILABLE  5  // §VR-MIC：host 找不到 Steam Streaming Microphone（主機沒裝 Steam），頭盔麥克風送不過去

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
#define VIPLE_VR_STATE_CODE_HOST_LOCKED            22  // host 停在 Windows 登入／鎖定畫面（2026-10-08；舊 client 顯示成一般錯誤）

typedef struct _VIPLE_VR_TLV_LOSS {
    uint32_t firstLost;  // 幀號（含）
    uint32_t lastLost;   // 幀號（含）
    uint8_t  reason;     // VIPLE_VR_LOSS_*
} VIPLE_VR_TLV_LOSS;

// §VR-RATE-AUTO（2026-11-08）：串流中途切換「每幾張送一張」。SteamVR 與遊戲照協商的更新率跑（driver 的虛擬 vsync
// 不變），host 只編碼、送出每 div 張中的一張；頭盔每張顯示 div 格（轉頭由頭盔的重投影補）。只在 host 端的串流率
// ／div ≥ 45 Hz 時接受。舊 host 不認得 C2S_RATE、不會回 S2C_RATE，client 就維持全速。
#define VIPLE_VR_RATE_REASON_AUTO_DOWN  1  // 自動：網路不穩，降
#define VIPLE_VR_RATE_REASON_AUTO_UP    2  // 自動：試著升回
#define VIPLE_VR_RATE_REASON_MANUAL     3
typedef struct _VIPLE_VR_TLV_RATE {
    uint8_t  div;       // 1＝全速、2＝半速
    uint8_t  reason;    // VIPLE_VR_RATE_REASON_*
    uint16_t seq;       // client 的要求序號（S2C 回帶，對得上才算這次要求的結果）
} VIPLE_VR_TLV_RATE;

typedef struct _VIPLE_VR_TLV_RATE_APPLIED {
    uint8_t  div;         // host 現在的分頻（拒絕時是原本的值）
    uint8_t  accepted;    // 1＝照要求套用、0＝拒絕（串流率太低、不是 PCVR 等）
    uint16_t seq;         // 對應的要求序號
    uint32_t firstFrame;  // 從這個幀號（NV_VIDEO_PACKET.frameIndex）起照新的分頻送
} VIPLE_VR_TLV_RATE_APPLIED;

// §VR-NETSEL-HOLD（2026-11-08）：頭盔的自動網路選擇要暫停／接回家用 Wi-Fi 了。頭盔只有一個無線電，切換的那幾秒熱點也會
// 大量掉包；那不是塞車，host 在 holdMs 內不因丟包或延遲降碼（11-08 實測：接回家用 Wi-Fi 時一路降到 2 Mbps、90 秒才回到 60）。
// ENet 與每條已確認的連線各送一份，host 取最晚的結束時間（重複收到無妨）。舊 host 不認得就忽略。
#define VIPLE_VR_NET_SWITCH_PAUSE_HOME  1  // 暫停家用 Wi-Fi（只留轉接器）
#define VIPLE_VR_NET_SWITCH_RESUME_HOME 2  // 接回家用 Wi-Fi
typedef struct _VIPLE_VR_TLV_NET_SWITCH {
    uint8_t  phase;     // VIPLE_VR_NET_SWITCH_*
    uint8_t  reserved;
    uint16_t holdMs;    // 從收到起這麼久內不因網路訊號降碼（LE；host 端另有上限）
} VIPLE_VR_TLV_NET_SWITCH;

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
    // §VR-PREDICT（2026-10-03，36 B；舊 server 只讀前 32 B）：遊戲算繪姿態相對「該幀實際顯示時的頭部位置」
    // 的時間差，0.1 ms 單位，正值＝落後（SteamVR 預測得不夠遠）、負值＝超前。只算頭部有移動的幀；
    // 這 1 s 樣本不足時 poseLagP50_100us＝VIPLE_VR_POSE_LAG_NONE、poseLagCount＝0。
    int16_t  poseLagP50_100us;
    uint16_t poseLagCount;
    // §VR-BUFFER-REFILL（2026-10-29，44 B；舊 server 只讀前 36 B，舊 client 送 36 B＝這兩欄當 0）：client 開了 §VR-BUFFER
    // （--vr-buffer-frames 1）時，session 開始以來顯示的新串流幀裡，選「最新那張的前一張」（緩衝裡有一張）與「最新那張」
    // （緩衝空了）的累計次數；重複顯示同一張不算。沒開緩衝時兩個都是 0。server 看相鄰兩則的差：一直空著就把虛擬 vsync 的
    // 相位提前一整格（host 多送一張），把緩衝補回來。
    uint32_t bufLag1Total;
    uint32_t bufLag0Total;
} VIPLE_VR_TLV_CLIENT_TIMING;

#define VIPLE_VR_POSE_LAG_NONE 0x7FFF

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

// §VR-HOTSPOT-AUTO（2026-10-30）：頭盔熱點憑證與頻段資訊
#define VIPLE_VR_HOTSPOT_VERSION_1         1

#define VIPLE_VR_HOTSPOT_AUTH_WPA2_PSK     1
#define VIPLE_VR_HOTSPOT_AUTH_WPA3_SAE     2

#define VIPLE_VR_HOTSPOT_BAND_UNKNOWN      0
#define VIPLE_VR_HOTSPOT_BAND_2GHZ         2
#define VIPLE_VR_HOTSPOT_BAND_5GHZ         5
#define VIPLE_VR_HOTSPOT_BAND_6GHZ         6

// §VR-HOTSPOT-AUTO（2026-10-30，102 B，reliable）：頭盔熱點憑證與頻段資訊（C→S）
// 經由已加密的 VR 控制通道（ENet reliable AES-GCM）傳輸。
typedef struct _VIPLE_VR_TLV_HOTSPOT_INFO {
    uint8_t version;       // VIPLE_VR_HOTSPOT_VERSION_1
    uint8_t authType;      // VIPLE_VR_HOTSPOT_AUTH_*（1=WPA2-PSK, 2=WPA3-SAE）
    uint8_t band;          // VIPLE_VR_HOTSPOT_BAND_*（6=6 GHz, 5=5 GHz, 2=2.4 GHz）
    uint8_t channel;       // 頻道編號（例如 37；0＝未知）
    uint8_t ssidLen;       // SSID 長度（<= 32）
    uint8_t keyLen;        // 金鑰長度（<= 64）
    uint8_t ssid[32];      // SSID 字串（以 ssidLen 為準）
    uint8_t key[64];       // 金鑰字串（PSK，以 keyLen 為準）
} VIPLE_VR_TLV_HOTSPOT_INFO;

typedef struct _VIPLE_VR_TLV_HAPTIC {
    uint8_t  device;      // VIPLE_VR_POSE_LEFT／RIGHT
    uint8_t  reserved[3];
    uint32_t durationUs;
    float    frequencyHz;
    float    amplitude;   // 0–1
    uint32_t eventId;
    uint64_t timestampNs; // §VR-HAPTIC-REDUNDANT（2026-10-31，28 B）：原始送出時間（client 單調時鐘 ns，0＝未提供）
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

// ── §VR-MULTILINK（2026-10-06）：多連線 ─────────────────────────────
//
// client 的每張網卡一條連線（link）：兩端各一組專用 UDP socket（影像、音訊各一）。影像與音訊的 RTP
// 封包在每條連線各送一份（位元組與單一路徑時完全相同，接收端靠既有的 RTP 序號去重）；追蹤
// （0x5506）以 VIPLE_VR_LINK_T_DATA 加密後在每條連線各送一份。連線清單走既有的加密控制通道交換：
//   C→S 0x5507/0A LINK_HELLO：{u8 count, VIPLE_VR_LINK_DESC[count], [u8 epoch[count]]}
//   S→C 0x5508/07 LINK_READY：{u8 count, VIPLE_VR_LINK_PORTS[count], [u8 hubCaps]}
//   S→C 0x5508/09 LINK_ADDRS：VIPLE_VR_LINK_ADDRS（§VR-MULTILINK-HOTADD）
// hubCaps（VIPLE_VR_LINK_F_*）是 server 同意啟用的連線層功能；沒有任何功能時不附這個 byte（舊 client 本來就只讀陣列）。
// 只支援 IPv4。位址欄位是網路位元組序的 4 個 byte；其餘整數 little-endian。
#define VIPLE_VR_LINK_MAX        4
#define VIPLE_VR_LINK_MAGIC0     0x56  // 'V'
#define VIPLE_VR_LINK_MAGIC1     0x4C  // 'L'
#define VIPLE_VR_LINK_T_PING     1     // C→S（影像、音訊 socket 都送）
#define VIPLE_VR_LINK_T_PONG     2     // S→C（只在影像 socket 回）
#define VIPLE_VR_LINK_T_DATA     3     // C→S：AES-GCM 加密的控制訊息（0x5506；協商了 F_CTRL 之後也載 0x5507 的 LOSS／LATCH／NACK）
#define VIPLE_VR_LINK_T_S2C      4     // S→C（影像 socket）：AES-GCM 加密的 0x5508 TLV（§VR-LINK-CTRL）；整個資料報必須 < VIPLE_VR_LINK_CTRL_MAX_LEN
// 連線層功能位元：client 放在每個 VIPLE_VR_LINK_DESC.flags（它支援的），server 放在 LINK_READY 尾端的 hubCaps（同意啟用的）
#define VIPLE_VR_LINK_F_CTRL     0x01  // §VR-LINK-CTRL：時間敏感的控制訊息走連線。C→S 的 LOSS／LATCH 放進 DATA（每條已確認的連線各一份），
                                       // S→C 的 REFRESH_START 用 T_S2C。session 位址那條鏈路變弱時，恢復不再被它拖住
#define VIPLE_VR_LINK_F_REPAIR   0x02  // §VR-LINK-REPAIR：補包。C→S NACK（0x5507/0B）、S→C REPAIR_GONE（0x5508/08）；要同時有 F_CTRL
#define VIPLE_VR_LINK_F_REPAIR_RTT 0x04  // §VR-REPAIR-RTT：補包的門檻、等待與重報跟著量到的往返時間走（NACK 的 flags 帶 client 的估計）；
                                         // 要同時有 F_REPAIR。舊的一方不認得這個位元，兩邊就都照原本的規則走
#define VIPLE_VR_LINK_F_HOTADD   0x08  // §VR-MULTILINK-HOTADD：串流中途熱插拔（client 定期掃描網卡、同 id 重綁、死亡 slot 回收）。
                                       // 不依賴 CTRL／REPAIR，可獨立成立；舊的一方不認得這個位元就不做熱插拔
// §VR-LINK-DEDICATED（2026-11-08）：不是功能位元，是這一條連線的屬性——走頭盔自己的串流轉接器熱點（wlanap，專用 6 GHz）。
// server 的「只剩一條連線就壓上限」只套在家用 Wi-Fi，不套在這一條。舊 server 的功能協商只取認得的位元，不受影響
#define VIPLE_VR_LINK_F_DEDICATED 0x10
#define VIPLE_VR_LINK_PING_F_CONFIRMED  0x01  // client 最近 1 s 內在這條連線收過 PONG。§VR-LINK-STANDBY：有區網直連時，
                                             // 走 Tailscale 的那一條即使收得到 PONG 也不帶（server 就不在上面送影像）
#define VIPLE_VR_LINK_READY_OK       0
#define VIPLE_VR_LINK_READY_REFUSED  1
// S→C 影像 socket 上長度小於這個值的資料報是連線控制（PONG），其餘是影像 RTP。
#define VIPLE_VR_LINK_CTRL_MAX_LEN   64
#define VIPLE_VR_LINK_RTT_UNKNOWN    0xFFFF

typedef struct _VIPLE_VR_LINK_DESC {
    uint8_t  linkId;           // 1..VIPLE_VR_LINK_MAX
    uint8_t  flags;            // VIPLE_VR_LINK_F_*：client 支援的連線層功能（每條連線填一樣的值；舊 server 不讀這一欄）
    uint16_t clientVideoPort;
    uint16_t clientAudioPort;
    uint8_t  clientAddr[4];
    uint8_t  serverAddr[4];    // client 配對到的 server 位址（同子網路，或 session 本身連的位址）
} VIPLE_VR_LINK_DESC;

typedef struct _VIPLE_VR_LINK_PORTS {
    uint8_t  linkId;
    uint8_t  status;           // VIPLE_VR_LINK_READY_*
    uint16_t serverVideoPort;
    uint16_t serverAudioPort;
} VIPLE_VR_LINK_PORTS;

// 連線資料報標頭（12 B）。PING／PONG：seq＝ping 序號、arg＝client 微秒時戳的低 32 bit（PONG 原樣帶回）。
// DATA：seq＝每個 session 遞增的訊息序號（同一則訊息在各連線的 seq 相同，server 以它去重並防重放）、
// arg＝0；後面接 tag[16]＋密文（明文＝u16 ptype（LE）＋payload）。IV＝seq（LE32）＋6 個 0＋'C'＋'L'。
// S2C：同樣的格式，方向相反：seq 由 server 發（自己的計數器，整場不歸零）、明文的 ptype＝0x5508、IV 尾端是 'H'＋'L'。
typedef struct _VIPLE_VR_LINK_HDR {
    uint8_t  magic[2];
    uint8_t  type;
    uint8_t  linkId;
    uint32_t seq;
    uint32_t arg;
} VIPLE_VR_LINK_HDR;

// PING 的本體（接在 VIPLE_VR_LINK_HDR 之後）。計數是 session 開始以來的累計值（PING 掉了也不漏算）。
typedef struct _VIPLE_VR_LINK_PING {
    uint8_t  flags;            // VIPLE_VR_LINK_PING_F_*
    uint8_t  burstSeq;         // 每量到一串封包（見 burstMbps）就加一；server 靠它分辨是不是新的量測
    uint16_t rttMs10;          // 最近一次量到的往返時間，0.1 ms；VIPLE_VR_LINK_RTT_UNKNOWN＝未知
    uint32_t rxPkts;           // 這條連線收到的影像封包
    uint32_t rxUsed;           // 其中先到、被佇列採用的
    uint16_t maxGapMs;         // 上一個 PING 以來這條連線影像封包的最大到達間隔
    uint16_t burstMbps;        // 最近一幀（≥ 32 個封包）在這條連線的到達速率，Mbps；65535＝整幀同一瞬間到、快到量不出來；0＝還沒量到。
                               // server 在 vr_multilink=auto 時用它決定要不要在這條連線送影像
} VIPLE_VR_LINK_PING;

// §VR-LINK-REPAIR：client 只回報「正在等的那個 FEC block 手上有哪些 shard」，補哪些、走哪條、補不補都由 server 決定。
// 每一份都是完整的現況（不是差異），掉一份或重複都無妨。結構後面接 have[(total+7)/8]：bit i＝第 i 個 shard 已收到
// （i＝RTP 序號 − 這個 block 的第一個序號；data 在前、parity 在後）。
// total＝0：這個 block 一個封包都還沒收到（client 不知道它有幾個 shard），不帶位元圖，server 用自己的紀錄。
typedef struct _VIPLE_VR_TLV_NACK {
    uint32_t frame;     // 幀號
    uint8_t  block;     // FEC block 序號（0 起）
    uint8_t  flags;     // VIPLE_VR_NACK_F_*｜eta（見下）；沒協商 VIPLE_VR_LINK_F_REPAIR_RTT 時恆為 0
    uint8_t  need;      // 還差幾個 shard 才能還原（total＝0 時填 0）
    uint8_t  total;     // 這個 block 的 shard 總數（data＋parity，≤ 255）
    uint8_t  attempt;   // 這個 block 的第幾份回報（1 起）
} VIPLE_VR_TLV_NACK;

// §VR-REPAIR-RTT：VIPLE_VR_TLV_NACK.flags（只在協商了 VIPLE_VR_LINK_F_REPAIR_RTT 時有意義）
#define VIPLE_VR_NACK_F_RTT         0x80  // 這份回報照 §VR-REPAIR-RTT 的規則送出（client 自己照往返時間決定來不來得及）
#define VIPLE_VR_NACK_ETA_MASK      0x7F  // 同一個 byte 的低 7 bit：client 估的「回報→補包到齊」（毫秒，無條件進位，1～127）

#define VIPLE_VR_GONE_EXPIRED  1  // 太舊，補了也來不及
#define VIPLE_VR_GONE_BUDGET   2  // 補包額度用完（鏈路已經壅塞，不再加碼）
#define VIPLE_VR_GONE_UNKNOWN  3  // server 沒有這個 block 的紀錄（已被覆寫，或是不做 FEC 的超大幀）

// server 補不了某個 block：client 不必再等，照原本的方式放棄並回報 LOSS。
typedef struct _VIPLE_VR_TLV_REPAIR_GONE {
    uint32_t frame;
    uint8_t  block;
    uint8_t  reason;    // VIPLE_VR_GONE_*
} VIPLE_VR_TLV_REPAIR_GONE;

// §VR-MULTILINK-HOTADD：S→C 的 LINK_ADDRS（0x5508/09，reliable）。內容是 server 目前所有可廣告位址的
// 「完整快照」（不是增量）：client 直接覆蓋自己的候選清單，掉一份或亂序都不會累積錯誤。
// addr 是網路位元組序的 IPv4；count 上限 8（與 QUIC_MAX_ALT_PEERS 相同）。舊 client 依 len 跳過這個 subtype。
#define VIPLE_VR_LINK_ADDR_MAX   8
typedef struct _VIPLE_VR_LINK_ADDRS {
    uint8_t count;              // 0..VIPLE_VR_LINK_ADDR_MAX
    uint8_t addr[VIPLE_VR_LINK_ADDR_MAX][4];
} VIPLE_VR_LINK_ADDRS;

// §ABR-ADAPT：每 100 ms 一則（有新收齊的幀才送），列出這段時間收齊的幀。時間都是 client 單調時鐘（µs）。
// 線上是 little-endian。與 LOSS／LATCH 一樣直接 memcpy 主機序（VR 只在 little-endian 平台協商，見檔頭）。
#define VIPLE_VR_FRAME_FEEDBACK_F_DROPPED 0x01  // bit0：這段期間 queue 有丟棄未完成的幀（有缺幀，不代表塞車）
typedef struct _VIPLE_VR_TLV_FRAME_FEEDBACK_HDR {
    uint32_t baseFrame;     // 第一筆紀錄的幀號（線上的 frameIndex）
    uint32_t baseTimeUs;    // 第一筆紀錄「最後一個封包到達時間」的低 32 位元（µs，會繞回，server 用差值）
    uint8_t  count;         // 後面幾筆紀錄（1..VIPLE_VR_FRAME_FEEDBACK_MAX）
    uint8_t  flags;         // bit0：VIPLE_VR_FRAME_FEEDBACK_F_DROPPED
    uint16_t droppedFrames; // 這段期間放棄的幀數（無法重組）
} VIPLE_VR_TLV_FRAME_FEEDBACK_HDR;   // 12 B
typedef struct _VIPLE_VR_FRAME_FEEDBACK_REC {
    uint16_t frameDelta;    // 幀號 − baseFrame
    uint16_t spreadUs;      // 這一幀第一個到最後一個封包的時間差（µs，超過 65535 夾住）
    uint32_t lastArrOffUs;  // 最後一個封包到達時間 − baseTimeUs（µs）
    uint32_t bytes;         // 這一幀實際收到的封包 payload 位元組數（含 FEC 封包、不含重複收到的）
} VIPLE_VR_FRAME_FEEDBACK_REC;       // 12 B
#define VIPLE_VR_FRAME_FEEDBACK_MAX 18   // 12 + 18×12 = 228 B，在 TLV 251 B 內

#pragma pack(pop)

VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_POSE) == 52, pose_52);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_CONTROLLER_INPUT) == 24, input_24);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_GAZE) == 8, gaze_8);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TRACKING) == 232, tracking_232);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_FRAME_HEADER) == VIPLE_VR_FRAME_HEADER_SIZE, frame_header_24);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_FRAME_HEADER_GAZE) == VIPLE_VR_FRAME_HEADER_GAZE_SIZE, frame_header_gaze_32);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TLV_LOSS) == 9, tlv_loss_9);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TLV_LATCH) == 12, tlv_latch_12);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TLV_RATE) == 4, tlv_rate_4);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TLV_RATE_APPLIED) == 8, tlv_rate_applied_8);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TLV_NET_SWITCH) == 4, tlv_net_switch_4);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TLV_REFRESH_START) == 6, tlv_refresh_6);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TLV_CLIENT_TIMING) == 44, tlv_client_timing_44);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TLV_STATE) == 4, tlv_state_4);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TLV_HAPTIC) == 28, tlv_haptic_28);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TLV_STATS) == 40, tlv_stats_40);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_LINK_DESC) == 14, link_desc_14);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_LINK_PORTS) == 6, link_ports_6);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_LINK_HDR) == 12, link_hdr_12);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_LINK_PING) == 16, link_ping_16);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TLV_NACK) == 9, tlv_nack_9);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TLV_REPAIR_GONE) == 6, tlv_repair_gone_6);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_LINK_ADDRS) == 33, link_addrs_33);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TRACKING) <= VIPLE_VR_MAX_CTRL_PAYLOAD, tracking_fits);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TLV_HOTSPOT_INFO) == 102, tlv_hotspot_102);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TLV_HOTSPOT_INFO) <= VIPLE_VR_MAX_CTRL_PAYLOAD, hotspot_fits);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_TLV_FRAME_FEEDBACK_HDR) == 12, tlv_frame_feedback_hdr_12);
VIPLE_VR_STATIC_ASSERT(sizeof(VIPLE_VR_FRAME_FEEDBACK_REC) == 12, tlv_frame_feedback_rec_12);
VIPLE_VR_STATIC_ASSERT(2 + sizeof(VIPLE_VR_TLV_FRAME_FEEDBACK_HDR) + VIPLE_VR_FRAME_FEEDBACK_MAX * sizeof(VIPLE_VR_FRAME_FEEDBACK_REC) <= VIPLE_VR_MAX_CTRL_PAYLOAD, frame_feedback_fits);

// ── 解碼後的每幀 VR metadata（client 內部，不上線）─────────────────

typedef struct _VIPLE_VR_FRAME_META {
    uint8_t  present;       // 1：這一幀帶了 0x81 或 0x82 VR header
    uint8_t  vrFlags;
    uint8_t  layoutEpoch;
    uint8_t  foveated;      // 0x82 header 且帶 VIPLE_VR_FF_FOVEATED 時為 1（原 reserved）
    uint32_t echoSampleId;
    float    renderRot[4];  // x, y, z, w
    float    renderPos[3];  // 公尺（POS_RELATIVE 時是位移）
    float    foveaCentre[2][2]; // [eye][0=x, 1=y]，正規化 [0..1]，y 由上緣算起（foveated == 0 時為 0.5f）
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

// IEEE 754 half-precision float（binary16）轉換工具
VIPLE_VR_INLINE uint16_t VipleVrFloatToHalf(float val) {
    uint32_t f;
    memcpy(&f, &val, sizeof(f));
    uint32_t sign = (f >> 16) & 0x8000;
    uint32_t rawExp = (f >> 23) & 0xFF;
    uint32_t mant = f & 0x007FFFFF;

    if (rawExp == 0xFF) {
        // Inf or NaN
        if (mant != 0) {
            uint16_t nanMant = (uint16_t)(mant >> 13);
            return (uint16_t)(sign | 0x7C00 | (nanMant ? nanMant : 1));
        }
        return (uint16_t)(sign | 0x7C00);
    }

    int32_t exp = (int32_t)rawExp - 127;

    if (exp > 15) {
        // Overflow to infinity
        return (uint16_t)(sign | 0x7C00);
    }
    if (exp < -24) {
        // Underflow to zero
        return (uint16_t)sign;
    }
    if (exp < -14) {
        // Denormalized
        uint32_t fullMant = mant | 0x00800000;
        int shift = 13 + (-14 - exp);
        uint16_t halfMant = (uint16_t)(fullMant >> shift);
        return (uint16_t)(sign | halfMant);
    }

    uint16_t halfExp = (uint16_t)((exp + 15) << 10);
    uint16_t halfMant = (uint16_t)(mant >> 13);
    return (uint16_t)(sign | halfExp | halfMant);
}

VIPLE_VR_INLINE float VipleVrHalfToFloat(uint16_t val) {
    uint32_t sign = ((uint32_t)val & 0x8000) << 16;
    uint32_t exp = (val >> 10) & 0x1F;
    uint32_t mant = val & 0x03FF;
    uint32_t f;

    if (exp == 0) {
        if (mant == 0) {
            f = sign;
        } else {
            // Denormalized half to float
            uint32_t biasedExp = 113;
            while ((mant & 0x0400) == 0) {
                mant <<= 1;
                biasedExp--;
            }
            mant &= 0x03FF;
            f = sign | (biasedExp << 23) | (mant << 13);
        }
    } else if (exp == 0x1F) {
        // Inf / NaN
        f = sign | 0x7F800000 | (mant << 13);
    } else {
        // Normalized
        f = sign | ((exp + 112) << 23) | (mant << 13);
    }

    float res;
    memcpy(&res, &f, sizeof(res));
    return res;
}

#ifdef __cplusplus
}
#endif

#endif // VIPLE_VR_H
