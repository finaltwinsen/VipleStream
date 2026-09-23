/**
 * @file src/rtsp.h
 * @brief Declarations for RTSP streaming.
 */
#pragma once

// standard includes
#include <atomic>

// local includes
#include "crypto.h"
#include "thread_safe.h"

namespace rtsp_stream {
  constexpr auto RTSP_SETUP_PORT = 21;

  struct launch_session_t {
    uint32_t id;

    crypto::aes_t gcm_key;
    crypto::aes_t iv;

    std::string av_ping_payload;
    uint32_t control_connect_data;

    bool host_audio;
    std::string unique_id;
    int width;
    int height;
    int fps;
    int gcmap;
    int appid;
    int surround_info;
    std::string surround_params;
    bool continuous_audio;
    bool enable_hdr;
    bool enable_sops;

    std::optional<crypto::cipher::gcm_t> rtsp_cipher;
    std::string rtsp_url_scheme;
    uint32_t rtsp_iv_counter;

    // §M01-D 2026-09-23：發出 /launch 或 /resume 的 client 身分（TLS
    // client 憑證 UUID，nvhttp 的 caller_uuid_for()）。空字串 = 未知
    // （relay localhost 的 HTTP 路徑沒有 client 憑證）。不能用 unique_id：
    // moonlight-common-c 一律送 0123456789ABCDEF，分不出是哪個 client。
    // 用途：同一 client 被強制關閉後立刻重開時，收掉它自己殘留的
    // stream session 與還沒被消化的 pending launch（見 rtsp.cpp）。
    std::string client_cert_uuid;
  };

  void launch_session_raise(std::shared_ptr<launch_session_t> launch_session);

  /**
   * @brief Clear state for the specified launch session.
   * @param launch_session_id The ID of the session to clear.
   */
  void launch_session_clear(uint32_t launch_session_id);

  /**
   * @brief Get the number of active sessions.
   * @return Count of active sessions.
   */
  int session_count();

  /**
   * @brief Terminates all running streaming sessions.
   */
  void terminate_sessions();

  /**
   * @brief §M01-D：終止指定 client（TLS 憑證 UUID）自己殘留的 stream session。
   *
   * 同一個 client 被強制關閉（沒送 /cancel、沒斷 ENet）後立刻重開時，
   * 舊 session 仍在 host 上編碼；MP-QUIC 只以 IP 當 key，新 client 的
   * QUIC 連線一連上，舊 session 的畫面就會灌進新連線（-101 事故）。
   * nvhttp 的 /resume、/launch 在擁有權判斷通過之後、檢查
   * session_count() 之前呼叫，讓流程走「斷線後 resume」的正規路徑。
   *
   * @param uuid 呼叫者的 TLS 憑證 UUID；空字串（身分未知）時不做任何事。
   * @param via  呼叫端（"/resume" 或 "/launch"），只用於 log。
   * @return 被終止的 session 數。
   */
  int terminate_sessions_for_client(const std::string &uuid, const char *via);

  /**
   * @brief Runs the RTSP server loop.
   */
  void start();
}  // namespace rtsp_stream
