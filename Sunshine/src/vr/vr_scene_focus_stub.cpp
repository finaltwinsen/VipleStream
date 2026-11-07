/**
 * @file src/vr/vr_scene_focus_stub.cpp
 * @brief §VR-FOCUS：非 Windows 平台沒有 PCVR，不做事。
 */
#include "vr_scene_focus.h"

namespace vr::scene_focus {
  std::shared_ptr<void> start(std::function<uint32_t()>) {
    return nullptr;
  }
}  // namespace vr::scene_focus
