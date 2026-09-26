/**
 * @file src/vr/vr_platform.h
 * @brief VipleStream 2.0 §VR：平台層的 VR 支援介面（docs/vr_architecture.md §3.11）。
 *
 * 實作依平台分開：
 *   - Windows：src/platform/windows/vr_platform_win.cpp（回報支援）
 *   - Linux／macOS：src/platform/linux/vr_stub.cpp（一律回報不支援）
 * 不支援的平台上 /serverinfo 的 PCVR bit 恆為 0，/launch 帶 vr=1 一律回 VR_DISABLED
 * （不變式 10）。
 */
#pragma once

namespace vr {
  /**
   * @brief 這個平台能不能跑 PCVR session。
   * @return Windows 為 true；其他平台為 false。
   */
  bool platform_supported();
}  // namespace vr
