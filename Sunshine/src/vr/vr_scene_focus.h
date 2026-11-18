/**
 * @file src/vr/vr_scene_focus.h
 * @brief §VR-FOCUS（2026-11-05）：PCVR session 期間讓 SteamVR 的場景程式（遊戲）保持前景視窗。
 *
 * 串流時 host 桌面沒有人操作，前景常常停在 SteamVR 的狀態視窗（vrmonitor）或 Steam。
 * Unreal 引擎的遊戲預設在視窗不在前景時把音量乘上 0（UnfocusedVolumeMultiplier），
 * 頭盔裡就完全沒聲音（2026-11-05 GRIP 實測：拉到前景後音量計從 0 跳到 0.37）；有些遊戲在背景還會降速。
 *
 * 2026-11-18：遊戲還沒變成場景程式之前也要顧。全螢幕獨占的遊戲（CarX Drift Racing Online）啟動後被 SteamVR 的
 * 狀態視窗搶走前景就自己縮到最小、整個暫停，永遠不會開始 VR 畫面——頭盔裡只有 SteamVR 的空場景，而 scene_pid
 * 一直是 0、原本的邏輯完全不會動。所以沒有場景程式時改看「這一場啟動的那個遊戲」的行程（launched_pids）。
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace vr::scene_focus {
  /// 開一條背景執行緒輪詢 scene_pid；回傳的物件解構時停止。不支援的平台回傳 nullptr。
  /// launched_pids：沒有場景程式時要顧的行程（這一場啟動的遊戲；可以是空的函式）。
  std::shared_ptr<void> start(std::function<uint32_t()> scene_pid, std::function<std::vector<uint32_t>()> launched_pids = {});
}  // namespace vr::scene_focus
