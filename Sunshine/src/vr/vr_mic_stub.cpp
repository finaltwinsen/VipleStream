/**
 * @file src/vr/vr_mic_stub.cpp
 * @brief §VR-MIC：非 Windows 平台沒有虛擬麥克風（Linux 的 host 只做 2D）。
 */
#include "vr_mic.h"

namespace vr::mic {
  bool available() {
    return false;
  }

  std::unique_ptr<sink_t> sink_t::create() {
    return nullptr;
  }
}  // namespace vr::mic
