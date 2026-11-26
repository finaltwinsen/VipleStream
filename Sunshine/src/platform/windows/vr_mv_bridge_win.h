/**
 * @file src/platform/windows/vr_mv_bridge_win.h
 * @brief VipleStream §VR-GAME-MV（M-MV1）：遊戲內運動向量外掛（UEVR / Unity MV Plugin）之 Named Pipe 與 SHM 橋接器。
 *
 * 設計依據：docs/specs/vr_game_motion_vectors.md §3.1～§3.3、Sunshine/src/vr/vr_mv_ipc_abi.h 規則 A～D。
 *   - 單一連線 Named Pipe：`\\.\pipe\VipleStreamVRMV-<session_id>`。
 *   - 建立時 DACL 僅 SYSTEM（O:SY），主控台使用者登入後才加 logon SID ACE（不含 FILE_CREATE_PIPE_INSTANCE）。
 *   - 驗證：client PID 前後兩次一致、同 session、token 為同一主控台使用者且 Medium+ 非 AppContainer、
 *     且 PID 必須等於目前 `vr::bridge::status().scene_pid`（不符或為 0 → REJECT IDENTITY）。
 *   - 握手：建立不具名 section（`VRMV_SECTION_SIZE`、SD 僅 SY），寫妥 `vrmv_server_block_t`，
 *     以 `FILE_MAP_READ | FILE_MAP_WRITE` DuplicateHandle 進遊戲行程，送出 `WELCOME`。
 *   - 穩態：每 100 ms 更新 `server_heartbeat_qpc`；pipe 斷線、`scene_pid` 改變、或 VR session 結束時，
 *     送出 `BYE`（若可送）、斷線、unmap、generation 作廢。
 */
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "src/vr/vr_mv_ipc_abi.h"

namespace vr::mv_bridge {

  namespace detail {
    /// 快速路徑旗標：目前是否有已完成握手且活躍的遊戲 MV 外掛連線（供擷取執行緒單次判斷，零鎖成本）
    extern std::atomic<bool> g_active_connected;
  }  // namespace detail

  /**
   * @brief 啟動 MV IPC 橋接執行緒並開啟 Named Pipe（若 `vr_game_mv != disabled` 且 `vr_pcvr != disabled`）。
   * @details 冪等：若已在執行中則直接回傳 true。
   * @return 啟動成功或已在執行中回傳 true；設定停用或初始化失敗回傳 false。
   */
  bool start();

  /**
   * @brief 停止 MV IPC 橋接執行緒，若有連線則送出 BYE、斷開 pipe、釋放 SHM 並使 generation 作廢。
   * @details 冪等：可安全地重複呼叫或在未啟動時呼叫。
   */
  void stop();

  /**
   * @brief 快速判斷目前是否有已握手成功且在線的 MV 外掛連線。
   * @details 擷取執行緒在未啟用或無連線時僅需這一次 inline atomic 讀取（零成本）。
   */
  inline bool is_active() noexcept {
    return detail::g_active_connected.load(std::memory_order_acquire);
  }

  /**
   * @brief 查詢背景橋接服務執行緒是否正在執行。
   */
  bool is_running() noexcept;

  /**
   * @brief 快速讀取目前 SHM writer 的 `write_index` 與 `writer_heartbeat_qpc`（供 ≤ 0.5 ms 短輪詢使用）。
   * @return 若目前有活躍連線回傳 true，否則回傳 false。
   */
  bool peek_write_index(uint64_t &out_write_index, int64_t *out_writer_heartbeat_qpc = nullptr) noexcept;

  /**
   * @brief 從目前活躍的 SHM 拍攝最近 `VRMV_SLOTS`（4）筆已通過 seqlock 與欄位範圍驗證的描述符本地副本。
   * @param out_descs 輸出的本地描述符陣列。
   * @param out_count 實際有效的描述符筆數（0..VRMV_SLOTS）。
   * @param out_write_index 選填：拍下時的 `writer.write_index`。
   * @param out_writer_heartbeat_qpc 選填：拍下時的 `writer.writer_heartbeat_qpc`。
   * @param out_writer_block 選填：整份複製一次的 `vrmv_writer_block_t` 本地副本。
   * @return 若目前有活躍連線回傳 true，否則回傳 false。
   */
  bool snapshot_latest(
    std::array<vrmv_desc_t, VRMV_SLOTS> &out_descs,
    size_t &out_count,
    uint64_t *out_write_index = nullptr,
    int64_t *out_writer_heartbeat_qpc = nullptr,
    vrmv_writer_block_t *out_writer_block = nullptr
  ) noexcept;

  /**
   * @brief 在 seqlock 保護下複製已配對描述符對應 slot 的 payload 至本地緩衝區，並驗證前後序號與 `mv_frame_id` 一致。
   * @details 複製成功時會以 release 語意更新 `shm->server.last_consumed_mv_frame_id`。
   * @param desc 已配對並驗證過的描述符本地副本。
   * @param dst 目標緩衝區（容量至少 `desc.payload_bytes`）。
   * @param dst_capacity 目標緩衝區容量（bytes）。
   * @return 複製成功且 seqlock 前後一致回傳 true；撕裂、被覆寫或無連線回傳 false。
   */
  bool copy_payload(const vrmv_desc_t &desc, void *dst, size_t dst_capacity) noexcept;

  /**
   * @brief 以 `mv_frame_id` 在 seqlock 保護下複製對應 slot 的 payload 至本地緩衝區。
   * @param mv_frame_id 目標 MV 幀序號（> 0）。
   * @param dst 目標緩衝區。
   * @param dst_capacity 目標緩衝區容量（bytes）。
   * @param out_desc 選填：成功時輸出經過 seqlock 驗證的描述符本地副本。
   * @return 複製成功且 seqlock 前後一致回傳 true；撕裂、被覆寫或無連線回傳 false。
   */
  bool copy_payload(uint64_t mv_frame_id, void *dst, size_t dst_capacity, vrmv_desc_t *out_desc = nullptr) noexcept;

  /**
   * @brief 讀取目前連線的 `vrmv_writer_block_t` 本地副本（供 10 秒統計 log 讀取外掛端計數）。
   * @param out_writer 輸出的 writer block 本地副本。
   * @param out_generation 選填：目前連線的 generation。
   * @return 若有活躍連線回傳 true，否則回傳 false。
   */
  bool read_writer_block(vrmv_writer_block_t &out_writer, uint64_t *out_generation = nullptr) noexcept;

}  // namespace vr::mv_bridge
