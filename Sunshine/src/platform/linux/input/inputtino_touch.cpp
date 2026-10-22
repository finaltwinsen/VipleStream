/**
 * @file src/platform/linux/input/inputtino_touch.cpp
 * @brief Definitions for inputtino touch input handling.
 */
// standard includes
#include <cmath>

// lib includes
#include <boost/locale.hpp>
#include <inputtino/input.hpp>
#include <libevdev/libevdev.h>

// local includes
#include "inputtino_common.h"
#include "inputtino_touch.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf::touch {
  void update(client_input_raw_t *raw, const touch_port_t &touch_port, const touch_input_t &touch) {
    if (!raw || !raw->touch) {
      return;
    }

    switch (touch.eventType) {
      case LI_TOUCH_EVENT_HOVER:
      case LI_TOUCH_EVENT_DOWN:
      case LI_TOUCH_EVENT_MOVE:
        {
          if (!std::isfinite(touch.x) || !std::isfinite(touch.y) || !std::isfinite(touch.pressureOrDistance)) {
            return;
          }

          int adjusted_angle = 0;
          if (touch.rotation != LI_ROT_UNKNOWN) {
            adjusted_angle = static_cast<int>(touch.rotation % 360);

            if (adjusted_angle > 90 && adjusted_angle < 270) {
              // Lower hemisphere
              adjusted_angle = 180 - adjusted_angle;
            }

            // Wrap the value if it's out of range
            if (adjusted_angle > 90) {
              adjusted_angle -= 360;
            } else if (adjusted_angle < -90) {
              adjusted_angle += 360;
            }
          }

          (*raw->touch).place_finger(touch.pointerId, touch.x, touch.y, touch.pressureOrDistance, adjusted_angle);
          break;
        }
      case LI_TOUCH_EVENT_CANCEL:
      case LI_TOUCH_EVENT_UP:
      case LI_TOUCH_EVENT_HOVER_LEAVE:
        {
          (*raw->touch).release_finger(touch.pointerId);
          break;
        }
        // TODO: LI_TOUCH_EVENT_CANCEL_ALL
    }
  }
}  // namespace platf::touch
