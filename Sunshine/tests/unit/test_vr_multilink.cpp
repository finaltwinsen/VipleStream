/**
 * @file tests/unit/test_vr_multilink.cpp
 * @brief VipleStream 2.0 §VR-MULTILINK：多連線的純邏輯（去重視窗、飽和偵測、故障注入、LINK_HELLO／LINK_READY）。
 */
#include "../tests_common.h"

#include <src/vr/vr_multilink_logic.h>

namespace ml = vr::multilink;
using namespace std::chrono_literals;

// ── 去重／防重放視窗 ──────────────────────────────────────────────

TEST(VrMultilinkReplay, FirstCopyWinsAndDuplicatesAreRejected) {
  ml::replay_window_t w;
  for (uint32_t seq = 1; seq <= 1000; ++seq) {
    ASSERT_TRUE(w.fresh(seq)) << seq;
    w.mark(seq);
    ASSERT_FALSE(w.fresh(seq)) << seq;  // 另一條連線送來的同一則訊息
  }
}

TEST(VrMultilinkReplay, ReorderedWithinWindowIsAcceptedOnce) {
  ml::replay_window_t w;
  w.mark(100);
  EXPECT_TRUE(w.fresh(99));  // 慢的那條路晚到，但還沒見過
  w.mark(99);
  EXPECT_FALSE(w.fresh(99));
  EXPECT_TRUE(w.fresh(37));  // 視窗 64 格：100-63=37 還在視窗內
  EXPECT_FALSE(w.fresh(36));  // 太舊：一律當重放
  w.mark(37);
  EXPECT_FALSE(w.fresh(37));
}

TEST(VrMultilinkReplay, LargeJumpClearsOldBits) {
  ml::replay_window_t w;
  for (uint32_t seq = 1; seq <= 10; ++seq) {
    w.mark(seq);
  }
  w.mark(1000);
  EXPECT_FALSE(w.fresh(1000));
  EXPECT_TRUE(w.fresh(999));
  EXPECT_FALSE(w.fresh(10));  // 落在視窗外
}

TEST(VrMultilinkReplay, SequenceWrapAround) {
  ml::replay_window_t w;
  w.mark(0xFFFFFFFEu);
  w.mark(0xFFFFFFFFu);
  EXPECT_TRUE(w.fresh(0));
  w.mark(0);
  EXPECT_FALSE(w.fresh(0xFFFFFFFFu));
  EXPECT_FALSE(w.fresh(0));
  EXPECT_TRUE(w.fresh(1));
}

TEST(VrMultilinkReplay, FreshDoesNotMutate) {
  // 解密失敗的偽造封包只會呼叫 fresh()，不能讓之後真的封包被擋
  ml::replay_window_t w;
  w.mark(5);
  EXPECT_TRUE(w.fresh(500));
  EXPECT_TRUE(w.fresh(6));
  w.mark(6);
  EXPECT_FALSE(w.fresh(6));
}

// ── 飽和偵測 ──────────────────────────────────────────────────────

namespace {
  using clk = ml::saturation_t::clock;

  /// 以每 2.5 ms 一個批次餵 duration，回傳被判定飽和的視窗數
  int feed(ml::saturation_t &s, clk::time_point &t, std::chrono::milliseconds duration, int bad_every) {
    int saturated = 0;
    int i = 0;
    for (const auto end = t + duration; t < end; t += 2500us, ++i) {
      const bool ok = bad_every == 0 || (i % bad_every) != 0;
      saturated += s.report(ok, t) ? 1 : 0;
    }
    return saturated;
  }
}  // namespace

TEST(VrMultilinkSaturation, HealthyLinkNeverTriggers) {
  ml::saturation_t s;
  auto t = clk::time_point {} + 1s;
  EXPECT_EQ(feed(s, t, 5s, 0), 0);
  EXPECT_EQ(feed(s, t, 5s, 10), 0);  // 10% 的批次沒送完：低於 40% 門檻
}

TEST(VrMultilinkSaturation, StalledLinkTriggersWithinOneWindow) {
  ml::saturation_t s;
  auto t = clk::time_point {} + 1s;
  feed(s, t, 1s, 0);
  // 之後每個批次都沒送完：下一個 1 s 視窗結束時就要判定
  int n = 0;
  const auto start = t;
  while (!s.report(false, t)) {
    t += 2500us;
    ASSERT_LT(++n, 2000);
  }
  EXPECT_LE(t - start, 2010ms);  // 最壞情況：剛好錯過一個視窗的結尾
  EXPECT_EQ(s.next_hold(), 2000ms);
}

TEST(VrMultilinkSaturation, HoldDoublesUpToCapAndResetsAfterHealthyPeriod) {
  ml::saturation_t s;
  EXPECT_EQ(s.next_hold(), 2000ms);
  EXPECT_EQ(s.next_hold(), 4000ms);
  EXPECT_EQ(s.next_hold(), 8000ms);
  EXPECT_EQ(s.next_hold(), 16000ms);
  EXPECT_EQ(s.next_hold(), 30000ms);
  EXPECT_EQ(s.next_hold(), 30000ms);

  // 健康滿 10 s：退避歸零，下一次又從 2 s 開始
  auto t = clk::time_point {} + 100s;
  EXPECT_EQ(feed(s, t, 11s, 0), 0);
  EXPECT_EQ(s.next_hold(), 2000ms);
}

TEST(VrMultilinkSaturation, TooFewBatchesAreNotJudged) {
  // 影像幾乎沒在送（例如剛起播）：樣本不足不判定
  ml::saturation_t s;
  auto t = clk::time_point {} + 1s;
  for (int i = 0; i < 40; ++i) {
    EXPECT_FALSE(s.report(false, t));
    t += 100ms;  // 每個 1 s 視窗只有約 10 個批次
  }
}

namespace {
  /// 每秒一次 stall 長度的停頓（停頓期間的批次都送不完），其餘正常；回傳被判定飽和的視窗數
  int feed_periodic_stall(ml::saturation_t &s, clk::time_point &t, std::chrono::milliseconds duration, std::chrono::milliseconds stall) {
    int saturated = 0;
    const auto begin = t;
    for (const auto end = t + duration; t < end; t += 2500us) {
      const auto phase = (t - begin) % 1000ms;
      saturated += s.report(phase >= stall, t) ? 1 : 0;
    }
    return saturated;
  }
}  // namespace

TEST(VrMultilinkSaturation, ShortPeriodicStallsDoNotPauseAGoodLink) {
  // 2026-10-07 遊玩位置的適配器鏈路：每秒約一次 50～100 ms（偶爾 150～200 ms）的停頓，整體送達 99.98%。
  // 這種鏈路不能被暫停——暫停它就只剩較差的那一條。
  for (const auto stall : {50ms, 100ms, 150ms, 200ms, 300ms}) {
    ml::saturation_t s;
    auto t = clk::time_point {} + 1s;
    EXPECT_EQ(feed_periodic_stall(s, t, 20s, stall), 0) << stall.count() << " ms";
  }
}

TEST(VrMultilinkSaturation, LongOutagesStillPause) {
  // 每秒有一半以上的時間送不動：1～2 s 內要判定
  ml::saturation_t s;
  auto t = clk::time_point {} + 1s;
  EXPECT_GT(feed_periodic_stall(s, t, 5s, 600ms), 0);
}

// ── 故障注入 ──────────────────────────────────────────────────────

TEST(VrMultilinkFault, ParseAndSchedule) {
  const auto f = ml::fault_t::parse("1:60:40,2:40:60:40");
  ASSERT_TRUE(f.has_value());
  EXPECT_TRUE(f->any());
  // 連線 1：每 100 ms 前 60 ms 通、後 40 ms 斷
  EXPECT_FALSE(f->blocked(1, 0));
  EXPECT_FALSE(f->blocked(1, 59));
  EXPECT_TRUE(f->blocked(1, 60));
  EXPECT_TRUE(f->blocked(1, 99));
  EXPECT_FALSE(f->blocked(1, 100));
  // 連線 2 相位提前 40 ms：每 100 ms 的 60～100 ms 通，和連線 1 互補（任何時刻至少一條通）
  for (int ms = 0; ms < 1000; ++ms) {
    EXPECT_FALSE(f->blocked(1, ms) && f->blocked(2, ms)) << ms;
  }
  // 沒設規則的連線永遠通
  EXPECT_FALSE(f->blocked(3, 70));
  EXPECT_FALSE(f->blocked(0, 70));
  EXPECT_FALSE(f->blocked(99, 70));
}

TEST(VrMultilinkFault, RejectsMalformedSpecs) {
  EXPECT_FALSE(ml::fault_t::parse("1:60").has_value());
  EXPECT_FALSE(ml::fault_t::parse("0:60:40").has_value());  // linkId 從 1 起
  EXPECT_FALSE(ml::fault_t::parse("5:60:40").has_value());
  EXPECT_FALSE(ml::fault_t::parse("1:a:40").has_value());
  EXPECT_FALSE(ml::fault_t::parse("1:60:40:0:9").has_value());
  EXPECT_FALSE(ml::fault_t::parse("1:0:0").has_value());
  EXPECT_TRUE(ml::fault_t::parse("").has_value());  // 空字串＝沒有規則
  EXPECT_FALSE(ml::fault_t::parse("")->any());
}

TEST(VrMultilinkFault, RateCapRule) {
  const auto f = ml::fault_t::parse("r2:20,1:60:40");
  ASSERT_TRUE(f.has_value());
  EXPECT_TRUE(f->any());
  EXPECT_EQ(f->rate_cap(2), 20);
  EXPECT_EQ(f->rate_cap(1), 0);
  EXPECT_EQ(f->rate_cap(99), 0);
  EXPECT_TRUE(f->blocked(1, 70));   // 同一份規則裡的空檔規則照常生效
  EXPECT_FALSE(f->blocked(2, 70));  // 限速不是空檔

  EXPECT_TRUE(ml::fault_t::parse("r1:5")->any());
  EXPECT_FALSE(ml::fault_t::parse("r0:20").has_value());
  EXPECT_FALSE(ml::fault_t::parse("r5:20").has_value());
  EXPECT_FALSE(ml::fault_t::parse("r2:0").has_value());
  EXPECT_FALSE(ml::fault_t::parse("r2").has_value());
  EXPECT_FALSE(ml::fault_t::parse("r2:20x").has_value());
}

TEST(VrMultilinkFault, DelayRule) {
  const auto f = ml::fault_t::parse("d2:8,r1:60");
  ASSERT_TRUE(f.has_value());
  EXPECT_TRUE(f->any());
  EXPECT_EQ(f->delay(2), 8);
  EXPECT_EQ(f->delay(1), 0);
  EXPECT_EQ(f->delay(99), 0);
  EXPECT_EQ(f->rate_cap(1), 60);
  EXPECT_EQ(f->rate_cap(2), 0);  // 延遲不是限速

  EXPECT_TRUE(ml::fault_t::parse("d1:1")->any());
  EXPECT_FALSE(ml::fault_t::parse("d0:8").has_value());
  EXPECT_FALSE(ml::fault_t::parse("d2:0").has_value());
  EXPECT_TRUE(ml::fault_t::parse("d2:40").has_value());
  EXPECT_FALSE(ml::fault_t::parse("d2:41").has_value());  // 上限 40 ms（再久會超過佇列上限，變成丟包）
  EXPECT_FALSE(ml::fault_t::parse("d2").has_value());
}

TEST(VrMultilinkSaturation, RestartKeepsBackoffButDropsWindowAndHealth) {
  using clock = ml::saturation_t::clock;
  ml::saturation_t s;
  auto t = clock::time_point {} + std::chrono::seconds {100};
  // 一個全壞的視窗 → 飽和，退避 2 s 起算
  bool sat = false;
  for (int i = 0; i < 40; ++i) {
    sat = s.report(false, t) || sat;
    t += std::chrono::milliseconds {30};
  }
  ASSERT_TRUE(sat);
  EXPECT_EQ(s.next_hold(), std::chrono::milliseconds {2000});

  // 探測期間送得出去的批次不該被算成健康期：restart 之後從零開始
  for (int i = 0; i < 10; ++i) {
    s.report(true, t);
    t += std::chrono::milliseconds {30};
  }
  s.restart();
  EXPECT_EQ(s.ok, 0);
  EXPECT_EQ(s.bad, 0);
  EXPECT_FALSE(s.healthy);
  EXPECT_EQ(s.next_hold(), std::chrono::milliseconds {4000});  // 退避照舊加倍
}

// ── automatic：量到夠快才送影像 ──────────────────────────────────

TEST(VrMultilinkProbeGate, NeedsTwoFreshGoodMeasurements) {
  ml::probe_gate_t g;
  const uint32_t need = ml::probe_need_mbps(180);  // 270
  EXPECT_EQ(need, 270u);
  EXPECT_FALSE(g.on_report(1, 600, need));  // 第一次夠快
  EXPECT_FALSE(g.on_report(1, 600, need));  // 同一個量測被 PING 重複回報：不算第二次
  EXPECT_FALSE(g.on_report(1, 600, need));
  EXPECT_TRUE(g.on_report(2, 500, need));   // 第二次新的量測也夠快
}

TEST(VrMultilinkProbeGate, SlowMeasurementResetsTheCount) {
  ml::probe_gate_t g;
  const uint32_t need = ml::probe_need_mbps(180);
  EXPECT_FALSE(g.on_report(1, 600, need));
  EXPECT_FALSE(g.on_report(2, 40, need));   // 慢：歸零
  EXPECT_FALSE(g.on_report(3, 600, need));
  EXPECT_TRUE(g.on_report(4, 600, need));
  g.reset();
  EXPECT_FALSE(g.on_report(5, 600, need));  // 退回探測之後要重新累積
  EXPECT_TRUE(g.on_report(6, 600, need));
}

TEST(VrMultilinkProbeGate, WeakLinkNeverQualifies) {
  // 10-06 實測的弱鏈路：PHY 17～50 Mbps，量到的速率遠低於需求
  ml::probe_gate_t g;
  const uint32_t need = ml::probe_need_mbps(200);
  for (int i = 1; i <= 100; ++i) {
    EXPECT_FALSE(g.on_report((uint8_t) i, (uint32_t) (17 + i % 30), need)) << i;
  }
}

TEST(VrMultilinkProbeGate, NeedHasAFloorAndSequenceWraps) {
  EXPECT_EQ(ml::probe_need_mbps(0), 50u);    // 位元率還沒量到
  EXPECT_EQ(ml::probe_need_mbps(20), 50u);
  EXPECT_EQ(ml::probe_need_mbps(100), 150u);
  ml::probe_gate_t g;
  EXPECT_FALSE(g.on_report(255, 900, 300));
  EXPECT_TRUE(g.on_report(0, 900, 300));     // burstSeq 是 8 bit，繞回也算新的量測
}

// ── LINK_HELLO／LINK_READY ────────────────────────────────────────

namespace {
  std::vector<uint8_t> hello_body(std::initializer_list<VIPLE_VR_LINK_DESC> links) {
    std::vector<uint8_t> b;
    b.push_back((uint8_t) links.size());
    for (const auto &d : links) {
      const auto *p = (const uint8_t *) &d;
      b.insert(b.end(), p, p + sizeof(d));
    }
    return b;
  }

  VIPLE_VR_LINK_DESC desc(uint8_t id, uint16_t vport, uint16_t aport, uint8_t last_octet) {
    VIPLE_VR_LINK_DESC d {};
    d.linkId = id;
    d.clientVideoPort = vport;
    d.clientAudioPort = aport;
    const uint8_t c[4] = {192, 168, 1, last_octet};
    const uint8_t s[4] = {192, 168, 1, 1};
    std::memcpy(d.clientAddr, c, 4);
    std::memcpy(d.serverAddr, s, 4);
    return d;
  }
}  // namespace

TEST(VrMultilinkHello, ParsesTwoLinks) {
  const auto body = hello_body({desc(1, 50000, 50001, 70), desc(2, 50002, 50003, 71)});
  std::vector<ml::hello_link_t> out;
  ASSERT_TRUE(ml::parse_hello(body.data(), body.size(), out));
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].id, 1);
  EXPECT_EQ(out[0].client_video_port, 50000);
  EXPECT_EQ(out[0].client_audio_port, 50001);
  EXPECT_EQ(out[0].client_addr, (std::array<uint8_t, 4> {192, 168, 1, 70}));
  EXPECT_EQ(out[1].server_addr, (std::array<uint8_t, 4> {192, 168, 1, 1}));
}

TEST(VrMultilinkHello, RejectsBadInput) {
  std::vector<ml::hello_link_t> out;
  EXPECT_FALSE(ml::parse_hello(nullptr, 0, out));

  auto body = hello_body({desc(1, 50000, 50001, 70)});
  EXPECT_FALSE(ml::parse_hello(body.data(), body.size() - 1, out));  // 截斷

  body = hello_body({desc(1, 50000, 50001, 70), desc(1, 50002, 50003, 71)});
  EXPECT_FALSE(ml::parse_hello(body.data(), body.size(), out));  // linkId 重複

  body = hello_body({desc(0, 50000, 50001, 70)});
  EXPECT_FALSE(ml::parse_hello(body.data(), body.size(), out));  // linkId 超出範圍

  body = hello_body({desc(5, 50000, 50001, 70)});
  EXPECT_FALSE(ml::parse_hello(body.data(), body.size(), out));

  body = hello_body({desc(1, 0, 50001, 70)});
  EXPECT_FALSE(ml::parse_hello(body.data(), body.size(), out));  // 埠為 0

  const uint8_t zero[1] = {0};
  EXPECT_FALSE(ml::parse_hello(zero, 1, out));  // count=0
  EXPECT_TRUE(out.empty());
}

TEST(VrMultilinkHello, ReadyTlvLayout) {
  std::vector<ml::ready_link_t> links(2);
  links[0].id = 1;
  links[0].status = VIPLE_VR_LINK_READY_OK;
  links[0].server_video_port = 0x1234;
  links[0].server_audio_port = 0x5678;
  links[1].id = 2;  // 預設 status＝REFUSED
  const auto tlv = ml::format_ready_tlv(links);
  ASSERT_EQ(tlv.size(), 2u + 1u + 2u * sizeof(VIPLE_VR_LINK_PORTS));
  EXPECT_EQ(tlv[0], VIPLE_VR_S2C_LINK_READY);
  EXPECT_EQ(tlv[1], tlv.size() - 2);
  EXPECT_EQ(tlv[2], 2);
  VIPLE_VR_LINK_PORTS p;
  std::memcpy(&p, tlv.data() + 3, sizeof(p));
  EXPECT_EQ(p.linkId, 1);
  EXPECT_EQ(p.status, VIPLE_VR_LINK_READY_OK);
  EXPECT_EQ(p.serverVideoPort, 0x1234);
  EXPECT_EQ(p.serverAudioPort, 0x5678);
  std::memcpy(&p, tlv.data() + 3 + sizeof(p), sizeof(p));
  EXPECT_EQ(p.linkId, 2);
  EXPECT_EQ(p.status, VIPLE_VR_LINK_READY_REFUSED);
}

TEST(VrMultilinkHello, FeatureBitsRoundTrip) {
  // client 在 LINK_DESC.flags 宣告支援的功能；server 在 LINK_READY 尾端回同意啟用的
  auto d1 = desc(1, 50000, 50001, 70);
  auto d2 = desc(2, 50002, 50003, 71);
  d1.flags = VIPLE_VR_LINK_F_CTRL | VIPLE_VR_LINK_F_REPAIR;
  d2.flags = VIPLE_VR_LINK_F_CTRL;
  const auto body = hello_body({d1, d2});
  std::vector<ml::hello_link_t> out;
  ASSERT_TRUE(ml::parse_hello(body.data(), body.size(), out));
  EXPECT_EQ(out[0].flags, VIPLE_VR_LINK_F_CTRL | VIPLE_VR_LINK_F_REPAIR);
  EXPECT_EQ(out[1].flags, VIPLE_VR_LINK_F_CTRL);

  std::vector<ml::ready_link_t> links(1);
  links[0].id = 1;
  links[0].status = VIPLE_VR_LINK_READY_OK;
  links[0].server_video_port = 1;
  links[0].server_audio_port = 2;
  // 沒有功能：位元組與舊版完全相同（舊 client 看到的 LINK_READY 不變）
  const auto plain = ml::format_ready_tlv(links);
  EXPECT_EQ(ml::format_ready_tlv(links, 0), plain);
  // 有功能：陣列之後多一個 byte，TLV 長度跟著加一
  const auto with = ml::format_ready_tlv(links, VIPLE_VR_LINK_F_CTRL);
  ASSERT_EQ(with.size(), plain.size() + 1);
  EXPECT_EQ(with[1], plain[1] + 1);
  EXPECT_EQ(with.back(), VIPLE_VR_LINK_F_CTRL);
  EXPECT_TRUE(std::equal(plain.begin() + 2, plain.end(), with.begin() + 2));
}

TEST(VrMultilinkFault, CtrlRule) {
  // c:<on>:<off>[:<phase>]：off 期間 ENet 上的 VR 即時訊息全部丟掉；不影響任何一條連線
  const auto f = ml::fault_t::parse("c:60:40,1:10:10");
  ASSERT_TRUE(f.has_value());
  EXPECT_TRUE(f->any());
  EXPECT_FALSE(f->ctrl_blocked(0));
  EXPECT_FALSE(f->ctrl_blocked(59));
  EXPECT_TRUE(f->ctrl_blocked(60));
  EXPECT_TRUE(f->ctrl_blocked(99));
  EXPECT_FALSE(f->ctrl_blocked(100));
  EXPECT_TRUE(f->blocked(1, 15));  // 同一份規則裡的連線空檔照常生效
  EXPECT_FALSE(f->blocked(2, 70));

  const auto always = ml::fault_t::parse("c:0:1000");
  ASSERT_TRUE(always.has_value());
  EXPECT_TRUE(always->ctrl_blocked(0));
  EXPECT_TRUE(always->ctrl_blocked(123456));

  EXPECT_FALSE(ml::fault_t::parse("1:60:40")->ctrl_blocked(70));  // 沒有 c: 規則
  EXPECT_FALSE(ml::fault_t::parse("c:60").has_value());
  EXPECT_FALSE(ml::fault_t::parse("c:0:0").has_value());
  EXPECT_FALSE(ml::fault_t::parse("c:a:40").has_value());
  EXPECT_FALSE(ml::fault_t::parse("c:60:40:0:9").has_value());
}

// ── §VR-LINK-REPAIR ──────────────────────────────────────────────

namespace {
  std::vector<uint8_t> nack_body(uint32_t frame, uint8_t block, uint8_t need, uint8_t total, uint8_t attempt, const ml::shard_bits_t &have) {
    VIPLE_VR_TLV_NACK n {};
    n.frame = frame;
    n.block = block;
    n.need = need;
    n.total = total;
    n.attempt = attempt;
    std::vector<uint8_t> b((const uint8_t *) &n, (const uint8_t *) &n + sizeof(n));
    b.insert(b.end(), have.begin(), have.begin() + (total + 7) / 8);
    return b;
  }

  ml::shard_bits_t all_but(unsigned total, std::initializer_list<unsigned> missing) {
    ml::shard_bits_t b {};
    for (unsigned i = 0; i < total; ++i) {
      ml::bit_set(b, i);
    }
    for (unsigned i : missing) {
      b[i >> 3] &= (uint8_t) ~(1u << (i & 7));
    }
    return b;
  }

  ml::shard_bits_t first_n(unsigned n) {
    ml::shard_bits_t b {};
    for (unsigned i = 0; i < n; ++i) {
      ml::bit_set(b, i);
    }
    return b;
  }
}  // namespace

TEST(VrMultilinkRepair, NackRoundTrip) {
  const auto have = all_but(216, {3, 100, 215});
  const auto body = nack_body(1234, 1, 2, 216, 1, have);
  ASSERT_EQ(body.size(), sizeof(VIPLE_VR_TLV_NACK) + 27u);
  ml::nack_t n;
  ASSERT_TRUE(ml::parse_nack(body.data(), body.size(), n));
  EXPECT_EQ(n.frame, 1234u);
  EXPECT_EQ(n.block, 1);
  EXPECT_EQ(n.total, 216);
  EXPECT_TRUE(ml::bit_get(n.have, 0));
  EXPECT_FALSE(ml::bit_get(n.have, 3));
  EXPECT_FALSE(ml::bit_get(n.have, 100));
  EXPECT_FALSE(ml::bit_get(n.have, 215));
  EXPECT_FALSE(ml::bit_get(n.have, 216));  // 超出 total 的位元不算

  EXPECT_FALSE(ml::parse_nack(body.data(), body.size() - 1, n));  // 位元圖被截斷
  EXPECT_FALSE(ml::parse_nack(body.data(), sizeof(VIPLE_VR_TLV_NACK) - 1, n));
  EXPECT_FALSE(ml::parse_nack(nullptr, 0, n));

  // total＝0：client 什麼都沒收到，沒有位元圖
  const auto empty = nack_body(77, 0, 0, 0, 1, {});
  ASSERT_EQ(empty.size(), sizeof(VIPLE_VR_TLV_NACK));
  ASSERT_TRUE(ml::parse_nack(empty.data(), empty.size(), n));
  EXPECT_EQ(n.total, 0);

  // 最後一個 byte 裡超出 total 的位元要清掉（client 填了也不採信）
  ml::shard_bits_t dirty {};
  dirty.fill(0xFF);
  const auto d = nack_body(1, 0, 0, 10, 1, dirty);
  ASSERT_TRUE(ml::parse_nack(d.data(), d.size(), n));
  EXPECT_TRUE(ml::bit_get(n.have, 9));
  EXPECT_FALSE(ml::bit_get(n.have, 10));
}

TEST(VrMultilinkRepair, ResendsJustEnoughToDecodeDataFirst) {
  // 180 data＋36 parity，掉了 40 個（38 個 data、2 個 parity）：FEC 補不回來（只能補 36 個），還差 4 個。
  // client 一湊滿 180 個就還原，多送的全是浪費 → 只補 4 個＋2 個餘裕，挑缺的 data（序號小的在前）
  ml::shard_bits_t have = first_n(216);
  for (unsigned i = 50; i < 88; ++i) {
    have[i >> 3] &= (uint8_t) ~(1u << (i & 7));
  }
  have[200 >> 3] &= (uint8_t) ~(1u << (200 & 7));
  have[201 >> 3] &= (uint8_t) ~(1u << (201 & 7));
  ml::nack_t n;
  n.total = 216;
  n.have = have;
  unsigned needed = 99;
  const auto plan = ml::plan_repair(n, 180, 216, first_n(216), nullptr, &needed);
  ASSERT_EQ(plan.size(), 6u);
  EXPECT_EQ(plan.front(), 50);
  EXPECT_EQ(plan.back(), 55);
  EXPECT_EQ(needed, 4u);  // 記 ABR 的帳用：實際缺的是 4 個，另外 2 個是餘裕
}

TEST(VrMultilinkRepair, AlreadyDecodableNeedsNothing) {
  // 掉了 36 個：剛好還能用 FEC 還原，client 不該回報；真的收到就什麼都不補
  ml::nack_t n;
  n.total = 216;
  n.have = first_n(180);
  unsigned needed = 99;
  EXPECT_TRUE(ml::plan_repair(n, 180, 216, first_n(216), nullptr, &needed).empty());
  EXPECT_EQ(needed, 0u);
}

TEST(VrMultilinkRepair, ManyMissingOnlyTopsUpToDecodable) {
  // 只收到前 60 個 data：還差 120 個 → 補到夠還原再多八分之一（120＋15），data 用完了才輪到 parity
  ml::nack_t n;
  n.total = 216;
  n.have = first_n(60);
  const auto plan = ml::plan_repair(n, 180, 216, first_n(216));
  EXPECT_EQ(plan.size(), 120u + 15u);
  EXPECT_EQ(plan.front(), 60);
  for (size_t i = 1; i < plan.size(); ++i) {
    EXPECT_EQ(plan[i], plan[i - 1] + 1);  // data 在前，依序
  }
}

TEST(VrMultilinkRepair, NothingReceivedResendsEnoughToDecode) {
  // total＝0（整個 block 沒到）：server 用自己的紀錄，補到夠還原
  ml::nack_t n;
  unsigned needed = 0;
  const auto plan = ml::plan_repair(n, 180, 216, first_n(216), nullptr, &needed);
  EXPECT_EQ(plan.size(), 180u + 22u);
  EXPECT_EQ(needed, 180u);
  // client 的 total 和 server 的對不上：位元圖不可信，同樣當成全部都缺
  ml::nack_t bad;
  bad.total = 100;
  bad.have = first_n(100);
  EXPECT_EQ(ml::plan_repair(bad, 180, 216, first_n(216)).size(), 202u);
}

TEST(VrMultilinkRepair, OnlyStoredShardsAreResent) {
  // 回報到的時候後半個 block 還沒送出（倉裡只有前 100 個）：沒送出的不算缺，也補不了
  ml::nack_t n;
  n.total = 216;
  n.have = all_but(100, {10, 20});  // 前 100 個裡缺 2 個；100 以後的位元都是 0
  const auto plan = ml::plan_repair(n, 180, 216, first_n(100));
  ASSERT_EQ(plan.size(), 2u);
  EXPECT_EQ(plan[0], 10);
  EXPECT_EQ(plan[1], 20);
  // 不合理的參數
  EXPECT_TRUE(ml::plan_repair(n, 0, 216, first_n(216)).empty());
  EXPECT_TRUE(ml::plan_repair(n, 217, 216, first_n(216)).empty());
}

TEST(VrMultilinkRepair, ShardsStillInFlightAreNotResentAgain) {
    // client 等不及、補包還沒到就再報一次：位元圖和上一份一樣。剛補過的那些當成會收到，不再補第二次
  ml::shard_bits_t have = first_n(216);
  for (unsigned i = 50; i < 88; ++i) {
    have[i >> 3] &= (uint8_t) ~(1u << (i & 7));
  }
  ml::nack_t n;
  n.total = 216;
  n.have = have;  // 缺 38 個 data（parity 都到了：還差 2 個）
  const auto first = ml::plan_repair(n, 180, 216, first_n(216));
  ASSERT_EQ(first.size(), 4u);  // 2 個＋2 個餘裕
  ml::shard_bits_t resent {};
  for (const uint8_t s : first) {
    ml::bit_set(resent, s);
  }
  EXPECT_TRUE(ml::plan_repair(n, 180, 216, first_n(216), &resent).empty());
  // 路上的只有一部分（上一輪只補了前 10 個），而且連 parity 也沒收到（補上那 10 個還差 28 個）：補其餘的 data，
  // 餘裕（3 個）用 parity
  ml::nack_t worse;
  worse.total = 216;
  worse.have = first_n(180);  // parity 全沒到
  for (unsigned i = 50; i < 88; ++i) {
    worse.have[i >> 3] &= (uint8_t) ~(1u << (i & 7));
  }
  ml::shard_bits_t part {};
  for (unsigned i = 50; i < 60; ++i) {
    ml::bit_set(part, i);
  }
  unsigned needed = 0;
  const auto rest = ml::plan_repair(worse, 180, 216, first_n(216), &part, &needed);
  ASSERT_EQ(rest.size(), 31u);
  EXPECT_EQ(needed, 28u);
  EXPECT_EQ(rest.front(), 60);
  EXPECT_EQ(rest[27], 87);
  EXPECT_EQ(rest[28], 180);
  EXPECT_EQ(rest.back(), 182);
  // parity 有收到時，路上那 10 個到了就夠還原（142＋36＋10 ≥ 180）：不必再補
  EXPECT_TRUE(ml::plan_repair(n, 180, 216, first_n(216), &part).empty());
  // 超過往返時間還缺（呼叫端不再給 in_flight）：重新補一次
  EXPECT_EQ(ml::plan_repair(n, 180, 216, first_n(216), nullptr).size(), 4u);
}

TEST(VrMultilinkRepair, StoreKeepsRecentBlocksAndOverwritesOldest) {
  ml::packet_store_t store;
  std::vector<uint8_t> pkts(8 * 100);
  const auto put = [&](uint32_t frame, uint8_t block, uint16_t first, size_t count, uint8_t fill) {
    std::fill(pkts.begin(), pkts.end(), fill);
    ml::video_meta_t m;
    m.frame = frame;
    m.block = block;
    m.first_shard = first;
    m.data_shards = 6;
    m.total_shards = 8;
    return store.put(m, pkts.data(), 100, count, 1000);
  };
  // 一個 block 分兩批進來
  ASSERT_TRUE(put(10, 0, 0, 5, 0xA1));
  ASSERT_TRUE(put(10, 0, 5, 3, 0xA2));
  auto *b = store.find(10, 0);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(b->total, 8);
  EXPECT_EQ(b->data, 6);
  EXPECT_TRUE(ml::bit_get(b->stored, 0));
  EXPECT_TRUE(ml::bit_get(b->stored, 7));
  EXPECT_EQ(b->pkt(4)[0], 0xA1);
  EXPECT_EQ(b->pkt(5)[99], 0xA2);
  // 同一幀的第二個 block 是另一格
  ASSERT_TRUE(put(10, 1, 0, 8, 0xB0));
  EXPECT_NE(store.find(10, 1), store.find(10, 0));
  // 只進來一部分：stored 只標那幾個
  ASSERT_TRUE(put(11, 0, 0, 3, 0xC0));
  EXPECT_TRUE(ml::bit_get(store.find(11, 0)->stored, 2));
  EXPECT_FALSE(ml::bit_get(store.find(11, 0)->stored, 3));
  // 再塞滿一圈之後最舊的被覆寫
  for (uint32_t f = 100; f < 100 + ml::packet_store_t::kBlocks; ++f) {
    ASSERT_TRUE(put(f, 0, 0, 8, 0xD0));
  }
  EXPECT_EQ(store.find(10, 0), nullptr);
  EXPECT_EQ(store.find(11, 0), nullptr);
  EXPECT_NE(store.find(100 + ml::packet_store_t::kBlocks - 1, 0), nullptr);
  // 不存的：total > 255（不做 FEC 的超大幀）、批次超出 block、空的
  ml::video_meta_t big;
  big.frame = 500;
  big.data_shards = 300;
  big.total_shards = 300;
  EXPECT_FALSE(store.put(big, pkts.data(), 100, 8, 0));
  EXPECT_EQ(store.find(500, 0), nullptr);
  EXPECT_FALSE(put(600, 0, 6, 3, 0));  // 6＋3 > 8
  EXPECT_FALSE(put(601, 0, 0, 0, 0));
}

TEST(VrMultilinkRepair, BudgetRefillsWithVideoRate) {
  ml::repair_budget_t b;
  const double pps = 16000;  // 約 180 Mbps
  int64_t t = 1'000'000'000;
  const unsigned burst = (unsigned) ml::repair_budget_t::kBurst;
  EXPECT_TRUE(b.take(burst / 2, pps, t));   // 一開始是滿的
  EXPECT_TRUE(b.take(burst / 2, pps, t));
  EXPECT_FALSE(b.take(1, pps, t));    // 用完了
  t += 50'000'000;                    // 50 ms：補進 16000×15%×0.05＝120
  EXPECT_FALSE(b.take(121, pps, t));
  EXPECT_TRUE(b.take(119, pps, t));
  t += 10'000'000'000;                // 很久之後也只會補到上限
  EXPECT_TRUE(b.take(burst, pps, t));
  EXPECT_FALSE(b.take(1, pps, t));
}

TEST(VrMultilinkRepair, BudgetCoversTwoWholeFramesInARow) {
  // 鏈路斷一小段、連續兩整幀沒到：72 Hz 預設位元率時一幀 213 個 data，整幀補一次是 213＋26＝239 個。
  // 兩幀相隔一個往返（6 ms）都要補得起；第三次（再 6 ms 後）才擋下來
  ml::repair_budget_t b;
  const double pps = 16000;
  int64_t t = 1'000'000'000;
  EXPECT_TRUE(b.take(239, pps, t));
  t += 6'000'000;
  EXPECT_TRUE(b.take(239, pps, t));
  t += 6'000'000;
  EXPECT_FALSE(b.take(239, pps, t));
}

TEST(VrMultilinkFault, BurstRule) {
  const auto f = ml::fault_t::parse("b1:100:40,c:0:1000");
  ASSERT_TRUE(f.has_value());
  EXPECT_TRUE(f->any());
  EXPECT_EQ(f->burst_rule(1).period_ms, 100);
  EXPECT_EQ(f->burst_rule(1).pkts, 40);
  EXPECT_EQ(f->burst_rule(2).pkts, 0);
  EXPECT_EQ(f->burst_rule(99).pkts, 0);
  EXPECT_TRUE(f->ctrl_blocked(5));
  EXPECT_TRUE(ml::fault_t::parse("b2:10:1")->any());
  EXPECT_FALSE(ml::fault_t::parse("b0:100:40").has_value());
  EXPECT_FALSE(ml::fault_t::parse("b1:5:40").has_value());     // 週期至少 10 ms
  EXPECT_FALSE(ml::fault_t::parse("b1:100:0").has_value());
  EXPECT_FALSE(ml::fault_t::parse("b1:100").has_value());
  EXPECT_FALSE(ml::fault_t::parse("b1:100:40:1").has_value());
}

// ── 審查與首次頭盔實測之後補的規則 ────────────────────────────────

TEST(VrMultilinkProbeGate, GoodMeasurementsMustBeConsecutiveProbes) {
  // 兩次「夠快」中間有探測沒量到（封包沒到齊，client 不會回報）：不算連續
  ml::probe_gate_t g;
  EXPECT_FALSE(g.on_report(1, 600, 300, 10));  // 第 10 批探測量到夠快
  EXPECT_FALSE(g.on_report(2, 600, 300, 14));  // 中間 3 批都沒量到：重新算第一次
  EXPECT_TRUE(g.on_report(3, 600, 300, 15));   // 緊接著的下一批：連續第二次
}

TEST(VrMultilinkProbeGate, StaleResultDoesNotPassOnRepeatedReports) {
  // 量到兩次夠快之後鏈路變差：之後的探測都沒量到，PING 一直重複回報同一個 seq——不能拿舊成績放行
  ml::probe_gate_t g;
  EXPECT_FALSE(g.on_report(1, 600, 300, 1));
  EXPECT_TRUE(g.on_report(2, 600, 300, 2));
  EXPECT_TRUE(g.on_report(2, 600, 300, 3));   // 還在容許範圍
  EXPECT_TRUE(g.on_report(2, 600, 300, 4));
  EXPECT_FALSE(g.on_report(2, 600, 300, 5));  // 又送了 3 批都沒有新的量測：作廢
  EXPECT_FALSE(g.on_report(2, 600, 300, 9));
  EXPECT_FALSE(g.on_report(3, 600, 300, 10));  // 要重新累積兩次
  EXPECT_TRUE(g.on_report(4, 600, 300, 11));
}

TEST(VrMultilinkProbeGate, ResetAlignsToCurrentMeasurement) {
  // 退回探測時：退回之前（還在送影像）量到的那一筆不能算第一次
  ml::probe_gate_t g;
  g.reset(7);
  EXPECT_FALSE(g.on_report(7, 900, 300, 0));  // 還是退回前的那一筆
  EXPECT_FALSE(g.on_report(8, 900, 300, 1));  // 第一次新的量測
  EXPECT_TRUE(g.on_report(9, 900, 300, 2));
}

TEST(VrMultilinkSaturation, ReportsHowMuchWasActuallySent) {
  ml::saturation_t s;
  auto t = clk::time_point {} + 1s;
  feed(s, t, 3s, 4);  // 每 4 批有 1 批沒送完
  EXPECT_NEAR(s.last_ok_pct, 75, 2);
}

TEST(VrMultilinkDelivery, RatioPerSecondFromCumulativeCounters) {
  ml::delivery_t d;
  const int64_t s = 1'000'000'000;
  d.on_ping(0, 0, 0);
  EXPECT_EQ(d.pct, -1);
  d.on_ping(5000, 4900, s / 2);  // 還不到 1 s：不結算
  EXPECT_EQ(d.pct, -1);
  d.on_ping(20000, 14000, s);    // 這 1 s 送 20000、頭盔收到 14000
  EXPECT_EQ(d.pct, 70);
  d.on_ping(40000, 33800, 2 * s);
  EXPECT_EQ(d.pct, 99);
  d.on_ping(40100, 33900, 3 * s);  // 幾乎沒送（待命或暫停）：不知道
  EXPECT_EQ(d.pct, -1);
}

TEST(VrMultilinkDelivery, ClientCounterWrapAndOvershoot) {
  ml::delivery_t d;
  const int64_t s = 1'000'000'000;
  d.on_ping(1000, 0xFFFFFF00u, 0);
  d.on_ping(21000, 0x00004D20u, s);  // 32 bit 繞回：實際收到 20000
  EXPECT_EQ(d.pct, 100);
  d.on_ping(41000, 0x00004D20u + 30000u, 2 * s);  // 回報落後補上來，超過 100% 就夾住
  EXPECT_EQ(d.pct, 100);
}
