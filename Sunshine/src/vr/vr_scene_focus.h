/**
 * @file src/vr/vr_scene_focus.h
 * @brief §VR-FOCUS（2026-11-05）：PCVR session 期間讓 SteamVR 的場景程式（遊戲）保持前景視窗。
 *
 * 串流時 host 桌面沒有人操作，前景常常停在 SteamVR 的狀態視窗（vrmonitor）或 Steam。
 * Unreal 引擎的遊戲預設在視窗不在前景時把音量乘上 0（UnfocusedVolumeMultiplier），
 * 頭盔裡就完全沒聲音（2026-11-05 GRIP 實測：拉到前景後音量計從 0 跳到 0.37）；有些遊戲在背景還會降速。
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace vr::scene_focus {
  /// 開一條背景執行緒輪詢 scene_pid；回傳的物件解構時停止。不支援的平台回傳 nullptr。
  std::shared_ptr<void> start(std::function<uint32_t()> scene_pid);
}  // namespace vr::scene_focus
