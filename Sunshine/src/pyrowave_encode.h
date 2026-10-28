/**
 * @file src/pyrowave_encode.h
 * @brief PyroWave GPU wavelet video encoder integration for Sunshine.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "platform/common.h"
#include "thread_safe.h"
#include "video.h"
#include "video_colorspace.h"

namespace pyrowave {

  /**
   * @brief Probe whether the system has a Vulkan GPU capable of PyroWave encoding.
   *        Result is cached after the first call.
   */
  bool is_available();

  /**
   * @brief Lightweight encode_device_t marker used when config.videoFormat == 3 (PyroWave).
   */
  class pyrowave_encode_device_t: public platf::encode_device_t {
  public:
    int convert(platf::img_t &img) override {
      return 0;
    }
  };

  /**
   * @brief Encoded PyroWave frame output ready for RTP packetization in stream.cpp.
   */
  struct encoded_frame_t {
    std::vector<uint8_t> data;
    int64_t frame_index = 0;
    bool idr = false;
    bool after_ref_frame_invalidation = false;
  };

  /**
   * @brief PyroWave encoding session implementing video::encode_session_t.
   */
  class pyrowave_encode_session_t: public video::encode_session_t {
  public:
    pyrowave_encode_session_t() = default;
    ~pyrowave_encode_session_t() override;

    pyrowave_encode_session_t(const pyrowave_encode_session_t &) = delete;
    pyrowave_encode_session_t &operator=(const pyrowave_encode_session_t &) = delete;

    bool init(
      platf::display_t *disp,
      const video::config_t &config,
      int capture_width,
      int capture_height,
      const video::sunshine_colorspace_t &colorspace
    );

    int convert(platf::img_t &img) override;
    void request_idr_frame() override;
    void request_normal_frame() override;
    void invalidate_ref_frames(int64_t first_frame, int64_t last_frame) override;
    bool request_intra_refresh(int frameCnt) override;

    void reconfigure_bitrate(int bitrate_kbps, int framerate);
    void reconfigure_fps(int framerate);

    encoded_frame_t encode_frame(int64_t frame_nr);

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl_;
  };

  std::unique_ptr<pyrowave_encode_session_t> make_encode_session(
    platf::display_t *disp,
    const video::config_t &config,
    int width,
    int height,
    std::unique_ptr<platf::encode_device_t> encode_device
  );

  int encode_pyrowave(
    int64_t frame_nr,
    pyrowave_encode_session_t &session,
    safe::mail_raw_t::queue_t<video::packet_t> &packets,
    void *channel_data,
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
    const video::vr_frame_meta_t *vr_meta
  );

}  // namespace pyrowave
