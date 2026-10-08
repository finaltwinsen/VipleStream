/**
 * @file tests/unit/test_vr_params.cpp
 * @brief Test src/vr/vr_session.*（/launch VR 參數的嚴格解析、round-trip、LOSS／wave 狀態機）。
 */
#include "../tests_common.h"

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include <src/vr/vr_session.h>

namespace {
  using query_map_t = std::map<std::string, std::string, std::less<>>;

  /// 把 "a=1&b=2" 拆成 map（測試用；值裡不會有 '&'）
  query_map_t split_query(std::string_view query) {
    query_map_t out;
    std::size_t pos = 0;
    while (pos <= query.size()) {
      auto amp = query.find('&', pos);
      auto item = query.substr(pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);
      if (!item.empty()) {
        auto eq = item.find('=');
        out[std::string(item.substr(0, eq))] = eq == std::string_view::npos ? std::string {} : std::string(item.substr(eq + 1));
      }
      if (amp == std::string_view::npos) {
        break;
      }
      pos = amp + 1;
    }
    return out;
  }

  vr::parse_result_t parse_map(const query_map_t &args) {
    return vr::parse_launch_params([&args](std::string_view name) -> std::optional<std::string> {
      auto it = args.find(name);
      if (it == args.end()) {
        return std::nullopt;
      }
      return it->second;
    });
  }

  /// 必填欄位齊全、其餘用預設值的最小合法參數
  query_map_t minimal_args() {
    return {
      {"vr", "1"},
      {"vrEye", "1728x1728"},
      {"vrHz", "90"},
      {"vrCodecs", "3"},
      {"vrCaps", "3"},
    };
  }

  /// 預期失敗，且錯誤訊息含有 needle
  void expect_error(const query_map_t &args, std::string_view needle) {
    auto r = parse_map(args);
    EXPECT_FALSE(r.params.has_value());
    EXPECT_NE(r.error.find(needle), std::string::npos) << "error was: " << r.error;
  }

  query_map_t with(query_map_t args, const std::string &key, const std::string &value) {
    args[key] = value;
    return args;
  }

  query_map_t without(query_map_t args, const std::string &key) {
    args.erase(key);
    return args;
  }
}  // namespace

TEST(VrParamsTest, MinimalUsesDefaults) {
  auto r = parse_map(minimal_args());
  ASSERT_TRUE(r.params.has_value()) << r.error;
  const auto &p = *r.params;
  EXPECT_EQ(p.eye_width, 1728);
  EXPECT_EQ(p.eye_height, 1728);
  EXPECT_EQ(p.hz, 90);
  EXPECT_EQ(p.period_ns, 1000000000LL / 90);
  for (int v : p.fov) {
    EXPECT_EQ(v, 10000);
  }
  EXPECT_EQ(p.ipd, 6300);
  const std::array<int, 14> eth {-3150, 0, 0, 0, 0, 0, 10000, 3150, 0, 0, 0, 0, 0, 10000};
  EXPECT_EQ(p.eye_to_head, eth);
  EXPECT_EQ(p.caps, 3u);
  EXPECT_EQ(p.codecs, 3u);
  EXPECT_EQ(p.ctrl, "touch");
  EXPECT_EQ(p.overscan, 0);
  EXPECT_FALSE(p.force);
}

TEST(VrParamsTest, DefaultEyeToHeadFollowsIpd) {
  auto r = parse_map(with(minimal_args(), "vrIpd", "6401"));
  ASSERT_TRUE(r.params.has_value()) << r.error;
  EXPECT_EQ(r.params->eye_to_head[0], -3200);
  EXPECT_EQ(r.params->eye_to_head[7], 3200);
}

TEST(VrParamsTest, RoundTrip) {
  vr::launch_params_t p;
  p.eye_width = 2016;
  p.eye_height = 2240;
  p.hz = 120;
  p.period_ns = 8333333;
  p.fov = {-9500, 8800, 10200, -10400, -8800, 9500, 10200, -10400};
  p.ipd = 6450;
  p.eye_to_head = {-3225, 10, -150, 0, 436, 0, 9999, 3225, 10, -150, 0, -436, 0, 9999};
  p.caps = 0x0B;
  p.codecs = VIPLE_VR_CODEC_HEVC | VIPLE_VR_CODEC_H264 | VIPLE_VR_CODEC_AV1;
  p.ctrl = "index";
  p.overscan = 50;
  p.force = true;

  const auto query = vr::format_launch_params(p);
  auto r = parse_map(split_query(query));
  ASSERT_TRUE(r.params.has_value()) << r.error << " query=" << query;
  EXPECT_EQ(*r.params, p);
  EXPECT_EQ(vr::format_launch_params(*r.params), query);
}

TEST(VrParamsTest, FormatMatchesClientOrder) {
  auto r = parse_map(minimal_args());
  ASSERT_TRUE(r.params.has_value());
  EXPECT_EQ(
    vr::format_launch_params(*r.params),
    "vr=1&vrEye=1728x1728&vrHz=90&vrPeriodNs=11111111"
    "&vrFov=10000,10000,10000,10000,10000,10000,10000,10000&vrIpd=6300"
    "&vrEyeToHead=-3150,0,0,0,0,0,10000,3150,0,0,0,0,0,10000"
    "&vrCaps=3&vrCodecs=3&vrCtrl=touch&vrOverscan=0&vrForce=0"
  );
}

TEST(VrParamsTest, CapsAcceptsHexWithOrWithoutPrefix) {
  auto a = parse_map(with(minimal_args(), "vrCaps", "0x1F"));
  ASSERT_TRUE(a.params.has_value()) << a.error;
  EXPECT_EQ(a.params->caps, 0x1Fu);
  auto b = parse_map(with(minimal_args(), "vrCaps", "ff"));
  ASSERT_TRUE(b.params.has_value()) << b.error;
  EXPECT_EQ(b.params->caps, 0xFFu);
}

TEST(VrParamsTest, MissingRequiredFields) {
  expect_error(without(minimal_args(), "vr"), "missing vr");
  expect_error(without(minimal_args(), "vrEye"), "missing vrEye");
  expect_error(without(minimal_args(), "vrHz"), "missing vrHz");
  expect_error(without(minimal_args(), "vrCodecs"), "missing vrCodecs");
  expect_error(without(minimal_args(), "vrCaps"), "missing vrCaps");
}

TEST(VrParamsTest, VrMustBeOne) {
  expect_error(with(minimal_args(), "vr", "2"), "vr='2'");
  expect_error(with(minimal_args(), "vr", ""), "vr=''");
}

TEST(VrParamsTest, OutOfRange) {
  expect_error(with(minimal_args(), "vrEye", "255x1728"), "vrEye.W=255 out of range");
  expect_error(with(minimal_args(), "vrEye", "1728x4097"), "vrEye.H=4097 out of range");
  expect_error(with(minimal_args(), "vrHz", "59"), "vrHz=59 out of range");
  expect_error(with(minimal_args(), "vrHz", "145"), "vrHz=145 out of range");
  expect_error(with(minimal_args(), "vrPeriodNs", "0"), "vrPeriodNs=0 out of range");
  expect_error(with(minimal_args(), "vrIpd", "8001"), "vrIpd=8001 out of range");
  expect_error(with(minimal_args(), "vrOverscan", "201"), "vrOverscan=201 out of range");
  expect_error(with(minimal_args(), "vrFov", "10000,10000,100001,10000,10000,10000,10000,10000"), "vrFov[2]=100001 out of range");
  expect_error(with(minimal_args(), "vrFov", "10000,10000,10000,10000,10000,10000,10000,-100001"), "vrFov[7]=-100001 out of range");
  expect_error(with(minimal_args(), "vrEyeToHead", "-3150,0,0,0,0,0,10000,8001,0,0,0,0,0,10000"), "vrEyeToHead[7]=8001 out of range");
  expect_error(with(minimal_args(), "vrEyeToHead", "-3150,0,0,0,0,0,10001,3150,0,0,0,0,0,10000"), "vrEyeToHead[6]=10001 out of range");
  expect_error(with(minimal_args(), "vrEyeToHead", "-3150,0,0,0,0,0,0,3150,0,0,0,0,0,10000"), "left eye quaternion is not unit length");
  expect_error(with(minimal_args(), "vrCaps", "100000000"), "vrCaps='100000000'");
  expect_error(with(minimal_args(), "vrCodecs", "4294967296"), "vrCodecs=4294967296 out of range");
}

TEST(VrParamsTest, TrailingGarbage) {
  expect_error(with(minimal_args(), "vrHz", "90abc"), "vrHz='90abc'");
  expect_error(with(minimal_args(), "vrHz", "90 "), "vrHz='90 '");
  expect_error(with(minimal_args(), "vrHz", " 90"), "vrHz=' 90'");
  expect_error(with(minimal_args(), "vrHz", "+90"), "vrHz='+90'");
  expect_error(with(minimal_args(), "vrHz", ""), "vrHz=''");
  expect_error(with(minimal_args(), "vrEye", "1728x1728x2"), "vrEye='1728x1728x2'");
  expect_error(with(minimal_args(), "vrEye", "1728*1728"), "vrEye='1728*1728'");
  expect_error(with(minimal_args(), "vrEye", "1728x"), "vrEye.H=''");
  expect_error(with(minimal_args(), "vrCaps", "0x"), "vrCaps='0x'");
  expect_error(with(minimal_args(), "vrCaps", "3g"), "vrCaps='3g'");
  expect_error(with(minimal_args(), "vrFov", "10000,10000,10000,10000,10000,10000,10000,10000x"), "vrFov[7]='10000x'");
  expect_error(with(minimal_args(), "vrCtrl", "touchx"), "vrCtrl='touchx'");
  expect_error(with(minimal_args(), "vrForce", "01"), "vrForce='01'");
}

TEST(VrParamsTest, WrongListCount) {
  expect_error(with(minimal_args(), "vrFov", "10000,10000,10000,10000,10000,10000,10000"), "vrFov needs exactly 8 comma-separated values, got 7");
  expect_error(with(minimal_args(), "vrFov", "10000,10000,10000,10000,10000,10000,10000,10000,10000"), "vrFov needs exactly 8 comma-separated values, got more");
  expect_error(with(minimal_args(), "vrFov", "10000,10000,10000,,10000,10000,10000,10000"), "vrFov[3]=''");
  expect_error(with(minimal_args(), "vrFov", "10000,10000,10000,10000,10000,10000,10000,10000,"), "got more");
  expect_error(with(minimal_args(), "vrEyeToHead", "-3150,0,0,0,0,0,10000"), "vrEyeToHead needs exactly 14 comma-separated values, got 7");
}

TEST(VrParamsTest, NegativeValues) {
  // fov 與 eyeToHead 的元素可以是負值
  auto ok = parse_map(with(with(minimal_args(), "vrFov", "-10000,10000,10000,-10000,-10000,10000,10000,-10000"),
                           "vrEyeToHead",
                           "-3150,-10,-20,0,0,0,-10000,3150,0,0,0,0,0,10000"));
  ASSERT_TRUE(ok.params.has_value()) << ok.error;
  EXPECT_EQ(ok.params->fov[0], -10000);
  EXPECT_EQ(ok.params->eye_to_head[6], -10000);

  // 其餘欄位禁止負值
  expect_error(with(minimal_args(), "vrHz", "-90"), "vrHz='-90' is not a non-negative");
  expect_error(with(minimal_args(), "vrEye", "-1728x1728"), "vrEye.W='-1728' is not a non-negative");
  expect_error(with(minimal_args(), "vrPeriodNs", "-11111111"), "vrPeriodNs='-11111111' is not a non-negative");
  expect_error(with(minimal_args(), "vrIpd", "-6300"), "vrIpd='-6300' is not a non-negative");
  expect_error(with(minimal_args(), "vrCodecs", "-3"), "vrCodecs='-3' is not a non-negative");
  expect_error(with(minimal_args(), "vrOverscan", "-1"), "vrOverscan='-1' is not a non-negative");
  expect_error(with(minimal_args(), "vrIpd", "-0"), "vrIpd='-0' is not a non-negative");
  expect_error(with(minimal_args(), "vrCaps", "-3"), "vrCaps='-3'");
}

// §VR-FOVEA：vrFovea 選填（100＝關）；關閉時 query 與協商回應都和以前一樣，只有真的 driver 會回 fovea=
TEST(VrParamsTest, Foveation) {
  EXPECT_EQ(parse_map(minimal_args()).params->fovea, 100);
  auto r = parse_map(with(minimal_args(), "vrFovea", "150"));
  ASSERT_TRUE(r.params.has_value()) << r.error;
  EXPECT_EQ(r.params->fovea, 150);
  const auto query = vr::format_launch_params(*r.params);
  EXPECT_NE(query.find("&vrForce=0&vrFovea=150"), std::string::npos) << query;
  auto back = parse_map(split_query(query));
  ASSERT_TRUE(back.params.has_value()) << back.error;
  EXPECT_EQ(*back.params, *r.params);
  expect_error(with(minimal_args(), "vrFovea", "99"), "vrFovea=99 out of range");
  expect_error(with(minimal_args(), "vrFovea", "251"), "vrFovea=251 out of range");

  vr::negotiated_t neg;
  neg.params = *r.params;
  neg.guid = "g";
  EXPECT_EQ(vr::format_session_element(neg).find("fovea"), std::string::npos);  // stub 不合成影像
  neg.pcvr = true;
  const auto el = vr::format_session_element(neg);
  EXPECT_EQ(el.substr(el.size() - 10), ";fovea=150") << el;
  neg.params.fovea = 100;
  EXPECT_EQ(vr::format_session_element(neg).find("fovea"), std::string::npos);
}

TEST(VrParamsTest, SessionElement) {
  vr::negotiated_t neg;
  neg.params = *parse_map(minimal_args()).params;
  neg.codec = vr::codec_e::hevc;
  neg.recovery_intra = true;
  neg.ir_frames = 8;
  neg.guid = "0000-guid";
  EXPECT_EQ(vr::format_session_element(neg),
            "proto=1;packed=3456x1728;hz=90;codec=hevc;layout=sbs;overscan=0;recovery=intra;irFrames=8;"
            "transport=rtp;universeId=0;session=0000-guid;mode=stub");
  neg.codec = vr::codec_e::h264;
  neg.recovery_intra = false;
  EXPECT_EQ(vr::format_session_element(neg),
            "proto=1;packed=3456x1728;hz=90;codec=h264;layout=sbs;overscan=0;recovery=idr;irFrames=0;"
            "transport=rtp;universeId=0;session=0000-guid;mode=stub");
}

TEST(VrSessionStateTest, LossWaveStateMachine) {
  vr::negotiated_t neg;
  neg.params = *parse_map(minimal_args()).params;
  neg.recovery_intra = true;
  neg.ir_frames = 8;
  vr::session_state_t st {neg, nullptr};

  // 第一筆 → 開 wave；client 的第二份 → 吸收
  EXPECT_EQ(st.on_loss(100, 101, VIPLE_VR_LOSS_NETWORK), vr::loss_action_e::new_wave);
  EXPECT_EQ(st.on_loss(100, 101, VIPLE_VR_LOSS_NETWORK), vr::loss_action_e::duplicate);
  // wave 已排定、encoder 還沒開始：更晚的掉幀一樣在 wave 之前 → 吸收
  EXPECT_EQ(st.on_loss(100, 103, VIPLE_VR_LOSS_NETWORK), vr::loss_action_e::absorbed);

  st.on_wave_begin(110, 8, VIPLE_VR_REFRESH_LOSS);
  ASSERT_TRUE(st.active_wave().has_value());
  EXPECT_EQ(st.active_wave()->start_frame, 110u);
  // lastLost < waveStart → 吸收
  EXPECT_EQ(st.on_loss(100, 109, VIPLE_VR_LOSS_RESEND), vr::loss_action_e::absorbed);
  // wave 內的幀掉了 → 重啟 wave
  EXPECT_EQ(st.on_loss(100, 112, VIPLE_VR_LOSS_NETWORK), vr::loss_action_e::new_wave);
  st.on_wave_begin(115, 8, VIPLE_VR_REFRESH_LOSS);
  st.on_wave_end();
  EXPECT_FALSE(st.active_wave().has_value());
  // wave 結束後的新掉幀 → 新 wave
  EXPECT_EQ(st.on_loss(200, 200, VIPLE_VR_LOSS_NETWORK), vr::loss_action_e::new_wave);
  // encoder 放棄（IDR cooldown）→ 下一筆不同的 LOSS 可以再開
  st.on_wave_abort();
  EXPECT_EQ(st.on_loss(200, 201, VIPLE_VR_LOSS_NETWORK), vr::loss_action_e::new_wave);
  EXPECT_EQ(st.stats().loss_rx, 7u);
  EXPECT_EQ(st.stats().refresh_waves, 2u);
}

TEST(VrSessionStateTest, ScheduledWaveEstimateIsStableAndNeverBehindTheLoss) {
  vr::negotiated_t neg;
  neg.params = *parse_map(minimal_args()).params;
  neg.recovery_intra = true;
  neg.ir_frames = 8;
  vr::session_state_t st {neg, nullptr};
  EXPECT_FALSE(st.scheduled_wave().has_value());

  // 排定當下 encoder 即將編第 1003 幀：這就是估計的起點
  st.set_next_frame(1003);
  EXPECT_EQ(st.on_loss(1000, 1000, VIPLE_VR_LOSS_NETWORK), vr::loss_action_e::new_wave);
  ASSERT_TRUE(st.scheduled_wave().has_value());
  EXPECT_EQ(st.scheduled_wave()->start_frame, 1003u);
  EXPECT_EQ(st.scheduled_wave()->frame_cnt, 8);
  EXPECT_EQ(st.scheduled_wave()->reason, VIPLE_VR_REFRESH_LOSS);
  // encoder 已經決定從 1003 開 wave、正在等影像：next_frame 變成 1004，被吸收的 LOSS 回的仍是 1003（不能多算一幀）
  st.set_next_frame(1004);
  EXPECT_EQ(st.on_loss(1000, 1001, VIPLE_VR_LOSS_NETWORK), vr::loss_action_e::absorbed);
  EXPECT_EQ(st.scheduled_wave()->start_frame, 1003u);
  // wave 一直沒開始（等 IDR cooldown）、encoder 繼續出幀，之後又掉了第 1006 幀：估計值推到 1007，重送也一致
  st.set_next_frame(1008);
  EXPECT_EQ(st.on_loss(1000, 1006, VIPLE_VR_LOSS_NETWORK), vr::loss_action_e::absorbed);
  EXPECT_EQ(st.scheduled_wave()->start_frame, 1007u);
  EXPECT_EQ(st.on_loss(1000, 1006, VIPLE_VR_LOSS_RESEND), vr::loss_action_e::absorbed);
  EXPECT_EQ(st.scheduled_wave()->start_frame, 1007u);
  // wave 真的開始之後回的是精確值
  st.on_wave_begin(1009, 8, VIPLE_VR_REFRESH_IDR);
  EXPECT_EQ(st.scheduled_wave()->start_frame, 1009u);
  EXPECT_EQ(st.scheduled_wave()->reason, VIPLE_VR_REFRESH_IDR);
  st.on_wave_end();
  EXPECT_FALSE(st.scheduled_wave().has_value());
}

TEST(VrSessionStateTest, LossResendIsNeverDuplicate) {
  vr::negotiated_t neg;
  neg.params = *parse_map(minimal_args()).params;
  neg.recovery_intra = true;
  neg.ir_frames = 8;
  vr::session_state_t st {neg, nullptr};

  EXPECT_EQ(st.on_loss(300, 300, VIPLE_VR_LOSS_NETWORK), vr::loss_action_e::new_wave);
  // client 的第二份（時間窗內、完全相同）→ 重複
  EXPECT_EQ(st.on_loss(300, 300, VIPLE_VR_LOSS_NETWORK), vr::loss_action_e::duplicate);
  // REFRESH_START 掉了，client 連續重送：每一次都要處理（wave 已排定 → absorbed，呼叫端據此
  // 重送 REFRESH_START），不能被當成重複吞掉
  EXPECT_EQ(st.on_loss(300, 300, VIPLE_VR_LOSS_RESEND), vr::loss_action_e::absorbed);
  EXPECT_EQ(st.on_loss(300, 300, VIPLE_VR_LOSS_RESEND), vr::loss_action_e::absorbed);
  EXPECT_EQ(st.on_loss(300, 300, VIPLE_VR_LOSS_RESEND), vr::loss_action_e::absorbed);
}

TEST(VrSessionStateTest, LossWaveWrapAround) {
  vr::negotiated_t neg;
  neg.recovery_intra = true;
  vr::session_state_t st {neg, nullptr};
  EXPECT_EQ(st.on_loss(0xFFFFFFF0u, 0xFFFFFFF1u, VIPLE_VR_LOSS_NETWORK), vr::loss_action_e::new_wave);
  st.on_wave_begin(0xFFFFFFFEu, 8, VIPLE_VR_REFRESH_LOSS);
  // 0xFFFFFFF5 在 0xFFFFFFFE 之前 → 吸收；迴繞後的 3 在 wave 之後 → 重啟
  EXPECT_EQ(st.on_loss(0xFFFFFFF0u, 0xFFFFFFF5u, VIPLE_VR_LOSS_NETWORK), vr::loss_action_e::absorbed);
  EXPECT_EQ(st.on_loss(0xFFFFFFF0u, 3u, VIPLE_VR_LOSS_NETWORK), vr::loss_action_e::new_wave);
}

TEST(VrSessionStateTest, RecoveryIdr) {
  vr::negotiated_t neg;
  neg.recovery_intra = false;
  vr::session_state_t st {neg, nullptr};
  EXPECT_EQ(st.on_loss(10, 11, VIPLE_VR_LOSS_NETWORK), vr::loss_action_e::idr);
  EXPECT_EQ(st.on_loss(10, 11, VIPLE_VR_LOSS_NETWORK), vr::loss_action_e::duplicate);
}

TEST(VrSessionStateTest, TrackingOrderAndGap) {
  vr::negotiated_t neg;
  vr::session_state_t st {neg, nullptr};
  EXPECT_FALSE(st.snapshot().has_value());

  VIPLE_VR_TRACKING t {};
  t.version = VIPLE_VR_TRACKING_VERSION;
  t.sampleId = 5;
  t.pose[VIPLE_VR_POSE_HMD].pos[1] = 1.5f;
  t.pose[VIPLE_VR_POSE_HMD].rot[3] = 1.0f;
  EXPECT_TRUE(st.on_tracking(t, false, 0));
  t.sampleId = 4;  // 倒退
  EXPECT_FALSE(st.on_tracking(t, false, 0));
  t.sampleId = 5;  // 重複
  EXPECT_FALSE(st.on_tracking(t, true, 0));
  t.sampleId = 9;  // 跳號 3
  EXPECT_TRUE(st.on_tracking(t, false, 0));
  t.version = 0;  // 版本不合
  t.sampleId = 10;
  EXPECT_FALSE(st.on_tracking(t, false, 0));

  auto s = st.stats();
  EXPECT_EQ(s.pose_rx, 4u);
  EXPECT_EQ(s.pose_ooo, 2u);
  EXPECT_EQ(s.pose_gap, 3u);
  EXPECT_EQ(s.pose_via_quic, 1u);
  EXPECT_EQ(s.pose_bad, 1u);
  EXPECT_EQ(s.last_sample_id, 9u);
  ASSERT_TRUE(st.snapshot().has_value());
  EXPECT_EQ(st.snapshot()->sample_id, 9u);
  EXPECT_FLOAT_EQ(st.snapshot()->pos[1], 1.5f);

  // client 重新計數（大幅倒退）→ 接受並計 resets
  t.version = VIPLE_VR_TRACKING_VERSION;
  st.on_tracking(t, false, 0);  // 10
  for (uint32_t id = 11; id < 3000; ++id) {
    t.sampleId = id;
    st.on_tracking(t, false, 0);
  }
  t.sampleId = 1;
  EXPECT_TRUE(st.on_tracking(t, false, 0));
  EXPECT_EQ(st.stats().pose_resets, 1u);
  EXPECT_EQ(st.snapshot()->sample_id, 1u);
}

TEST(VrSessionStateTest, OutboxAndTlv) {
  vr::negotiated_t neg;
  vr::session_state_t st {neg, nullptr};
  st.queue_state(VIPLE_VR_STATE_STUB_ECHO, 100, VIPLE_VR_STATE_CODE_NONE);
  st.queue_refresh_start({1234, 8, VIPLE_VR_REFRESH_LOSS});
  st.queue_stats();
  auto msgs = st.drain_s2c();
  // REFRESH_START 的第二份延到下一次 drain（分開 datagram），這一次只有 3 則
  ASSERT_EQ(msgs.size(), 3u);
  EXPECT_TRUE(msgs[0].reliable);
  ASSERT_EQ(msgs[0].tlv.size(), 2u + sizeof(VIPLE_VR_TLV_STATE));
  EXPECT_EQ(msgs[0].tlv[0], VIPLE_VR_S2C_STATE);
  EXPECT_EQ(msgs[0].tlv[1], sizeof(VIPLE_VR_TLV_STATE));
  EXPECT_EQ(msgs[0].tlv[2], VIPLE_VR_STATE_STUB_ECHO);
  EXPECT_EQ(msgs[0].tlv[3], 100);
  EXPECT_FALSE(msgs[1].reliable);
  ASSERT_EQ(msgs[1].tlv.size(), 2u + sizeof(VIPLE_VR_TLV_REFRESH_START));
  VIPLE_VR_TLV_REFRESH_START rs;
  std::memcpy(&rs, msgs[1].tlv.data() + 2, sizeof(rs));
  EXPECT_EQ(rs.startFrame, 1234u);
  EXPECT_EQ(rs.frameCnt, 8);
  EXPECT_EQ(rs.reason, VIPLE_VR_REFRESH_LOSS);
  EXPECT_EQ(msgs[2].tlv[0], VIPLE_VR_S2C_STATS);
  EXPECT_EQ(msgs[2].tlv.size(), 2u + sizeof(VIPLE_VR_TLV_STATS));

  auto next = st.drain_s2c();
  ASSERT_EQ(next.size(), 1u);
  EXPECT_FALSE(next[0].reliable);
  EXPECT_EQ(next[0].tlv, msgs[1].tlv);
  EXPECT_TRUE(st.drain_s2c().empty());
}

TEST(VrSessionStateTest, ActiveAndBusy) {
  vr::negotiated_t neg;
  neg.owner_uuid = "owner";
  auto st = std::make_shared<vr::session_state_t>(neg, nullptr);

  EXPECT_FALSE(vr::busy_for("other"));
  vr::reserve("owner", std::chrono::seconds {10});
  EXPECT_TRUE(vr::busy_for("other"));
  EXPECT_TRUE(vr::busy_for(""));
  EXPECT_FALSE(vr::busy_for("owner"));

  vr::set_active(st);
  EXPECT_EQ(vr::active(), st);
  EXPECT_TRUE(vr::busy_for("other"));
  EXPECT_FALSE(vr::busy_for("owner"));

  auto other = std::make_shared<vr::session_state_t>(neg, nullptr);
  vr::clear_active_if(other.get());  // 不是目前的 active → 不清
  EXPECT_EQ(vr::active(), st);
  vr::clear_active_if(st.get());
  EXPECT_FALSE(vr::active());
  EXPECT_FALSE(vr::busy_for("other"));

  vr::reserve("owner", std::chrono::seconds {10});
  vr::clear_reservation();
  EXPECT_FALSE(vr::busy_for("other"));
}
