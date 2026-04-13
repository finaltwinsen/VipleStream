/**
 * @file src/stun.h
 * @brief Declarations for STUN NAT traversal.
 */
#pragma once

#include <memory>
#include <string>
#include <atomic>

#include "platform/common.h"

namespace stun {

  enum nat_type_e {
    NAT_UNKNOWN,
    NAT_FULL_CONE,       // EIM + EIF: any external host can send
    NAT_RESTRICTED,      // EIM + ADF: only hosts we've sent to can reply
    NAT_PORT_RESTRICTED, // EIM + APDF: only exact IP:port we sent to can reply
    NAT_SYMMETRIC,       // APDM: different mapping per destination → hole punch won't work
  };

  struct endpoint_t {
    std::string ip;
    uint16_t port = 0;
    nat_type_e nat_type = NAT_UNKNOWN;

    bool valid() const { return !ip.empty() && port > 0; }
    std::string to_string() const { return ip + ":" + std::to_string(port); }
  };

  // Get the last discovered STUN endpoint (thread-safe)
  endpoint_t get_endpoint();

  // Start the STUN prober background thread (call once from main)
  std::unique_ptr<platf::deinit_t> start();

}  // namespace stun
