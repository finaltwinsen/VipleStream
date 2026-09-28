// ipc_proto.h - VipleStream §VR：driver 端 IPC 的純函式（不碰 Win32、不配置記憶體）。
//
// 內容：pipe 訊息 header 驗證、WELCOME／shm header／session config／TEXTURES 驗證、
// seqlock 讀寫（tracking、frames、pacing）、SPSC 生產者（haptic、timing、log）、重連退避。
// ipc_client.cpp 與 vr_probe --mode unit 共用；規則全部出自設計 §B.3、§B.4、§B.6、§B.8。
//
// 注意：shm 的另一半由 server 寫；這裡的讀取一律「整份複製 → 驗證 → 只用本地副本」（ABI 規則 6），
// 寫入一律以呼叫端的本地計數器為準，絕不從 shm 讀回自己寫過的值。
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "vr_ipc_abi.h"

namespace vrdrv::proto {

  // ── pipe 訊息 ────────────────────────────────────────────────────────────
  // 所有訊息型別中最小的是 24 B（REJECT／BYE／READY）；HELLO 的凍結前綴也是 24 B。
  constexpr uint32_t k_min_msg = 24;

  // 依型別回傳本 ABI 版本的固定大小；未知型別回 0。
  constexpr uint32_t expected_size(uint32_t type) {
    switch (type) {
      case VRIPC_MSG_HELLO:
        return (uint32_t) sizeof(vripc_hello_t);
      case VRIPC_MSG_WELCOME:
        return (uint32_t) sizeof(vripc_welcome_t);
      case VRIPC_MSG_REJECT:
        return (uint32_t) sizeof(vripc_reject_t);
      case VRIPC_MSG_TEXTURES:
        return (uint32_t) sizeof(vripc_textures_t);
      case VRIPC_MSG_READY:
        return (uint32_t) sizeof(vripc_ready_t);
      case VRIPC_MSG_STATE:
        return (uint32_t) sizeof(vripc_state_t);
      case VRIPC_MSG_BYE:
        return (uint32_t) sizeof(vripc_bye_t);
      default:
        return 0;
    }
  }

  // 先驗凍結的 header（ABI 規則 7）：bytes ≥ 24、≤ VRIPC_PIPE_MAX_MSG、magic、hdr.size == bytes、flags == 0。
  // 通過後 out 是 header 的本地副本；why 指向靜態字串。
  inline bool check_header(const void *buf, uint32_t bytes, vripc_msg_hdr_t &out, const char *&why) {
    if (bytes < k_min_msg) {
      why = "short";
      return false;
    }
    if (bytes > VRIPC_PIPE_MAX_MSG) {
      why = "oversize";
      return false;
    }
    std::memcpy(&out, buf, sizeof(out));
    if (out.magic != VRIPC_MAGIC_PIPE) {
      why = "magic";
      return false;
    }
    if (out.size != bytes) {
      why = "size-mismatch";
      return false;
    }
    if (out.flags != 0) {
      why = "flags";
      return false;
    }
    why = "ok";
    return true;
  }

  // header 通過後的大小規則：REJECT 只要 ≥ 24（凍結，任何 ABI 版本都要能解析）；
  // 其他型別必須等於本版 sizeof（server 的 abi 已在 WELCOME 前比對過，之後的訊息都是同一版）。
  inline bool check_body_size(const vripc_msg_hdr_t &h, uint32_t bytes, const char *&why) {
    const uint32_t want = expected_size(h.type);
    if (want == 0) {
      why = "unknown-type";
      return false;
    }
    if (h.type == VRIPC_MSG_REJECT) {
      if (bytes < (uint32_t) sizeof(vripc_reject_t)) {
        why = "reject-short";
        return false;
      }
      why = "ok";
      return true;
    }
    if (bytes != want) {
      why = "body-size";
      return false;
    }
    why = "ok";
    return true;
  }

  // 組 header；seq 由呼叫端的本地計數器提供（每個方向從 1 起）。
  inline vripc_msg_hdr_t make_header(uint32_t type, uint32_t size, uint32_t seq) {
    vripc_msg_hdr_t h {};
    h.magic = VRIPC_MAGIC_PIPE;
    h.type = (uint16_t) type;
    h.flags = 0;
    h.size = size;
    h.seq = seq;
    return h;
  }

  // handle 值的合理性（值來自 server；server 是可信方，這只是防呆）：
  // 非 0、4 的倍數、落在 32 bit（64 位元 Windows 的 handle 值是 32 bit 可攜）。
  // pseudo handle（-1、-2 的符號延伸值）不會通過。
  constexpr bool plausible_handle_value(uint64_t v) {
    return v != 0 && (v & 3u) == 0 && v <= 0xFFFFFFFFull;
  }

  // ── WELCOME ──────────────────────────────────────────────────────────────
  inline bool check_welcome(const vripc_welcome_t &w, int64_t own_qpf, const char *&why) {
    if (w.abi_version != VRIPC_ABI_VERSION) {
      why = "welcome-abi";
      return false;
    }
    if (w.generation == 0) {
      why = "welcome-generation";
      return false;
    }
    if (w.shm_size < sizeof(vripc_shm_t) || w.shm_size > (16ull << 20)) {
      why = "welcome-shm-size";
      return false;
    }
    if (w.qpc_frequency != own_qpf) {
      why = "welcome-qpc-frequency";
      return false;
    }
    if (!plausible_handle_value(w.dup_shm_handle) || !plausible_handle_value(w.dup_evt_trk) ||
        !plausible_handle_value(w.dup_evt_frm)) {
      why = "welcome-handle";
      return false;
    }
    why = "ok";
    return true;
  }

  // ── shm header（server 寫；driver 整份複製後驗） ───────────────────────────
  inline bool check_shm_header(const vripc_shm_header_t &h, const vripc_welcome_t &w, const char *&why) {
    if (h.magic != VRIPC_MAGIC_SHM) {
      why = "shm-magic";
      return false;
    }
    if (h.abi_version != VRIPC_ABI_VERSION) {
      why = "shm-abi";
      return false;
    }
    if (h.struct_size != sizeof(vripc_shm_t)) {
      why = "shm-struct-size";
      return false;
    }
    if (h.shm_size != w.shm_size) {
      why = "shm-size";
      return false;
    }
    if (h.generation != w.generation) {
      why = "shm-generation";
      return false;
    }
    if (h.qpc_frequency != w.qpc_frequency) {
      why = "shm-qpc-frequency";
      return false;
    }
    if (h.armed > 1 || h.server_state > VRIPC_SRV_CLOSING) {
      why = "shm-state";
      return false;
    }
    why = "ok";
    return true;
  }

  // ── session config（server 寫，同一 generation 不變） ─────────────────────
  // eye_width == 0 是 idle generation：其餘欄位不看。
  // 非 idle 時只擋會讓 driver 算壞或配置過大的值；FOV 的幾何合理性由 hmd_device（V4）判斷。
  inline bool check_config(const vripc_session_config_t &c, const char *&why) {
    if (c.eye_width == 0) {
      why = "ok";
      return true;
    }
    if (c.eye_width > 8192 || c.eye_height == 0 || c.eye_height > 8192) {
      why = "config-eye-size";
      return false;
    }
    if (c.packed_width < c.eye_width || c.packed_width > 16384 || c.packed_height < c.eye_height ||
        c.packed_height > 8192) {
      why = "config-packed-size";
      return false;
    }
    if (c.dxgi_format != VRIPC_DXGI_FORMAT_B8G8R8A8_UNORM) {
      why = "config-format";
      return false;
    }
    if (c.refresh_mhz < 30000 || c.refresh_mhz > 500000) {
      why = "config-refresh";
      return false;
    }
    if (c.universe_id > 0xFFFFFFFFull) {
      why = "config-universe";
      return false;
    }
    if (c.adapter_luid_low == 0 && c.adapter_luid_high == 0) {
      why = "config-luid";
      return false;
    }
    for (int e = 0; e < 2; ++e) {
      for (int k = 0; k < 4; ++k) {
        if (!std::isfinite(c.fov_tan[e][k])) {
          why = "config-fov";
          return false;
        }
      }
      for (int k = 0; k < 7; ++k) {
        if (!std::isfinite(c.eye_to_head[e][k])) {
          why = "config-eye-to-head";
          return false;
        }
      }
    }
    if (!std::isfinite(c.ipd_m) || !std::isfinite(c.overscan_deg)) {
      why = "config-ipd";
      return false;
    }
    why = "ok";
    return true;
  }

  // ── TEXTURES（server → driver） ──────────────────────────────────────────
  // 回傳 0 = 通過；否則是要回給 server 的 VRIPC_REJ_*。
  inline uint32_t check_textures(const vripc_textures_t &t, uint64_t generation, const vripc_session_config_t &c, const char *&why) {
    if (t.generation != generation) {
      why = "textures-generation";
      return VRIPC_REJ_GENERATION;
    }
    if (c.eye_width == 0) {
      why = "textures-in-idle-generation";
      return VRIPC_REJ_BAD_MESSAGE;
    }
    if (t.tex_count != VRIPC_TEX_COUNT || t.dxgi_format != VRIPC_DXGI_FORMAT_B8G8R8A8_UNORM) {
      why = "textures-format";
      return VRIPC_REJ_TEXTURE_INVALID;
    }
    if (t.width != c.packed_width || t.height != c.packed_height) {
      why = "textures-size";
      return VRIPC_REJ_TEXTURE_INVALID;
    }
    if (t.adapter_luid_low != c.adapter_luid_low || t.adapter_luid_high != c.adapter_luid_high) {
      why = "textures-luid";
      return VRIPC_REJ_ADAPTER_MISMATCH;
    }
    for (uint32_t i = 0; i < VRIPC_TEX_COUNT; ++i) {
      if (!plausible_handle_value(t.tex_handle[i])) {
        why = "textures-handle";
        return VRIPC_REJ_TEXTURE_INVALID;
      }
    }
    if (!plausible_handle_value(t.shared_fence_handle) || !plausible_handle_value(t.consumed_fence_handle)) {
      why = "textures-fence-handle";
      return VRIPC_REJ_TEXTURE_INVALID;
    }
    why = "ok";
    return 0;
  }

  // ── seqlock（§B.3） ─────────────────────────────────────────────────────
  enum class seq_read_e {
    ok,
    empty,  // write_index == 0（還沒有資料）
    torn  // 重試 4 次仍不一致：本輪視為沒有新資料並計數（寫入方不可信，不可無限重試）
  };

  // 多格 seqlock 讀「最新一筆」。Slot 的第一個欄位必須是 uint64_t seq。
  template<class Slot, uint32_t N>
  inline seq_read_e seq_read_latest(uint64_t *write_index, Slot *slots, Slot &out, uint64_t &idx_out) {
    static_assert(offsetof(Slot, seq) == 0, "seq must be first");
    for (int attempt = 0; attempt < 4; ++attempt) {
      const uint64_t w = vripc_load_acquire_u64(write_index);
      if (w == 0) {
        return seq_read_e::empty;
      }
      const uint64_t idx = w - 1;
      Slot *s = &slots[idx % N];
      const uint64_t s1 = vripc_load_acquire_u64(&s->seq);
      if (s1 != 2 * idx + 2) {
        continue;
      }
      std::memcpy(&out, (const void *) s, sizeof(Slot));
      VRIPC_COMPILER_BARRIER();
      const uint64_t s2 = vripc_load_acquire_u64(&s->seq);
      if (s1 == s2 && out.seq == s1) {
        idx_out = idx;
        return seq_read_e::ok;
      }
    }
    return seq_read_e::torn;
  }

  // 多格 seqlock 寫（單一寫入臨界區內呼叫）。payload 取 in 的第 8 B 之後；local_index 是寫入方自己的計數器。
  template<class Slot, uint32_t N>
  inline void seq_write(uint64_t *write_index, Slot *slots, uint64_t &local_index, const Slot &in) {
    static_assert(offsetof(Slot, seq) == 0, "seq must be first");
    const uint64_t idx = local_index;
    Slot *s = &slots[idx % N];
    vripc_seq_write_begin(&s->seq, 2 * idx + 1);
    std::memcpy((char *) s + sizeof(uint64_t), (const char *) &in + sizeof(uint64_t), sizeof(Slot) - sizeof(uint64_t));
    vripc_store_release_u64(&s->seq, 2 * idx + 2);
    vripc_store_release_u64(write_index, idx + 1);
    local_index = idx + 1;
  }

  // 單格 seqlock 讀（pacing）：s1 偶數、非 0、s1 == s2 才算成功；失敗由呼叫端沿用上一份。
  inline bool seq_read_single(vripc_pacing_t *p, vripc_pacing_t &out) {
    const uint64_t s1 = vripc_load_acquire_u64(&p->seq);
    if (s1 == 0 || (s1 & 1u) != 0) {
      return false;
    }
    std::memcpy(&out, (const void *) p, sizeof(out));
    VRIPC_COMPILER_BARRIER();
    const uint64_t s2 = vripc_load_acquire_u64(&p->seq);
    return s1 == s2 && out.seq == s1;
  }

  // ── SPSC 生產者（haptic、timing、log；driver 寫 head、server 寫 tail） ───────
  enum class spsc_push_e {
    ok,
    full,  // 丟棄並計數
    corrupt  // shm 的 tail 跑到 head 前面（對方亂寫）：丟棄並計數，不覆寫
  };

  template<class Item, uint32_t N>
  inline spsc_push_e spsc_push(uint64_t *head, uint64_t *tail, Item *items, uint64_t &local_head, const Item &it) {
    const uint64_t t = vripc_load_acquire_u64(tail);
    if (t > local_head) {
      return spsc_push_e::corrupt;
    }
    if (local_head - t >= N) {
      return spsc_push_e::full;
    }
    std::memcpy((void *) &items[local_head % N], &it, sizeof(Item));
    ++local_head;
    vripc_store_release_u64(head, local_head);
    return spsc_push_e::ok;
  }

  // log 行：只留可列印 ASCII（其他換成 '?'），長度夾到 < VRIPC_LOG_TEXT；回傳寫入的長度。
  // server 仍會再過濾一次（§B.8 第 8 點）；這裡只是讓 driver 自己不送出垃圾。
  inline uint32_t make_log_text(char (&dst)[VRIPC_LOG_TEXT], const char *src) {
    uint32_t n = 0;
    std::memset(dst, 0, sizeof(dst));
    if (!src) {
      return 0;
    }
    while (src[n] != '\0' && n < VRIPC_LOG_TEXT - 1) {
      const unsigned char ch = (unsigned char) src[n];
      dst[n] = (ch >= 0x20 && ch < 0x7F) ? (char) ch : '?';
      ++n;
    }
    return n;
  }

  // ── 重連退避（§B.6 DISCONNECTED） ────────────────────────────────────────
  // 第 1 次失敗 500 ms，之後每次 ×2，上限 5 s。failures == 0 → 0（立即）。
  constexpr uint32_t backoff_ms(uint32_t failures) {
    if (failures == 0) {
      return 0;
    }
    uint32_t ms = 500;
    for (uint32_t i = 1; i < failures && ms < 5000; ++i) {
      ms *= 2;
    }
    return ms > 5000 ? 5000 : ms;
  }

  constexpr uint32_t k_untrusted_backoff_ms = 5000;  // VERIFY_SERVER 不過（§B.1）
  constexpr uint32_t k_hello_timeout_ms = 2000;  // HELLO → WELCOME／REJECT（§B.4）
  constexpr uint32_t k_textures_timeout_ms = 5000;  // MAPPED（有 config）→ TEXTURES（§B.6）
  constexpr uint32_t k_write_timeout_ms = 100;  // 所有 pipe 寫入（§B.4）
  constexpr uint32_t k_heartbeat_ms = 100;  // driver heartbeat 週期
  constexpr uint32_t k_server_stale_ms = 1000;  // server heartbeat > 1 s → 只進 standby（§B.6）
  constexpr uint32_t k_quiet_log_ms = 60000;  // FILE_NOT_FOUND／ACCESS_DENIED 每 60 s 最多記一次

  // openvr_sdk_packed 的打包方式（vr_ipc_abi.h HELLO 註解）。
  constexpr uint32_t pack_sdk(uint32_t major, uint32_t minor, uint32_t build) {
    return (major << 24) | (minor << 16) | (build & 0xFFFFu);
  }

}  // namespace vrdrv::proto
