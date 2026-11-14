/**
 * @file src/upnp.h
 * @brief Declarations for UPnP port mapping.
 */
#pragma once

// lib includes
#include <miniupnpc/miniupnpc.h>

// local includes
#include "platform/common.h"

/**
 * @brief UPnP port mapping.
 */
namespace upnp {
  constexpr auto INET6_ADDRESS_STRLEN = 46;
  constexpr auto IPv4 = 0;
  constexpr auto IPv6 = 1;
  constexpr auto PORT_MAPPING_LIFETIME = 3600s;
  constexpr auto REFRESH_INTERVAL = 120s;
  // VipleStream §UPNP-DIAG: 同一種失敗連續達此輪數後，改為每 FAIL_REPORT_INTERVAL 印一行 warning（含連續次數），
  // 直到成功再印一行恢復訊息。
  constexpr unsigned FAIL_FULL_DETAIL_CYCLES = 3;
  constexpr auto FAIL_REPORT_INTERVAL = 30min;
  // §PERF-1113：區網裡沒有 IGD（沒有任何 SSDP 回應、或回應的裝置都不是 IGD）而且這次執行從來沒找到過 IGD 時，
  // 同一種失敗連續 FAIL_FULL_DETAIL_CYCLES 輪之後拉長重試間隔：從 FAIL_BACKOFF_FIRST 起每次加倍、
  // FAIL_BACKOFF_MAX 封頂（4／8／16／30 分鐘）。這種區網每 REFRESH_INTERVAL 探一次只是白送多播與 HTTP。
  // 其餘的失敗（IGD 未連線、port mapping 失敗、discover 自己出錯）與成功之後的定期更新都維持 REFRESH_INTERVAL。
  constexpr auto FAIL_BACKOFF_FIRST = 4min;
  constexpr auto FAIL_BACKOFF_MAX = 30min;

  using device_t = util::safe_ptr<UPNPDev, freeUPNPDevlist>;

  KITTY_USING_MOVE_T(urls_t, UPNPUrls, , {
    FreeUPNPUrls(&el);
  });

  /**
   * @brief Get the valid IGD status.
   * @param device The device.
   * @param urls The URLs.
   * @param data The IGD data.
   * @param lan_addr The LAN address.
   * @return The UPnP Status.
   * @note VipleStream §UPNP-DIAG: miniupnpc >= 2.2.8 (MINIUPNPC_API_VERSION >= 18) 的語意；
   *       2.2.4~2.2.7 為 0=無 IGD、1=已連線、2=未連線、3=非 IGD。
   * @retval 0 No IGD found (UPNP_NO_IGD).
   * @retval 1 A valid connected IGD has been found (UPNP_CONNECTED_IGD).
   * @retval 2 A valid connected IGD whose WAN address is reserved/private, i.e. double NAT / CGNAT (UPNP_PRIVATEIP_IGD).
   * @retval 3 A valid IGD has been found but it reported as not connected (UPNP_DISCONNECTED_IGD).
   * @retval 4 An UPnP device has been found but was not recognized as an IGD (UPNP_UNKNOWN_DEVICE).
   */
  int UPNP_GetValidIGDStatus(device_t &device, urls_t *urls, IGDdatas *data, std::array<char, INET6_ADDRESS_STRLEN> &lan_addr);

  [[nodiscard]] std::unique_ptr<platf::deinit_t> start();
}  // namespace upnp
