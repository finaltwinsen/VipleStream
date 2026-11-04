/**
 * @file src/input.h
 * @brief Declarations for gamepad, keyboard, and mouse input handling.
 */
#pragma once

// standard includes
#include <functional>

// local includes
#include "platform/common.h"
#include "thread_safe.h"

namespace input {
  struct input_t;

  void print(void *input);
  void reset(std::shared_ptr<input_t> &input);
  void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data);

  [[nodiscard]] std::unique_ptr<platf::deinit_t> init();

  bool probe_gamepads();

  std::shared_ptr<input_t> alloc(safe::mail_t mail);

  // §VR-PADMODE：VR 手把模式的虛擬 Xbox 360 手把（不屬於任何 client 手把；回饋送到這個 session 的佇列、
  // controllerNumber 用 15，client 沒有這支手把會略過）。都在 input 的 task pool 執行緒上執行，跟一般手把同一條序列。
  constexpr std::uint8_t VR_PAD_CONTROLLER_NUMBER = 15;
  /// @return 全域手把 id；< 0＝建立失敗（沒有 ViGEmBus、或手把數滿了）
  int vr_pad_alloc(const std::shared_ptr<input_t> &input);
  void vr_pad_update(int id, const platf::gamepad_state_t &state);
  void vr_pad_free(int id);

  struct touch_port_t: public platf::touch_port_t {
    int env_width;
    int env_height;

    // Offset x and y coordinates of the client
    float client_offsetX;
    float client_offsetY;

    float scalar_inv;
    float scalar_tpcoords;

    int env_logical_width;
    int env_logical_height;

    explicit operator bool() const {
      return width != 0 && height != 0 && env_width != 0 && env_height != 0;
    }
  };

  /**
   * @brief Scale the ellipse axes according to the provided size.
   * @param val The major and minor axis pair.
   * @param rotation The rotation value from the touch/pen event.
   * @param scalar The scalar cartesian coordinate pair.
   * @return The major and minor axis pair.
   */
  std::pair<float, float> scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar);
}  // namespace input
