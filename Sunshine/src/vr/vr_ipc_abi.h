/*
 * vr_ipc_abi.h - VipleStream §VR：SteamVR driver（vrserver.exe 內，MSVC /MT）與
 * server（viplestream-server.exe，MinGW UCRT64 GCC）之間的 IPC ABI。
 * 單一來源：Sunshine/src/vr/vr_ipc_abi.h；driver 以相對路徑 include 同一個檔。
 *
 * 規則（違反任何一條都要 VRIPC_ABI_VERSION + 1）：
 *  1. 純 C POD；兩個編譯器都以 VRIPC_STATIC_ASSERT 鎖 sizeof／offsetof。
 *  2. 只用固定寬度整數、float、char／uint16_t 陣列；不用 bool、enum、bitfield、
 *     long double、指標、HANDLE、LUID、wchar_t；不用 #pragma pack。
 *  3. 不同寫入方的區塊各自從 64 B 邊界開始、長度是 64 的倍數。
 *  4. 同步欄位是 8 B 對齊的 uint64_t，只經 vripc_load_acquire_u64／vripc_store_release_u64／
 *     vripc_seq_write_begin（或 C++20 std::atomic_ref<uint64_t>）存取；shm 內不放 std::atomic。
 *     seqlock 寫入端：開頭的奇數 seq 一律用 vripc_seq_write_begin（store 之後加屏障，
 *     payload 的 store 才不會被編譯器提前到奇數 seq 之前），結尾的偶數 seq 用 store_release。
 *     每個 seqlock／ring 只有一個寫入執行緒或臨界區。
 *  5. 時間一律 raw QPC tick（int64），頻率在 header 與 WELCOME；不跨行程傳 steady_clock。
 *  6. 讀對方寫的區塊：整份複製到本地 → 驗證 → 之後只用本地副本（不回讀）。
 *     寫自己的區塊：以本地狀態為準，絕不從 shm 讀回自己寫過的值（對方可以亂寫整個 section）。
 *  7. 凍結區（所有 ABI 版本都不變，讓不同版本的兩端仍能互相 REJECT）：
 *     vripc_msg_hdr_t、vripc_hello_t 的前 24 B、vripc_reject_t。讀 pipe 一律以
 *     VRIPC_PIPE_MAX_MSG 為緩衝，先驗 hdr（bytes ≥ 24、hdr.size == bytes），比完 abi_version
 *     才要求 bytes == 該型別的 sizeof。
 *  8. handle 值一律是「接收方行程內」的值（server 以 DuplicateHandle 放進 vrserver）；
 *     訊息送達即移交所有權，接收方唯一負責關閉。handle 值絕不寫進 log。
 */
#ifndef VIPLESTREAM_VR_IPC_ABI_H
#define VIPLESTREAM_VR_IPC_ABI_H

#include <stddef.h>
#include <stdint.h>

#if !defined(_M_X64) && !defined(__x86_64__)
  #error "vr_ipc_abi.h: x64 only (seqlock relies on x64 TSO and aligned 8-byte atomicity)"
#endif

#ifdef __cplusplus
  #define VRIPC_STATIC_ASSERT(c, m) static_assert(c, m)
extern "C" {
#else
  #define VRIPC_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif

#if defined(_MSC_VER) && !defined(__clang__)
  #include <intrin.h>
  #define VRIPC_COMPILER_BARRIER() _ReadWriteBarrier()
#else
  #define VRIPC_COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")
#endif

/* x64：8 B 對齊的 MOV 是原子的；load 不與先前的 load 重排、store 不與先前的 store 重排（TSO），
 * 所以 acquire/release 只需要擋編譯器。GCC 的 volatile 不排序非 volatile 存取，屏障不可省。 */
static inline uint64_t vripc_load_acquire_u64(const uint64_t *p) {
  uint64_t v = *(const volatile uint64_t *) p;
  VRIPC_COMPILER_BARRIER();
  return v;
}

static inline void vripc_store_release_u64(uint64_t *p, uint64_t v) {
  VRIPC_COMPILER_BARRIER();
  *(volatile uint64_t *) p = v;
}

/* seqlock 寫入開頭（奇數 seq）：先 store、再屏障，後面的 payload store 不會被編譯器提前。
 * C++ 端等同 relaxed store 加 std::atomic_thread_fence(release)。x64 硬體不重排 store。 */
static inline void vripc_seq_write_begin(uint64_t *p, uint64_t v) {
  *(volatile uint64_t *) p = v;
  VRIPC_COMPILER_BARRIER();
}

/* ── 常數 ─────────────────────────────────────────────────────────────── */
#define VRIPC_ABI_VERSION 1u
#define VRIPC_MAGIC_PIPE 0x50525356u /* "VSRP"（little-endian） */
#define VRIPC_MAGIC_SHM 0x4D485356u /* "VSHM" */
#define VRIPC_SHM_SECTION_SIZE 65536u /* server 建立的 section 大小；driver 以 VirtualQuery 驗證 ≥ sizeof(vripc_shm_t) */
#define VRIPC_PIPE_MAX_MSG 512u
#define VRIPC_PIPE_NAME_PREFIX L"\\\\.\\pipe\\VipleStreamVR-" /* 後接十進位 session id */

#define VRIPC_TRK_SLOTS 8u
#define VRIPC_FRM_SLOTS 8u
#define VRIPC_TEX_COUNT 3u
#define VRIPC_HAPTIC_SLOTS 32u
#define VRIPC_TIMING_SLOTS 512u
#define VRIPC_LOG_SLOTS 64u
#define VRIPC_LOG_TEXT 240u
#define VRIPC_IFACE_CHARS 48u
#define VRIPC_DXGI_FORMAT_B8G8R8A8_UNORM 87u /* ring 貼圖唯一允許的格式 */

/* pipe 訊息型別 */
#define VRIPC_MSG_HELLO 1u /* driver → server */
#define VRIPC_MSG_WELCOME 2u /* server → driver：shm 與兩個 event 的 handle */
#define VRIPC_MSG_REJECT 3u /* 雙向 */
#define VRIPC_MSG_TEXTURES 4u /* server → driver：ring 貼圖 ×3 與兩個 fence 的 handle（本 generation 有 config 時） */
#define VRIPC_MSG_READY 5u /* driver → server：已在同一張卡開好 TEXTURES 的物件 */
#define VRIPC_MSG_STATE 6u /* 雙向 */
#define VRIPC_MSG_BYE 7u /* 雙向 */

/* REJECT.reason */
#define VRIPC_REJ_ABI_MISMATCH 1u
#define VRIPC_REJ_IDENTITY 2u
#define VRIPC_REJ_BAD_MESSAGE 3u
#define VRIPC_REJ_ADAPTER_MISMATCH 4u /* driver：TEXTURES 的 LUID ≠ 已建立的 direct-mode device */
#define VRIPC_REJ_TEXTURE_INVALID 5u /* driver：開啟失敗或 GetDesc 不符（防呆） */
#define VRIPC_REJ_BUSY 6u
#define VRIPC_REJ_VR_DISABLED 7u
#define VRIPC_REJ_TIMEOUT 8u
#define VRIPC_REJ_GENERATION 9u

/* BYE.reason */
#define VRIPC_BYE_NORMAL 0u
#define VRIPC_BYE_RECONFIG 1u /* session 設定改變：driver 立刻重連（不退避），新 generation */
#define VRIPC_BYE_SERVER_SHUTDOWN 2u
#define VRIPC_BYE_DRIVER_CLEANUP 3u
#define VRIPC_BYE_PROTOCOL_ERROR 4u
#define VRIPC_BYE_DEVICE_LOST 5u /* 任一方偵測到對方的 fence 為 UINT64_MAX 或自己的 device removed；立即重連 */

/* STATE.kind：server → driver 用 0x01–0x3F，driver → server 用 0x81–0xBF */
#define VRIPC_ST_ARM 0x01u
#define VRIPC_ST_DISARM 0x02u
#define VRIPC_ST_REQUEST_QUIT 0x03u /* 只在我們的 HMD 存在時：driver 在 RunFrame 以 HMD index 送 VREvent_DriverRequestedQuit */
#define VRIPC_ST_DEV_SET_V2P 0x04u /* 只在 VRIPC_WF_DEV_PACING：arg = vsync_to_photons µs，driver 不重連直接改屬性（PoC-10 E7） */
#define VRIPC_ST_SET_V2P 0x05u /* §VR-PREDICT（2.0.0）：arg = vsync_to_photons µs（1000–200000，超出範圍 driver 忽略），不需 dev mode、
                                  driver 不重連直接改 HMD 屬性。舊 driver 當未知 kind 忽略 */
#define VRIPC_ST_HMD_ADDED 0x81u /* TrackedDeviceAdded(HMD) 回 true */
#define VRIPC_ST_HMD_ADD_REJECTED 0x82u /* TrackedDeviceAdded(HMD) 回 false */
#define VRIPC_ST_HMD_ACTIVATED 0x83u /* Activate() 被呼叫；driver_status.act_* 此時已寫好 */
#define VRIPC_ST_HMD_PRESENTING 0x84u /* ARM 後第一次合成並發布成功 = HMD_ACTIVE */
#define VRIPC_ST_HMD_STANDBY 0x85u
#define VRIPC_ST_DEVICE_LOST 0x86u /* arg = HRESULT */
#define VRIPC_ST_EXITING 0x87u /* IsExiting() 或 Cleanup() */
#define VRIPC_ST_IFACE_UNSUPPORTED 0x88u /* GetComponent 被要求未知版本；arg = 版本號（例 IVRDriverDirectModeComponent_010 → 10） */
#define VRIPC_ST_DEGRADED 0x89u /* 回呼內攔到例外，driver 停止合成；arg = 例外碼低 32 bit */

/* header.server_state */
#define VRIPC_SRV_INIT 0u
#define VRIPC_SRV_READY 1u
#define VRIPC_SRV_CLOSING 2u

/* driver_status.driver_state */
#define VRIPC_DRV_MAPPED 1u
#define VRIPC_DRV_READY 3u /* TEXTURES 已開好、READY 已送 */
#define VRIPC_DRV_HMD_ADDED 4u
#define VRIPC_DRV_HMD_PRESENTING 5u
#define VRIPC_DRV_STANDBY 6u
#define VRIPC_DRV_DEVICE_LOST 7u
#define VRIPC_DRV_DEGRADED 8u

/* driver_status.other_hmd */
#define VRIPC_OTHER_HMD_UNKNOWN 0u
#define VRIPC_OTHER_HMD_NONE 1u
#define VRIPC_OTHER_HMD_PRESENT 2u /* device 0 存在而且不是我們 */
#define VRIPC_OTHER_HMD_OURS 3u

/* HELLO.driver_caps */
#define VRIPC_DCAP_VSYNC_EVENTS 0x1u
#define VRIPC_DCAP_TIMING_RING 0x2u
#define VRIPC_DCAP_PEER 0x80000000u /* vr_probe ipcpeer（不是 SteamVR driver）；只供 log 與 selftest 判讀 */

/* WELCOME.welcome_flags */
#define VRIPC_WF_DEV_PACING 0x1u /* 只在 selftest：允許 pacing.mode ≠ 0、config.dev_flags、STATE DEV_SET_V2P */

/* config.dev_flags（只在 VRIPC_WF_DEV_PACING 時生效） */
#define VRIPC_DEV_SENDS_VSYNC_EVENTS 0x1u /* 設 Prop_DriverDirectModeSendsVsyncEvents_Bool=true（PoC-10 E2/E3） */
#define VRIPC_DEV_SYS_LAYER_OWN_POSES 0x2u /* Prop 2096 設 false（預設 true；P-B2 對照，guard preset 同時翻設定鍵） */
#define VRIPC_DEV_DRIVER_CONTROLS_TEX_INDEX 0x4u /* Prop 2116 設 false（預設 true；P-B2 對照） */
#define VRIPC_DEV_CHAPERONE_OFFSET 0x8u /* chaperone JSON 的 standing 改成 yaw 30°、translation (0.5, 0, -0.3)（G-DRV-3'） */
#define VRIPC_DEV_REPORT_DROPPED 0x10u /* GetFrameTiming 每 90 幀故意回報一次 m_nNumDroppedFrames=1（PoC-10 E5b） */
#define VRIPC_DEV_FAULT_INJECT 0x20u /* 在 SubmitLayer／Present／RunFrame 各製造一次 AV（T7；由 config.dev_arg 選哪一個） */

/* config.pose_flags（2026-10-05，由原本的 reserved2 拆出：版面不變、不升 ABI 版號；0＝舊行為，舊 driver 讀不到這個欄位） */
#define VRIPC_POSE_F_CTRL_OFFSET 0x1u /* §VR-CTRL-OFFSET：控制器也設 poseTimeOffset（與 HMD 同一個目標時間，上限 ctrl_extrap_cap_us） */
#define VRIPC_POSE_F_STALE_HOLD 0x2u /* §VR-STALE-HOLD：短空窗（2T～stale_oor_ctrl_us，預設 100 ms）保留速度與 poseTimeOffset，不歸零 */
#define VRIPC_POSE_F_ANGVEL_LOCAL 0x4u /* §VR-ANGVEL-LOCAL：角速度轉成機體座標再交給 SteamVR（SteamVR 把 vecAngularVelocity 當本地座標，
                                         10-05 vr_probe --mode predict 實測；client／OpenXR 給的是世界座標） */

/* frame descriptor flags：低 8 bit 與 0x81 vrFlags 同值；server 轉送時只取 & 0x2F */
#define VRIPC_FRM_POSE_VALID 0x01u
#define VRIPC_FRM_ECHO_MATCHED 0x02u
#define VRIPC_FRM_POSE_FALLBACK 0x04u
#define VRIPC_FRM_REPEATED 0x08u
#define VRIPC_FRM_DERIVED_POSE 0x20u
#define VRIPC_FRM_SPACE_DELTA 0x100u /* 診斷：renderPose 已套 space-delta 修正 */

/* pacing.pacing_flags／pacing.mode */
#define VRIPC_PF_ALLOW_SNAP 0x1u /* 這個 epoch 允許一次性相位對齊 */
#define VRIPC_PF_HAS_ANCHOR 0x2u /* anchor_qpc 有效；沒有就只跟週期、不追相位 */
#define VRIPC_PM_PRODUCTION 0u /* E1：PostPresent 睡到虛擬 vsync */
#define VRIPC_PM_E0_FREE 1u /* PostPresent 立即返回 */
#define VRIPC_PM_E2_VSYNC_EVT 2u /* vsync 執行緒每 T 呼叫 VsyncEvent(offset)，PostPresent 立即返回 */
#define VRIPC_PM_E3_BOTH 3u /* E1 + E2 並用 */
#define VRIPC_PACING_PERIOD_TOL_PCT 5u /* driver 只接受 [0.95, 1.05] × 1/Hz(Activate 時) 的週期 */
#define VRIPC_PACING_SLEW_PPM_MAX 2000u /* driver 只接受 slew_ppm_max ≤ 這個值 */

/* timing ring event */
#define VRIPC_TE_VSYNC_VIRTUAL 1u /* arg0 = 目前有效週期（ns），arg1 = 這次跳過的 vsync 數 */
#define VRIPC_TE_SUBMIT_LAYER 2u /* arg0 = layer index，arg1 = 預測秒數×1e6 */
#define VRIPC_TE_PRESENT_ENTER 3u
#define VRIPC_TE_PRESENT_EXIT 4u /* arg0 = 0 成功／1 AcquireSync 逾時／2 無 slot／3 格式／4 無輸出目標（idle 或 TEARDOWN） */
#define VRIPC_TE_POSTPRESENT_ENTER 5u /* arg0 = nFramesToThrottle，arg1 = nAdditionalFramesToPredict */
#define VRIPC_TE_POSTPRESENT_EXIT 6u /* arg0 = 睡眠 µs */
#define VRIPC_TE_GETFRAMETIMING 7u /* arg0 = 傳入的 m_nReprojectionFlags，arg1 = 回報的 dropped */
#define VRIPC_TE_VSYNC_EVENT 8u /* arg0 = offset µs */
#define VRIPC_TE_COMPOSE_DONE 9u /* arg0 = slot，arg1 = fence 值 */
#define VRIPC_TE_POSE_UPDATED 10u /* arg0 = sampleId，arg1 = poseTimeOffset µs */
#define VRIPC_TE_STALE 11u /* arg0 = 1 速度歸零／2 OutOfRange／3 invalid */
#define VRIPC_TE_PHASE_SNAP 12u /* arg0 = 跳動 ns */
#define VRIPC_TE_PACING_REJECTED 13u /* arg0 = 收到的週期（ns），arg1 = 收到的 slew ppm；改用 Activate 時的 1/Hz */
#define VRIPC_TE_NEXT_INDEX 14u /* GetNextSwapTextureSetIndex 被呼叫（P-B2 的 2116 生效證據） */

/* ── pipe 訊息（message mode，每則固定長度；hdr.size 必須等於該型別的 sizeof；讀取規則見檔頭第 7 條） ── */
typedef struct vripc_msg_hdr_t { /* 凍結 */
  uint32_t magic; /* VRIPC_MAGIC_PIPE */
  uint16_t type; /* VRIPC_MSG_* */
  uint16_t flags; /* 0 */
  uint32_t size; /* 含 header */
  uint32_t seq; /* 每個方向從 1 起單調遞增 */
} vripc_msg_hdr_t;

typedef struct vripc_hello_t { /* driver → server，連線後第一則；前 24 B 凍結 */
  vripc_msg_hdr_t hdr;
  uint32_t abi_version;
  uint32_t driver_pid; /* 只供 log；身分以 GetNamedPipeClientProcessId 為準 */
  uint32_t driver_version_packed; /* driver_version.h 的 VIPLE_DRIVER_VERSION_PACKED */
  uint32_t shm_struct_size; /* driver 編譯出的 sizeof(vripc_shm_t) */
  uint64_t reserved_mb; /* 0；原本的 driver_module_base 已刪（模組檢查沒有安全價值，且需要 PROCESS_VM_READ） */
  uint32_t driver_caps; /* VRIPC_DCAP_* */
  uint32_t openvr_sdk_packed; /* (major<<24)|(minor<<16)|build，2.15.6 */
  char iface_directmode[VRIPC_IFACE_CHARS]; /* "IVRDriverDirectModeComponent_009"，NUL 結尾 */
  uint64_t reserved[2];
} vripc_hello_t;

typedef struct vripc_welcome_t { /* server → driver */
  vripc_msg_hdr_t hdr;
  uint32_t abi_version;
  uint32_t server_pid; /* 只供 log */
  uint64_t generation; /* server 行程內每次成功握手 +1（從 1 起） */
  uint64_t shm_size; /* section 大小 */
  int64_t qpc_frequency;
  uint32_t adapter_luid_low; /* server（NVENC）使用的 adapter；driver 必須在同一張卡建 device */
  int32_t adapter_luid_high;
  uint64_t dup_shm_handle; /* vrserver 內的 handle 值（FILE_MAP_READ|FILE_MAP_WRITE） */
  uint64_t dup_evt_trk; /* SYNCHRONIZE */
  uint64_t dup_evt_frm; /* EVENT_MODIFY_STATE */
  uint32_t server_version_packed;
  uint32_t welcome_flags; /* VRIPC_WF_* */
  uint64_t reserved[2];
} vripc_welcome_t;

typedef struct vripc_reject_t { /* 凍結 */
  vripc_msg_hdr_t hdr;
  uint32_t reason; /* VRIPC_REJ_* */
  uint32_t detail; /* 例：對方的 abi_version、HRESULT */
} vripc_reject_t;

typedef struct vripc_textures_t { /* server → driver；本 generation 有 config 時送一次（idle generation 不送） */
  vripc_msg_hdr_t hdr;
  uint64_t generation;
  uint32_t adapter_luid_low; /* server 建立這些物件的 adapter（== config 的 LUID）；driver 的 direct-mode device 必須在同一張卡 */
  int32_t adapter_luid_high;
  uint32_t width; /* == config.packed_width */
  uint32_t height; /* == config.packed_height */
  uint32_t dxgi_format; /* == VRIPC_DXGI_FORMAT_B8G8R8A8_UNORM */
  uint32_t tex_count; /* == VRIPC_TEX_COUNT */
  uint64_t tex_handle[VRIPC_TEX_COUNT]; /* vrserver 行程內的 NT handle 值（server 以 DuplicateHandle 放入；driver 開完即關） */
  uint64_t shared_fence_handle; /* driver GPU Signal、server CPU 觀察 */
  uint64_t consumed_fence_handle; /* server GPU Signal、driver CPU 觀察 */
  uint64_t shared_fence_initial; /* 建立時的值（0）；第一幀的 fence_value 必須 > 它 */
} vripc_textures_t;

typedef struct vripc_ready_t { /* driver → server：TEXTURES 的物件已在 direct-mode device 上開好、GetDesc 相符 */
  vripc_msg_hdr_t hdr;
  uint64_t generation;
} vripc_ready_t;

typedef struct vripc_state_t {
  vripc_msg_hdr_t hdr;
  uint64_t generation;
  uint32_t kind; /* VRIPC_ST_* */
  uint32_t arg;
} vripc_state_t;

typedef struct vripc_bye_t {
  vripc_msg_hdr_t hdr;
  uint32_t reason; /* VRIPC_BYE_* */
  uint32_t detail;
} vripc_bye_t;

/* ── 共享記憶體 ───────────────────────────────────────────────────────── */
typedef struct vripc_shm_header_t { /* 寫入方：server（WELCOME 前寫好；armed/state/heartbeat 之後更新） */
  uint32_t magic; /* VRIPC_MAGIC_SHM */
  uint32_t abi_version;
  uint32_t shm_size;
  uint32_t struct_size; /* sizeof(vripc_shm_t) */
  uint64_t generation;
  int64_t qpc_frequency;
  uint32_t server_pid;
  uint32_t armed; /* 0/1；driver 在 Init 同步握手時以這裡為初值，之後以 STATE ARM/DISARM 為準 */
  uint32_t server_state; /* VRIPC_SRV_* */
  uint32_t reserved0;
  int64_t server_heartbeat_qpc; /* 每 100 ms */
  uint64_t reserved1;
} vripc_shm_header_t;

typedef struct vripc_driver_status_t { /* 寫入方：driver；server 整份複製、驗證後只用於診斷、衝突判斷與 K20 參數比對（同使用者可控，只供參考） */
  uint64_t generation_ack;
  uint32_t driver_pid;
  uint32_t driver_state; /* VRIPC_DRV_* */
  int64_t driver_heartbeat_qpc; /* 每 100 ms */
  uint64_t frames_presented; /* Present 被呼叫次數 */
  uint64_t frames_composited; /* 成功發布的幀 */
  uint64_t drop_noslot; /* consumedFence 沒放出 slot */
  uint64_t drop_acquire_timeout; /* sync texture AcquireSync 逾時 */
  uint64_t drop_format; /* app 貼圖格式不在白名單 */
  uint64_t stale_pose_count; /* > 2T 沒新樣本、速度歸零的次數 */
  uint64_t outofrange_count; /* > 100 ms 標 OutOfRange 的次數 */
  uint64_t posehist_miss; /* scene layer 找不到對應樣本 */
  int64_t last_vsync_qpc; /* 最近一次虛擬 vsync */
  uint32_t swapsets_live;
  uint32_t last_layer_count;
  int32_t space_delta_mdeg; /* 目前 space-delta 的旋轉角（毫度） */
  uint32_t other_hmd; /* VRIPC_OTHER_HMD_* */
  char other_hmd_system[16]; /* device 0 的 Prop_TrackingSystemName；server 強制 NUL 結尾、只留可列印字元、'[' 換成 '?' */
  /* HMD 已 Activate 的不可變參數（K20）；act_refresh_mhz == 0 表示本 vrserver 生命週期還沒有我們的 HMD */
  uint32_t act_refresh_mhz;
  uint32_t act_eye_width;
  uint32_t act_eye_height;
  uint32_t act_luid_low; /* direct-mode device 所在的 adapter */
  int32_t act_luid_high;
  uint32_t degraded_code; /* 0 = 正常；否則為攔到的例外碼低 32 bit（degraded 後停止合成） */
  uint64_t next_index_calls; /* GetNextSwapTextureSetIndex 呼叫數（P-B2：2116 生效證據） */
  uint64_t drop_unknown_layer; /* SubmitLayer 的 handle 不在 swap set map、或超過 16 層 */
  uint32_t max_layer_count; /* 目前看過的最大 layer 數 */
  uint32_t reserved0;
  uint64_t reserved1[2];
} vripc_driver_status_t;

typedef struct vripc_session_config_t { /* 寫入方：server，WELCOME 前寫一次，同一 generation 內不變 */
  uint32_t eye_width; /* 0 = 沒有 session 設定（idle），driver 不送 TEXTURES */
  uint32_t eye_height;
  uint32_t packed_width; /* sbs：2 × eye_width */
  uint32_t packed_height;
  uint32_t dxgi_format; /* VRIPC_DXGI_FORMAT_B8G8R8A8_UNORM */
  uint32_t refresh_mhz; /* Hz × 1000 */
  uint64_t period_ns; /* /launch vrPeriodNs（client 時鐘） */
  float fov_tan[2][4]; /* 每眼 left, right, up, down 的 tan（OpenXR 慣例：left/down 為負）；已含 overscan */
  float eye_to_head[2][7]; /* 每眼 pos xyz（公尺）+ quat xyzw */
  float ipd_m;
  float overscan_deg; /* 只供 log：fov_tan 已含 overscan，driver 不再套用 */
  uint32_t controller_profile; /* 0 touch（M1b 唯一值） */
  uint32_t vr_caps; /* /launch vrCaps */
  uint64_t universe_id; /* 值必須 ≤ UINT32_MAX（chaperone JSON 的 universeID 是 uint32）；M1b 固定 0x5649504C */
  uint32_t adapter_luid_low;
  int32_t adapter_luid_high;
  uint16_t space_epoch;
  uint8_t layout_epoch;
  uint8_t dev_arg; /* VRIPC_DEV_FAULT_INJECT：1 SubmitLayer、2 Present、3 RunFrame；其他情況 0 */
  uint32_t dev_flags; /* VRIPC_DEV_*，只在 VRIPC_WF_DEV_PACING 時生效 */
  uint16_t audio_endpoint_id[128]; /* MMDevice endpoint ID，UTF-16、NUL 結尾 */
  uint32_t vsync_to_photons_us; /* Prop_SecondsFromVsyncToPhotons_Float */
  uint32_t hmd_extrap_cap_us; /* poseTimeOffset 上限（HMD） */
  uint32_t ctrl_extrap_cap_us; /* poseTimeOffset 上限（控制器） */
  uint32_t stale_oor_ctrl_us; /* 控制器 OutOfRange 門檻，預設 100000 */
  uint32_t stale_zero_vel_us; /* 2T */
  uint32_t stale_oor_hmd_us; /* HMD OutOfRange（灰畫面）門檻，預設 1000000 */
  uint32_t pose_flags; /* VRIPC_POSE_F_*；0＝舊行為 */
  uint32_t reserved2;
} vripc_session_config_t;

typedef struct vripc_pacing_t { /* 寫入方：server；單格 seqlock（seq 偶數 = 完成）；idle generation 也必須寫有效週期。
                                  driver 把它當不可信輸入：週期或 slew 超出 VRIPC_PACING_* 就改用 1/Hz(Activate 時) */
  uint64_t seq;
  uint64_t period_q32; /* 虛擬 vsync 週期：QPC ticks × 2^32（已含時鐘 skew 修正） */
  int64_t anchor_qpc; /* 期望的 vsync 時刻（VRIPC_PF_HAS_ANCHOR 時有效） */
  uint32_t slew_ppm_max; /* 穩態 200 */
  uint32_t pacing_flags; /* VRIPC_PF_* */
  int32_t vsync_event_offset_us; /* E2：VsyncEvent 的參數 */
  uint32_t mode; /* VRIPC_PM_*；非 0 只在 selftest */
  uint32_t epoch; /* 每次 server 改 anchor 策略 +1 */
  uint32_t reserved0;
  uint64_t reserved1;
  uint64_t reserved2;
} vripc_pacing_t;

typedef struct vripc_pose_t {
  float pos[3];
  float rot[4]; /* x, y, z, w */
  float lin_vel[3];
  float ang_vel[3];
} vripc_pose_t;

typedef struct vripc_ctrl_input_t { /* 與 0x5506 input 區塊逐欄相同 */
  uint32_t buttons;
  uint32_t touches;
  uint32_t press_ctr;
  uint16_t trigger;
  uint16_t grip;
  int16_t stick_x;
  int16_t stick_y;
  uint8_t battery;
  uint8_t flags; /* b0 active、b1 focused */
  uint8_t profile; /* VIPLE_VR_CTRL_PROFILE_*（M4a 收尾；原 reserved 低位元組，位移不變、舊端為 0＝未知） */
  uint8_t reserved;
} vripc_ctrl_input_t;

/* vripc_ctrl_input_t.profile：與 VipleVr.h 的 VIPLE_VR_CTRL_PROFILE_* 同值（driver 不 include common-c） */
#define VRIPC_CTRL_PROFILE_UNKNOWN 0u
#define VRIPC_CTRL_PROFILE_TOUCH 1u
#define VRIPC_CTRL_PROFILE_INDEX 2u
#define VRIPC_CTRL_PROFILE_FRAME 3u
#define VRIPC_CTRL_PROFILE_OTHER 4u

typedef struct vripc_tracking_slot_t { /* 寫入方：server（bridge 的 writer mutex 內）；per-slot seqlock */
  uint64_t seq; /* 2*idx+1 寫入中、2*idx+2 完成（idx 編進 seq 防 ABA） */
  uint32_t sample_id;
  uint16_t space_epoch;
  uint8_t flags; /* 0x5506 flags */
  uint8_t reserved0;
  uint64_t sample_time_ns; /* client 時鐘，只供診斷 */
  int64_t arrival_qpc;
  int64_t target_server_qpc; /* sampleTime + predictNs + offset，已換成 server QPC */
  uint32_t predict_ns;
  uint32_t reserved1;
  vripc_pose_t pose[3]; /* HMD、L、R */
  vripc_ctrl_input_t input[2];
  uint32_t reserved2;
  uint16_t gaze_yaw_f16;
  uint16_t gaze_pitch_f16;
  uint8_t gaze_conf;
  uint8_t gaze_flags;
  uint16_t reserved3;
  uint8_t reserved4[56];
} vripc_tracking_slot_t;

typedef struct vripc_tracking_ring_t {
  uint64_t write_index; /* 寫入方：server；已完成的樣本數（下一個 idx） */
  uint8_t pad0[56];
  vripc_tracking_slot_t slot[VRIPC_TRK_SLOTS];
} vripc_tracking_ring_t;

typedef struct vripc_frame_desc_t { /* 寫入方：driver（Present 內，持 direct_mode 的鎖）；per-slot seqlock */
  uint64_t seq;
  uint64_t frame_id; /* 單調遞增 */
  uint64_t generation; /* 必須等於本 generation */
  uint64_t fence_value; /* sharedFence 要到達的值；嚴格遞增 */
  uint32_t tex_idx; /* < VRIPC_TEX_COUNT */
  uint32_t flags; /* VRIPC_FRM_* */
  uint32_t echo_sample_id;
  uint16_t space_epoch;
  uint8_t layout_epoch;
  uint8_t reserved0;
  float render_pos[3]; /* client 空間 */
  float render_rot[4];
  uint32_t layer_count;
  int64_t t_target_qpc; /* SubmitLayer 當下 + flHmdPosePredictionTimeInSecondsFromNow */
  int64_t present_qpc; /* Present() 進入時 */
  int64_t submit_qpc; /* Signal + Flush 之後、SetEvent 之前 */
  uint32_t compose_gpu_us; /* 抽樣的 GPU timestamp；0 = 沒量 */
  uint32_t reserved1;
  uint64_t reserved2[2];
} vripc_frame_desc_t;

typedef struct vripc_frame_ring_t {
  uint64_t write_index; /* 寫入方：driver */
  uint8_t pad0[56];
  vripc_frame_desc_t slot[VRIPC_FRM_SLOTS];
} vripc_frame_ring_t;

typedef struct vripc_consume_status_t { /* 寫入方：server；只供診斷（driver 以 consumedFence 為準） */
  uint64_t last_consumed_fence;
  uint64_t last_consumed_frame_id;
  uint64_t frames_copied;
  uint64_t frames_skipped_latest;
  uint64_t frames_fence_timeout;
  uint64_t frames_invalid;
  uint64_t frames_device_lost;
  uint64_t reserved;
} vripc_consume_status_t;

typedef struct vripc_haptic_evt_t {
  uint32_t device; /* 1 L、2 R */
  uint32_t duration_us;
  float frequency_hz;
  float amplitude;
  int64_t qpc;
} vripc_haptic_evt_t;

typedef struct vripc_haptic_ring_t { /* SPSC：driver 寫 head、server 寫 tail */
  uint64_t head;
  uint8_t pad0[56];
  uint64_t tail;
  uint8_t pad1[56];
  vripc_haptic_evt_t evt[VRIPC_HAPTIC_SLOTS];
} vripc_haptic_ring_t;

typedef struct vripc_timing_rec_t {
  int64_t qpc;
  uint32_t frame_id; /* 低 32 bit */
  uint16_t event; /* VRIPC_TE_* */
  uint16_t flags;
  int64_t arg0;
  int64_t arg1;
} vripc_timing_rec_t;

typedef struct vripc_timing_ring_t { /* SPSC：driver 寫 head、server 寫 tail */
  uint64_t head;
  uint8_t pad0[56];
  uint64_t tail;
  uint8_t pad1[56];
  vripc_timing_rec_t rec[VRIPC_TIMING_SLOTS];
} vripc_timing_ring_t;

typedef struct vripc_log_line_t {
  int64_t qpc;
  uint32_t level; /* 0 info、1 warning、2 error */
  uint32_t len; /* server 夾到 < VRIPC_LOG_TEXT，並過濾非可列印字元與 '[' */
  char text[VRIPC_LOG_TEXT];
} vripc_log_line_t;

typedef struct vripc_log_ring_t { /* SPSC：driver 寫 head、server 寫 tail */
  uint64_t head;
  uint8_t pad0[56];
  uint64_t tail;
  uint8_t pad1[56];
  vripc_log_line_t line[VRIPC_LOG_SLOTS];
} vripc_log_ring_t;

typedef struct vripc_shm_t {
  vripc_shm_header_t header; /* server */
  vripc_driver_status_t driver; /* driver */
  vripc_session_config_t config; /* server */
  vripc_pacing_t pacing; /* server */
  vripc_tracking_ring_t tracking; /* server */
  vripc_frame_ring_t frames; /* driver */
  vripc_consume_status_t consume; /* server */
  vripc_haptic_ring_t haptic; /* driver head / server tail */
  vripc_timing_ring_t timing; /* driver head / server tail */
  vripc_log_ring_t log; /* driver head / server tail */
} vripc_shm_t;

/* ── 版面鎖定（兩個編譯器都要過） ───────────────────────────────────────── */
VRIPC_STATIC_ASSERT(sizeof(vripc_msg_hdr_t) == 16, "msg_hdr");
VRIPC_STATIC_ASSERT(sizeof(vripc_hello_t) == 112, "hello");
VRIPC_STATIC_ASSERT(offsetof(vripc_hello_t, driver_version_packed) == 24, "hello frozen prefix");
VRIPC_STATIC_ASSERT(offsetof(vripc_hello_t, reserved_mb) == 32, "hello.reserved_mb");
VRIPC_STATIC_ASSERT(offsetof(vripc_hello_t, iface_directmode) == 48, "hello.iface");
VRIPC_STATIC_ASSERT(sizeof(vripc_welcome_t) == 104, "welcome");
VRIPC_STATIC_ASSERT(offsetof(vripc_welcome_t, dup_shm_handle) == 56, "welcome.dup");
VRIPC_STATIC_ASSERT(sizeof(vripc_reject_t) == 24, "reject");
VRIPC_STATIC_ASSERT(sizeof(vripc_textures_t) == 96, "textures");
VRIPC_STATIC_ASSERT(offsetof(vripc_textures_t, tex_handle) == 48, "textures.tex");
VRIPC_STATIC_ASSERT(sizeof(vripc_ready_t) == 24, "ready");
VRIPC_STATIC_ASSERT(sizeof(vripc_state_t) == 32, "state");
VRIPC_STATIC_ASSERT(sizeof(vripc_bye_t) == 24, "bye");
VRIPC_STATIC_ASSERT(sizeof(vripc_welcome_t) <= VRIPC_PIPE_MAX_MSG, "welcome fits");

VRIPC_STATIC_ASSERT(sizeof(vripc_shm_header_t) == 64, "header");
VRIPC_STATIC_ASSERT(sizeof(vripc_driver_status_t) == 192, "driver_status");
VRIPC_STATIC_ASSERT(offsetof(vripc_driver_status_t, act_refresh_mhz) == 128, "driver_status.act");
VRIPC_STATIC_ASSERT(offsetof(vripc_driver_status_t, other_hmd_system) == 112, "driver_status.other");
VRIPC_STATIC_ASSERT(sizeof(vripc_session_config_t) == 448, "config");
VRIPC_STATIC_ASSERT(offsetof(vripc_session_config_t, audio_endpoint_id) == 160, "config.audio");
VRIPC_STATIC_ASSERT(offsetof(vripc_session_config_t, vsync_to_photons_us) == 416, "config.v2p");
VRIPC_STATIC_ASSERT(offsetof(vripc_session_config_t, stale_oor_hmd_us) == 436, "config.oor_hmd");
VRIPC_STATIC_ASSERT(offsetof(vripc_session_config_t, pose_flags) == 440, "config.pose_flags");
VRIPC_STATIC_ASSERT(sizeof(vripc_pacing_t) == 64, "pacing");
VRIPC_STATIC_ASSERT(sizeof(vripc_pose_t) == 52, "pose");
VRIPC_STATIC_ASSERT(sizeof(vripc_ctrl_input_t) == 24, "ctrl_input");
VRIPC_STATIC_ASSERT(sizeof(vripc_tracking_slot_t) == 320, "tracking_slot");
VRIPC_STATIC_ASSERT(offsetof(vripc_tracking_slot_t, pose) == 48, "tracking_slot.pose");
VRIPC_STATIC_ASSERT(offsetof(vripc_tracking_slot_t, input) == 204, "tracking_slot.input");
VRIPC_STATIC_ASSERT(sizeof(vripc_frame_desc_t) == 128, "frame_desc");
VRIPC_STATIC_ASSERT(offsetof(vripc_frame_desc_t, t_target_qpc) == 80, "frame_desc.t_target");
VRIPC_STATIC_ASSERT(sizeof(vripc_consume_status_t) == 64, "consume");
VRIPC_STATIC_ASSERT(sizeof(vripc_haptic_evt_t) == 24, "haptic_evt");
VRIPC_STATIC_ASSERT(sizeof(vripc_timing_rec_t) == 32, "timing_rec");
VRIPC_STATIC_ASSERT(sizeof(vripc_log_line_t) == 256, "log_line");

VRIPC_STATIC_ASSERT(offsetof(vripc_shm_t, driver) == 64, "shm.driver");
VRIPC_STATIC_ASSERT(offsetof(vripc_shm_t, config) == 256, "shm.config");
VRIPC_STATIC_ASSERT(offsetof(vripc_shm_t, pacing) == 704, "shm.pacing");
VRIPC_STATIC_ASSERT(offsetof(vripc_shm_t, tracking) == 768, "shm.tracking");
VRIPC_STATIC_ASSERT(offsetof(vripc_shm_t, frames) == 3392, "shm.frames");
VRIPC_STATIC_ASSERT(offsetof(vripc_shm_t, consume) == 4480, "shm.consume");
VRIPC_STATIC_ASSERT(offsetof(vripc_shm_t, haptic) == 4544, "shm.haptic");
VRIPC_STATIC_ASSERT(offsetof(vripc_shm_t, timing) == 5440, "shm.timing");
VRIPC_STATIC_ASSERT(offsetof(vripc_shm_t, log) == 21952, "shm.log");
VRIPC_STATIC_ASSERT(sizeof(vripc_shm_t) == 38464, "shm");
VRIPC_STATIC_ASSERT(sizeof(vripc_shm_t) <= VRIPC_SHM_SECTION_SIZE, "shm fits section");
VRIPC_STATIC_ASSERT(offsetof(vripc_shm_t, driver) % 64 == 0 && offsetof(vripc_shm_t, config) % 64 == 0 &&
                      offsetof(vripc_shm_t, pacing) % 64 == 0 && offsetof(vripc_shm_t, tracking) % 64 == 0 &&
                      offsetof(vripc_shm_t, frames) % 64 == 0 && offsetof(vripc_shm_t, consume) % 64 == 0 &&
                      offsetof(vripc_shm_t, haptic) % 64 == 0 && offsetof(vripc_shm_t, timing) % 64 == 0 &&
                      offsetof(vripc_shm_t, log) % 64 == 0,
                    "writer regions cache-line aligned");

#ifdef __cplusplus
}
#endif
#endif /* VIPLESTREAM_VR_IPC_ABI_H */
