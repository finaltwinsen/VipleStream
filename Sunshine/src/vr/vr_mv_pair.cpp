/**
 * @file src/vr/vr_mv_pair.cpp
 * @brief VipleStream §VR-GAME-MV（M-MV1）：遊戲內 MV 描述符驗證、SHM seqlock 讀取與視訊幀配對實作。
 */
#include "vr_mv_pair.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vr::mv_pair {

  namespace {
    constexpr uint32_t k_allowed_flags =
      VRMV_F_LEFT_VALID | VRMV_F_RIGHT_VALID | VRMV_F_HAS_DEPTH | VRMV_F_RESET | VRMV_F_DEPTH_INVERTED;

  }  // namespace

  const char *miss_reason_name(miss_reason_e r) noexcept {
    switch (r) {
      case miss_reason_e::none:
        return "none";
      case miss_reason_e::no_data:
        return "no-data";
      case miss_reason_e::too_old:
        return "too-old";
      case miss_reason_e::too_new:
        return "too-new";
      case miss_reason_e::missing_eye:
        return "missing-eye";
      case miss_reason_e::already_used:
        return "already-used";
      case miss_reason_e::pending:
        return "pending";
      case miss_reason_e::torn:
        return "torn";
    }
    return "unknown";
  }

  uint8_t compute_k_step(uint64_t cur_mv_frame_id, uint64_t prev_encoded_mv_frame_id) noexcept {
    if (prev_encoded_mv_frame_id == 0 || cur_mv_frame_id <= prev_encoded_mv_frame_id) {
      return 1;
    }
    const uint64_t diff = cur_mv_frame_id - prev_encoded_mv_frame_id;
    return static_cast<uint8_t>(std::clamp<uint64_t>(diff, 1, 4));
  }

  bool validate_desc(const vrmv_desc_t &d, uint64_t expected_generation) noexcept {
    if (expected_generation == 0 || d.generation != expected_generation) {
      return false;
    }
    if (d.seq == 0 || (d.seq & 1u) != 0) {
      return false;
    }
    if (d.mv_frame_id == 0) {
      return false;
    }
    if (d.grid_w < 2 || d.grid_w > VRMV_MAX_GRID_W || (d.grid_w & 1u) != 0) {
      return false;
    }
    if (d.grid_h < 1 || d.grid_h > VRMV_MAX_GRID_H) {
      return false;
    }
    const uint32_t expected_bytes =
      static_cast<uint32_t>(d.grid_w) * static_cast<uint32_t>(d.grid_h) * VRMV_CELL_BYTES;
    if (d.payload_bytes != expected_bytes || d.payload_bytes > VRMV_SLOT_PAYLOAD_BYTES) {
      return false;
    }
    if ((d.flags & ~k_allowed_flags) != 0) {
      return false;
    }
    if (d.mv_units != VRMV_UNITS_MVTEX_PIXELS && d.mv_units != VRMV_UNITS_EYE_UV) {
      return false;
    }
    if (d.mv_units == VRMV_UNITS_MVTEX_PIXELS && (d.mv_tex_w == 0 || d.mv_tex_h == 0)) {
      return false;
    }
    if (!std::isfinite(d.mv_scale[0]) || !std::isfinite(d.mv_scale[1]) || !std::isfinite(d.depth_ref)) {
      return false;
    }
    return true;
  }

  pair_result_t match_mv_frame(
    const vrmv_desc_t *candidates,
    size_t count,
    int64_t driver_present_qpc,
    uint64_t last_matched_mv_frame_id,
    int64_t t_vsync_qpc,
    int64_t qpc_frequency,
    uint64_t pending_mv_frame_id,
    int64_t pending_gpu_done_qpc
  ) noexcept {
    pair_result_t res {};
    res.miss_reason = miss_reason_e::no_data;

    if (!candidates || count == 0) {
      return res;
    }

    const int64_t qpf = qpc_frequency > 0 ? qpc_frequency : 10'000'000;
    const int64_t vsync = t_vsync_qpc > 0 ? t_vsync_qpc : (qpf / 90);
    // 規則（M-MV1 實測後修正，原設計 §3.3 條件 2 的 1.5·T_vsync 時間窗在 GPU 吃滿的遊戲幾乎全判太舊：
    // Palworld 37 fps 時 Present→GPU 做完 p50 7.5 ms、p95 11 ms，再等合成就超過 16.7 ms）：
    //   SteamVR 合成時拿的是「GPU 已做完的最新一張遊戲幀」，所以候選＝gpu_done_qpc ≤ driver Present 的那些，
    //   取 mv_frame_id 最大的一筆。它若已經配過（合成器重送同一張）＝already_used，不重送 MV。
    //   年齡上限 max(6·T_vsync, 50 ms)：外掛停了很久之後剩下的舊資料不拿來配。
    const int64_t max_age_qpc = std::max<int64_t>(6 * vsync, qpf / 20);

    const vrmv_desc_t *best = nullptr;
    size_t best_i = 0;
    bool any = false;
    bool any_not_done = false;
    bool any_too_old = false;
    for (size_t i = 0; i < count; ++i) {
      const vrmv_desc_t &c = candidates[i];
      if (c.mv_frame_id == 0) {
        continue;
      }
      any = true;
      if (c.gpu_done_qpc > driver_present_qpc) {
        any_not_done = true;  // Present 時這一幀的 GPU 還沒做完：不可能是被合成的那張
        continue;
      }
      if (driver_present_qpc - c.game_present_qpc > max_age_qpc) {
        any_too_old = true;
        continue;
      }
      if (!best || c.mv_frame_id > best->mv_frame_id) {
        best = &c;
        best_i = i;
      }
    }

    if (best && pending_mv_frame_id > best->mv_frame_id && pending_gpu_done_qpc > 0 &&
        pending_gpu_done_qpc <= driver_present_qpc) {
      // 被合成的是 GPU 已做完、但還沒發布的那一幀：不拿 best（較舊）頂替
      res.miss_reason = miss_reason_e::pending;
    } else if (best) {
      if (best->mv_frame_id <= last_matched_mv_frame_id) {
        res.miss_reason = miss_reason_e::already_used;
      } else if ((best->flags & VRMV_F_BOTH_EYES) != VRMV_F_BOTH_EYES) {
        res.miss_reason = miss_reason_e::missing_eye;
      } else {
        res.hit = true;
        res.index = best_i;
        res.matched = *best;
        res.delta_qpc = driver_present_qpc - best->game_present_qpc;
      }
    } else if (any_too_old) {
      res.miss_reason = miss_reason_e::too_old;
    } else if (any_not_done) {
      res.miss_reason = miss_reason_e::too_new;
    } else if (!any) {
      res.miss_reason = miss_reason_e::no_data;
    }

    if (res.hit) {
      res.miss_reason = miss_reason_e::none;
      res.k_step = compute_k_step(res.matched.mv_frame_id, last_matched_mv_frame_id);
    }
    return res;
  }

  size_t shm_snapshot_descs(
    const vrmv_shm_t *shm,
    uint64_t expected_generation,
    vrmv_desc_t *out_descs,
    size_t max_descs
  ) noexcept {
    if (!shm || !out_descs || max_descs == 0 || expected_generation == 0) {
      return 0;
    }
    size_t count = 0;
    for (uint32_t i = 0; i < VRMV_SLOTS && count < max_descs; ++i) {
      const vrmv_desc_t *slot_ptr = &shm->slot[i];
      const uint64_t s1 = vrmv_load_acquire_u64(&slot_ptr->seq);
      if (s1 == 0 || (s1 & 1u) != 0) {
        continue;
      }
      vrmv_desc_t local {};
      std::memcpy(&local, static_cast<const void *>(slot_ptr), sizeof(vrmv_desc_t));
      VRMV_COMPILER_BARRIER();
      const uint64_t s2 = vrmv_load_acquire_u64(&slot_ptr->seq);
      if (s1 != s2 || local.seq != s1) {
        continue;
      }
      if ((local.mv_frame_id % VRMV_SLOTS) != i) {
        continue;
      }
      if (!validate_desc(local, expected_generation)) {
        continue;
      }
      out_descs[count++] = local;
    }
    return count;
  }

  bool shm_copy_payload(
    const vrmv_shm_t *shm,
    const vrmv_desc_t &expected_desc,
    void *dst,
    size_t dst_capacity
  ) noexcept {
    if (!shm || !dst || expected_desc.seq == 0 || (expected_desc.seq & 1u) != 0) {
      return false;
    }
    if (expected_desc.payload_bytes == 0 || expected_desc.payload_bytes > VRMV_SLOT_PAYLOAD_BYTES ||
        dst_capacity < expected_desc.payload_bytes) {
      return false;
    }
    const uint32_t slot_idx = static_cast<uint32_t>(expected_desc.mv_frame_id % VRMV_SLOTS);
    const vrmv_desc_t *slot_ptr = &shm->slot[slot_idx];
    const uint64_t s1 = vrmv_load_acquire_u64(&slot_ptr->seq);
    if (s1 != expected_desc.seq) {
      return false;
    }
    const auto *base = reinterpret_cast<const uint8_t *>(shm);
    const uint8_t *src_payload = base + vrmv_payload_offset(slot_idx);
    std::memcpy(dst, src_payload, expected_desc.payload_bytes);
    VRMV_COMPILER_BARRIER();
    const uint64_t s2 = vrmv_load_acquire_u64(&slot_ptr->seq);
    return s1 == s2;
  }

  bool shm_read_slot_with_payload(
    const vrmv_shm_t *shm,
    uint32_t slot_idx,
    uint64_t expected_generation,
    vrmv_desc_t &out_desc,
    void *dst_payload,
    size_t dst_capacity
  ) noexcept {
    if (!shm || !dst_payload || slot_idx >= VRMV_SLOTS || expected_generation == 0) {
      return false;
    }
    const vrmv_desc_t *slot_ptr = &shm->slot[slot_idx];
    const uint64_t s1 = vrmv_load_acquire_u64(&slot_ptr->seq);
    if (s1 == 0 || (s1 & 1u) != 0) {
      return false;
    }
    vrmv_desc_t local {};
    std::memcpy(&local, static_cast<const void *>(slot_ptr), sizeof(vrmv_desc_t));
    VRMV_COMPILER_BARRIER();
    if (local.seq != s1 || (local.mv_frame_id % VRMV_SLOTS) != slot_idx) {
      return false;
    }
    if (!validate_desc(local, expected_generation) || dst_capacity < local.payload_bytes) {
      return false;
    }
    const auto *base = reinterpret_cast<const uint8_t *>(shm);
    const uint8_t *src_payload = base + vrmv_payload_offset(slot_idx);
    std::memcpy(dst_payload, src_payload, local.payload_bytes);
    VRMV_COMPILER_BARRIER();
    const uint64_t s2 = vrmv_load_acquire_u64(&slot_ptr->seq);
    if (s1 != s2) {
      return false;
    }
    out_desc = local;
    return true;
  }

  void shm_write_slot_for_test(
    vrmv_shm_t *shm,
    const vrmv_desc_t &desc,
    const void *payload,
    size_t payload_len,
    uint64_t &inout_seq_counter
  ) noexcept {
    // §REVIEW-1127 vr #10：以 desc.payload_bytes 當長度 memcpy，payload 沒有大小資訊——加長度參數擋讀越界
    if (!shm || !payload || desc.mv_frame_id == 0 || desc.payload_bytes > VRMV_SLOT_PAYLOAD_BYTES || desc.payload_bytes > payload_len) {
      return;
    }
    const uint32_t slot_idx = static_cast<uint32_t>(desc.mv_frame_id % VRMV_SLOTS);
    vrmv_desc_t *slot_ptr = &shm->slot[slot_idx];
    auto *base = reinterpret_cast<uint8_t *>(shm);
    uint8_t *dst_payload = base + vrmv_payload_offset(slot_idx);

    const uint64_t odd_seq = (inout_seq_counter * 2u) + 1u;
    const uint64_t even_seq = (inout_seq_counter * 2u) + 2u;
    ++inout_seq_counter;

    // 1. 寫入奇數 seq 並加編譯器屏障
    vrmv_seq_write_begin(&slot_ptr->seq, odd_seq);
    // 2. 寫入 desc 其餘欄位與 payload
    vrmv_desc_t tmp = desc;
    tmp.seq = even_seq;
    std::memcpy(
      reinterpret_cast<char *>(slot_ptr) + sizeof(uint64_t),
      reinterpret_cast<const char *>(&tmp) + sizeof(uint64_t),
      sizeof(vrmv_desc_t) - sizeof(uint64_t)
    );
    std::memcpy(dst_payload, payload, desc.payload_bytes);
    // 3. 寫入偶數 seq 並更新 write_index
    vrmv_store_release_u64(&slot_ptr->seq, even_seq);
    vrmv_store_release_u64(&shm->write_index, desc.mv_frame_id);
  }

}  // namespace vr::mv_pair
