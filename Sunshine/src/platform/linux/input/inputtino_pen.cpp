/**
 * @file src/platform/linux/input/inputtino_pen.cpp
 * @brief Definitions for inputtino pen input handling.
 */
// standard includes
#include <cmath>

// lib includes
#include <boost/locale.hpp>
#include <inputtino/input.hpp>
#include <libevdev/libevdev.h>

// local includes
#include "inputtino_common.h"
#include "inputtino_pen.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf::pen {
  void update(client_input_raw_t *raw, const touch_port_t &touch_port, const pen_input_t &pen) {
    if (!raw || !raw->pen) {
      return;
    }

    if (!std::isfinite(pen.x) || !std::isfinite(pen.y) || !std::isfinite(pen.pressureOrDistance)) {
      return;
    }

    // First set the buttons
    (*raw->pen).set_btn(inputtino::PenTablet::PRIMARY, pen.penButtons & LI_PEN_BUTTON_PRIMARY);
    (*raw->pen).set_btn(inputtino::PenTablet::SECONDARY, pen.penButtons & LI_PEN_BUTTON_SECONDARY);
    (*raw->pen).set_btn(inputtino::PenTablet::TERTIARY, pen.penButtons & LI_PEN_BUTTON_TERTIARY);

    // Set the tool
    inputtino::PenTablet::TOOL_TYPE tool;
    switch (pen.toolType) {
      case LI_TOOL_TYPE_PEN:
        tool = inputtino::PenTablet::PEN;
        break;
      case LI_TOOL_TYPE_ERASER:
        tool = inputtino::PenTablet::ERASER;
        break;
      default:
        tool = inputtino::PenTablet::SAME_AS_BEFORE;
        break;
    }

    // Normalize rotation value to 0-359 degree range
    auto rotation = pen.rotation;
    if (rotation != LI_ROT_UNKNOWN) {
      rotation %= 360;
    }

    // Here we receive:
    //  - Rotation: degrees from vertical in Y dimension (parallel to screen, 0..360)
    //  - Tilt: degrees from vertical in Z dimension (perpendicular to screen, 0..90)
    float tilt_x = 0.0f;
    float tilt_y = 0.0f;
    // Convert polar coordinates into Y tilt angles
    if (pen.tilt != LI_TILT_UNKNOWN && rotation != LI_ROT_UNKNOWN && pen.tilt <= 90) {
      auto rotation_rads = deg2rad(rotation);
      auto tilt_rads = deg2rad(pen.tilt);
      auto r = std::sin(tilt_rads);
      auto z = std::cos(tilt_rads);

      constexpr float RAD_TO_DEG = 57.295779513082320876f;  // 180 / pi
      tilt_x = std::atan2(std::sin(-rotation_rads) * r, z) * RAD_TO_DEG;
      tilt_y = std::atan2(std::cos(-rotation_rads) * r, z) * RAD_TO_DEG;
    }

    bool is_touching = pen.eventType == LI_TOUCH_EVENT_DOWN || pen.eventType == LI_TOUCH_EVENT_MOVE;

    (*raw->pen).place_tool(tool, pen.x, pen.y, is_touching ? pen.pressureOrDistance : -1.0f, is_touching ? -1.0f : pen.pressureOrDistance, tilt_x, tilt_y);
  }
}  // namespace platf::pen
