/**
 * @file src/platform/windows/display_vram.h
 * @brief Windows VRAM 擷取後端共用的影像型別（img_d3d_t）與 fence 輔助類別（texture_fence_helper）。
 * @details M1b S1-04：兩者原本定義在 display_vram.cpp 內部（舊版 :135-231），原樣搬到這裡，
 *          讓 display_vr_t（display_vr.cpp，S1-08）也能用同一套影像與 fence 模型。純搬移、行為中性。
 */
#pragma once

// standard includes
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

// local includes
#include "display.h"
#include "src/logging.h"

namespace platf::dxgi {

  struct img_d3d_t: public platf::img_t {
    // These objects are owned by the display_t's ID3D11Device
    texture2d_t capture_texture;
    render_target_t capture_rt;

    // D3D11 Fence for GPU-GPU synchronization (replaces keyed mutex for lower overhead)
    fence_t capture_fence;
    HANDLE fence_shared_handle = {};
    std::atomic<uint64_t> fence_value {0};

    // This is the shared handle used by hwdevice_t to open capture_texture
    HANDLE encoder_texture_handle = {};

    // Set to true if the image corresponds to a dummy texture used prior to
    // the first successful capture of a desktop frame
    bool dummy = false;

    // Set to true if the image is blank (contains no content at all, including a cursor)
    bool blank = true;

    // Unique identifier for this image
    uint32_t id = 0;

    // DXGI format of this image texture
    DXGI_FORMAT format;

    virtual ~img_d3d_t() override {
      if (fence_shared_handle) {
        CloseHandle(fence_shared_handle);
      }
      if (encoder_texture_handle) {
        CloseHandle(encoder_texture_handle);
      }
    };
  };

  /**
   * RAII helper that signals a D3D11 fence upon scope exit.
   * When the helper goes out of scope, it signals the fence on the GPU command queue,
   * telling the encoder device that capture writes are complete.
   * Unlike the previous keyed mutex, the signal is NON-BLOCKING for the CPU.
   */
  struct texture_fence_helper {
    device_ctx4_t _ctx4;
    img_d3d_t *_img = nullptr;
    bool _armed = false;

    texture_fence_helper(const texture_fence_helper &) = delete;
    texture_fence_helper &operator=(const texture_fence_helper &) = delete;

    texture_fence_helper(texture_fence_helper &&other) noexcept
        : _ctx4(std::move(other._ctx4)), _img(other._img), _armed(other._armed) {
      other._armed = false;
      other._img = nullptr;
    }

    texture_fence_helper &operator=(texture_fence_helper &&other) noexcept {
      signal_if_armed();
      _ctx4 = std::move(other._ctx4);
      _img = other._img;
      _armed = other._armed;
      other._armed = false;
      other._img = nullptr;
      return *this;
    }

    texture_fence_helper(std::nullptr_t): _ctx4(), _img(nullptr), _armed(false) {}

    texture_fence_helper(ID3D11DeviceContext4 *ctx4, img_d3d_t *img)
        : _img(img), _armed(false) {
      if (ctx4) {
        _ctx4.reset(ctx4);
        _ctx4->AddRef();
      }
    }

    ~texture_fence_helper() {
      signal_if_armed();
    }

    void signal_if_armed() {
      if (_armed && _ctx4 && _img && _img->capture_fence) {
        auto val = _img->fence_value.fetch_add(1) + 1;
        _ctx4->Signal(_img->capture_fence.get(), val);
        _armed = false;
      }
    }

    bool arm() {
      if (!_ctx4 || !_img || !_img->capture_fence) {
        BOOST_LOG(error) << "Cannot arm fence helper: missing context or fence";
        return false;
      }
      _armed = true;
      return true;
    }
  };
}  // namespace platf::dxgi
