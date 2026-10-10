// VipleStream 2.0 §VR-REPAIR-RTT：模擬器的 server 端（模型，不是實測）。
//
// 補包的決策直接用 Sunshine/src/vr/vr_multilink_logic.h 的真邏輯（封包倉、NACK 解析、plan_repair、額度、年齡上限、
// 「上一輪還在路上」的視窗）；這裡只有 hub_t::on_nack 裡把它們串起來的那幾行，socket、執行緒與加密都沒有。
// 改 vr_multilink.cpp 的 on_nack 時，這裡要跟著改。
//
//   -DSIM_SERVER_ORIG   vr_multilink_logic.h 是還沒有 §VR-REPAIR-RTT 的版本（從舊提交取出來產生 golden 用）：
//                       沒有的那幾個函式在這裡補上「原本寫在 vr_multilink.cpp 裡的算式」
#include "src/vr/vr_multilink_logic.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace vr::multilink;

namespace {
  packet_store_t *g_store = nullptr;
  repair_budget_t g_budget;

#ifdef SIM_SERVER_ORIG
  constexpr int64_t kRepairGapMinNs = 4'000'000;
  constexpr int64_t kRepairGapMaxNs = 12'000'000;

  int64_t repair_max_age_ns(bool, int64_t video_max_age_ns) {
    return video_max_age_ns * 3 / 2;
  }

  int64_t repair_in_flight_gap_ns(bool, unsigned, int64_t legacy_gap_ns) {
    return legacy_gap_ns;
  }
#endif
}  // namespace

extern "C" void srv_reset(void) {
  delete g_store;
  g_store = new packet_store_t();
  g_budget = repair_budget_t {};
}

extern "C" int srv_put(uint32_t frame, uint8_t block, uint16_t first, uint16_t data, uint16_t total, const uint8_t *pkts, size_t pkt_size,
                       size_t count, int64_t now_ns) {
  video_meta_t m;
  m.frame = frame;
  m.block = block;
  m.first_shard = first;
  m.data_shards = data;
  m.total_shards = total;
  return g_store->put(m, pkts, pkt_size, count, now_ns) ? 1 : 0;
}

// rtt_feature＝協商了 VIPLE_VR_LINK_F_REPAIR_RTT
// 回傳：>0＝補送的 shard 數（out_shards 填序號）；0＝不送（why 說明）；<0＝GONE（-reason）
extern "C" int srv_on_nack(const uint8_t *body, size_t len, int64_t now_ns, int rtt_ms10, int64_t video_max_age_us, double video_pps,
                           int rtt_feature, uint8_t *out_shards, int *out_needed, double *out_age_ms, int *out_round, char *why,
                           size_t whylen) {
  nack_t nack;
  *out_needed = 0;
  *out_age_ms = -1;
  *out_round = 0;
  why[0] = 0;
  if (!parse_nack(body, len, nack)) {
    std::snprintf(why, whylen, "parse failed");
    return 0;
  }
#ifdef SIM_SERVER_ORIG
  (void) rtt_feature;
  const bool rtt_mode = false;
  const unsigned eta_ms = 0;
#else
  const bool rtt_mode = nack_rtt_mode(rtt_feature ? VIPLE_VR_LINK_F_REPAIR_RTT : 0, nack.flags);
  const unsigned eta_ms = nack_eta_ms(nack.flags);
#endif

  // 上一輪補的封包多久之內算「還在路上」
  int64_t gap_ns = kRepairGapMaxNs;
  if (rtt_ms10 >= 0) {
    gap_ns = std::min<int64_t>(gap_ns, (int64_t) rtt_ms10 * 100'000 + 3'000'000);
  }
  gap_ns = std::clamp(gap_ns, kRepairGapMinNs, kRepairGapMaxNs);
  gap_ns = repair_in_flight_gap_ns(rtt_mode, eta_ms, gap_ns);

  auto *b = g_store->find(nack.frame, nack.block);
  if (!b) {
    std::snprintf(why, whylen, "GONE unknown (not in store)");
    return -VIPLE_VR_GONE_UNKNOWN;
  }
  *out_age_ms = (now_ns - b->first_ns) / 1e6;
  *out_round = b->rounds + 1;
  const int64_t max_age_ns = repair_max_age_ns(rtt_mode, video_max_age_us * 1000);
  if (now_ns - b->first_ns > max_age_ns) {
    std::snprintf(why, whylen, "GONE expired (age %.1f ms > %.1f ms)", *out_age_ms, max_age_ns / 1e6);
    return -VIPLE_VR_GONE_EXPIRED;
  }
  const bool in_flight = b->last_round_ns != 0 && now_ns - b->last_round_ns < gap_ns;
  unsigned needed = 0;
  const auto plan = plan_repair(nack, b->data, b->total, b->stored, in_flight ? &b->resent : nullptr, &needed);
  if (plan.empty()) {
    std::snprintf(why, whylen, "nothing to send (%s; in-flight window %.1f ms)", in_flight ? "last round still in flight" : "already decodable",
                  gap_ns / 1e6);
    return 0;
  }
  if (b->rounds >= kRepairMaxRounds) {
    std::snprintf(why, whylen, "exhausted (%u rounds)", (unsigned) b->rounds);
    return 0;
  }
  if (!g_budget.take((unsigned) plan.size(), video_pps, now_ns)) {
    std::snprintf(why, whylen, "GONE budget");
    return -VIPLE_VR_GONE_BUDGET;
  }
  ++b->rounds;
  if (!in_flight) {
    b->resent.fill(0);
  }
  for (const uint8_t s : plan) {
    bit_set(b->resent, s);
  }
  b->last_round_ns = now_ns;
  for (size_t i = 0; i < plan.size(); ++i) {
    out_shards[i] = plan[i];
  }
  *out_needed = (int) std::min<unsigned>(needed, (unsigned) plan.size());
  std::snprintf(why, whylen, "resend %zu (needed %u; in-flight window %.1f ms%s)", plan.size(), needed, gap_ns / 1e6, rtt_mode ? "; rtt-mode" : "");
  return (int) plan.size();
}
