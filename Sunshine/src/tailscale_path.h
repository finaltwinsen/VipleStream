/**
 * @file src/tailscale_path.h
 * @brief §TS-PATH：串流對象走 Tailscale 中繼時，自動嘗試恢復直連。
 *
 * 只在 client 位址落在 100.64.0.0/10 時啟動。模組自己持有狀態，不引用 session。
 */
#pragma once

#include <cstdint>
#include <string>

namespace tailscale_path {

  /// 回傳這次監看的代號（不是 Tailscale 位址回 0）
  std::uint64_t on_session_start(const std::string &client_ip);

  /// 只有 gen 仍是目前那一次監看時才停；gen==0 什麼都不做。必須立刻返回：只改狀態＋notify，不可 join、不可 sleep。
  void on_session_end(std::uint64_t gen);
  /// §ABR-PATH-UPGRADE：到目前為止「中繼 → 直連」發生過幾次（行程累計，只增不減）。碼率控制用它知道路徑換了
  std::uint64_t direct_upgrade_count();

}  // namespace tailscale_path
