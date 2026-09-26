/**
 * @file src/platform/windows/vr_platform_win.cpp
 * @brief VipleStream 2.0 §VR：Windows 平台的 VR 支援回報。
 *
 * M1a 只有 stub 模式（桌面擷取＋tracking 回聲），不需要 SteamVR driver，所以 Windows
 * 一律回報支援；實際是否接受 VR session 由 config `vr_pcvr` 決定。M1b 之後
 * SteamVR driver／IPC 的探測會放在這裡。
 */
#include "src/vr/vr_platform.h"

namespace vr {
  bool platform_supported() {
    return true;
  }
}  // namespace vr
