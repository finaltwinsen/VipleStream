/**
 * @file src/platform/linux/vr_stub.cpp
 * @brief VipleStream 2.0 §VR：非 Windows 平台的 VR stub。
 *
 * Linux PCVR 不在 2.0 範圍內（docs/vr_architecture.md 不變式 10）：VR 程式碼照常
 * 編譯，但這裡恆回報不支援，所以 /serverinfo 的 PCVR bit 固定為 0、/launch 帶
 * vr=1 一律回 VR_DISABLED，linux-server .deb 的建置與 KMS 擷取行為都不受影響。
 * macOS 的 cmake 也掛這個檔（macOS 目前不出貨，只是讓連結不缺符號）。
 */
#include "src/vr/vr_platform.h"

namespace vr {
  bool platform_supported() {
    return false;
  }
}  // namespace vr
