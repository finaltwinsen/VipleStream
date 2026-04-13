/**
 * @file src/relay.h
 * @brief VipleStream signaling relay client.
 *
 * Connects to the relay server via WebSocket, registers the server UUID
 * and periodically publishes the STUN endpoint so clients can discover us.
 */
#pragma once

#include <memory>
#include <string>
#include "platform/common.h"

namespace relay {

  /**
   * Start the relay client background thread.
   * Reads relay_url and relay_psk from config.
   * No-op if relay_url is empty.
   */
  std::unique_ptr<platf::deinit_t> start();

}  // namespace relay
