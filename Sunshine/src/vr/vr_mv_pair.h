/**
 * @file src/vr/vr_mv_pair.h
 * @brief VipleStream §VR-GAME-MV（M-MV1）：遊戲內 MV 描述符驗證、SHM seqlock 讀取與視訊幀配對純函式模組。
 *
 * 設計依據：docs/specs/vr_game_motion_vectors.md §3.1～§3.3。
 * 規則：
 *   - 純函式、可單元測試、不含任何 Win32 API。
 *   - 嚴格遵守 vr_mv_ipc_abi.h 規則 A～C：讀 SHM 一律先經 seqlock 整份複製到本地 → 驗證 → 只用本地副本。
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "vr_mv_ipc_abi.h"

namespace vr::mv_pair {

  /// 配對未命中（Miss）的原因碼
  enum class miss_reason_e : uint8_t {
    none = 0,          ///< 配對命中
    no_data = 1,       ///< 無候選資料（環狀緩衝為空或無有效描述符）
    too_old = 2,       ///< 可用的候選都比 max(6·T_vsync, 50 ms) 舊
    too_new = 3,       ///< 候選的 GPU 都還沒在 driver Present 之前做完
    missing_eye = 4,   ///< 缺眼（(flags & VRMV_F_BOTH_EYES) != VRMV_F_BOTH_EYES）
    already_used = 5,  ///< 已消費過（mv_frame_id <= last_matched_mv_frame_id）
    torn = 6,          ///< payload 複製時 seqlock 撕裂（或已被新幀覆寫）
    pending = 7,       ///< 被合成的那一幀 GPU 已做完但外掛還沒發布（writer.pending_*）：不拿上一幀頂替
  };

  /// Miss 原因碼的簡短字串（供 log 與單元測試使用）
  const char *miss_reason_name(miss_reason_e r) noexcept;

  /// 配對結果
  struct pair_result_t {
    bool hit = false;                             ///< 是否命中
    size_t index = 0;                             ///< 命中的候選陣列索引
    vrmv_desc_t matched {};                       ///< 命中的描述符本地副本
    int64_t delta_qpc = 0;                        ///< driver_present_qpc - matched.game_present_qpc
    uint8_t k_step = 1;                           ///< K_step = clamp(M_cur - M_prev, 1, 4)
    miss_reason_e miss_reason = miss_reason_e::no_data;  ///< 未命中時的原因碼
  };

  /**
   * @brief 計算兩張編碼幀之間跨越的遊戲渲染幀數 K_step = clamp(M_cur - M_prev, 1, 4)（設計 §3.3 第 3 點）。
   * @param cur_mv_frame_id 本次配對命中的 mv_frame_id (M_cur)
   * @param prev_encoded_mv_frame_id 上一張實際送進編碼器的影像配對到的 mv_frame_id (M_prev；0 表示尚無上一張)
   */
  uint8_t compute_k_step(uint64_t cur_mv_frame_id, uint64_t prev_encoded_mv_frame_id) noexcept;

  /**
   * @brief 驗證本地複製出來的 vrmv_desc_t 是否符合 ABI 規則 A（generation、grid、payload_bytes、旗標與有限浮點數）。
   */
  bool validate_desc(const vrmv_desc_t &d, uint64_t expected_generation) noexcept;

  /**
   * @brief 從候選描述符本地副本陣列中，為 driver 的一筆視訊幀挑選配對的 MV 描述符（設計 §3.3 條件 1、2）。
   *
   * 條件 1：雙眼齊備（(flags & VRMV_F_BOTH_EYES) == VRMV_F_BOTH_EYES）且 mv_frame_id > last_matched_mv_frame_id。
   * 條件 2（M-MV1 實測後修正）：候選＝gpu_done_qpc ≤ driver_present_qpc、且不比 max(6·T_vsync, 50 ms) 舊；
   *   取 mv_frame_id 最大的一筆（SteamVR 合成時拿的是 GPU 已做完的最新一張遊戲幀）。它已配過＝already_used。
   * 條件 3：writer 的 pending 幀比它新、且 GPU 在 Present 前做完 → pending（被合成的是還沒發布的那張）。
   *
   * @param candidates 已驗證的候選描述符本地副本陣列
   * @param count 候選數量
   * @param driver_present_qpc driver 的 d.present_qpc
   * @param last_matched_mv_frame_id 上一張實際送進編碼器的影像配對到的 mv_frame_id（0＝尚無）
   * @param t_vsync_qpc host 虛擬 vsync 週期（QPC tick）
   * @param qpc_frequency QPC 頻率（ticks/s）
   * @param pending_mv_frame_id writer.pending_mv_frame_id（0＝不檢查）
   * @param pending_gpu_done_qpc writer.pending_gpu_done_qpc
   */
  pair_result_t match_mv_frame(
    const vrmv_desc_t *candidates,
    size_t count,
    int64_t driver_present_qpc,
    uint64_t last_matched_mv_frame_id,
    int64_t t_vsync_qpc,
    int64_t qpc_frequency,
    uint64_t pending_mv_frame_id = 0,
    int64_t pending_gpu_done_qpc = 0
  ) noexcept;

  // ── SHM Seqlock 讀寫純函式（供 vr_mv_bridge_win 與 unit_tests 共用）─────────

  /**
   * @brief 從 SHM 掃描所有 VRMV_SLOTS 槽，以 seqlock 複製並驗證屬於 expected_generation 的描述符。
   * @param shm 指向共享記憶體開頭的唯讀指標
   * @param expected_generation 預期的 generation（必須非 0）
   * @param out_descs 輸出通過 seqlock 與驗證的本地副本陣列（容量至少 VRMV_SLOTS）
   * @return 寫入 out_descs 的有效筆數（0 .. VRMV_SLOTS）
   */
  size_t shm_snapshot_descs(
    const vrmv_shm_t *shm,
    uint64_t expected_generation,
    vrmv_desc_t *out_descs,
    size_t max_descs
  ) noexcept;

  /**
   * @brief 在 per-slot seqlock 保護下，從 SHM 複製指定描述符對應的 payload 到本地緩衝區（規則 A）。
   * @param shm 指向共享記憶體開頭的唯讀指標
   * @param expected_desc 配對選中的描述符本地副本（使用其 mv_frame_id、seq、generation、payload_bytes）
   * @param dst 本地目標緩衝區
   * @param dst_capacity 本地目標緩衝區容量（必須 >= expected_desc.payload_bytes）
   * @return true 表示複製前後 seq 一致且為該幀的偶數 seq；false 表示撕裂或已被覆寫
   */
  bool shm_copy_payload(
    const vrmv_shm_t *shm,
    const vrmv_desc_t &expected_desc,
    void *dst,
    size_t dst_capacity
  ) noexcept;

  /**
   * @brief 在 per-slot seqlock 保護下一次讀取指定 slot 的 desc 與 payload（供 seqlock 撕裂壓力單元測試與讀取驗證）。
   * @return true 表示 desc 與 payload 在同一個偶數 seq 下完整讀出且通過驗證；false 表示正在寫入、撕裂或無效
   */
  bool shm_read_slot_with_payload(
    const vrmv_shm_t *shm,
    uint32_t slot_idx,
    uint64_t expected_generation,
    vrmv_desc_t &out_desc,
    void *dst_payload,
    size_t dst_capacity
  ) noexcept;

  /**
   * @brief 寫入端 seqlock 寫入一筆 desc 與 payload 到 SHM（供單元測試模擬外掛高速寫入）。
   *        嚴格遵守：奇數 seq (vrmv_seq_write_begin) → 寫 desc 與 payload → 偶數 seq (vrmv_store_release_u64) → write_index。
   */
  void shm_write_slot_for_test(
    vrmv_shm_t *shm,
    const vrmv_desc_t &desc,
    const void *payload,
    uint64_t &inout_seq_counter
  ) noexcept;

}  // namespace vr::mv_pair
