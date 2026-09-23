/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once

// standard includes
#include <utility>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "audio.h"
#include "crypto.h"
#include "video.h"

namespace stream {
  constexpr auto VIDEO_STREAM_PORT = 9;
  constexpr auto CONTROL_PORT = 10;
  constexpr auto AUDIO_STREAM_PORT = 11;

  struct session_t;

  struct config_t {
    audio::config_t audio;
    video::config_t monitor;

    int packetsize;
    int minRequiredFecPackets;
    int mlFeatureFlags;
    int controlProtocolType;
    int audioQosType;
    int videoQosType;

    uint32_t encryptionFlagsEnabled;

    bool autoAdjustBitrate = true;

    std::optional<int> gcmap;
  };

  namespace session {
    enum class state_e : int {
      STOPPED,  ///< The session is stopped
      STOPPING,  ///< The session is stopping
      STARTING,  ///< The session is starting
      RUNNING,  ///< The session is running
    };

    std::shared_ptr<session_t> alloc(config_t &config, rtsp_stream::launch_session_t &launch_session);
    int start(session_t &session, const std::string &addr_string);
    void stop(session_t &session);
    void join(session_t &session);
    state_e state(session_t &session);

    // §M01-D：rtsp.cpp 看不到 session_t 的內部，經 accessor 取得／標記。
    // 建立這條 session 的 client 身分（TLS 憑證 UUID，空 = 未知）。
    const std::string &client_cert_uuid(session_t &session);
    // 標記「被同一 client 的新 /resume、/launch 取代」。必須在 stop 之前
    // 呼叫；join 拆除時據此把它的 QUIC 連線一併從 listener map 退役。
    void mark_superseded(session_t &session);
  }  // namespace session
}  // namespace stream
