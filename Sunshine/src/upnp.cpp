/**
 * @file src/upnp.cpp
 * @brief Definitions for UPnP port mapping.
 */
// standard includes
#include <stddef.h>  // workaround for type_t error in miniupnpc 2.3.3, see https://github.com/miniupnp/miniupnp/commit/e263ab6f56c382e10fed31347ec68095d691a0e8

// lib includes
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>  // VipleStream §UPNP-DIAG: strupnperror()（libminiupnpc 已含，pkg-config 已 -lminiupnpc）

// local includes
#include "config.h"
#include "confighttp.h"
#include "globals.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "rtsp.h"
#include "stream.h"
#include "upnp.h"
#include "utility.h"

using namespace std::literals;

namespace upnp {

  struct mapping_t {
    struct {
      std::string wan;
      std::string lan;
      std::string proto;
    } port;

    std::string description;
  };

  /**
   * VipleStream §UPNP-DIAG: UPNP_GetValidIGD() 狀態碼 → 文字。
   * miniupnpc 2.2.8（MINIUPNPC_API_VERSION 18，Changelog 2024/05/08）起回傳值語意改為 0..4：
   * 2 = IGD 的 WAN 位址是保留位址（雙層 NAT / CGNAT）、3 = IGD 未連線、4 = 找到 UPnP 裝置但不是 IGD。
   * 上游 Sunshine 的對照表仍是舊語意，所以 4 只印 "Unknown status"、2/3 的文字錯位——
   * host .226 每 ~124 s 一行 "Error: Unknown status" 就是這個 4。
   * 具名巨集 UPNP_NO_IGD..UPNP_UNKNOWN_DEVICE 要到 2.3.0（API 19）才有，這裡用數值以涵蓋 API 18；
   * Linux .deb 下限 miniupnpc 2.2.4（API 17）走舊表。呼叫端一律另外印出數值碼。
   */
  static std::string_view status_string(int status) {
#if (MINIUPNPC_API_VERSION >= 18)
    switch (status) {
      case 0:  // UPNP_NO_IGD
        return "No IGD device found"sv;
      case 1:  // UPNP_CONNECTED_IGD
        return "Valid IGD device found"sv;
      case 2:  // UPNP_PRIVATEIP_IGD
        return "Valid IGD device found, but its WAN address is reserved/private (double NAT or CGNAT)"sv;
      case 3:  // UPNP_DISCONNECTED_IGD
        return "Valid IGD device found, but it isn't connected"sv;
      case 4:  // UPNP_UNKNOWN_DEVICE
        return "A UPnP device has been found, but it wasn't recognized as an IGD"sv;
    }
#else
    switch (status) {
      case 0:
        return "No IGD device found"sv;
      case 1:
        return "Valid IGD device found"sv;
      case 2:
        return "Valid IGD device found, but it isn't connected"sv;
      case 3:
        return "A UPnP device has been found, but it wasn't recognized as an IGD"sv;
    }
#endif

    return "Unrecognized IGD status"sv;
  }

  /**
   * VipleStream §UPNP-DIAG: miniupnpc / UPnP SOAP 錯誤碼 → "<數值> (<文字>)"。
   * strupnperror() 涵蓋 UPNPCOMMAND_*、UPNPDISCOVER_* 與 UPnP 標準錯誤碼（606、714、718…），
   * 對未定義碼回傳 NULL，此時只印數值。注意它不適用於 UPNP_GetValidIGD 的 0..4 狀態碼（用 status_string）。
   */
  static std::string err_string(int err) {
    const char *text = strupnperror(err);
    return text ? std::to_string(err) + " ("s + text + ')' : std::to_string(err);
  }

  /**
   * This function is a wrapper around UPNP_GetValidIGD() that returns the status code. There is a pre-processor
   * check to determine which version of the function to call based on the version of the MiniUPnPc library.
   */
  int UPNP_GetValidIGDStatus(device_t &device, urls_t *urls, IGDdatas *data, std::array<char, INET6_ADDRESS_STRLEN> &lan_addr) {
#if (MINIUPNPC_API_VERSION >= 18)
    return UPNP_GetValidIGD(device.get(), &urls->el, data, lan_addr.data(), (int) lan_addr.size(), nullptr, 0);
#else
    return UPNP_GetValidIGD(device.get(), &urls->el, data, lan_addr.data(), (int) lan_addr.size());
#endif
  }

  class deinit_t: public platf::deinit_t {
  public:
    deinit_t() {
      auto rtsp = std::to_string(net::map_port(rtsp_stream::RTSP_SETUP_PORT));
      auto video = std::to_string(net::map_port(stream::VIDEO_STREAM_PORT));
      auto audio = std::to_string(net::map_port(stream::AUDIO_STREAM_PORT));
      auto control = std::to_string(net::map_port(stream::CONTROL_PORT));
      auto gs_http = std::to_string(net::map_port(nvhttp::PORT_HTTP));
      auto gs_https = std::to_string(net::map_port(nvhttp::PORT_HTTPS));
      auto wm_http = std::to_string(net::map_port(confighttp::PORT_HTTPS));

      mappings.assign({
        {{rtsp, rtsp, "TCP"s}, "Sunshine - RTSP"s},
        {{video, video, "UDP"s}, "Sunshine - Video"s},
        {{audio, audio, "UDP"s}, "Sunshine - Audio"s},
        {{control, control, "UDP"s}, "Sunshine - Control"s},
        {{gs_http, gs_http, "TCP"s}, "Sunshine - Client HTTP"s},
        {{gs_https, gs_https, "TCP"s}, "Sunshine - Client HTTPS"s},
      });

      // Only map port for the Web Manager if it is configured to accept connection from WAN
      if (net::from_enum_string(config::nvhttp.origin_web_ui_allowed) > net::LAN) {
        mappings.emplace_back(mapping_t {{wm_http, wm_http, "TCP"s}, "Sunshine - Web UI"s});
      }

      // Start the mapping thread
      upnp_thread = std::thread {&deinit_t::upnp_thread_proc, this};
    }

    ~deinit_t() {
      upnp_thread.join();
    }

    /**
     * VipleStream §UPNP-DIAG: 週期性失敗的去重／降級。
     * 同一種失敗（op + code）連續出現時：前 FAIL_FULL_DETAIL_CYCLES 輪照常以 error 印完整訊息，
     * 第 FAIL_FULL_DETAIL_CYCLES 輪再補一行 warning 宣告降級；之後每 FAIL_REPORT_INTERVAL 才印一行 warning
     * 帶連續次數與估計時間；失敗種類一變就重新計數；成功時由 report_cycle_success() 印一行 info 並歸零。
     * 上游行為（每 REFRESH_INTERVAL 一行 error、永不停止）在 host 上是 45 次 × ~124 s 全是同一句
     * "Unknown status"。這裡只改 log，不改重試節奏，也不影響串流（本執行緒獨立、無人等它）。
     * 只在 upnp 執行緒上呼叫，不需要鎖。
     */
    void report_cycle_failure(std::string_view op, int code, const std::string &detail) {
      auto now = std::chrono::steady_clock::now();
      if (fail.op == op && fail.code == code) {
        ++fail.streak;
      } else {
        fail.op = op;
        fail.code = code;
        fail.streak = 1;
      }

      if (fail.streak <= FAIL_FULL_DETAIL_CYCLES) {
        BOOST_LOG(error) << "[VIPLE-UPNP] "sv << op << " failed (code "sv << code << "): "sv << detail
                         << " [attempt "sv << fail.streak << '/' << FAIL_FULL_DETAIL_CYCLES
                         << ", retry in "sv << REFRESH_INTERVAL.count() << " s]"sv;
        if (fail.streak == FAIL_FULL_DETAIL_CYCLES) {
          BOOST_LOG(warning) << "[VIPLE-UPNP] "sv << op << " failed "sv << FAIL_FULL_DETAIL_CYCLES
                             << " cycles in a row with the same code; further reports every "sv
                             << std::chrono::duration_cast<std::chrono::minutes>(FAIL_REPORT_INTERVAL).count()
                             << " min until it recovers"sv;
        }
        fail.last_report = now;
      } else if (now - fail.last_report >= FAIL_REPORT_INTERVAL) {
        auto elapsed_min = fail.streak * REFRESH_INTERVAL.count() / 60;
        BOOST_LOG(warning) << "[VIPLE-UPNP] "sv << op << " still failing (code "sv << code << "): "sv << detail
                           << " [x"sv << fail.streak << " consecutive cycles, ~"sv << elapsed_min << " min]"sv;
        fail.last_report = now;
      }
    }

    // VipleStream §UPNP-DIAG: 從失敗狀態恢復時印一行 info（沒失敗過就安靜）
    void report_cycle_success(const std::string &what) {
      if (fail.streak == 0) {
        return;
      }
      BOOST_LOG(info) << "[VIPLE-UPNP] recovered: "sv << what << " [after "sv << fail.streak
                      << " failed cycle(s), last failure: "sv << fail.op << " code "sv << fail.code << ']';
      fail = {};
    }

    /**
     * VipleStream §UPNP-DIAG: map_upnp_port() 內 per-port 錯誤行的等級。
     * port mapping 連續失敗達 FAIL_FULL_DETAIL_CYCLES 輪後降為 debug（min_log_level=debug 仍看得到），
     * 由 report_cycle_failure() 的彙整行負責提醒。
     */
    boost::log::sources::severity_logger<int> &port_fail_log() {
      return (fail.op == "port mapping"sv && fail.streak >= FAIL_FULL_DETAIL_CYCLES) ? debug : error;
    }

    /**
     * @brief Opens pinholes for IPv6 traffic if the IGD is capable.
     * @details Not many IGDs support this feature, so we perform error logging with debug level.
     * @return `true` if the pinholes were opened successfully.
     */
    bool create_ipv6_pinholes() {
      int err;
      device_t device {upnpDiscover(2000, nullptr, nullptr, 0, IPv6, 2, &err)};
      if (!device || err) {
        BOOST_LOG(debug) << "Couldn't discover any IPv6 UPNP devices"sv;
        return false;
      }

      IGDdatas data;
      urls_t urls;
      std::array<char, INET6_ADDRESS_STRLEN> lan_addr;
      auto status = upnp::UPNP_GetValidIGDStatus(device, &urls, &data, lan_addr);
      if (status != 1 && status != 2) {
        BOOST_LOG(debug) << "[VIPLE-UPNP] UPNP_GetValidIGD(IPv6) status "sv << status << ": "sv << status_string(status);
        return false;
      }

      if (data.IPv6FC.controlurl[0] != 0) {
        int firewallEnabled;
        int pinholeAllowed;

        // Check if this firewall supports IPv6 pinholes
        err = UPNP_GetFirewallStatus(urls->controlURL_6FC, data.IPv6FC.servicetype, &firewallEnabled, &pinholeAllowed);
        if (err == UPNPCOMMAND_SUCCESS) {
          BOOST_LOG(debug) << "UPnP IPv6 firewall control available. Firewall is "sv
                           << (firewallEnabled ? "enabled"sv : "disabled"sv)
                           << ", pinhole is "sv
                           << (pinholeAllowed ? "allowed"sv : "disallowed"sv);

          if (pinholeAllowed) {
            // Create pinholes for each port
            auto mapping_period = std::to_string(PORT_MAPPING_LIFETIME.count());
            auto shutdown_event = mail::man->event<bool>(mail::shutdown);

            for (auto it = std::begin(mappings); it != std::end(mappings) && !shutdown_event->peek(); ++it) {
              auto mapping = *it;
              char uniqueId[8];

              // Open a pinhole for the LAN port, since there will be no WAN->LAN port mapping on IPv6
              err = UPNP_AddPinhole(urls->controlURL_6FC, data.IPv6FC.servicetype, "", "0", lan_addr.data(), mapping.port.lan.c_str(), mapping.port.proto.c_str(), mapping_period.c_str(), uniqueId);
              if (err == UPNPCOMMAND_SUCCESS) {
                BOOST_LOG(debug) << "Successfully created pinhole for "sv << mapping.port.proto << ' ' << mapping.port.lan;
              } else {
                BOOST_LOG(debug) << "[VIPLE-UPNP] AddPinhole "sv << mapping.port.proto << ' ' << mapping.port.lan << " failed: "sv << err_string(err);
              }
            }

            return err == 0;
          } else {
            BOOST_LOG(debug) << "IPv6 pinholes are not allowed by the IGD"sv;
            return false;
          }
        } else {
          BOOST_LOG(debug) << "[VIPLE-UPNP] GetFirewallStatus failed: "sv << err_string(err);
          return false;
        }
      } else {
        BOOST_LOG(debug) << "IPv6 Firewall Control is not supported by the IGD"sv;
        return false;
      }
    }

    /**
     * @brief Maps a port via UPnP.
     * @param data IGDdatas from UPNP_GetValidIGD()
     * @param urls urls_t from UPNP_GetValidIGD()
     * @param lan_addr Local IP address to map to
     * @param mapping Information about port to map
     * @return `true` on success.
     */
    bool map_upnp_port(const IGDdatas &data, const urls_t &urls, const std::string &lan_addr, const mapping_t &mapping) {
      char intClient[16];
      char intPort[6];
      char desc[80];
      char enabled[4];
      char leaseDuration[16];
      bool indefinite = false;

      // First check if this port is already mapped successfully
      BOOST_LOG(debug) << "Checking for existing UPnP port mapping for "sv << mapping.port.wan;
      auto err = UPNP_GetSpecificPortMappingEntry(
        urls->controlURL,
        data.first.servicetype,
        // In params
        mapping.port.wan.c_str(),
        mapping.port.proto.c_str(),
        nullptr,
        // Out params
        intClient,
        intPort,
        desc,
        enabled,
        leaseDuration
      );
      if (err == 714) {  // NoSuchEntryInArray
        BOOST_LOG(debug) << "Mapping entry not found for "sv << mapping.port.wan;
      } else if (err == UPNPCOMMAND_SUCCESS) {
        // Some routers change the description, so we can't check that here
        if (!std::strcmp(intClient, lan_addr.c_str())) {
          if (std::atoi(leaseDuration) == 0) {
            BOOST_LOG(debug) << "Static mapping entry found for "sv << mapping.port.wan;

            // It's a static mapping, so we're done here
            return true;
          } else {
            BOOST_LOG(debug) << "Mapping entry found for "sv << mapping.port.wan << " ("sv << leaseDuration << " seconds remaining)"sv;
          }
        } else {
          BOOST_LOG(warning) << "UPnP conflict detected with: "sv << intClient;

          // Some UPnP IGDs won't let unauthenticated clients delete other conflicting port mappings
          // for security reasons, but we will give it a try anyway.
          err = UPNP_DeletePortMapping(
            urls->controlURL,
            data.first.servicetype,
            mapping.port.wan.c_str(),
            mapping.port.proto.c_str(),
            nullptr
          );
          if (err) {
            // VipleStream §UPNP-DIAG: 操作 + port + 數值碼 + 文字；連續失敗多輪後由 port_fail_log() 降為 debug
            last_port_err = err;
            BOOST_LOG(port_fail_log()) << "[VIPLE-UPNP] DeletePortMapping "sv << mapping.port.proto << ' ' << mapping.port.wan
                                       << " (conflict with "sv << intClient << ") failed: "sv << err_string(err);
            return false;
          }
        }
      } else {
        // VipleStream §UPNP-DIAG: 非致命（下面仍會嘗試 AddPortMapping），但要看得出是哪個 port、哪個碼
        BOOST_LOG(port_fail_log()) << "[VIPLE-UPNP] GetSpecificPortMappingEntry "sv << mapping.port.proto << ' ' << mapping.port.wan
                                   << " failed: "sv << err_string(err) << " (will still try AddPortMapping)"sv;

        // If we get a strange error from the router, we'll assume it's some old broken IGDv1
        // device and only use indefinite lease durations to hopefully avoid confusing it.
        if (err != 606) {  // Unauthorized
          indefinite = true;
        }
      }

      // Add/update the port mapping
      auto mapping_period = std::to_string(indefinite ? 0 : PORT_MAPPING_LIFETIME.count());
      err = UPNP_AddPortMapping(
        urls->controlURL,
        data.first.servicetype,
        mapping.port.wan.c_str(),
        mapping.port.lan.c_str(),
        lan_addr.data(),
        mapping.description.c_str(),
        mapping.port.proto.c_str(),
        nullptr,
        mapping_period.c_str()
      );

      if (err != UPNPCOMMAND_SUCCESS && !indefinite) {
        // This may be an old/broken IGD that doesn't like non-static mappings.
        BOOST_LOG(debug) << "[VIPLE-UPNP] AddPortMapping "sv << mapping.port.proto << ' ' << mapping.port.wan << " failed: "sv << err_string(err) << "; retrying as static mapping"sv;
        err = UPNP_AddPortMapping(
          urls->controlURL,
          data.first.servicetype,
          mapping.port.wan.c_str(),
          mapping.port.lan.c_str(),
          lan_addr.data(),
          mapping.description.c_str(),
          mapping.port.proto.c_str(),
          nullptr,
          "0"
        );
      }

      if (err) {
        last_port_err = err;
        BOOST_LOG(port_fail_log()) << "[VIPLE-UPNP] AddPortMapping "sv << mapping.port.proto << ' ' << mapping.port.wan << "->"sv << mapping.port.lan
                                   << (indefinite ? " (static)"sv : ""sv) << " failed: "sv << err_string(err);
        return false;
      }

      BOOST_LOG(debug) << "Successfully mapped "sv << mapping.port.proto << ' ' << mapping.port.lan;
      return true;
    }

    /**
     * @brief Unmaps all ports.
     * @param urls urls_t from UPNP_GetValidIGD()
     * @param data IGDdatas from UPNP_GetValidIGD()
     */
    void unmap_all_upnp_ports(const urls_t &urls, const IGDdatas &data) {
      for (auto it = std::begin(mappings); it != std::end(mappings); ++it) {
        auto status = UPNP_DeletePortMapping(
          urls->controlURL,
          data.first.servicetype,
          it->port.wan.c_str(),
          it->port.proto.c_str(),
          nullptr
        );

        if (status && status != 714) {  // NoSuchEntryInArray
          BOOST_LOG(warning) << "[VIPLE-UPNP] DeletePortMapping "sv << it->port.proto << ' ' << it->port.wan << " (shutdown unmap) failed: "sv << err_string(status);
        } else {
          BOOST_LOG(debug) << "Successfully unmapped "sv << it->port.proto << ' ' << it->port.lan;
        }
      }
    }

    /**
     * @brief Maintains UPnP port forwarding rules
     */
    void upnp_thread_proc() {
      platf::set_thread_name("upnp");
      auto shutdown_event = mail::man->event<bool>(mail::shutdown);
      bool mapped = false;
      IGDdatas data;
      urls_t mapped_urls;
      auto address_family = net::af_from_enum_string(config::sunshine.address_family);

      // Refresh UPnP rules every few minutes. They can be lost if the router reboots,
      // WAN IP address changes, or various other conditions.
      do {
        int err = 0;
        device_t device {upnpDiscover(2000, nullptr, nullptr, 0, IPv4, 2, &err)};
        if (!device || err) {
          // VipleStream §UPNP-DIAG: 上游這裡是 warning 且不印碼；err == 0 代表 2000 ms 內沒有任何 SSDP 回應
          report_cycle_failure("upnpDiscover(IPv4)"sv, err,
                               err ? "Couldn't discover any IPv4 UPNP devices: "s + err_string(err) : "no SSDP response within 2000 ms"s);
          mapped = false;
          continue;
        }

        // VipleStream §UPNP-DIAG: 把回應 SSDP 的裝置列成一行（最多 4 台），status 4（非 IGD）時才看得出是誰在回應
        std::string responders;
        int responder_count = 0;
        for (auto dev = device.get(); dev != nullptr; dev = dev->pNext) {
          BOOST_LOG(debug) << "Found device: "sv << dev->descURL;
          if (responder_count++ < 4) {
            responders += (responders.empty() ? ""sv : ", "sv);
            responders += dev->descURL;
          }
        }
        if (responder_count > 4) {
          responders += ", ..."sv;
        }

        std::array<char, INET6_ADDRESS_STRLEN> lan_addr;

        urls_t urls;
        auto status = upnp::UPNP_GetValidIGDStatus(device, &urls, &data, lan_addr);
        if (status != 1 && status != 2) {
          // VipleStream §UPNP-DIAG: 印出操作、數值碼、文字與回應者；重複失敗由 report_cycle_failure() 去重／降級
          report_cycle_failure("UPNP_GetValidIGD"sv, status,
                               std::string(status_string(status)) + "; "s + std::to_string(responder_count) + " SSDP responder(s): "s + responders);
          mapped = false;
          continue;
        }

        std::string lan_addr_str {lan_addr.data()};

        BOOST_LOG(debug) << "Found valid IGD device: "sv << urls->rootdescURL;

#if (MINIUPNPC_API_VERSION >= 18)
        // VipleStream §UPNP-DIAG: 2.2.8+ 的 status 2 = IGD 的 WAN 是保留位址（雙層 NAT / CGNAT）。
        // 上游把它當成功繼續 map（維持不變），但 WAN 端其實不可達；只在剛從未 mapped 進入時提示一次。
        if (status == 2 && !mapped) {
          BOOST_LOG(warning) << "[VIPLE-UPNP] UPNP_GetValidIGD status 2: "sv << status_string(status)
                             << " via "sv << urls->rootdescURL << "; mappings will not be reachable from the internet"sv;
        }
#endif

        // VipleStream §UPNP-DIAG: 統計這輪失敗的 port，做為週期性失敗的去重鍵（第一個失敗 port 的碼）
        unsigned failed_ports = 0;
        int first_fail_err = 0;
        std::string first_fail_port;
        for (auto it = std::begin(mappings); it != std::end(mappings) && !shutdown_event->peek(); ++it) {
          if (!map_upnp_port(data, urls, lan_addr_str, *it)) {
            if (failed_ports++ == 0) {
              first_fail_err = last_port_err;
              first_fail_port = it->port.proto + ' ' + it->port.wan;
            }
          }
        }

        if (failed_ports) {
          report_cycle_failure("port mapping"sv, first_fail_err,
                               std::to_string(failed_ports) + "/"s + std::to_string(mappings.size()) + " ports failed via "s + urls->rootdescURL
                                 + ", first: "s + first_fail_port + " -> "s + err_string(first_fail_err));
        } else {
          report_cycle_success("port mappings to "s + lan_addr_str + " via "s + urls->rootdescURL);
        }

        if (!mapped) {
          BOOST_LOG(info) << "Completed UPnP port mappings to "sv << lan_addr_str << " via "sv << urls->rootdescURL;
        }

        // If we are listening on IPv6 and the IGD has an IPv6 firewall enabled, try to create IPv6 firewall pinholes
        if (address_family == net::af_e::BOTH) {
          if (create_ipv6_pinholes() && !mapped) {
            // Only log the first time through
            BOOST_LOG(info) << "Successfully opened IPv6 pinholes on the IGD"sv;
          }
        }

        mapped = true;
        mapped_urls = std::move(urls);
      } while (!shutdown_event->view(REFRESH_INTERVAL));

      if (mapped) {
        // Unmap ports upon termination
        BOOST_LOG(info) << "Unmapping UPNP ports..."sv;
        unmap_all_upnp_ports(mapped_urls, data);
      }
    }

    std::vector<mapping_t> mappings;
    std::thread upnp_thread;

    // VipleStream §UPNP-DIAG: 週期性失敗的去重狀態（只在 upnp 執行緒上讀寫）
    struct {
      std::string_view op {};  // 失敗的操作名（都是 string literal）
      int code = 0;            // 該操作的數值碼
      unsigned streak = 0;     // 連續相同失敗的輪數
      std::chrono::steady_clock::time_point last_report {};
    } fail;
    int last_port_err = 0;  // map_upnp_port() 最後一次失敗的碼，供本輪彙整行使用
  };

  std::unique_ptr<platf::deinit_t> start() {
    if (!config::sunshine.flags[config::flag::UPNP]) {
      return nullptr;
    }

    return std::make_unique<deinit_t>();
  }
}  // namespace upnp
