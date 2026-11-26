/*
 * vr_mv_ipc_abi.h - VipleStream §VR-GAME-MV（M-MV1）：遊戲行程內的 MV 外掛
 * （viple_uevr_mv_plugin.dll，MSVC）與 server（viplestream-server.exe，MinGW GCC）之間的 IPC ABI。
 * 設計：docs/specs/vr_game_motion_vectors.md §3。
 *
 * 規則沿用 vr_ipc_abi.h 檔頭的 1～8 條（純 C POD、固定寬度、64 B 區塊、seqlock、raw QPC、
 * 讀對方區塊先整份複製再驗證、handle 值是接收方行程內的值）。另外：
 *  A. 寫入方是遊戲行程＝不可信。server 讀 desc 與 payload 一律：seqlock 前後 seq 相同且為偶數 →
 *     整份複製到本地 → 驗證 generation、grid、payload_bytes、旗標 → 之後只用本地副本。
 *  B. 交接只走「遊戲端 readback → SHM」這一條（設計 §3.2 的備援通道）：遊戲行程不開 server 的
 *     貼圖或 fence，server 也絕不 Wait 遊戲的任何物件（不變式 8）。
 *  C. server 只用 CPU 時間窗配對（設計 §3.3 條件 1、2）；pose 簽章（條件 3）留到 M-MV2 之後。
 *  D. 任何版面變動 → VRMV_ABI_VERSION + 1；HELLO 前 24 B 與 REJECT 凍結。
 */
#ifndef VIPLESTREAM_VR_MV_IPC_ABI_H
#define VIPLESTREAM_VR_MV_IPC_ABI_H

#include <stddef.h>
#include <stdint.h>

#if !defined(_M_X64) && !defined(__x86_64__)
  #error "vr_mv_ipc_abi.h: x64 only (seqlock relies on x64 TSO and aligned 8-byte atomicity)"
#endif

#ifdef __cplusplus
  #define VRMV_STATIC_ASSERT(c, m) static_assert(c, m)
extern "C" {
#else
  #define VRMV_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif

#if defined(_MSC_VER) && !defined(__clang__)
  #include <intrin.h>
  #define VRMV_COMPILER_BARRIER() _ReadWriteBarrier()
#else
  #define VRMV_COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")
#endif

static inline uint64_t vrmv_load_acquire_u64(const uint64_t *p) {
  uint64_t v = *(const volatile uint64_t *) p;
  VRMV_COMPILER_BARRIER();
  return v;
}

static inline void vrmv_store_release_u64(uint64_t *p, uint64_t v) {
  VRMV_COMPILER_BARRIER();
  *(volatile uint64_t *) p = v;
}

static inline void vrmv_seq_write_begin(uint64_t *p, uint64_t v) {
  *(volatile uint64_t *) p = v;
  VRMV_COMPILER_BARRIER();
}

/* ── 常數 ─────────────────────────────────────────────────────────────── */
#define VRMV_ABI_VERSION 1u
#define VRMV_MAGIC_PIPE 0x564D5356u /* "VSMV"（little-endian） */
#define VRMV_MAGIC_SHM 0x484D4D56u /* "VMMH" */
#define VRMV_PIPE_NAME_PREFIX L"\\\\.\\pipe\\VipleStreamVRMV-" /* 後接十進位 session id（同 vr_ipc_abi.h） */
#define VRMV_PIPE_MAX_MSG 256u

#define VRMV_SLOTS 4u /* desc＋payload 環；120 Hz 下每格 33 ms 才被覆寫 */
#define VRMV_MAX_GRID_W 640u /* 雙眼並排 1/8 總寬上限（每眼 2560 算圖） */
#define VRMV_MAX_GRID_H 360u
#define VRMV_CELL_BYTES 8u /* RGBA16F：x,y＝MV（見 desc 的 mv_units）、z＝正規化逆深度、w＝有效度 */
#define VRMV_SLOT_PAYLOAD_BYTES (VRMV_MAX_GRID_W * VRMV_MAX_GRID_H * VRMV_CELL_BYTES) /* 1,843,200 */
#define VRMV_HEADER_BYTES 4096u /* 前 4 KB：server 區塊＋writer 區塊＋desc 環 */
#define VRMV_SECTION_SIZE (VRMV_HEADER_BYTES + VRMV_SLOTS * VRMV_SLOT_PAYLOAD_BYTES) /* 7,376,896 */

/* pipe 訊息型別 */
#define VRMV_MSG_HELLO 1u /* plugin → server */
#define VRMV_MSG_WELCOME 2u /* server → plugin：section handle（plugin 行程內的值） */
#define VRMV_MSG_REJECT 3u /* server → plugin */
#define VRMV_MSG_BYE 4u /* 雙向 */

/* REJECT.reason */
#define VRMV_REJ_ABI_MISMATCH 1u
#define VRMV_REJ_IDENTITY 2u /* 連線方不是 SteamVR 目前的場景程式、或不在同一個 session */
#define VRMV_REJ_BAD_MESSAGE 3u
#define VRMV_REJ_BUSY 4u /* 已有另一個 plugin 連著 */
#define VRMV_REJ_DISABLED 5u /* server 設定 vr_game_mv = disabled，或沒有 VR session */

/* vrmv_desc_t.flags */
#define VRMV_F_LEFT_VALID 0x01u
#define VRMV_F_RIGHT_VALID 0x02u
#define VRMV_F_BOTH_EYES 0x03u
#define VRMV_F_HAS_DEPTH 0x04u
#define VRMV_F_RESET 0x08u /* NGX Reset／feature 重建：本幀禁止外插 */
#define VRMV_F_DEPTH_INVERTED 0x10u /* DLSS DepthInverted（逆深度＝reverse-Z） */

/* vrmv_desc_t.mv_units */
#define VRMV_UNITS_MVTEX_PIXELS 1u /* x,y 是 MV 貼圖像素（mv_tex_w/h），再乘 mv_scale；DLSS 未設 MVLowRes 時 */
#define VRMV_UNITS_EYE_UV 2u /* x,y 已是單眼正規化 UV 位移（uv_prev − uv_cur，已乘 mv_scale、除以 MV 貼圖尺寸、扣過 jitter）；轉 SBS UV 只要 x × 0.5 */

/* vrmv_desc_t.upscaler_kind */
#define VRMV_UPSCALER_DLSS 1u

typedef struct vrmv_msg_hdr_t { /* 凍結 */
  uint32_t magic; /* VRMV_MAGIC_PIPE */
  uint16_t type; /* VRMV_MSG_* */
  uint16_t flags; /* 0 */
  uint32_t size; /* 含 header */
  uint32_t seq; /* 每個方向從 1 起 */
} vrmv_msg_hdr_t;

typedef struct vrmv_hello_t { /* plugin → server；前 24 B 凍結 */
  vrmv_msg_hdr_t hdr;
  uint32_t abi_version;
  uint32_t plugin_pid; /* 只供 log；身分以 GetNamedPipeClientProcessId 為準 */
  uint32_t plugin_version_packed;
  uint32_t upscaler_kind; /* VRMV_UPSCALER_* */
  uint64_t reserved[2];
} vrmv_hello_t;

typedef struct vrmv_welcome_t { /* server → plugin */
  vrmv_msg_hdr_t hdr;
  uint32_t abi_version;
  uint32_t server_pid; /* 只供 log */
  uint64_t generation; /* desc.generation 必須等於它 */
  uint64_t section_size; /* VRMV_SECTION_SIZE */
  int64_t qpc_frequency;
  uint64_t dup_section_handle; /* plugin 行程內的 handle 值（FILE_MAP_READ|FILE_MAP_WRITE） */
  uint64_t reserved[2];
} vrmv_welcome_t;

typedef struct vrmv_reject_t { /* 凍結 */
  vrmv_msg_hdr_t hdr;
  uint32_t reason; /* VRMV_REJ_* */
  uint32_t reserved;
} vrmv_reject_t;

typedef struct vrmv_bye_t {
  vrmv_msg_hdr_t hdr;
  uint32_t reason; /* 0＝正常 */
  uint32_t reserved;
} vrmv_bye_t;

/* ── SHM 版面 ─────────────────────────────────────────────────────────── */
typedef struct vrmv_server_block_t { /* 寫入方：server；WELCOME 前寫好，之後只更新 heartbeat 與診斷 */
  uint32_t magic; /* VRMV_MAGIC_SHM */
  uint32_t abi_version;
  uint64_t section_size;
  uint64_t generation;
  int64_t qpc_frequency;
  uint32_t server_pid;
  uint32_t reserved0;
  uint64_t server_heartbeat_qpc; /* store_release */
  uint64_t last_consumed_mv_frame_id; /* 只供診斷；plugin 不得依它做流程決定 */
  uint64_t reserved1;
} vrmv_server_block_t;
VRMV_STATIC_ASSERT(sizeof(vrmv_server_block_t) == 64, "vrmv_server_block_t");

typedef struct vrmv_writer_block_t { /* 寫入方：plugin；server 只讀（診斷） */
  uint64_t writer_heartbeat_qpc; /* store_release */
  uint64_t frames_published;
  uint64_t frames_dropped_gpu_late; /* readback 沒在時限內完成而放棄 */
  uint64_t frames_dropped_incomplete; /* 缺一眼 */
  uint32_t writer_pid;
  uint32_t reserved0;
  /* GPU 一做完（還沒複製、還沒發布）就先寫：先 pending_gpu_done_qpc、再 store_release pending_mv_frame_id。
   * server 配對時若看到比手上候選更新、且 GPU 在 driver Present 前就做完的 pending 幀，表示被合成的是那一幀、
   * 只是還沒發布 → 這張不配 MV（不能拿上一幀的 MV 頂替，會差一幀）。 */
  uint64_t pending_mv_frame_id;
  int64_t pending_gpu_done_qpc;
  uint64_t reserved1;
} vrmv_writer_block_t;
VRMV_STATIC_ASSERT(sizeof(vrmv_writer_block_t) == 64, "vrmv_writer_block_t");

typedef struct vrmv_desc_t { /* 寫入方：plugin（單一 publish 執行緒）；per-slot seqlock，同時保護對應的 payload 槽 */
  uint64_t seq; /* 2*n+1 寫入中、2*n+2 完成；n＝這一格被寫的次數 */
  uint64_t mv_frame_id; /* 從 1 起單調遞增；slot ＝ mv_frame_id % VRMV_SLOTS */
  uint64_t generation; /* 必須等於 WELCOME.generation */
  int64_t game_present_qpc; /* 該遊戲幀雙眼齊備後第一次 on_present（Signal readback fence 的時刻） */
  int64_t gpu_done_qpc; /* publish 執行緒觀察到 readback fence 完成的時刻 */
  int64_t publish_qpc; /* payload 寫完、即將寫偶數 seq 的時刻 */
  uint32_t flags; /* VRMV_F_* */
  uint32_t payload_bytes; /* == grid_w * grid_h * VRMV_CELL_BYTES，≤ VRMV_SLOT_PAYLOAD_BYTES */
  uint16_t grid_w; /* 雙眼並排總寬（左眼 [0, grid_w/2)），偶數 */
  uint16_t grid_h;
  uint16_t render_eye_w; /* DLSS 輸入（算圖）單眼尺寸 */
  uint16_t render_eye_h;
  uint16_t mv_tex_w; /* MV 貼圖尺寸（DLSS 未設 MVLowRes 時＝輸出尺寸） */
  uint16_t mv_tex_h;
  uint16_t mv_units; /* VRMV_UNITS_* */
  uint8_t upscaler_kind; /* VRMV_UPSCALER_* */
  uint8_t game_frame_step; /* 距上一筆 publish 的遊戲幀數（通常 1；中間丟過幀就 > 1，封頂 255） */
  float mv_scale[2]; /* NGX MV scale */
  float depth_ref; /* shader 正規化逆深度用的參考值（見 cs_downsample_mv_8x8.hlsl） */
  uint32_t reserved0;
  uint64_t reserved1[5];
} vrmv_desc_t;
VRMV_STATIC_ASSERT(sizeof(vrmv_desc_t) == 128, "vrmv_desc_t");

typedef struct vrmv_shm_t { /* section 開頭；payload 槽從 VRMV_HEADER_BYTES 起，每槽 VRMV_SLOT_PAYLOAD_BYTES */
  vrmv_server_block_t server;
  vrmv_writer_block_t writer;
  uint64_t write_index; /* 寫入方：plugin；最新完成的 mv_frame_id（0＝還沒有）；store_release */
  uint8_t pad0[56];
  vrmv_desc_t slot[VRMV_SLOTS];
} vrmv_shm_t;
VRMV_STATIC_ASSERT(offsetof(vrmv_shm_t, writer) == 64, "vrmv_shm_t.writer");
VRMV_STATIC_ASSERT(offsetof(vrmv_shm_t, write_index) == 128, "vrmv_shm_t.write_index");
VRMV_STATIC_ASSERT(offsetof(vrmv_shm_t, slot) == 192, "vrmv_shm_t.slot");
VRMV_STATIC_ASSERT(sizeof(vrmv_shm_t) <= VRMV_HEADER_BYTES, "vrmv_shm_t fits header");

static inline uint64_t vrmv_payload_offset(uint32_t slot) {
  return (uint64_t) VRMV_HEADER_BYTES + (uint64_t) slot * (uint64_t) VRMV_SLOT_PAYLOAD_BYTES;
}

#ifdef __cplusplus
}
#endif

#endif /* VIPLESTREAM_VR_MV_IPC_ABI_H */
