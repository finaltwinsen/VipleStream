// unit_tests.cpp - VipleStream §VR：vr_probe --mode unit（設計 K16、§F.4、§F.5 T6）。
//
// driver 端（MSVC）純模組的單元測試；server 的 selftest T6 以 run_command 啟動、只看最後一行
// `unit pass=<n> fail=<n>` 與 exit code（fail == 0 才是 0）。開頭先印 ABI 表（T6 逐字比對 GCC 與 MSVC 的版面）。
//
// V2 的內容：
//   - ipc_proto.h：header／body 大小、handle 合理性、WELCOME／shm header／config／TEXTURES 驗證、
//     seqlock（多格、單格；撕裂與 ABA）、SPSC（滿、毀損）、log 文字過濾、重連退避、SDK 打包；
//   - ipc_client_t::expected_server_image_from_dll_path（§B.1 的「往上 6 層」）；
//   - driver_log 的節流（每秒 20 行、突發 20、suppressed 標註）；
//   - loopback（--no-loopback 可略過）：本行程扮假 server（非 SYSTEM，所以以 verify_server_override 取代
//     server 驗證），走真的 ipc_client：HELLO／WELCOME（idle）→ tracking／STATE／haptic／log 往返 →
//     BYE(RECONFIG) 立即重連 → 帶 config 的 generation：TEXTURES（真的 D3D11 共享 ring／fence）→ READY →
//     frame descriptor 與跨 device 的 fence 往返 → pipe 斷線 → REJECT(BAD_MESSAGE) 退避重連 →
//     REJECT(ABI_MISMATCH) 進 DEAD（不再重連）→ stop() ≤ 500 ms。
// V4 起加入 fov_to_rect、virtual_vsync、pose_history（檔案存在時 Build-SteamVRDriver.ps1 會一起連）。
//
// 行格式：`unit-fail case=<名稱> <說明>`（每一個失敗）、`unit-group name=<群組> pass=<n> fail=<n>`、
// `unit-skip case=<名稱> reason=<…>`（環境不支援、不計分）、最後一行 `unit pass=<n> fail=<n>`。
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "driver_log.h"
#include "driver_version.h"
#include "ipc_client.h"
#include "ipc_proto.h"
#include "pose_history.h"
#include "pose_policy.h"
#include "probe_common.h"
#include "probe_d3d.h"
#include "virtual_vsync.h"
#include "vr_math.h"
#include "vr_mv_codec.h"
#include "vr_mv_pair.h"
#include "vr_scene_pattern.h"

namespace probe {
  namespace {
    namespace proto = vrdrv::proto;

    // ── 計分 ────────────────────────────────────────────────────────────
    struct tally_t {
      int pass = 0;
      int fail = 0;
      int group_pass = 0;
      int group_fail = 0;
      const char *group = "";

      void begin(const char *name) {
        group = name;
        group_pass = 0;
        group_fail = 0;
      }

      void end() {
        line("unit-group name=%s pass=%d fail=%d", group, group_pass, group_fail);
      }

      bool verbose = false;  ///< --unit-verbose：通過的項目也印出數值（看離門檻多遠）

      void check(bool ok, const char *name, const char *fmt = nullptr, ...) {
        char detail[200] = "";
        if (fmt && (!ok || verbose)) {
          va_list ap;
          va_start(ap, fmt);
          vsnprintf(detail, sizeof(detail), fmt, ap);
          va_end(ap);
        }
        if (ok) {
          ++pass;
          ++group_pass;
          if (verbose && fmt) {
            line("unit-ok case=%s.%s %s", group, name, detail);
          }
          return;
        }
        ++fail;
        ++group_fail;
        line("unit-fail case=%s.%s %s", group, name, detail);
      }
    };

    int64_t own_qpf() {
      return qpf();
    }

    // 等待條件成立（每 5 ms 檢查一次）；回傳是否在時限內成立
    template<class F>
    bool wait_for(DWORD timeout_ms, F &&cond) {
      const int64_t end = qpc() + (int64_t) timeout_ms * qpf() / 1000;
      for (;;) {
        if (cond()) {
          return true;
        }
        if (qpc() >= end) {
          return false;
        }
        Sleep(5);
      }
    }

    // ── ipc_proto：pipe 訊息 ───────────────────────────────────────────────
    void test_messages(tally_t &t) {
      t.begin("msg");
      t.check(proto::expected_size(VRIPC_MSG_HELLO) == sizeof(vripc_hello_t), "size-hello");
      t.check(proto::expected_size(VRIPC_MSG_WELCOME) == sizeof(vripc_welcome_t), "size-welcome");
      t.check(proto::expected_size(VRIPC_MSG_REJECT) == sizeof(vripc_reject_t), "size-reject");
      t.check(proto::expected_size(VRIPC_MSG_TEXTURES) == sizeof(vripc_textures_t), "size-textures");
      t.check(proto::expected_size(VRIPC_MSG_READY) == sizeof(vripc_ready_t), "size-ready");
      t.check(proto::expected_size(VRIPC_MSG_STATE) == sizeof(vripc_state_t), "size-state");
      t.check(proto::expected_size(VRIPC_MSG_BYE) == sizeof(vripc_bye_t), "size-bye");
      t.check(proto::expected_size(0) == 0 && proto::expected_size(99) == 0, "size-unknown");
      t.check(proto::k_min_msg == 24 && sizeof(vripc_reject_t) == 24, "frozen-24");

      alignas(8) uint8_t buf[VRIPC_PIPE_MAX_MSG + 8] = {};
      vripc_msg_hdr_t h = proto::make_header(VRIPC_MSG_BYE, (uint32_t) sizeof(vripc_bye_t), 7);
      std::memcpy(buf, &h, sizeof(h));
      vripc_msg_hdr_t out {};
      const char *why = "";
      t.check(proto::check_header(buf, (uint32_t) sizeof(vripc_bye_t), out, why) && out.seq == 7 && out.type == VRIPC_MSG_BYE, "header-ok", "why=%s", why);
      t.check(!proto::check_header(buf, 23, out, why) && std::strcmp(why, "short") == 0, "header-short", "why=%s", why);
      t.check(!proto::check_header(buf, VRIPC_PIPE_MAX_MSG + 1, out, why) && std::strcmp(why, "oversize") == 0, "header-oversize", "why=%s", why);
      t.check(!proto::check_header(buf, 32, out, why) && std::strcmp(why, "size-mismatch") == 0, "header-size-mismatch", "why=%s", why);
      vripc_msg_hdr_t bad = h;
      bad.magic ^= 1;
      std::memcpy(buf, &bad, sizeof(bad));
      t.check(!proto::check_header(buf, (uint32_t) sizeof(vripc_bye_t), out, why) && std::strcmp(why, "magic") == 0, "header-magic", "why=%s", why);
      bad = h;
      bad.flags = 1;
      std::memcpy(buf, &bad, sizeof(bad));
      t.check(!proto::check_header(buf, (uint32_t) sizeof(vripc_bye_t), out, why) && std::strcmp(why, "flags") == 0, "header-flags", "why=%s", why);

      vripc_msg_hdr_t r = proto::make_header(VRIPC_MSG_REJECT, 40, 1);
      t.check(proto::check_body_size(r, 40, why), "body-reject-longer-ok", "why=%s", why);  // 凍結：任何 ABI 版本的 REJECT 都能解析
      vripc_msg_hdr_t st = proto::make_header(VRIPC_MSG_STATE, 40, 1);
      t.check(!proto::check_body_size(st, 40, why) && std::strcmp(why, "body-size") == 0, "body-state-size", "why=%s", why);
      vripc_msg_hdr_t unk = proto::make_header(42, 24, 1);
      t.check(!proto::check_body_size(unk, 24, why) && std::strcmp(why, "unknown-type") == 0, "body-unknown", "why=%s", why);

      t.check(!proto::plausible_handle_value(0), "handle-zero");
      t.check(!proto::plausible_handle_value(6), "handle-unaligned");
      t.check(proto::plausible_handle_value(4) && proto::plausible_handle_value(0xFFFFFFFCull), "handle-ok");
      t.check(!proto::plausible_handle_value(0x100000000ull), "handle-64bit");
      t.check(!proto::plausible_handle_value(0xFFFFFFFFFFFFFFFCull), "handle-pseudo");
      t.check(proto::pack_sdk(2, 15, 6) == 0x020F0006u, "pack-sdk");
      t.check(VIPLE_DRIVER_VERSION_PACKED == ((VIPLE_DRIVER_VERSION_MAJOR << 24) | (VIPLE_DRIVER_VERSION_MINOR << 16) | VIPLE_DRIVER_VERSION_PATCH), "driver-version-packed");
      t.end();
    }

    // ── ipc_proto：WELCOME／shm header／config／TEXTURES ─────────────────────
    vripc_welcome_t good_welcome() {
      vripc_welcome_t w {};
      w.hdr = proto::make_header(VRIPC_MSG_WELCOME, (uint32_t) sizeof(w), 1);
      w.abi_version = VRIPC_ABI_VERSION;
      w.generation = 3;
      w.shm_size = VRIPC_SHM_SECTION_SIZE;
      w.qpc_frequency = own_qpf();
      w.dup_shm_handle = 0x100;
      w.dup_evt_trk = 0x104;
      w.dup_evt_frm = 0x108;
      return w;
    }

    vripc_session_config_t good_config() {
      vripc_session_config_t c {};
      c.eye_width = 1024;
      c.eye_height = 1024;
      c.packed_width = 2048;
      c.packed_height = 1024;
      c.dxgi_format = VRIPC_DXGI_FORMAT_B8G8R8A8_UNORM;
      c.refresh_mhz = 90000;
      c.period_ns = 11111111;
      for (auto &eye : c.fov_tan) {
        eye[0] = -1.0f;
        eye[1] = 1.0f;
        eye[2] = 1.0f;
        eye[3] = -1.0f;
      }
      c.eye_to_head[0][6] = 1.0f;
      c.eye_to_head[1][6] = 1.0f;
      c.ipd_m = 0.063f;
      c.universe_id = 0x5649504Cu;
      c.adapter_luid_low = 0x1234;
      c.adapter_luid_high = 0;
      c.space_epoch = 1;
      c.layout_epoch = 1;
      return c;
    }

    void test_validation(tally_t &t) {
      t.begin("validate");
      const char *why = "";
      const vripc_welcome_t w0 = good_welcome();
      t.check(proto::check_welcome(w0, own_qpf(), why), "welcome-ok", "why=%s", why);
      {
        auto w = w0;
        w.abi_version = VRIPC_ABI_VERSION + 1;  // 原本寫死 2，ABI 升到 2 之後這一項就變成在測「版號相同應該失敗」
        t.check(!proto::check_welcome(w, own_qpf(), why), "welcome-abi");
      }
      {
        auto w = w0;
        w.generation = 0;
        t.check(!proto::check_welcome(w, own_qpf(), why), "welcome-generation");
      }
      {
        auto w = w0;
        w.shm_size = sizeof(vripc_shm_t) - 1;
        t.check(!proto::check_welcome(w, own_qpf(), why), "welcome-shm-small");
        w.shm_size = 1ull << 30;
        t.check(!proto::check_welcome(w, own_qpf(), why), "welcome-shm-huge");
      }
      {
        auto w = w0;
        w.qpc_frequency = own_qpf() + 1;
        t.check(!proto::check_welcome(w, own_qpf(), why), "welcome-qpf");
      }
      {
        auto w = w0;
        w.dup_evt_frm = 0;
        t.check(!proto::check_welcome(w, own_qpf(), why), "welcome-handle");
      }

      vripc_shm_header_t h0 {};
      h0.magic = VRIPC_MAGIC_SHM;
      h0.abi_version = VRIPC_ABI_VERSION;
      h0.shm_size = VRIPC_SHM_SECTION_SIZE;
      h0.struct_size = (uint32_t) sizeof(vripc_shm_t);
      h0.generation = w0.generation;
      h0.qpc_frequency = w0.qpc_frequency;
      t.check(proto::check_shm_header(h0, w0, why), "shm-ok", "why=%s", why);
      {
        auto h = h0;
        h.magic = 0;
        t.check(!proto::check_shm_header(h, w0, why), "shm-magic");
      }
      {
        auto h = h0;
        h.struct_size -= 64;
        t.check(!proto::check_shm_header(h, w0, why), "shm-struct-size");
      }
      {
        auto h = h0;
        h.generation = 99;
        t.check(!proto::check_shm_header(h, w0, why), "shm-generation");
      }
      {
        auto h = h0;
        h.armed = 2;
        t.check(!proto::check_shm_header(h, w0, why), "shm-armed");
      }
      {
        auto h = h0;
        h.server_state = VRIPC_SRV_CLOSING + 1;
        t.check(!proto::check_shm_header(h, w0, why), "shm-state");
      }

      const vripc_session_config_t c0 = good_config();
      t.check(proto::check_config(c0, why), "config-ok", "why=%s", why);
      {
        vripc_session_config_t idle {};
        idle.eye_width = 0;
        idle.fov_tan[0][0] = std::numeric_limits<float>::quiet_NaN();  // idle 時其他欄位不看
        t.check(proto::check_config(idle, why), "config-idle", "why=%s", why);
      }
      {
        auto c = c0;
        c.eye_width = 9000;
        t.check(!proto::check_config(c, why), "config-eye");
      }
      {
        auto c = c0;
        c.packed_width = 100;
        t.check(!proto::check_config(c, why), "config-packed");
      }
      {
        auto c = c0;
        c.dxgi_format = 28;
        t.check(!proto::check_config(c, why), "config-format");
      }
      {
        auto c = c0;
        c.refresh_mhz = 0;
        t.check(!proto::check_config(c, why), "config-refresh");
      }
      {
        auto c = c0;
        c.universe_id = 0x100000000ull;
        t.check(!proto::check_config(c, why), "config-universe");
      }
      {
        auto c = c0;
        c.adapter_luid_low = 0;
        t.check(!proto::check_config(c, why), "config-luid");
      }
      {
        auto c = c0;
        c.fov_tan[1][2] = std::numeric_limits<float>::infinity();
        t.check(!proto::check_config(c, why), "config-fov");
      }
      {
        auto c = c0;
        c.eye_to_head[0][3] = std::numeric_limits<float>::quiet_NaN();
        t.check(!proto::check_config(c, why), "config-eye-to-head");
      }
      {
        auto c = c0;
        c.ipd_m = std::numeric_limits<float>::quiet_NaN();
        t.check(!proto::check_config(c, why), "config-ipd");
      }

      vripc_textures_t x0 {};
      x0.generation = 3;
      x0.adapter_luid_low = c0.adapter_luid_low;
      x0.adapter_luid_high = c0.adapter_luid_high;
      x0.width = c0.packed_width;
      x0.height = c0.packed_height;
      x0.dxgi_format = VRIPC_DXGI_FORMAT_B8G8R8A8_UNORM;
      x0.tex_count = VRIPC_TEX_COUNT;
      x0.tex_handle[0] = 0x200;
      x0.tex_handle[1] = 0x204;
      x0.tex_handle[2] = 0x208;
      x0.shared_fence_handle = 0x20C;
      x0.consumed_fence_handle = 0x210;
      t.check(proto::check_textures(x0, 3, c0, why) == 0, "textures-ok", "why=%s", why);
      t.check(proto::check_textures(x0, 4, c0, why) == VRIPC_REJ_GENERATION, "textures-generation");
      {
        vripc_session_config_t idle {};
        t.check(proto::check_textures(x0, 3, idle, why) == VRIPC_REJ_BAD_MESSAGE, "textures-idle");
      }
      {
        auto x = x0;
        x.tex_count = 2;
        t.check(proto::check_textures(x, 3, c0, why) == VRIPC_REJ_TEXTURE_INVALID, "textures-count");
      }
      {
        auto x = x0;
        x.height = 1;
        t.check(proto::check_textures(x, 3, c0, why) == VRIPC_REJ_TEXTURE_INVALID, "textures-size");
      }
      {
        auto x = x0;
        x.adapter_luid_high = 1;
        t.check(proto::check_textures(x, 3, c0, why) == VRIPC_REJ_ADAPTER_MISMATCH, "textures-luid");
      }
      {
        auto x = x0;
        x.tex_handle[1] = 0;
        t.check(proto::check_textures(x, 3, c0, why) == VRIPC_REJ_TEXTURE_INVALID, "textures-handle");
      }
      {
        auto x = x0;
        x.consumed_fence_handle = 3;
        t.check(proto::check_textures(x, 3, c0, why) == VRIPC_REJ_TEXTURE_INVALID, "textures-fence-handle");
      }
      t.end();
    }

    // ── seqlock 與 SPSC ─────────────────────────────────────────────────
    void test_rings(tally_t &t) {
      t.begin("ring");
      auto shm = std::make_unique<vripc_shm_t>();
      std::memset(shm.get(), 0, sizeof(vripc_shm_t));
      auto &trk = shm->tracking;

      vripc_tracking_slot_t out {};
      uint64_t idx = 0;
      t.check(proto::seq_read_latest<vripc_tracking_slot_t, VRIPC_TRK_SLOTS>(&trk.write_index, trk.slot, out, idx) == proto::seq_read_e::empty, "trk-empty");

      uint64_t local = 0;
      for (uint32_t i = 1; i <= 11; ++i) {
        vripc_tracking_slot_t s {};
        s.seq = 0xDEADBEEF;  // 寫入端忽略呼叫端給的 seq
        s.sample_id = i;
        s.pose[0].rot[3] = 1.0f;
        proto::seq_write<vripc_tracking_slot_t, VRIPC_TRK_SLOTS>(&trk.write_index, trk.slot, local, s);
      }
      const auto r = proto::seq_read_latest<vripc_tracking_slot_t, VRIPC_TRK_SLOTS>(&trk.write_index, trk.slot, out, idx);
      t.check(r == proto::seq_read_e::ok && idx == 10 && out.sample_id == 11 && out.seq == 22 && local == 11, "trk-latest", "r=%d idx=%llu id=%u seq=%llu", (int) r, (unsigned long long) idx, out.sample_id, (unsigned long long) out.seq);

      // 寫入中（奇數 seq）→ 重試用完 → torn
      trk.slot[10 % VRIPC_TRK_SLOTS].seq = 2 * 10 + 1;
      t.check(proto::seq_read_latest<vripc_tracking_slot_t, VRIPC_TRK_SLOTS>(&trk.write_index, trk.slot, out, idx) == proto::seq_read_e::torn, "trk-torn-odd");
      // ABA：slot 的 seq 是舊一輪的偶數（idx 編進 seq）→ torn，不會把舊資料當新
      trk.slot[10 % VRIPC_TRK_SLOTS].seq = 2 * 2 + 2;
      t.check(proto::seq_read_latest<vripc_tracking_slot_t, VRIPC_TRK_SLOTS>(&trk.write_index, trk.slot, out, idx) == proto::seq_read_e::torn, "trk-torn-aba");

      // 單格 seqlock（pacing）
      vripc_pacing_t p {};
      t.check(!proto::seq_read_single(&shm->pacing, p), "pacing-zero");
      shm->pacing.seq = 3;
      t.check(!proto::seq_read_single(&shm->pacing, p), "pacing-odd");
      shm->pacing.seq = 4;
      shm->pacing.period_q32 = 12345;
      t.check(proto::seq_read_single(&shm->pacing, p) && p.period_q32 == 12345, "pacing-ok");

      // SPSC 生產者：滿、毀損、恢復
      auto &hr = shm->haptic;
      uint64_t head = 0;
      vripc_haptic_evt_t e {};
      e.device = 1;
      bool all_ok = true;
      for (uint32_t i = 0; i < VRIPC_HAPTIC_SLOTS; ++i) {
        all_ok = all_ok && proto::spsc_push<vripc_haptic_evt_t, VRIPC_HAPTIC_SLOTS>(&hr.head, &hr.tail, hr.evt, head, e) == proto::spsc_push_e::ok;
      }
      t.check(all_ok && hr.head == VRIPC_HAPTIC_SLOTS, "spsc-fill");
      t.check(proto::spsc_push<vripc_haptic_evt_t, VRIPC_HAPTIC_SLOTS>(&hr.head, &hr.tail, hr.evt, head, e) == proto::spsc_push_e::full && head == VRIPC_HAPTIC_SLOTS, "spsc-full");
      hr.tail = 5;
      t.check(proto::spsc_push<vripc_haptic_evt_t, VRIPC_HAPTIC_SLOTS>(&hr.head, &hr.tail, hr.evt, head, e) == proto::spsc_push_e::ok && hr.head == VRIPC_HAPTIC_SLOTS + 1, "spsc-after-consume");
      hr.tail = head + 10;  // 對方亂寫：tail 跑到 head 前面
      t.check(proto::spsc_push<vripc_haptic_evt_t, VRIPC_HAPTIC_SLOTS>(&hr.head, &hr.tail, hr.evt, head, e) == proto::spsc_push_e::corrupt && hr.head == VRIPC_HAPTIC_SLOTS + 1, "spsc-corrupt");

      // log 文字：控制字元與非 ASCII 換 '?'、截到 239、nullptr → 0
      char txt[VRIPC_LOG_TEXT];
      const uint32_t n1 = proto::make_log_text(txt, "ab\x01" "c\xE4[d");
      t.check(n1 == 7 && std::strcmp(txt, "ab?c?[d") == 0, "log-text-filter", "n=%u", n1);
      std::string longs(500, 'x');
      const uint32_t n2 = proto::make_log_text(txt, longs.c_str());
      t.check(n2 == VRIPC_LOG_TEXT - 1 && txt[VRIPC_LOG_TEXT - 1] == '\0', "log-text-truncate", "n=%u", n2);
      t.check(proto::make_log_text(txt, nullptr) == 0 && txt[0] == '\0', "log-text-null");

      // 重連退避（§B.6）：500 ms 起、×2、上限 5 s
      t.check(proto::backoff_ms(0) == 0 && proto::backoff_ms(1) == 500 && proto::backoff_ms(2) == 1000 && proto::backoff_ms(3) == 2000 && proto::backoff_ms(4) == 4000 && proto::backoff_ms(5) == 5000 && proto::backoff_ms(100) == 5000, "backoff");
      t.end();
    }

    // ── 路徑推導與 log 節流 ───────────────────────────────────────────────
    void test_paths(tally_t &t) {
      t.begin("path");
      using ic = vrdrv::ipc_client_t;
      const std::wstring dll = L"C:\\Program Files\\VipleStream-Server\\config\\steamvr\\1.5.276-habcdef12\\viplestream\\bin\\win64\\driver_viplestream.dll";
      t.check(ic::expected_server_image_from_dll_path(dll) == L"C:\\Program Files\\VipleStream-Server\\viplestream-server.exe", "six-levels");
      const std::wstring fwd = L"D:/x/config/steamvr/v/viplestream/bin/win64/d.dll";
      t.check(ic::expected_server_image_from_dll_path(fwd) == L"D:\\x\\viplestream-server.exe", "forward-slashes");
      t.check(ic::expected_server_image_from_dll_path(L"C:\\a\\b.dll").empty(), "too-short");
      t.end();
    }

    struct log_capture_t {
      std::mutex m;
      int lines = 0;
      std::string last;
    };

    log_capture_t g_cap;

    void capture_sink(void *, int, const char *text) {
      std::lock_guard<std::mutex> lk(g_cap.m);
      ++g_cap.lines;
      g_cap.last = text;
    }

    void test_log_throttle(tally_t &t) {
      t.begin("log");
      vrdrv::log::set_sink(&capture_sink, nullptr);
      vrdrv::log::reset_throttle_for_test();
      for (int i = 0; i < 50; ++i) {
        VRDRV_LOG_INFO("burst %d", i);
      }
      int burst = 0;
      {
        std::lock_guard<std::mutex> lk(g_cap.m);
        burst = g_cap.lines;
      }
      const uint32_t supp = vrdrv::log::suppressed_pending();
      t.check(burst >= 20 && burst <= 22, "burst-20", "lines=%d", burst);
      t.check(supp >= 28 && (int) supp + burst == 50, "suppressed-count", "suppressed=%u lines=%d", supp, burst);
      Sleep(150);  // 補 ≥ 2 個 token
      VRDRV_LOG_WARN("after-pause");
      std::string last;
      {
        std::lock_guard<std::mutex> lk(g_cap.m);
        last = g_cap.last;
      }
      t.check(last.rfind("(suppressed ", 0) == 0 && last.find("after-pause") != std::string::npos, "suppressed-prefix", "last=%.80s", last.c_str());
      VRDRV_LOG_INFO("ctl\x01" "chars\n");
      {
        std::lock_guard<std::mutex> lk(g_cap.m);
        last = g_cap.last;
      }
      t.check(last == "ctl?chars?", "sanitize", "last=%.80s", last.c_str());
      vrdrv::log::set_sink(nullptr, nullptr);
      vrdrv::log::reset_throttle_for_test();
      t.end();
    }

    // ════════════════════════════════════════════════════════════════════
    // loopback：本行程的假 server ↔ 真的 ipc_client
    // ════════════════════════════════════════════════════════════════════
    class fake_pipe_t {
    public:
      ~fake_pipe_t() {
        if (pipe_ != INVALID_HANDLE_VALUE) {
          CancelIoEx(pipe_, nullptr);
          CloseHandle(pipe_);
        }
        if (evt_) {
          CloseHandle(evt_);
        }
      }

      bool create(const std::wstring &name) {
        evt_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        pipe_ = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
        return evt_ && pipe_ != INVALID_HANDLE_VALUE;
      }

      bool accept(DWORD timeout_ms) {
        out_seq_ = 0;
        OVERLAPPED ov {};
        ov.hEvent = evt_;
        ResetEvent(evt_);
        if (ConnectNamedPipe(pipe_, &ov)) {
          return true;
        }
        const DWORD e = GetLastError();
        if (e == ERROR_PIPE_CONNECTED) {
          return true;
        }
        if (e != ERROR_IO_PENDING) {
          return false;
        }
        if (WaitForSingleObject(evt_, timeout_ms) != WAIT_OBJECT_0) {
          CancelIoEx(pipe_, &ov);
          DWORD n = 0;
          GetOverlappedResult(pipe_, &ov, &n, TRUE);
          return false;
        }
        DWORD n = 0;
        return GetOverlappedResult(pipe_, &ov, &n, FALSE) != FALSE;
      }

      // >0：訊息長度；-1：逾時；-2：對方關閉；-3：其他錯誤
      int read(void *buf, DWORD cap, DWORD timeout_ms) {
        OVERLAPPED ov {};
        ov.hEvent = evt_;
        ResetEvent(evt_);
        DWORD n = 0;
        if (!ReadFile(pipe_, buf, cap, nullptr, &ov)) {
          const DWORD e = GetLastError();
          if (e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED) {
            return -2;
          }
          if (e != ERROR_IO_PENDING) {
            return -3;
          }
          if (WaitForSingleObject(evt_, timeout_ms) != WAIT_OBJECT_0) {
            CancelIoEx(pipe_, &ov);
            GetOverlappedResult(pipe_, &ov, &n, TRUE);
            return -1;
          }
        }
        if (!GetOverlappedResult(pipe_, &ov, &n, FALSE)) {
          const DWORD e = GetLastError();
          return (e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED) ? -2 : -3;
        }
        return (int) n;
      }

      bool write(void *msg, uint32_t size) {
        auto *h = static_cast<vripc_msg_hdr_t *>(msg);
        h->seq = ++out_seq_;
        OVERLAPPED ov {};
        ov.hEvent = evt_;
        ResetEvent(evt_);
        DWORD n = 0;
        if (!WriteFile(pipe_, msg, size, nullptr, &ov)) {
          if (GetLastError() != ERROR_IO_PENDING) {
            return false;
          }
          if (WaitForSingleObject(evt_, 1000) != WAIT_OBJECT_0) {
            CancelIoEx(pipe_, &ov);
            GetOverlappedResult(pipe_, &ov, &n, TRUE);
            return false;
          }
        }
        return GetOverlappedResult(pipe_, &ov, &n, FALSE) && n == size;
      }

      void disconnect() {
        DisconnectNamedPipe(pipe_);
      }

    private:
      HANDLE pipe_ = INVALID_HANDLE_VALUE;
      HANDLE evt_ = nullptr;
      uint32_t out_seq_ = 0;
    };

    // 假 server 的一個 generation（section、兩個 event；與 bridge 的 §B.4 S2 相同的建立參數）
    struct fake_gen_t {
      HANDLE section = nullptr;
      HANDLE evt_trk = nullptr;
      HANDLE evt_frm = nullptr;
      vripc_shm_t *shm = nullptr;

      fake_gen_t() = default;
      fake_gen_t(const fake_gen_t &) = delete;
      fake_gen_t &operator=(const fake_gen_t &) = delete;

      ~fake_gen_t() {
        if (shm) {
          UnmapViewOfFile(shm);
        }
        for (HANDLE h : {section, evt_trk, evt_frm}) {
          if (h) {
            CloseHandle(h);
          }
        }
      }

      bool create(uint64_t generation, const vripc_session_config_t *cfg) {
        section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, VRIPC_SHM_SECTION_SIZE, nullptr);
        evt_trk = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        evt_frm = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!section || !evt_trk || !evt_frm) {
          return false;
        }
        shm = static_cast<vripc_shm_t *>(MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, VRIPC_SHM_SECTION_SIZE));
        if (!shm) {
          return false;
        }
        std::memset(shm, 0, sizeof(vripc_shm_t));
        shm->header.magic = VRIPC_MAGIC_SHM;
        shm->header.abi_version = VRIPC_ABI_VERSION;
        shm->header.shm_size = VRIPC_SHM_SECTION_SIZE;
        shm->header.struct_size = (uint32_t) sizeof(vripc_shm_t);
        shm->header.generation = generation;
        shm->header.qpc_frequency = own_qpf();
        shm->header.server_pid = GetCurrentProcessId();
        shm->header.server_state = VRIPC_SRV_INIT;
        shm->header.server_heartbeat_qpc = qpc();
        if (cfg) {
          shm->config = *cfg;
        }
        shm->pacing.period_q32 = (uint64_t) ((double) own_qpf() / 90.0 * 4294967296.0);
        shm->pacing.slew_ppm_max = 200;
        shm->pacing.seq = 2;
        return true;
      }

      void heartbeat() {
        vripc_store_release_u64(reinterpret_cast<uint64_t *>(&shm->header.server_heartbeat_qpc), (uint64_t) qpc());
      }

      // WELCOME：本行程內的 DuplicateHandle（最小權限，與 bridge 相同）；handle 的所有權交給 client
      bool welcome(fake_pipe_t &p, uint64_t generation) {
        HANDLE dshm = nullptr;
        HANDLE dtrk = nullptr;
        HANDLE dfrm = nullptr;
        const HANDLE self = GetCurrentProcess();
        if (!DuplicateHandle(self, section, self, &dshm, FILE_MAP_READ | FILE_MAP_WRITE, FALSE, 0) || !DuplicateHandle(self, evt_trk, self, &dtrk, SYNCHRONIZE, FALSE, 0) || !DuplicateHandle(self, evt_frm, self, &dfrm, EVENT_MODIFY_STATE, FALSE, 0)) {
          return false;
        }
        vripc_welcome_t w {};
        w.hdr = proto::make_header(VRIPC_MSG_WELCOME, (uint32_t) sizeof(w), 0);
        w.abi_version = VRIPC_ABI_VERSION;
        w.server_pid = GetCurrentProcessId();
        w.generation = generation;
        w.shm_size = VRIPC_SHM_SECTION_SIZE;
        w.qpc_frequency = own_qpf();
        w.dup_shm_handle = (uint64_t) (uintptr_t) dshm;
        w.dup_evt_trk = (uint64_t) (uintptr_t) dtrk;
        w.dup_evt_frm = (uint64_t) (uintptr_t) dfrm;
        w.server_version_packed = VIPLE_DRIVER_VERSION_PACKED;
        return p.write(&w, (uint32_t) sizeof(w));
      }
    };

    struct loop_obs_t {
      std::atomic<int> mapped {0};
      std::atomic<uint64_t> mapped_gen {0};
      std::atomic<int> textures {0};
      std::atomic<int> teardowns {0};
      std::atomic<const char *> teardown_reason {""};
      std::atomic<uint32_t> server_state {0};
      std::atomic<int> rejects {0};
      std::atomic<uint32_t> last_reject {0};
      std::mutex ring_mtx;
      std::shared_ptr<d3d::device_t> dev;
      std::shared_ptr<d3d::opened_ring_t> ring;
    };

    bool read_hello(fake_pipe_t &p, vripc_hello_t &hello, DWORD timeout_ms) {
      alignas(8) uint8_t buf[VRIPC_PIPE_MAX_MSG] = {};
      const int n = p.read(buf, sizeof(buf), timeout_ms);
      if (n != (int) sizeof(vripc_hello_t)) {
        return false;
      }
      std::memcpy(&hello, buf, sizeof(hello));
      return hello.hdr.magic == VRIPC_MAGIC_PIPE && hello.hdr.type == VRIPC_MSG_HELLO && hello.hdr.size == sizeof(hello);
    }

    void test_loopback(tally_t &t) {
      t.begin("loop");
      wchar_t name[96];
      swprintf(name, 96, L"\\\\.\\pipe\\VipleStreamVR-unit-%lu", GetCurrentProcessId());
      fake_pipe_t fp;
      if (!fp.create(name)) {
        t.check(false, "create-pipe", "err=%lu", GetLastError());
        t.end();
        return;
      }

      auto obs = std::make_shared<loop_obs_t>();
      vrdrv::ipc_client_config_t cfg;
      cfg.pipe_name = name;
      cfg.driver_version_packed = VIPLE_DRIVER_VERSION_PACKED;
      cfg.openvr_sdk_packed = proto::pack_sdk(2, 15, 6);
      cfg.iface_directmode = "IVRDriverDirectModeComponent_009";
      cfg.peer_mode = true;
      // 假 server 不是 SYSTEM（owner ≠ S-1-5-18）：以 override 取代 §B.1 driver 端檢查（driver DLL 沒有這個欄位）
      cfg.hooks.verify_server_override = [](HANDLE, const char *&why) {
        why = "unit-loopback";
        return true;
      };

      vrdrv::ipc_callbacks_t cb;
      cb.on_mapped = [obs](const vrdrv::generation_ptr &gen, bool) {
        obs->mapped_gen.store(gen->id());
        obs->mapped.fetch_add(1);
      };
      cb.on_textures = [obs](const vrdrv::generation_ptr &, const vripc_textures_t &msg, const HANDLE tex[VRIPC_TEX_COUNT], HANDLE sf, HANDLE cf) {
        vrdrv::textures_result_t r;
        LUID luid {};
        luid.LowPart = msg.adapter_luid_low;
        luid.HighPart = msg.adapter_luid_high;
        auto dev = std::make_shared<d3d::device_t>();
        HRESULT hr = d3d::create_device(&luid, *dev);
        if (FAILED(hr)) {
          r.reject_detail = (uint32_t) hr;
          return r;
        }
        auto ring = std::make_shared<d3d::opened_ring_t>();
        const char *why = "";
        hr = d3d::open_ring(*dev, tex, sf, cf, msg.width, msg.height, *ring, why);
        if (FAILED(hr)) {
          r.reject_detail = (uint32_t) hr;
          return r;
        }
        {
          std::lock_guard<std::mutex> lk(obs->ring_mtx);
          obs->dev = dev;
          obs->ring = ring;
        }
        obs->textures.fetch_add(1);
        r.ok = true;
        r.reject_reason = 0;
        return r;
      };
      cb.on_teardown = [obs](uint64_t, const char *reason) {
        obs->teardown_reason.store(reason);
        obs->teardowns.fetch_add(1);
        std::lock_guard<std::mutex> lk(obs->ring_mtx);
        obs->ring.reset();
      };
      cb.on_server_state = [obs](uint32_t kind, uint32_t) {
        obs->server_state.store(kind);
      };
      cb.on_rejected = [obs](uint32_t reason, uint32_t) {
        obs->last_reject.store(reason);
        obs->rejects.fetch_add(1);
      };

      auto client = std::make_unique<vrdrv::ipc_client_t>();
      if (!client->start(cfg, cb)) {
        t.check(false, "client-start");
        t.end();
        return;
      }
      auto stop_client = [&]() -> bool {
        return client->stop(500);
      };

      // ── 1. 第一次連線：HELLO → WELCOME（idle generation）──
      bool ok = fp.accept(3000);
      t.check(ok, "accept-1");
      vripc_hello_t hello {};
      ok = ok && read_hello(fp, hello, 2000);
      t.check(ok && hello.hdr.seq == 1 && hello.abi_version == VRIPC_ABI_VERSION && (hello.driver_caps & VRIPC_DCAP_PEER) != 0 && hello.shm_struct_size == sizeof(vripc_shm_t) && hello.driver_version_packed == VIPLE_DRIVER_VERSION_PACKED && std::strcmp(hello.iface_directmode, "IVRDriverDirectModeComponent_009") == 0 && hello.driver_pid == GetCurrentProcessId(), "hello-1", "seq=%u abi=%u caps=0x%x", hello.hdr.seq, hello.abi_version, hello.driver_caps);

      auto g1 = std::make_unique<fake_gen_t>();
      ok = ok && g1->create(1, nullptr) && g1->welcome(fp, 1);
      t.check(ok, "welcome-1");
      const auto first = client->wait_first(2000);
      t.check(first == vrdrv::ipc_client_t::first_e::mapped, "wait-first", "first=%d", (int) first);
      const bool acked = ok && wait_for(1000, [&]() {
                           return vripc_load_acquire_u64(&g1->shm->driver.generation_ack) == 1 && g1->shm->driver.driver_state == VRIPC_DRV_MAPPED && g1->shm->driver.driver_heartbeat_qpc != 0;
                         });
      t.check(acked && obs->mapped.load() == 1 && obs->mapped_gen.load() == 1, "mapped-1", "mapped=%d", obs->mapped.load());

      // tracking：server 以同一個 seqlock 寫入端寫、client 從真的 shm 讀
      if (ok) {
        uint64_t local = 0;
        for (uint32_t id = 100; id < 110; ++id) {
          vripc_tracking_slot_t s {};
          s.sample_id = id;
          s.pose[0].rot[3] = 1.0f;
          proto::seq_write<vripc_tracking_slot_t, VRIPC_TRK_SLOTS>(&g1->shm->tracking.write_index, g1->shm->tracking.slot, local, s);
        }
        SetEvent(g1->evt_trk);
        auto gen = client->current();
        vripc_tracking_slot_t got {};
        uint64_t idx = 0;
        const bool rd = gen && WaitForSingleObject(gen->evt_trk(), 500) == WAIT_OBJECT_0 && gen->read_latest_tracking(got, idx);
        t.check(rd && got.sample_id == 109 && idx == 9, "tracking-roundtrip", "id=%u idx=%llu", got.sample_id, (unsigned long long) idx);

        // STATE ARM（S→D）
        vripc_state_t arm {};
        arm.hdr = proto::make_header(VRIPC_MSG_STATE, (uint32_t) sizeof(arm), 0);
        arm.generation = 1;
        arm.kind = VRIPC_ST_ARM;
        const bool armed = fp.write(&arm, (uint32_t) sizeof(arm)) && wait_for(1000, [&]() {
                             return obs->server_state.load() == VRIPC_ST_ARM;
                           });
        t.check(armed, "state-arm");

        // haptic／log ring（D→S）
        if (gen) {
          vripc_haptic_evt_t he {};
          he.device = 2;
          he.duration_us = 1234;
          const bool hp = gen->push_haptic(he);
          t.check(hp && vripc_load_acquire_u64(&g1->shm->haptic.head) == 1 && g1->shm->haptic.evt[0].device == 2 && g1->shm->haptic.evt[0].duration_us == 1234, "haptic-ring");
          const bool lp = gen->push_log(1, "hello\x01[x");
          t.check(lp && vripc_load_acquire_u64(&g1->shm->log.head) == 1 && g1->shm->log.line[0].len == 8 && g1->shm->log.line[0].level == 1 && std::strcmp(g1->shm->log.line[0].text, "hello?[x") == 0, "log-ring", "len=%u", g1->shm->log.line[0].len);
        }

        // STATE（D→S）：send_state 經 ipc 執行緒送出；driver 的 seq 接在 HELLO 之後
        client->send_state(VRIPC_ST_HMD_STANDBY, 5);
        alignas(8) uint8_t buf[VRIPC_PIPE_MAX_MSG] = {};
        const int n = fp.read(buf, sizeof(buf), 1000);
        vripc_state_t sm {};
        if (n == (int) sizeof(sm)) {
          std::memcpy(&sm, buf, sizeof(sm));
        }
        t.check(n == (int) sizeof(sm) && sm.hdr.type == VRIPC_MSG_STATE && sm.hdr.seq == 2 && sm.generation == 1 && sm.kind == VRIPC_ST_HMD_STANDBY && sm.arg == 5, "state-d2s", "n=%d seq=%u kind=0x%x", n, sm.hdr.seq, sm.kind);

        // ── 2. BYE(RECONFIG) → client 立刻重連（不退避）──
        g1->heartbeat();
        vripc_bye_t bye {};
        bye.hdr = proto::make_header(VRIPC_MSG_BYE, (uint32_t) sizeof(bye), 0);
        bye.reason = VRIPC_BYE_RECONFIG;
        const int64_t t_bye = qpc();
        const bool sent = fp.write(&bye, (uint32_t) sizeof(bye));
        const int closed = fp.read(buf, sizeof(buf), 1000);  // client 收到 BYE 就關 pipe
        fp.disconnect();
        const bool td = wait_for(1000, [&]() {
          return obs->teardowns.load() == 1;
        });
        t.check(sent && closed == -2 && td && std::strcmp(obs->teardown_reason.load(), "bye-reconfig") == 0 && !client->current(), "bye-reconfig", "closed=%d teardowns=%d reason=%s", closed, obs->teardowns.load(), obs->teardown_reason.load());
        const bool acc2 = fp.accept(2000);
        const double reconnect_ms = qpc_to_ms(qpc() - t_bye);
        t.check(acc2 && reconnect_ms < 1200.0, "reconnect-immediate", "ms=%.1f", reconnect_ms);
        g1.reset();  // 舊 generation：server 端放掉自己的參照（client 端的 view 由 generation_t 管）

        // ── 3. 帶 config 的 generation：TEXTURES（真的 D3D11 共享 ring／fence）→ READY ──
        ok = acc2 && read_hello(fp, hello, 2000) && hello.hdr.seq == 1;
        t.check(ok, "hello-2");
        d3d::device_t sdev;
        const HRESULT dhr = ok ? d3d::create_device(nullptr, sdev) : E_FAIL;
        auto g2 = std::make_unique<fake_gen_t>();
        if (ok && FAILED(dhr)) {
          line("unit-skip case=loop.textures reason=no-d3d-device hr=0x%08x", (unsigned) dhr);
          ok = g2->create(2, nullptr) && g2->welcome(fp, 2);
          t.check(ok, "welcome-2-idle");
        } else if (ok) {
          vripc_session_config_t c = good_config();
          c.eye_width = 64;
          c.eye_height = 64;
          c.packed_width = 128;
          c.packed_height = 64;
          c.adapter_luid_low = sdev.luid.LowPart;
          c.adapter_luid_high = sdev.luid.HighPart;
          d3d::server_ring_t sring;
          HRESULT rhr = d3d::create_server_ring(sdev, c.packed_width, c.packed_height, sring);
          ok = SUCCEEDED(rhr) && g2->create(2, &c) && g2->welcome(fp, 2);
          t.check(ok, "welcome-2", "hr=0x%08x", (unsigned) rhr);
          const bool m2 = ok && wait_for(1000, [&]() {
                            return obs->mapped.load() == 2 && obs->mapped_gen.load() == 2;
                          });
          t.check(m2, "mapped-2");

          vripc_textures_t tx {};
          tx.hdr = proto::make_header(VRIPC_MSG_TEXTURES, (uint32_t) sizeof(tx), 0);
          tx.generation = 2;
          tx.adapter_luid_low = c.adapter_luid_low;
          tx.adapter_luid_high = c.adapter_luid_high;
          tx.width = c.packed_width;
          tx.height = c.packed_height;
          tx.dxgi_format = VRIPC_DXGI_FORMAT_B8G8R8A8_UNORM;
          tx.tex_count = VRIPC_TEX_COUNT;
          const HANDLE self = GetCurrentProcess();
          bool dup_ok = ok;
          HANDLE srcs[VRIPC_TEX_COUNT + 2] = {sring.tex_nt[0], sring.tex_nt[1], sring.tex_nt[2], sring.shared_fence_nt, sring.consumed_fence_nt};
          uint64_t vals[VRIPC_TEX_COUNT + 2] = {};
          for (int i = 0; i < (int) (VRIPC_TEX_COUNT + 2) && dup_ok; ++i) {
            HANDLE d = nullptr;
            dup_ok = DuplicateHandle(self, srcs[i], self, &d, 0, FALSE, DUPLICATE_SAME_ACCESS) != FALSE;
            vals[i] = (uint64_t) (uintptr_t) d;
          }
          for (uint32_t i = 0; i < VRIPC_TEX_COUNT; ++i) {
            tx.tex_handle[i] = vals[i];
          }
          tx.shared_fence_handle = vals[VRIPC_TEX_COUNT];
          tx.consumed_fence_handle = vals[VRIPC_TEX_COUNT + 1];
          const bool tsent = dup_ok && fp.write(&tx, (uint32_t) sizeof(tx));
          const int rn = tsent ? fp.read(buf, sizeof(buf), 5000) : -3;
          vripc_ready_t rdy {};
          if (rn == (int) sizeof(rdy)) {
            std::memcpy(&rdy, buf, sizeof(rdy));
          }
          const bool ready = rn == (int) sizeof(rdy) && rdy.hdr.type == VRIPC_MSG_READY && rdy.generation == 2 && obs->textures.load() == 1;
          t.check(ready, "textures-ready", "n=%d type=%u gen=%llu textures=%d", rn, (unsigned) rdy.hdr.type, (unsigned long long) rdy.generation, obs->textures.load());
          const bool drv_ready = ready && wait_for(1000, [&]() {
                                   return g2->shm->driver.driver_state == VRIPC_DRV_READY;
                                 });
          t.check(drv_ready, "driver-state-ready");

          // frame descriptor（driver 寫）＋跨 device 的 fence 往返（client Signal → server 觀察；server Signal → client 觀察）
          std::shared_ptr<d3d::device_t> cdev;
          std::shared_ptr<d3d::opened_ring_t> cring;
          {
            std::lock_guard<std::mutex> lk(obs->ring_mtx);
            cdev = obs->dev;
            cring = obs->ring;
          }
          auto gen2 = client->current();
          if (ready && cdev && cring && gen2) {
            const float color[4] = {1.0f, 0.0f, 0.0f, 1.0f};
            cdev->ctx->ClearRenderTargetView(cring->rtv[1].Get(), color);
            cdev->ctx4->Signal(cring->shared_fence.Get(), 1);
            cdev->ctx->Flush();
            vripc_frame_desc_t d {};
            d.frame_id = 1;
            d.generation = 2;
            d.fence_value = 1;
            d.tex_idx = 1;
            d.flags = VRIPC_FRM_POSE_VALID;
            d.render_rot[3] = 1.0f;
            d.layer_count = 1;
            d.present_qpc = qpc();
            d.submit_qpc = d.present_qpc;
            d.t_target_qpc = d.present_qpc;
            d.space_epoch = c.space_epoch;
            d.layout_epoch = c.layout_epoch;
            gen2->publish_frame(d);
            const bool evt = WaitForSingleObject(g2->evt_frm, 500) == WAIT_OBJECT_0;
            vripc_frame_desc_t fgot {};
            uint64_t fidx = 0;
            const auto fr = proto::seq_read_latest<vripc_frame_desc_t, VRIPC_FRM_SLOTS>(&g2->shm->frames.write_index, g2->shm->frames.slot, fgot, fidx);
            t.check(evt && fr == proto::seq_read_e::ok && fgot.frame_id == 1 && fgot.fence_value == 1 && fgot.tex_idx == 1 && fidx == 0, "frame-desc", "evt=%d r=%d frame=%llu", evt ? 1 : 0, (int) fr, (unsigned long long) fgot.frame_id);
            const bool sf_seen = wait_for(1000, [&]() {
              return sring.shared_fence->GetCompletedValue() >= 1;
            });
            t.check(sf_seen, "shared-fence-cross-device", "value=%llu", (unsigned long long) sring.shared_fence->GetCompletedValue());
            sdev.ctx4->Signal(sring.consumed_fence.Get(), 1);
            sdev.ctx->Flush();
            const bool cf_seen = wait_for(1000, [&]() {
              return cring->consumed_fence->GetCompletedValue() >= 1;
            });
            t.check(cf_seen, "consumed-fence-cross-device", "value=%llu", (unsigned long long) cring->consumed_fence->GetCompletedValue());
          } else {
            t.check(false, "frame-desc", "no ring/generation after READY");
          }
          cdev.reset();
          cring.reset();
          gen2.reset();
        }

        // ── 4. server 直接斷線（沒有 BYE）→ client TEARDOWN、退避後重連 ──
        const int before = obs->teardowns.load();
        fp.disconnect();
        const bool td2 = wait_for(1000, [&]() {
          return obs->teardowns.load() == before + 1;
        });
        t.check(td2 && std::strcmp(obs->teardown_reason.load(), "pipe-broken") == 0 && !client->current(), "pipe-broken", "reason=%s", obs->teardown_reason.load());
        g2.reset();

        // ── 5. REJECT(BAD_MESSAGE) → on_rejected、退避後再連 ──
        ok = fp.accept(4000) && read_hello(fp, hello, 2000);
        t.check(ok, "accept-3");
        vripc_reject_t rj {};
        rj.hdr = proto::make_header(VRIPC_MSG_REJECT, (uint32_t) sizeof(rj), 0);
        rj.reason = VRIPC_REJ_BAD_MESSAGE;
        ok = ok && fp.write(&rj, (uint32_t) sizeof(rj));
        const bool rj1 = ok && wait_for(1000, [&]() {
                           return obs->rejects.load() == 1 && obs->last_reject.load() == VRIPC_REJ_BAD_MESSAGE;
                         });
        const int c3 = fp.read(buf, sizeof(buf), 1000);
        fp.disconnect();
        t.check(rj1 && c3 == -2 && !client->stats().dead, "reject-bad-message", "rejects=%d closed=%d", obs->rejects.load(), c3);

        // ── 6. REJECT(ABI_MISMATCH) → DEAD（本行程不再重連）──
        ok = fp.accept(6000) && read_hello(fp, hello, 2000);
        t.check(ok, "accept-4");
        rj.reason = VRIPC_REJ_ABI_MISMATCH;
        rj.detail = VRIPC_ABI_VERSION + 1;
        ok = ok && fp.write(&rj, (uint32_t) sizeof(rj));
        const bool dead = ok && wait_for(1000, [&]() {
                            return client->stats().dead;
                          });
        const int c4 = fp.read(buf, sizeof(buf), 1000);
        fp.disconnect();
        t.check(dead && c4 == -2, "reject-abi-dead", "dead=%d closed=%d", client->stats().dead ? 1 : 0, c4);
        t.check(!fp.accept(1500), "dead-no-reconnect");
      }

      // ── 7. stop()：BYE(DRIVER_CLEANUP)／join ≤ 500 ms ──
      const int64_t t_stop = qpc();
      const bool stopped = stop_client();
      const double stop_ms = qpc_to_ms(qpc() - t_stop);
      t.check(stopped && stop_ms < 600.0, "stop", "ms=%.1f", stop_ms);
      if (!stopped) {
        (void) client.release();  // ipc 執行緒還在用：不可釋放（行程結束時一起收）
      }
      t.end();
    }

  }  // namespace

  // ── V4：fov_to_rect、virtual_vsync（pacing 是不可信輸入）、scene 圖案 ─────────────
  namespace {
    void test_v4(tally_t &t) {
      t.begin("v4");
      {
        // §E.2 drv-M-1：{-1.0, 1.1, 0.9, -1.3} → top=-1.3, bottom=0.9
        const float fov[4] = {-1.0f, 1.1f, 0.9f, -1.3f};
        const auto r = vrdrv::math::fov_to_rect(fov);
        t.check(r.left == -1.0f && r.right == 1.1f && r.top == -1.3f && r.bottom == 0.9f, "fov-to-rect", "l=%.2f r=%.2f t=%.2f b=%.2f", r.left, r.right, r.top, r.bottom);
      }
      {
        // 矩陣 ↔ 四元數往返（yaw 30°）
        const double h = 15.0 * 3.14159265358979 / 180.0;
        const vrdrv::math::quat_t q {0.0, std::sin(h), 0.0, std::cos(h)};
        float m[3][4];
        vrdrv::math::pose_to_matrix34(q, vrdrv::math::vec3_t {0.1, 1.6, -0.2}, m);
        vrdrv::math::quat_t q2;
        vrdrv::math::vec3_t p2;
        vrdrv::math::matrix34_to_pose(m, q2, p2);
        const double ang = vrdrv::math::angle_deg(q, q2);
        t.check(ang < 1e-3 && std::fabs(p2.y - 1.6) < 1e-5, "matrix-quat-roundtrip", "ang=%.6f", ang);
      }
      const int64_t f = qpf();
      const uint32_t mhz = 90000;
      const double p_nom = (double) f / 90.0;
      auto pacing = [&](double period_ticks, uint32_t slew, uint32_t mode) {
        vripc_pacing_t p {};
        p.period_q32 = (uint64_t) (period_ticks * 4294967296.0);
        p.slew_ppm_max = slew;
        p.mode = mode;
        return p;
      };
      struct case_t {
        const char *name;
        vripc_pacing_t p;
        bool expect_reject;
      };
      vripc_pacing_t anchor_far = pacing(p_nom, 200, 0);
      anchor_far.pacing_flags = VRIPC_PF_HAS_ANCHOR | VRIPC_PF_ALLOW_SNAP;
      anchor_far.anchor_qpc = qpc() + 3600 * f;  // 1 小時後：忽略
      const case_t cases[] = {
        {"vsync-zero", pacing(0.0, 200, 0), true},
        {"vsync-max", [] {
           vripc_pacing_t p {};
           p.period_q32 = UINT64_MAX;
           return p;
         }(),
         true},
        {"vsync-1.5x", pacing(p_nom * 1.5, 200, 0), true},
        {"vsync-0.5x", pacing(p_nom * 0.5, 200, 0), true},
        {"vsync-slew-huge", pacing(p_nom, 1000000, 0), true},
        {"vsync-mode-no-dev", pacing(p_nom, 200, 2), true},
        {"vsync-nominal", pacing(p_nom, 200, 0), false},
        {"vsync-anchor-far", anchor_far, false},
      };
      for (const auto &c : cases) {
        vrdrv::virtual_vsync_t v;
        const int64_t t0 = qpc();
        v.activate(f, mhz, t0);
        const auto r = v.step(&c.p, false, t0 + (int64_t) (p_nom * 1.5), 0);
        const double period_ms = (double) r.period_ns / 1e6;
        const bool period_ok = std::fabs(period_ms - 1000.0 / 90.0) < 0.05;
        t.check(r.pacing_rejected == c.expect_reject && period_ok && r.missed == 1 && !r.snapped, c.name, "rejected=%d periodMs=%.4f missed=%u", r.pacing_rejected ? 1 : 0, period_ms, r.missed);
      }
      {
        // §VR-LATCH-V3 定時 burst：週期 ±4000 ppm（server 平常只給 ±200）連走 120 格。driver 本體沒有改，這裡只是補上
        // 從沒測過的大數值：不被拒絕、每格都是正常的一格（missed == 1）、格點的總位移＝120 × 標稱週期 × (1 ± 0.004)。
        // 每一格都在上一個虛擬 vsync 過後四分之一個週期呼叫，和 PostPresent 睡到格點再返回的節奏一樣。
        struct burst_case_t {
          const char *name;
          double scale;
        };
        const burst_case_t bursts[] = {
          {"vsync-burst+4000ppm", 1.004},
          {"vsync-burst-4000ppm", 0.996},
        };
        for (const auto &b : bursts) {
          vrdrv::virtual_vsync_t v;
          const int64_t t0 = qpc();
          v.activate(f, mhz, t0);
          const vripc_pacing_t p = pacing(p_nom * b.scale, 200, 0);
          auto r = v.step(&p, false, t0 + (int64_t) (p_nom * 1.5), 0);  // 第一格當起點
          bool rejected = r.pacing_rejected;
          bool cadence_ok = r.missed == 1 && !r.snapped;
          const int64_t start = r.vsync_qpc;
          for (int i = 0; i < 120; ++i) {
            r = v.step(&p, false, r.vsync_qpc + (int64_t) (p_nom * 0.25), 0);
            rejected = rejected || r.pacing_rejected;
            cadence_ok = cadence_ok && r.missed == 1 && !r.snapped;
          }
          const double moved = (double) (r.vsync_qpc - start);
          const double expect = 120.0 * p_nom * b.scale;
          const double period_ms = (double) r.period_ns / 1e6;
          const bool period_ok = std::fabs(period_ms - 1000.0 / 90.0 * b.scale) < 0.001;
          t.check(!rejected && cadence_ok && std::fabs(moved - expect) <= 2.0 && period_ok, b.name, "rejected=%d cadenceOk=%d moved=%.1f expect=%.1f periodMs=%.4f", rejected ? 1 : 0,
                  cadence_ok ? 1 : 0, moved, expect, period_ms);
        }
      }
      {
        // 停頓 10 s 後：一次算出 missed、立即返回（不逐週期相加）、下一個 vsync 在 now 之後
        vrdrv::virtual_vsync_t v;
        const int64_t t0 = qpc();
        v.activate(f, mhz, t0);
        const int64_t later = t0 + 10 * f;
        const int64_t c0 = qpc();
        const auto r = v.step(nullptr, false, later, 0);
        const double ms = qpc_to_ms(qpc() - c0);
        t.check(r.missed >= 899 && r.missed <= 901 && r.vsync_qpc > later && ms < 1.0, "vsync-stall-10s", "missed=%u ms=%.3f", r.missed, ms);
        const auto r2 = v.step(nullptr, false, later + (int64_t) (p_nom * 0.5), 1);
        t.check(r2.missed == 0 && r2.target_qpc > r2.vsync_qpc, "vsync-throttle", "missed=%u", r2.missed);
      }
      {
        // wait_until：目標在 1 小時後也只睡 ≤ 2T
        vrdrv::virtual_vsync_t v;
        v.activate(f, mhz, qpc());
        HANDLE tm = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        const int64_t c0 = qpc();
        const uint32_t us = v.wait_until(tm, c0 + 3600 * f);
        const double ms = qpc_to_ms(qpc() - c0);
        if (tm) {
          CloseHandle(tm);
        }
        t.check(us >= 20000 && us <= 30000 && ms < 40.0, "vsync-sleep-cap", "us=%u ms=%.2f", us, ms);
      }
      {
        // scene 圖案往返（含全黑必須失敗）
        uint8_t q48[6] = {1, 2, 3, 4, 5, 6};
        uint8_t bytes[vr_scene_pattern::k_bytes];
        vr_scene_pattern::encode(0xBEEF, q48, bytes);
        const uint32_t w = 1728, h = 1728;
        auto luma = [&](uint32_t x, uint32_t y) -> uint32_t {
          for (int k = 0; k < vr_scene_pattern::k_bits; ++k) {
            uint32_t x0, y0, x1, y1;
            vr_scene_pattern::block_rect(k, w, h, x0, y0, x1, y1);
            if (x >= x0 && x < x1 && y >= y0 && y < y1) {
              return vr_scene_pattern::bit(bytes, k) ? 255u : 0u;
            }
          }
          return 64u;
        };
        uint16_t counter = 0;
        uint8_t out[6] = {};
        const bool ok = vr_scene_pattern::decode(w, h, luma, counter, out);
        t.check(ok && counter == 0xBEEF && std::memcmp(out, q48, 6) == 0, "scene-pattern-roundtrip");
        const bool black = vr_scene_pattern::decode(w, h, [](uint32_t, uint32_t) -> uint32_t {
          return 0u;
        }, counter, out);
        t.check(!black, "scene-pattern-black-rejected");
      }
      t.end();
    }
    // S2-06：pose_history 配對與 space-delta 採用規則（§E.2）
    void test_pose_history(tally_t &t) {
      t.begin("pose-history");
      using vrdrv::ph_pose_t;
      using vrdrv::ph_sample_t;
      using vrdrv::pose_history_t;
      namespace m = vrdrv::math;
      constexpr double k_pi = 3.14159265358979323846;
      const int64_t f = 10000000;  // 固定 qpf，結果與機器無關
      const int64_t t0 = 1000 * f;
      const double T = 1.0 / 90.0;
      auto yaw = [&](double deg) {
        const double h = 0.5 * deg * k_pi / 180.0;
        return m::quat_t {0.0, std::sin(h), 0.0, std::cos(h)};
      };
      auto qpc = [&](double s) {
        return t0 + (int64_t) (s * (double) f);
      };
      {
        ph_pose_t a {yaw(37.0), {0.3, 1.6, -0.2}};
        const ph_pose_t i = pose_history_t::compose(a, pose_history_t::inverse(a));
        t.check(m::angle_deg(i.rot, m::quat_t {}) < 1e-6 && pose_history_t::pos_dist(i.pos, m::vec3_t {}) < 1e-9, "compose-inverse-identity");
        ph_sample_t s;
        s.pose.rot = yaw(0.0);
        s.ang_vel = {0.0, 30.0 * k_pi / 180.0, 0.0};
        s.lin_vel = {1.0, 0.0, 0.0};
        const ph_pose_t e = pose_history_t::extrapolate(s, 0.1);
        t.check(std::fabs(m::angle_deg(e.rot, yaw(3.0))) < 1e-6 && std::fabs(e.pos.x - 0.1) < 1e-9, "extrapolate-yaw30", "ang=%.6f", m::angle_deg(e.rot, yaw(3.0)));
      }
      // 情境：以 90 Hz 回報 yaw 等速 w（度/秒）；app 的 mHmdPose = Δ_true · P(t_app − lag)
      struct run_t {
        int hits = 0, falls = 0, adopted = 0;
        std::string trigger;
        double client_err_deg = 0.0;
        bool nonidentity = false;
        uint32_t last_echo = 0, last_id = 0;
      };
      auto run = [&](double w_deg, double lag_s, const ph_pose_t &delta_true, int frames, const char *event) {
        pose_history_t ph(f);
        run_t r;
        uint32_t id = 0;
        for (int i = 0; i < frames; ++i) {
          const double ts = i * T;
          ph_sample_t s;
          s.sample_id = ++id;
          s.pose.rot = yaw(w_deg * ts);
          s.pose.pos = {0.0, 1.6, 0.0};
          s.ang_vel = {0.0, w_deg * k_pi / 180.0, 0.0};
          s.reported_qpc = qpc(ts);
          s.offset_s = 0.0;
          ph.record(s);
          if (event && i == 5) {
            ph.note_event(event, qpc(ts));
          }
          const double now = ts + 0.002;
          const double t_app = now + 0.03;
          const ph_pose_t truth {yaw(w_deg * (t_app - lag_s)), {0.0, 1.6, 0.0}};
          const ph_pose_t app = pose_history_t::compose(delta_true, truth);
          const auto mm = ph.match(app, qpc(t_app), qpc(now));
          r.hits += mm.hit ? 1 : 0;
          r.falls += mm.hit ? 0 : 1;
          if (mm.adopted) {
            ++r.adopted;
            r.trigger = mm.adopted_trigger;
          }
          r.client_err_deg = m::angle_deg(mm.client_pose.rot, truth.rot);
          r.nonidentity = mm.delta_nonidentity;
          r.last_echo = mm.echo;
          r.last_id = id;
        }
        return r;
      };
      const ph_pose_t ident {};
      {
        const auto r = run(30.0, 0.0, ident, 120, nullptr);
        t.check(r.falls == 0 && r.last_echo == r.last_id && !r.nonidentity, "match-yaw30-identity", "hits=%d falls=%d echo=%u/%u", r.hits, r.falls, r.last_echo, r.last_id);
      }
      {
        // 靜止＋SteamVR 端 yaw 10°／位移 10 cm：前 29 幀 fallback，第 30 幀以 still 採用，之後全命中、renderPose 回到 client 空間
        const ph_pose_t d {yaw(10.0), {0.1, 0.0, 0.0}};
        const auto r = run(0.0, 0.0, d, 60, nullptr);
        t.check(r.adopted == 1 && r.trigger == "still" && r.falls == 30 && r.nonidentity && r.client_err_deg < 0.01, "adopt-still-yaw10",
                "adopted=%d trigger=%s falls=%d err=%.4f", r.adopted, r.trigger.c_str(), r.falls, r.client_err_deg);
      }
      {
        // 設計 §E.2 必測：yaw 30°/s 且 app 晚 44 ms（穩定的 1.32° 假偏移）不可採用新 Δ
        const auto r = run(30.0, 0.044, ident, 120, nullptr);
        t.check(r.adopted == 0 && !r.nonidentity && r.hits == 0, "no-adopt-yaw30-lag44", "adopted=%d falls=%d", r.adopted, r.falls);
      }
      {
        // 同上但 2 s 內有 SeatedZeroPoseReset：允許採用（trigger=event:…）
        const ph_pose_t d {yaw(25.0), {0.0, 0.0, 0.0}};
        const auto r = run(30.0, 0.0, d, 60, "SeatedZeroPoseReset");
        t.check(r.adopted == 1 && r.trigger == "event:SeatedZeroPoseReset" && r.client_err_deg < 0.01, "adopt-event-yaw25",
                "adopted=%d trigger=%s err=%.4f", r.adopted, r.trigger.c_str(), r.client_err_deg);
      }
      {
        // 候選視窗外（樣本全都 > 100 ms 前）：fallback 且 have_candidate=false
        pose_history_t ph(f);
        ph_sample_t s;
        s.sample_id = 7;
        s.reported_qpc = qpc(0.0);
        ph.record(s);
        const auto mm = ph.match(ph_pose_t {}, qpc(0.25), qpc(0.2));
        t.check(!mm.hit && !mm.have_candidate && mm.echo == 0, "no-candidate-window");
      }
      t.end();
    }

    // ── pose_policy（2026-10-05：§VR-CTRL-OFFSET、§VR-STALE-HOLD） ─────────────
    struct swing_result_t {
      double max_err_in_hold_mm = 0;  ///< 空窗 ≤ hold 上限那段的最大誤差
      double max_err_mm = 0;  ///< 整段最大誤差
      int hold_reports = 0;
      int expired_reports = 0;
    };

    /**
     * 等速揮拍（1 m/s）模擬：client 每 5.556 ms 取樣、預測到「取樣＋31 ms」（等速運動預測完全正確）、上行 5 ms；
     * 從 t=200 ms 起有 gap_ms 的上行空窗。driver 在每張新樣本到達時、或距上次醒來 2T 時重送（同 tracking.cpp 的等待）。
     * SteamVR 從「回報時刻＋poseTimeOffset」以回報的速度外插到「現在＋40 ms」，與真值比較。
     */
    swing_result_t run_swing(double gap_ms, bool hold) {
      constexpr double v = 1.0;  // m/s
      constexpr double dt_s = 0.0055556;
      constexpr double ahead_s = 0.031;
      constexpr double uplink_s = 0.005;
      constexpr double steamvr_predict_s = 0.040;
      constexpr uint32_t zero_vel_us = 22222;
      swing_result_t r;
      double last_fresh_arrival = -1;
      double last_target = 0;
      double last_wake = 0;
      double next_sample = 0;
      const double gap_start = 0.2, gap_end = 0.2 + gap_ms / 1000.0;
      for (double now = 0; now < 0.6; now += 0.0001) {  // 0.1 ms 步進
        bool fresh = false;
        while (next_sample + uplink_s <= now) {
          const bool lost = next_sample >= gap_start && next_sample < gap_end;
          if (!lost) {
            fresh = true;
            last_target = next_sample + ahead_s;
            last_fresh_arrival = next_sample + uplink_s;
          }
          next_sample += dt_s;
        }
        if (last_fresh_arrival < 0) {
          continue;
        }
        const bool wake = fresh || now - last_wake >= zero_vel_us / 1e6;
        if (!wake) {
          continue;
        }
        last_wake = now;
        vrdrv::pose_policy_in_t in;
        in.age_us = (int64_t) std::llround((now - last_fresh_arrival) * 1e6);
        in.zero_vel_us = zero_vel_us;
        in.stale_blend = false;  // 這一組比的是舊規則與 hold（stale_blend 的預設 10-31 起是 true，不明講就變成在測 blend）
        in.hold_max_us = 100000;  // hold 上限的預設後來放寬到 300 ms；這一組的案例是照 100 ms 寫的
        in.stale_hold = hold;
        in.raw_off_s = last_target - now;
        const auto p = vrdrv::decide_pose_policy(in);
        if (!fresh && !p.stale) {
          continue;  // tracking.cpp：同一筆樣本已送過、還沒 stale 不重送
        }
        const double pos = v * last_target;
        const double vel = p.zero_vel ? 0.0 : v;
        const double est = pos + vel * (steamvr_predict_s - p.hmd_off_s);
        const double err_mm = std::abs(est - v * (now + steamvr_predict_s)) * 1000.0;
        r.max_err_mm = (std::max)(r.max_err_mm, err_mm);
        if (p.hold) {
          ++r.hold_reports;
          r.max_err_in_hold_mm = (std::max)(r.max_err_in_hold_mm, err_mm);
        } else if (p.stale && hold) {
          ++r.expired_reports;
        }
      }
      return r;
    }

    void test_pose_policy(tally_t &t) {
      t.begin("pose-policy");
      using vrdrv::decide_pose_policy;
      using vrdrv::pose_policy_in_t;
      {
        // 新樣本：offset 照算；ctrl_offset 關著時控制器 offset 是 0（舊行為；預設後來改成開）
        pose_policy_in_t in;
        in.ctrl_offset = false;
        in.raw_off_s = 0.025;
        auto p = decide_pose_policy(in);
        t.check(!p.stale && !p.hold && !p.zero_vel && p.ang_scale == 1.0, "fresh-flags");
        t.check(std::abs(p.hmd_off_s - 0.025) < 1e-12 && p.ctrl_off_s == 0.0, "fresh-legacy-ctrl-zero", "hmd=%f ctrl=%f", p.hmd_off_s, p.ctrl_off_s);
        in.ctrl_offset = true;
        p = decide_pose_policy(in);
        t.check(std::abs(p.ctrl_off_s - p.hmd_off_s) < 1e-12, "fresh-ctrl-same-as-hmd", "hmd=%f ctrl=%f", p.hmd_off_s, p.ctrl_off_s);
        in.ctrl_cap_us = 20000;  // 控制器上限獨立
        p = decide_pose_policy(in);
        t.check(std::abs(p.ctrl_off_s - 0.020) < 1e-12 && std::abs(p.hmd_off_s - 0.025) < 1e-12, "ctrl-cap", "ctrl=%f", p.ctrl_off_s);
      }
      {
        // 上下限：平常 −50 ms；cap 50 ms；非有限值給下限
        pose_policy_in_t in;
        in.raw_off_s = -0.09;
        t.check(std::abs(decide_pose_policy(in).hmd_off_s + 0.05) < 1e-12, "floor-legacy");
        in.raw_off_s = 0.08;
        t.check(std::abs(decide_pose_policy(in).hmd_off_s - 0.05) < 1e-12, "cap");
        in.raw_off_s = std::numeric_limits<double>::quiet_NaN();
        t.check(std::abs(decide_pose_policy(in).hmd_off_s + 0.05) < 1e-12, "nan-floor");
      }
      {
        // stale 舊規則：速度與 offset（HMD、控制器）歸零
        pose_policy_in_t in;
        in.stale_blend = false;
        in.age_us = 30000;
        in.raw_off_s = -0.005;
        in.ctrl_offset = true;
        const auto p = decide_pose_policy(in);
        t.check(p.stale && !p.hold && p.zero_vel && p.hmd_off_s == 0.0 && p.ctrl_off_s == 0.0, "stale-legacy");
      }
      {
        // hold：2T 之後保留 offset，下限放寬到 −100 ms；角速度 50 ms 內線性衰減；超過 100 ms 回到舊規則
        pose_policy_in_t in;
        in.stale_blend = false;
        in.hold_max_us = 100000;
        in.stale_hold = true;
        in.zero_vel_us = 22222;
        in.age_us = 22222 + 25000;
        in.raw_off_s = -0.09;
        auto p = decide_pose_policy(in);
        t.check(p.hold && !p.zero_vel && std::abs(p.hmd_off_s + 0.09) < 1e-12, "hold-floor-relaxed", "off=%f", p.hmd_off_s);
        t.check(std::abs(p.ang_scale - 0.5) < 1e-9, "hold-ang-half", "scale=%f", p.ang_scale);
        in.age_us = 22222 + 60000;
        p = decide_pose_policy(in);
        t.check(p.hold && p.ang_scale == 0.0, "hold-ang-zero", "scale=%f", p.ang_scale);
        in.age_us = 100001;
        p = decide_pose_policy(in);
        t.check(!p.hold && p.zero_vel && p.hmd_off_s == 0.0, "hold-expired");
        in.age_us = 1000;  // 沒 stale：hold 開關不影響
        p = decide_pose_policy(in);
        t.check(!p.stale && !p.hold && p.ang_scale == 1.0, "hold-not-stale");
      }
      {
        // 揮拍模擬：舊規則在空窗裡位置往回跳數公分；hold 在 100 ms 內的空窗維持連續（等速運動誤差 < 1 mm）
        const auto legacy44 = run_swing(44.0, false);
        t.check(legacy44.max_err_mm > 30.0, "swing-legacy-snaps", "maxErr=%.1fmm", legacy44.max_err_mm);
        for (const double g : {22.0, 44.0, 80.0}) {
          const auto h = run_swing(g, true);
          char name[48];
          snprintf(name, sizeof(name), "swing-hold-%.0fms", g);
          t.check(h.max_err_mm < 1.0 && h.expired_reports == 0, name, "maxErr=%.2fmm hold=%d expired=%d", h.max_err_mm, h.hold_reports, h.expired_reports);
        }
        const auto h150 = run_swing(150.0, true);
        t.check(h150.hold_reports > 0 && h150.max_err_in_hold_mm < 1.0 && h150.expired_reports > 0, "swing-hold-150ms-expires",
                "holdErr=%.2fmm hold=%d expired=%d", h150.max_err_in_hold_mm, h150.hold_reports, h150.expired_reports);
      }
      {
        // §VR-ANGVEL-LOCAL：SteamVR 以 q·exp(ω_local t) 外插（10-05 vr_probe --mode predict 實測）。把世界座標的角速度
        // 轉成 ω_local = q⁻¹ ω q 後，結果要等於世界座標的 exp(ω_world t)·q（pose_history 的外插）；沒轉的話軸是錯的
        namespace vm = vrdrv::math;
        auto expq = [](const vm::vec3_t &w, double dt) {
          const double mag = std::sqrt(w.x * w.x + w.y * w.y + w.z * w.z);
          const double h = 0.5 * mag * dt;
          const double k = mag > 1e-12 ? std::sin(h) / mag : 0.0;
          return vm::quat_t {w.x * k, w.y * k, w.z * k, std::cos(h)};
        };
        constexpr double k_deg = 3.14159265358979323846 / 180.0;
        const double sx = std::sin(15.0 * k_deg), cx = std::cos(15.0 * k_deg);  // 半角：繞 X 傾 30°
        const double sy = std::sin(10.0 * k_deg), cy = std::cos(10.0 * k_deg);  // 半角：再繞世界 Y 轉 20°
        const vm::quat_t q = vm::normalize(vm::mul(vm::quat_t {0.0, sy, 0.0, cy}, vm::quat_t {sx, 0.0, 0.0, cx}));
        const vm::vec3_t w_world {0.3, 2.0, -0.7};  // rad/s
        const double dt = 0.06;
        const vm::quat_t world_ref = vm::normalize(vm::mul(expq(w_world, dt), q));
        const vm::vec3_t w_local = vm::rotate(vm::conj(q), w_world);
        const vm::quat_t steamvr_fixed = vm::normalize(vm::mul(q, expq(w_local, dt)));
        const vm::quat_t steamvr_raw = vm::normalize(vm::mul(q, expq(w_world, dt)));
        const double err_fixed = vm::angle_deg(world_ref, steamvr_fixed);
        const double err_raw = vm::angle_deg(world_ref, steamvr_raw);
        t.check(err_fixed < 1e-6, "angvel-local-matches-world", "err=%.8f deg", err_fixed);
        t.check(err_raw > 1.0, "angvel-world-as-local-is-wrong", "err=%.3f deg", err_raw);
        const vm::vec3_t back = vm::rotate(q, w_local);
        t.check(std::abs(back.x - w_world.x) + std::abs(back.y - w_world.y) + std::abs(back.z - w_world.z) < 1e-12, "rotate-roundtrip");
      }
      t.end();
    }

    // ── stale_blender_t（2026-11-13：§VR-STALE-COAST；blend 的往回跳留一個案例當對照） ─────────────
    struct gap_result_t {
      double max_err_mm = 0;  ///< 顯示位置對真值的最大誤差（空窗前 10 ms 到結束後 100 ms）
      double max_back_mm = 0;  ///< 相鄰兩次查詢之間顯示位置倒退的最大量（真值一直往 +x 走，倒退就是我們製造的）
      double max_ang_err_deg = 0;
      int stale_reports = 0;
      double recover_jump_mm = 0;
      double lag_min_ms = 1e9;  ///< 空窗之前的穩定段裡（等速時），顯示的位置比真值晚多久
      double lag_max_ms = -1e9;
      double lag_gap_min_ms = 1e9;  ///< 同上，但量的是空窗前 10 ms 到結束後 100 ms：兩者差多少，軌跡在空窗前後就跳了多少
      double lag_gap_max_ms = -1e9;
    };

    /**
     * 手沿 +x 以 x(t) = v·t + ½·a·t² 移動、同時繞 z 軸以 w_z 等角速度轉。client 每 1/576 s 取樣、預測到「取樣＋24 ms」
     * （用真值，等於預測完全正確）、上行 5 ms；t=200 ms 起有 gap_ms 的上行空窗。
     * driver 的節奏照 tracking.cpp：新樣本到就回報；否則照等待時間醒來（coast：還沒 stale 時 26 ms、stale 後 5 ms；其餘 25 ms），
     * 已經 stale 才重送。first_wake_miss＝第一次逾時差幾微秒沒判成 stale（實機常見），要到下一次醒來才進得去。
     * SteamVR 的語意：姿態時刻＝回報時刻＋offset，照回報的速度（線速度、角速度）外插到「現在＋horizon」。
     * know_pred＝driver 知道 SteamVR 的預測時間（從 SubmitLayer 得知；不知道時 coast 只保證回報的姿態連續）。
     * keep_ms＝§VR-CTRL-EXTRAP 的上限（< 0＝不限制）。batch＝空窗裡的樣本不是掉了而是晚到：空窗結束時一次全到（舊的在前）。
     * 每次醒來比排定的時刻晚 0.2 ms（排程延遲；剛好落在門檻上時 age 不會大於門檻）。
     */
    gap_result_t run_gap(bool coast, double gap_ms, double v, double a, double w_z, bool first_wake_miss, bool know_pred = true, double horizon_s = 0.040,
                         double keep_ms = -1.0, bool batch = false) {
      namespace vm = vrdrv::math;
      constexpr int64_t qpf = 10'000'000;
      constexpr double dt_s = 1.0 / 576.0;
      constexpr double ahead_s = 0.024;
      constexpr double uplink_s = 0.005;
      constexpr uint32_t zero_vel_us = 25000;
      const double gap_start = 0.2, gap_end = 0.2 + gap_ms / 1000.0;
      auto x_at = [&](double t) {
        return v * t + 0.5 * a * t * t;
      };
      auto rot_at = [&](double t) {
        const double h = 0.5 * w_z * t;
        return vm::quat_t {0.0, 0.0, std::sin(h), std::cos(h)};
      };
      gap_result_t r;
      vrdrv::stale_blender_t bl;
      double next_sample = 0, last_fresh_arrival = -1, last_target = 0, next_wake = 1e9;
      bool stale = false, missed = false, have_rep = false, have_prev = false;
      double rep_t = 0, rep_off = 0, prev_disp = 0;
      vm::vec3_t rep_pos, rep_vel, rep_w;
      vm::quat_t rep_rot;
      float s_pos[3] = {0, 0, 0}, s_rot[4] = {0, 0, 0, 1}, s_vel[3] = {0, 0, 0}, s_ang[3] = {0, 0, 0};
      std::vector<double> held;  // batch：空窗裡取樣、還積在路上的樣本（各自的目標時間）
      // driver 的一次處理（tracking.cpp 的迴圈本體）：fresh＝這一次有新樣本（目標時間 target）
      auto driver_step = [&](double now, bool fresh, double target) {
        if (fresh) {
          last_target = target;
          last_fresh_arrival = now;
          const vm::quat_t q = rot_at(target);
          s_pos[0] = (float) x_at(target);
          s_vel[0] = (float) (v + a * target);
          s_rot[0] = (float) q.x;
          s_rot[1] = (float) q.y;
          s_rot[2] = (float) q.z;
          s_rot[3] = (float) q.w;
          s_ang[2] = (float) w_z;
        }
        vrdrv::pose_policy_in_t in;
        in.age_us = (int64_t) std::llround((now - last_fresh_arrival) * 1e6);
        in.zero_vel_us = zero_vel_us;
        in.stale_blend = true;  // coast 連 BLEND 一起設（vr_orchestrator.cpp）
        in.stale_coast = coast;
        in.ctrl_offset = true;
        in.raw_off_s = last_target - now;
        const auto p = vrdrv::decide_pose_policy(in);
        bool now_stale = p.stale;
        if (fresh) {
          stale = false;
          missed = false;
        } else if (first_wake_miss && !missed && !stale) {
          now_stale = false;  // 這一次差幾微秒沒過門檻
          missed = true;
        }
        if (fresh || now_stale) {
          if (!fresh) {
            stale = true;
            ++r.stale_reports;
          }
          const auto o = bl.process(fresh, s_pos, s_rot, s_vel, s_ang, p, p.ctrl_off_s, (int64_t) std::llround(now * (double) qpf), qpf,
                                    know_pred ? horizon_s : 0.0, keep_ms < 0.0 ? -1.0 : keep_ms / 1000.0, in.raw_off_s, 0.3);
          rep_t = now;
          rep_off = o.off_s;
          rep_pos = o.pos;
          rep_rot = o.rot;
          rep_vel = o.lin_vel;
          rep_w = o.ang_vel;
          have_rep = true;
          if (o.log_recover) {
            r.recover_jump_mm = o.recover_jump_mm;
          }
        }
        const double wait_s = coast ? (stale ? (now - last_fresh_arrival < 0.2 ? 0.005 : 0.025) : 0.026) : 0.025;
        next_wake = now + wait_s + 0.0002;
      };
      for (int step = 0; step < 6000; ++step) {  // 0.1 ms 步進、0.6 s
        const double now = step * 0.0001;
        std::vector<double> arrivals;
        while (next_sample + uplink_s <= now + 1e-9) {
          const bool in_gap = next_sample >= gap_start && next_sample < gap_end;
          if (!in_gap) {
            arrivals.push_back(next_sample + ahead_s);
          } else if (batch) {
            held.push_back(next_sample + ahead_s);
          }
          next_sample += dt_s;
        }
        if (!held.empty() && now + 1e-9 >= gap_end + uplink_s) {
          arrivals.insert(arrivals.begin(), held.begin(), held.end());  // 積在路上的一起到，舊的在前
          held.clear();
        }
        // 成批到達時 driver 可能每一格都讀到（最壞的情況）：每筆各處理一次
        for (const double tgt : arrivals) {
          driver_step(now, true, tgt);
        }
        if (arrivals.empty() && last_fresh_arrival >= 0 && now + 1e-9 >= next_wake) {
          driver_step(now, false, 0.0);
        }
        if (!have_rep) {
          continue;
        }
        const double tau = now + horizon_s;
        const double dtq = tau - (rep_t + rep_off);
        const double disp = rep_pos.x + rep_vel.x * dtq;
        if (now >= 0.1 && now < gap_start && v > 0.0 && a == 0.0) {
          // 等速時「顯示的位置比真值晚多久」
          const double lag_ms = (x_at(tau) - disp) / v * 1000.0;
          r.lag_min_ms = (std::min)(r.lag_min_ms, lag_ms);
          r.lag_max_ms = (std::max)(r.lag_max_ms, lag_ms);
        }
        if (now >= gap_start - 0.01 && now <= gap_end + 0.1) {
          if (v > 0.0 && a == 0.0) {
            const double lag_ms = (x_at(tau) - disp) / v * 1000.0;
            r.lag_gap_min_ms = (std::min)(r.lag_gap_min_ms, lag_ms);
            r.lag_gap_max_ms = (std::max)(r.lag_gap_max_ms, lag_ms);
          }
          r.max_err_mm = (std::max)(r.max_err_mm, std::abs(disp - x_at(tau)) * 1000.0);
          if (have_prev && disp < prev_disp) {
            r.max_back_mm = (std::max)(r.max_back_mm, (prev_disp - disp) * 1000.0);
          }
          const vm::quat_t qd = vm::normalize(vm::mul(vm::quat_from_angvel(rep_w, dtq), rep_rot));
          r.max_ang_err_deg = (std::max)(r.max_ang_err_deg, vm::angle_deg(qd, rot_at(tau)));
        }
        prev_disp = disp;
        have_prev = true;
      }
      return r;
    }

    // ── §VR-CTRL-EXTRAP-SMOOTH（2026-11-14）：上行的到達時間在抖時，SteamVR 交給遊戲的拍子軌跡是不是連續 ──────────
    // 手等速 v 往 +x 走；client 每 1/576 s 取樣（預測到取樣＋24 ms）；上行延遲＝5 ms＋每一筆不同的抖動（0～jitter_ms，
    // 決定性的序列），但不會後發先至。driver 每到一筆就回報（照 tracking.cpp）。SteamVR 的語意同 run_gap。
    // 回傳：暖機之後，「顯示的位置比真值晚多久」的最大值減最小值（毫秒）。等速時這個量應該是常數——差多少，
    // 遊戲看到的位置就多抖「速度×這個量」。
    double run_arrival_jitter(double keep_ms, double jitter_ms, double v, double horizon_s = 0.060) {
      namespace vm = vrdrv::math;
      constexpr int64_t qpf = 10'000'000;
      constexpr double dt_s = 1.0 / 576.0;
      constexpr double ahead_s = 0.024;
      vrdrv::stale_blender_t bl;
      vrdrv::pose_policy_in_t in;
      in.zero_vel_us = 25000;
      in.stale_blend = true;
      in.stale_coast = true;
      in.ctrl_offset = true;
      double lag_min = 1e9, lag_max = -1e9;
      double prev_arrival = 0.0;
      uint32_t seed = 12345u;
      bool have_rep = false;
      double rep_t = 0, rep_off = 0, rep_x = 0, rep_v = 0;
      double next_query = 0.0;
      for (int i = 0; i < 576 * 6; ++i) {  // 6 秒
        const double sample_t = i * dt_s;
        seed = seed * 1664525u + 1013904223u;
        const double jit = jitter_ms / 1000.0 * (double) ((seed >> 8) & 0xFFFF) / 65535.0;
        const double arrival = (std::max)(prev_arrival + 1e-5, sample_t + 0.005 + jit);
        // 這一筆到達之前的查詢（每 1 ms 一次，像遊戲在任意時刻取姿態）
        while (have_rep && next_query < arrival) {
          const double tau = next_query + horizon_s;
          const double disp = rep_x + rep_v * (tau - (rep_t + rep_off));
          if (next_query > 3.0) {
            const double lag_ms = (v * tau - disp) / v * 1000.0;
            lag_min = (std::min)(lag_min, lag_ms);
            lag_max = (std::max)(lag_max, lag_ms);
          }
          next_query += 0.001;
        }
        prev_arrival = arrival;
        const double target = sample_t + ahead_s;
        float s_pos[3] = {(float) (v * target), 0, 0}, s_rot[4] = {0, 0, 0, 1}, s_vel[3] = {(float) v, 0, 0}, s_ang[3] = {0, 0, 0};
        in.age_us = 0;
        in.raw_off_s = target - arrival;
        const auto p = vrdrv::decide_pose_policy(in);
        const auto o = bl.process(true, s_pos, s_rot, s_vel, s_ang, p, p.ctrl_off_s, (int64_t) std::llround(arrival * (double) qpf), qpf, horizon_s,
                                  keep_ms < 0.0 ? -1.0 : keep_ms / 1000.0, in.raw_off_s, 0.3);
        rep_t = arrival;
        rep_off = o.off_s;
        rep_x = o.pos.x;
        rep_v = o.lin_vel.x;
        have_rep = true;
        if (next_query < arrival) {
          next_query = arrival;
        }
      }
      return lag_max - lag_min;
    }

    // ── §VR-CTRL-PREDICT（2026-11-15）：有加速度的動作＋到達時間抖動，SteamVR 交給遊戲的軌跡 ───────────────────────
    struct predict_result_t {
      double rough_mm = 0;  ///< 顯示位置的二階差分減掉真值的二階差分，最大值：軌跡比真的動作粗多少
      double ang_rough_deg = 0;  ///< 同上，繞 z 的角度
      double err_mm = 0;  ///< 顯示位置對「顯示時刻的真值」的最大誤差
      double vel_err = 0;  ///< 回報的速度對「顯示時刻的真值速度」的最大誤差（m/s）
      double vel_incons = 0;  ///< 回報的速度對「顯示位置的差分」的最大差（m/s）
      double held_err_mm = 0;  ///< 扳機按著、穩定之後：顯示位置對「樣本那一刻的真值」的最大誤差
      double held_vel_err = 0;
      double release_err_mm = 0;  ///< 放開扳機之後 0.12 秒內：還是樣本那一刻的手（遊戲算拋出速度的那幾格）
      double max_step_mm = 0;  ///< 相鄰兩格顯示位置的最大位移
    };

    /**
     * 手沿 x 做 x(t)＝0.3·sin(2π·1.5·t)，同時繞 z 做 0.8·sin(2π·1.5·t＋1) rad 的擺動。client 每 1/576 s 取樣、預測到取樣＋24 ms
     * （真值）；上行 5 ms＋0～jitter_ms 的抖動（不會後發先至）。driver 每到一筆就回報（照 tracking.cpp：mode 1 先過
     * ctrl_predictor_t）。SteamVR 每 1/144 s 取一次「現在＋horizon」的姿態：位置＋速度×時間、旋轉＝exp(ω·時間)·q。
     * mode：0＝不限制（SteamVR 自己直線外插）、1＝§VR-CTRL-PREDICT、2＝上限 0。with_hold＝第 4～6 秒扳機按著。
     */
    predict_result_t run_predict_sim(int mode, double jitter_ms, bool with_hold, double horizon_s = 0.060) {
      namespace vm = vrdrv::math;
      constexpr int64_t qpf = 10'000'000;
      constexpr double dt_s = 1.0 / 576.0;
      constexpr double ahead_s = 0.024;
      constexpr double dq_s = 1.0 / 144.0;
      constexpr double amp = 0.3, w0 = 2.0 * 3.14159265358979323846 * 1.5, th = 0.8;
      auto x_at = [&](double t) {
        return amp * std::sin(w0 * t);
      };
      auto v_at = [&](double t) {
        return amp * w0 * std::cos(w0 * t);
      };
      auto ang_at = [&](double t) {
        return th * std::sin(w0 * t + 1.0);
      };
      auto angv_at = [&](double t) {
        return th * w0 * std::cos(w0 * t + 1.0);
      };
      vrdrv::stale_blender_t bl;
      vrdrv::ctrl_predictor_t pred;
      vrdrv::pose_policy_in_t in;
      in.zero_vel_us = 25000;
      in.stale_blend = true;
      in.stale_coast = true;
      in.ctrl_offset = true;
      predict_result_t r;
      double prev_arrival = 0.0;
      uint32_t seed = 4242u;
      bool have_rep = false;
      double rep_t = 0, rep_off = 0, rep_x = 0, rep_v = 0, rep_w = 0;
      vm::quat_t rep_rot {0, 0, 0, 1};
      double next_query = 0.0;
      double d1 = 0, d2 = 0, a1 = 0, a2 = 0;  // 前兩次查詢的顯示位置與角度
      double v1 = 0;  // 上一次查詢時回報的速度
      int nq = 0;
      for (int i = 0; i < 576 * 8; ++i) {  // 8 秒
        const double sample_t = i * dt_s;
        seed = seed * 1664525u + 1013904223u;
        const double jit = jitter_ms / 1000.0 * (double) ((seed >> 8) & 0xFFFF) / 65535.0;
        const double arrival = (std::max)(prev_arrival + 1e-5, sample_t + 0.005 + jit);
        while (next_query < arrival) {
          if (!have_rep) {
            next_query += dq_s;  // 查詢固定每 1/144 s 一次（二階差分要等間隔）
            continue;
          }
          const double tq = next_query;
          const double tau = tq + horizon_s;
          const double dtq = tau - (rep_t + rep_off);
          const double disp = rep_x + rep_v * dtq;
          const vm::quat_t qd = vm::normalize(vm::mul(vm::quat_from_angvel(vm::vec3_t {0, 0, rep_w}, std::abs(dtq)), rep_rot));
          // 繞 z 的角度（dtq 是負的時候上面少轉了，這個模擬裡 dtq 一律是正的）
          const double ang = 2.0 * std::atan2(qd.z, qd.w);
          const bool held_now = with_hold && tq >= 4.0 && tq < 6.0;
          if (nq >= 2 && tq > 2.0) {
            const bool steady = !with_hold || tq < 3.9;  // 一般的指標只看沒有按扳機的那一段
            if (steady) {
              const double dd = (disp - 2.0 * d1 + d2) - (x_at(tau) - 2.0 * x_at(tau - dq_s) + x_at(tau - 2.0 * dq_s));
              r.rough_mm = (std::max)(r.rough_mm, std::abs(dd) * 1000.0);
              const double da = (ang - 2.0 * a1 + a2) - (ang_at(tau) - 2.0 * ang_at(tau - dq_s) + ang_at(tau - 2.0 * dq_s));
              r.ang_rough_deg = (std::max)(r.ang_rough_deg, std::abs(da) * 180.0 / 3.14159265358979323846);
              r.err_mm = (std::max)(r.err_mm, std::abs(disp - x_at(tau)) * 1000.0);
              r.vel_err = (std::max)(r.vel_err, std::abs(rep_v - v_at(tau)));
              r.vel_incons = (std::max)(r.vel_incons, std::abs((disp - d1) / dq_s - 0.5 * (rep_v + v1)));
            }
            if (with_hold) {
              r.max_step_mm = (std::max)(r.max_step_mm, std::abs(disp - d1) * 1000.0);
              if (held_now && tq > 4.6 && tq < 5.9) {
                // 不預測：顯示的是「樣本那一刻」的手＝顯示時刻再往前 E
                const double te = tau - pred.extrap_s();
                r.held_err_mm = (std::max)(r.held_err_mm, std::abs(disp - x_at(te)) * 1000.0);
                r.held_vel_err = (std::max)(r.held_vel_err, std::abs(rep_v - v_at(te)));
              }
              if (tq >= 6.0 && tq < 6.12) {
                r.release_err_mm = (std::max)(r.release_err_mm, std::abs(disp - x_at(tau - pred.extrap_s())) * 1000.0);
              }
            }
          }
          d2 = d1;
          d1 = disp;
          a2 = a1;
          a1 = ang;
          v1 = rep_v;
          ++nq;
          next_query += dq_s;
        }
        prev_arrival = arrival;
        const double target = sample_t + ahead_s;
        const double half = 0.5 * ang_at(target);
        float s_pos[3] = {(float) x_at(target), 0, 0}, s_rot[4] = {0, 0, (float) std::sin(half), (float) std::cos(half)};
        float s_vel[3] = {(float) v_at(target), 0, 0}, s_ang[3] = {0, 0, (float) angv_at(target)};
        in.age_us = 0;
        in.raw_off_s = target - arrival;
        const auto p = vrdrv::decide_pose_policy(in);
        const int64_t now_qpc = (int64_t) std::llround(arrival * (double) qpf);
        if (mode == 1) {
          const bool held = with_hold && sample_t >= 4.0 && sample_t < 6.0;
          pred.on_fresh(s_pos, s_rot, s_vel, s_ang, (int64_t) std::llround(target * (double) qpf), vrdrv::steamvr_extrap_s(horizon_s, p.ctrl_off_s), held, now_qpc,
                        qpf);
          pred.apply(s_pos, s_rot, s_vel, s_ang);
        }
        const auto o = bl.process(true, s_pos, s_rot, s_vel, s_ang, p, p.ctrl_off_s, now_qpc, qpf, horizon_s, mode == 2 ? 0.0 : -1.0, in.raw_off_s, 0.3);
        rep_t = arrival;
        rep_off = o.off_s;
        rep_x = o.pos.x;
        rep_v = o.lin_vel.x;
        rep_w = o.ang_vel.z;
        rep_rot = o.rot;
        have_rep = true;
      }
      return r;
    }

    void test_stale_blender(tally_t &t) {
      t.begin("stale-blender");
      // 對照：blend（目前的預設）進 stale 時把姿態重新標成晚 25 ms 的時刻、位置卻沒補——3 m/s 時顯示位置倒退好幾公分
      {
        const auto b = run_gap(false, 45.0, 3.0, 0.0, 10.0, false);
        t.check(b.max_back_mm > 50.0, "blend-snaps-back", "back=%.1fmm err=%.1fmm", b.max_back_mm, b.max_err_mm);
      }
      // coast：等速運動、空窗 60 ms 以內——和沒有空窗一樣（位置、角度都連續，沒有倒退）
      for (const double g : {27.0, 45.0, 58.0}) {
        const auto c = run_gap(true, g, 3.0, 0.0, 10.0, false);
        char name[48];
        snprintf(name, sizeof(name), "coast-const-%.0fms", g);
        t.check(c.max_err_mm < 1.0 && c.max_back_mm < 0.01 && c.max_ang_err_deg < 0.05, name, "err=%.3fmm back=%.4fmm ang=%.4fdeg stale=%d jump=%.3fmm",
                c.max_err_mm, c.max_back_mm, c.max_ang_err_deg, c.stale_reports, c.recover_jump_mm);
      }
      {
        // 第一次逾時沒判成 stale（要到 52 ms 才進得去）：一樣連續
        const auto c = run_gap(true, 58.0, 3.0, 0.0, 10.0, true);
        t.check(c.max_err_mm < 1.0 && c.max_back_mm < 0.01 && c.max_ang_err_deg < 0.05, "coast-late-entry", "err=%.3fmm back=%.4fmm ang=%.4fdeg stale=%d",
                c.max_err_mm, c.max_back_mm, c.max_ang_err_deg, c.stale_reports);
      }
      {
        // 空窗 90 ms：60 ms 起衰減，顯示位置的落後量最多「速度 × y²/2T」（y＝30 ms → 11.25 ms），恢復時 40 ms 內補回，沒有倒退
        const auto c = run_gap(true, 90.0, 1.0, 0.0, 5.0, false);
        // （每 5 ms 重送一次：兩次之間 SteamVR 照上一次的速度外插，降速時每次多走 ½·(v/T)·Δt² ≈ 每 1 m/s 0.31 mm，下一次重送收回）
        t.check(c.max_err_mm < 14.0 && c.max_back_mm < 0.4 && c.stale_reports >= 8, "coast-90ms-decays", "err=%.2fmm back=%.4fmm stale=%d jump=%.2fmm",
                c.max_err_mm, c.max_back_mm, c.stale_reports, c.recover_jump_mm);
      }
      {
        // 空窗 150 ms：停在「60 ms 等速＋衰減積分 20 ms」之後不再前進（落後最多 70 ms 的路程）；恢復時在 40 ms 內補回，沒有倒退
        const auto c = run_gap(true, 150.0, 1.0, 0.0, 5.0, false);
        t.check(c.max_err_mm > 60.0 && c.max_err_mm < 75.0 && c.max_back_mm < 0.4, "coast-150ms-stops", "err=%.2fmm back=%.4fmm stale=%d jump=%.2fmm",
                c.max_err_mm, c.max_back_mm, c.stale_reports, c.recover_jump_mm);
      }
      {
        // 外出的情況：SteamVR 的預測時間 70 ms（外插量約 51 ms，比衰減時間還長）。知道預測時間：顯示位置一樣平順減速、不倒退
        const auto c = run_gap(true, 90.0, 3.0, 0.0, 10.0, false, true, 0.070);
        // （外插量 51 ms 時 29 ms 就開始降速、69 ms 停：到 90 ms 時落後 41 ms 的路程）
        t.check(c.max_err_mm < 3.0 * 43.0 && c.max_back_mm < 3.0 * 0.4, "coast-90ms-long-horizon", "err=%.2fmm back=%.4fmm jump=%.2fmm", c.max_err_mm, c.max_back_mm, c.recover_jump_mm);
        // 不知道預測時間（還沒有畫面）：回報的姿態連續，但降速時 SteamVR 少外插的那一段會縮回去——有界（不超過 速度×外插量）
        const auto n = run_gap(true, 90.0, 3.0, 0.0, 10.0, false, false, 0.070);
        t.check(n.max_back_mm > 1.0 && n.max_back_mm < 3.0 * 51.0, "coast-no-pred-bounded", "err=%.2fmm back=%.2fmm", n.max_err_mm, n.max_back_mm);
      }
      {
        // 加速中（10 m/s²）遇到 45 ms 的空窗：空窗裡的加速度沒有資料，誤差修不掉，但不倒退，而且比 blend 小
        const auto c = run_gap(true, 45.0, 0.0, 10.0, 5.0, false);
        const auto b = run_gap(false, 45.0, 0.0, 10.0, 5.0, false);
        t.check(c.max_back_mm < 0.01 && c.max_err_mm < b.max_err_mm, "coast-accel-no-snap", "coast err=%.2fmm back=%.4fmm | blend err=%.2fmm back=%.2fmm",
                c.max_err_mm, c.max_back_mm, b.max_err_mm, b.max_back_mm);
      }
      {
        // decide_pose_policy：coast 開著時 stale 也算 blend（沿用 keep_offset 與 pose_history 的路徑），速度不在這裡歸零
        vrdrv::pose_policy_in_t in;
        in.stale_coast = true;
        in.age_us = 90000;
        in.raw_off_s = 0.02;
        const auto p = vrdrv::decide_pose_policy(in);
        t.check(p.stale && p.blend && p.coast && p.keep_offset && !p.zero_vel && !p.hold, "coast-policy-flags");
        in.age_us = 1000;
        const auto q = vrdrv::decide_pose_policy(in);
        t.check(!q.stale && !q.blend && q.coast && q.blend_enabled && !q.zero_vel, "coast-policy-fresh");
      }
      {
        // §VR-CTRL-EXTRAP：要往回推多久＝SteamVR 會外插的長度（預測時間 − offset，0～100 ms）超過上限的部分
        using vrdrv::ctrl_backdate_s;
        t.check(ctrl_backdate_s(0.045, 0.0236, -1.0) == 0.0, "ctrl-back-auto");
        t.check(std::abs(ctrl_backdate_s(0.045, 0.0236, 0.0) - 0.0214) < 1e-9, "ctrl-back-keep0");
        t.check(std::abs(ctrl_backdate_s(0.045, 0.0236, 0.010) - 0.0114) < 1e-9, "ctrl-back-keep10");
        t.check(ctrl_backdate_s(0.045, 0.0236, 0.030) == 0.0, "ctrl-back-under-limit");
        t.check(ctrl_backdate_s(0.0, 0.0236, 0.0) == 0.0, "ctrl-back-no-pred");
        t.check(ctrl_backdate_s(0.020, 0.0236, 0.0) == 0.0, "ctrl-back-not-negative");
        t.check(std::abs(ctrl_backdate_s(0.300, 0.010, 0.020) - 0.080) < 1e-12, "ctrl-back-capped-at-100ms");
        t.check(ctrl_backdate_s(0.045, std::numeric_limits<double>::quiet_NaN(), 0.0) == 0.0, "ctrl-back-nan");
        // 整段模擬（預測時間 40 ms、offset 19 ms → SteamVR 外插 21 ms）：顯示的拍子比真值晚「21 ms − 上限」，純粹是時間平移
        // （模擬以 0.1 ms 步進，樣本到達時刻有 0～0.1 ms 的量化，offset 與外插量跟著差這麼多）
        const auto full = run_gap(false, 0.0, 3.0, 0.0, 10.0, false, true, 0.040, -1.0);
        const auto k10 = run_gap(false, 0.0, 3.0, 0.0, 10.0, false, true, 0.040, 10.0);
        const auto k0 = run_gap(false, 0.0, 3.0, 0.0, 10.0, false, true, 0.040, 0.0);
        t.check(std::abs(full.lag_min_ms) < 0.05 && std::abs(full.lag_max_ms) < 0.05, "ctrl-extrap-auto", "lag=%.3f..%.3fms", full.lag_min_ms, full.lag_max_ms);
        t.check(std::abs(k10.lag_min_ms - 11.0) < 0.15 && std::abs(k10.lag_max_ms - 11.0) < 0.15, "ctrl-extrap-10", "lag=%.3f..%.3fms", k10.lag_min_ms, k10.lag_max_ms);
        t.check(std::abs(k0.lag_min_ms - 21.0) < 0.15 && std::abs(k0.lag_max_ms - 21.0) < 0.15, "ctrl-extrap-0", "lag=%.3f..%.3fms", k0.lag_min_ms, k0.lag_max_ms);
        // 上限 0 再遇到空窗（coast）：一樣連續，顯示的拍子固定晚 21 ms（3 m/s → 63 mm、10 rad/s → 12°），空窗前後不變
        const auto c = run_gap(true, 45.0, 3.0, 0.0, 10.0, false, true, 0.040, 0.0);
        t.check(c.max_back_mm < 0.01 && c.max_err_mm > 62.5 && c.max_err_mm < 63.5 && c.max_ang_err_deg < 12.2, "ctrl-extrap-0-coast",
                "back=%.4fmm ang=%.4fdeg err=%.2fmm", c.max_back_mm, c.max_ang_err_deg, c.max_err_mm);
        // 舊的 blend 政策也套同一步（最後一步是共用的）：穩定段一樣只是時間平移
        const auto b0 = run_gap(false, 0.0, 3.0, 0.0, 10.0, false, true, 0.040, 0.0);
        t.check(std::abs(b0.lag_max_ms - 21.0) < 0.15, "ctrl-extrap-0-blend", "lag=%.3f..%.3fms", b0.lag_min_ms, b0.lag_max_ms);
      }
      {
        // 成批到達（空窗裡的樣本不是掉了而是晚到，結束時一次全到）：外出的空窗多半是這種
        // 45 ms：最舊的那筆目標時間才過去 26 ms（沒過期），每一筆都照實接受——等速時不倒退、落差接近 0
        const auto s45 = run_gap(true, 45.0, 3.0, 0.0, 10.0, false, true, 0.040, -1.0, true);
        t.check(s45.max_back_mm < 0.01 && s45.max_err_mm < 1.0 && s45.recover_jump_mm < 2.0, "coast-batch-45ms", "err=%.3fmm back=%.4fmm jump=%.3fmm",
                s45.max_err_mm, s45.max_back_mm, s45.recover_jump_mm);
        // 100 ms：前面幾筆的目標時間已經過去 50 ms 以上（offset 會被夾在 −50 ms）——略過，等同一批裡沒過期的；不倒退，
        // 恢復的落差就是降速造成的那一段（第一次在 59 ms 之後重送時起降速，到 100 ms 時少走約 17.5 ms 的路程＝53 mm），
        // 沒有多出「速度×被夾掉的量」（略過之前是 53＋93 mm）
        const auto s100 = run_gap(true, 100.0, 3.0, 0.0, 10.0, false, true, 0.040, -1.0, true);
        t.check(s100.max_back_mm < 3.0 * 0.4 && s100.recover_jump_mm > 45.0 && s100.recover_jump_mm < 60.0, "coast-batch-100ms-skips-expired",
                "err=%.2fmm back=%.4fmm jump=%.2fmm", s100.max_err_mm, s100.max_back_mm, s100.recover_jump_mm);
        // 外出的預測時間（70 ms）＋成批 45 ms：晚到的樣本 offset 是負的、SteamVR 把它多外插一段，落差要在顯示位置上算才不會跳
        const auto f45 = run_gap(true, 45.0, 3.0, 0.0, 10.0, false, true, 0.070, -1.0, true);
        t.check(f45.max_back_mm < 3.0 * 0.4, "coast-batch-45ms-long-horizon", "err=%.2fmm back=%.4fmm jump=%.2fmm", f45.max_err_mm, f45.max_back_mm, f45.recover_jump_mm);
      }
      {
        // §VR-CTRL-EXTRAP-SMOOTH：上行到達時間抖 0～8 ms、手 3 m/s。不限制外插時軌跡本來就連續；限制在 20 ms／0 ms 時也要連續
        // （每一筆照自己的 offset 往回推的舊做法：抖動一比一進到位置，這裡會是約 8 ms＝24 mm）
        const double un = run_arrival_jitter(-1.0, 8.0, 3.0);
        const double k20 = run_arrival_jitter(20.0, 8.0, 3.0);
        const double k0 = run_arrival_jitter(0.0, 8.0, 3.0);
        t.check(un < 0.05, "ctrl-extrap.jitter-unlimited-continuous", "spread=%.3f ms", un);
        t.check(k20 < 0.5, "ctrl-extrap.jitter-keep20-continuous", "spread=%.3f ms", k20);
        t.check(k0 < 0.5, "ctrl-extrap.jitter-keep0-continuous", "spread=%.3f ms", k0);
        // 沒有抖動時照舊是常數
        const double flat = run_arrival_jitter(20.0, 0.0, 3.0);
        t.check(flat < 0.05, "ctrl-extrap.no-jitter-keep20-constant", "spread=%.3f ms", flat);
      }
      {
        // 外插上限開著、35 ms 的上行空窗、空窗裡的樣本晚到（成批、舊的在前）、手等速 3 m/s：等速時 SteamVR 的外插是對的，
        // 顯示出來的位置在空窗前後應該一路連續。舊的算法把晚到的樣本少算「晚到的那段時間」，恢復時多補 3 m/s×35 ms＝105 mm
        for (const double keep : {20.0, 0.0}) {
          const auto b = run_gap(true, 35.0, 3.0, 0.0, 10.0, false, true, 0.060, keep, true);
          const double spread = b.lag_gap_max_ms - b.lag_gap_min_ms;
          t.check(spread < 3.0, keep > 0.0 ? "ctrl-extrap.batch-gap-keep20-continuous" : "ctrl-extrap.batch-gap-keep0-continuous", "lagSpread=%.2f ms back=%.1f mm", spread,
                  b.max_back_mm);
          t.check(b.max_back_mm < 3.0, keep > 0.0 ? "ctrl-extrap.batch-gap-keep20-no-back" : "ctrl-extrap.batch-gap-keep0-no-back", "back=%.1f mm", b.max_back_mm);
        }
        // 樣本真的掉了（不是晚到）時也一樣
        const auto d = run_gap(true, 35.0, 3.0, 0.0, 10.0, false, true, 0.060, 20.0, false);
        t.check(d.lag_gap_max_ms - d.lag_gap_min_ms < 3.0, "ctrl-extrap.drop-gap-keep20-continuous", "lagSpread=%.2f ms", d.lag_gap_max_ms - d.lag_gap_min_ms);
      }
      {
        // §VR-CTRL-PREDICT：手做 1.5 Hz 的來回（最快 2.83 m/s、26.6 m/s²）加繞 z 的擺動，上行到達時間抖 0～8 ms。
        // unlimited＝SteamVR 自己直線外插（2.0.175 的行為）；predict＝ctrl_predictor_t；keep0＝完全不外插（最平滑、但晚一整段）
        const auto un = run_predict_sim(0, 8.0, false);
        const auto pr = run_predict_sim(1, 8.0, false);
        const auto k0 = run_predict_sim(2, 8.0, false);
        // 軌跡的粗細：predict 要比 unlimited 細很多
        t.check(pr.rough_mm < 2.0 && un.rough_mm > 3.0 * pr.rough_mm, "predict.smoother-than-linear", "rough predict=%.2f unlimited=%.2f keep0=%.2f mm", pr.rough_mm,
                un.rough_mm, k0.rough_mm);
        t.check(pr.ang_rough_deg < un.ang_rough_deg / 3.0, "predict.rotation-smoother", "ang rough predict=%.3f unlimited=%.3f deg", pr.ang_rough_deg, un.ang_rough_deg);
        // 不延遲：對「顯示時刻的真值」的誤差只有直線預測本身的量（½·a·E²≈27 mm），不是 keep0 那種「速度×E」（約 127 mm）
        t.check(pr.err_mm < 45.0 && k0.err_mm > 100.0, "predict.no-lag", "err predict=%.1f unlimited=%.1f keep0=%.1f mm", pr.err_mm, un.err_mm, k0.err_mm);
        // 速度：回報的就是顯示時刻的速度，而且和顯示位置的差分一致
        t.check(pr.vel_err < 0.5 && un.vel_err > 0.9, "predict.velocity-at-display-time", "velErr predict=%.2f unlimited=%.2f m/s", pr.vel_err, un.vel_err);
        t.check(pr.vel_incons < 0.35 && un.vel_incons > 2.0 * pr.vel_incons, "predict.velocity-matches-motion", "incons predict=%.2f unlimited=%.2f m/s",
                pr.vel_incons, un.vel_incons);
        // 沒有抖動時也不能變差
        const auto pr0 = run_predict_sim(1, 0.0, false);
        t.check(pr0.rough_mm < 0.5, "predict.no-jitter-smooth", "rough=%.3f mm", pr0.rough_mm);
        // 扳機按著：那隻手不預測（位置＝樣本那一刻、速度＝那一刻的速度），進出時不跳
        const auto hd = run_predict_sim(1, 8.0, true);
        t.check(hd.held_err_mm < 3.0 && hd.held_vel_err < 0.3, "predict.held-is-unpredicted", "heldErr=%.2f mm heldVelErr=%.2f m/s", hd.held_err_mm, hd.held_vel_err);
        t.check(hd.max_step_mm < 2.83 * 1.3 * 1000.0 / 144.0 + 3.0, "predict.held-transition-no-jump", "maxStep=%.1f mm", hd.max_step_mm);
        // 放開之後的 0.12 秒內還沒開始回到預測：遊戲算拋出速度的那幾格看到的是樣本那一刻的手
        t.check(hd.release_err_mm < 3.0, "predict.release-holds-before-ramping", "releaseErr=%.2f mm", hd.release_err_mm);
        // 還不知道 SteamVR 的預測時間（full＝0）：樣本原樣通過
        {
          vrdrv::ctrl_predictor_t cp;
          const float pos[3] = {0.1f, 0.2f, 0.3f}, rot[4] = {0, 0, 0, 1}, v1[3] = {1, 0, 0}, v2[3] = {2, 0, 0}, w[3] = {0, 0, 3};
          cp.on_fresh(pos, rot, v1, w, 1000000, 0.0, false, 1000000, 10'000'000);
          cp.on_fresh(pos, rot, v2, w, 1017361, 0.0, false, 1017361, 10'000'000);
          float op[3] = {9, 9, 9}, orot[4] = {9, 9, 9, 9}, ov[3] = {9, 9, 9}, ow[3] = {9, 9, 9};
          cp.apply(op, orot, ov, ow);
          t.check(op[0] == 0.1f && ov[0] == 2.0f && ow[2] == 3.0f && orot[3] == 1.0f && cp.lag_s() == 0.0, "predict.unknown-pred-passthrough");
          // 空窗（兩筆相隔 30 ms 以上）之後加速度歸零，不拿跨空窗的差分去放大
          cp.on_fresh(pos, rot, v1, w, 1017361 + 400000, 0.045, false, 1017361 + 400000, 10'000'000);
          t.check(cp.acc_len() == 0.0, "predict.gap-resets-acceleration");
        }
      }
      t.end();
    }

    // ════════════════════════════════════════════════════════════════════
    // §VR-GAME-MV（M-MV1）：(a) seqlock 撕裂壓力測試、(b) codec round-trip 與計時、(c) 配對規則
    // ════════════════════════════════════════════════════════════════════
    void test_mv_seqlock(tally_t &t) {
      t.begin("mv-seqlock");
      std::vector<uint8_t> raw_mem(VRMV_SECTION_SIZE + 64, 0);
      const uintptr_t raw_addr = reinterpret_cast<uintptr_t>(raw_mem.data());
      const uintptr_t aligned_addr = (raw_addr + 63u) & ~uintptr_t {63u};
      auto *shm = reinterpret_cast<vrmv_shm_t *>(aligned_addr);

      constexpr uint64_t k_gen = 7;
      constexpr uint16_t k_gw = 160;
      constexpr uint16_t k_gh = 90;
      constexpr uint32_t k_payload_bytes = static_cast<uint32_t>(k_gw) * static_cast<uint32_t>(k_gh) * VRMV_CELL_BYTES;
      constexpr size_t k_u64_words = k_payload_bytes / sizeof(uint64_t);

      shm->server.magic = VRMV_MAGIC_SHM;
      shm->server.abi_version = VRMV_ABI_VERSION;
      shm->server.section_size = VRMV_SECTION_SIZE;
      shm->server.generation = k_gen;

      std::atomic<bool> stop_writer {false};
      std::atomic<uint64_t> writes_done {0};

      // 寫入執行緒：高速循環寫入 desc + payload（每次 payload 全填同一個值 = 完成時的偶數 seq）
      std::thread writer([&]() {
        std::vector<uint64_t> write_buf(k_u64_words, 0);
        uint64_t seq_counter = 0;
        uint64_t frame_id = 0;
        while (!stop_writer.load(std::memory_order_relaxed)) {
          ++frame_id;
          const uint64_t expected_even_seq = (seq_counter * 2u) + 2u;
          std::fill(write_buf.begin(), write_buf.end(), expected_even_seq);

          vrmv_desc_t d {};
          d.mv_frame_id = frame_id;
          d.generation = k_gen;
          d.game_present_qpc = static_cast<int64_t>(frame_id * 1000);
          d.flags = VRMV_F_BOTH_EYES | VRMV_F_HAS_DEPTH;
          d.payload_bytes = k_payload_bytes;
          d.grid_w = k_gw;
          d.grid_h = k_gh;
          d.render_eye_w = 1280;
          d.render_eye_h = 1440;
          d.mv_tex_w = 1280;
          d.mv_tex_h = 1440;
          d.mv_units = VRMV_UNITS_MVTEX_PIXELS;
          d.upscaler_kind = VRMV_UPSCALER_DLSS;
          d.game_frame_step = 1;
          d.mv_scale[0] = 1.0f;
          d.mv_scale[1] = 1.0f;
          d.depth_ref = 1.0f;

          vr::mv_pair::shm_write_slot_for_test(shm, d, write_buf.data(), seq_counter);
          writes_done.fetch_add(1, std::memory_order_relaxed);
        }
      });

      // 讀取執行緒：連續讀 >= 220 ms，讀到一致時 payload 每個 word 都必須等於 desc.seq
      const int64_t start_qpc = qpc();
      const int64_t min_duration_qpc = 220 * own_qpf() / 1000;  // 220 ms（>= 200 ms）
      std::vector<uint64_t> read_buf(k_u64_words, 0);
      uint64_t consistent_reads = 0;
      uint64_t torn_leaks = 0;
      uint32_t slot_cursor = 0;

      while (qpc() - start_qpc < min_duration_qpc || consistent_reads < 10) {
        const uint32_t slot_idx = (slot_cursor++) % VRMV_SLOTS;
        vrmv_desc_t out_d {};
        if (vr::mv_pair::shm_read_slot_with_payload(shm, slot_idx, k_gen, out_d, read_buf.data(), k_payload_bytes)) {
          ++consistent_reads;
          const uint64_t expect_val = out_d.seq;
          if (expect_val == 0 || (expect_val & 1u) != 0) {
            ++torn_leaks;
          } else {
            for (size_t w = 0; w < k_u64_words; ++w) {
              if (read_buf[w] != expect_val) {
                ++torn_leaks;
                break;
              }
            }
          }
        }
        // 同時驗證兩段式 snapshot_descs + copy_payload
        if ((slot_cursor & 3u) == 0) {
          vrmv_desc_t snaps[VRMV_SLOTS] {};
          const size_t n = vr::mv_pair::shm_snapshot_descs(shm, k_gen, snaps, VRMV_SLOTS);
          if (n > 0) {
            const vrmv_desc_t &picked = snaps[n - 1];
            if (vr::mv_pair::shm_copy_payload(shm, picked, read_buf.data(), k_payload_bytes)) {
              ++consistent_reads;
              const uint64_t expect_val = picked.seq;
              for (size_t w = 0; w < k_u64_words; ++w) {
                if (read_buf[w] != expect_val) {
                  ++torn_leaks;
                  break;
                }
              }
            }
          }
        }
      }

      const int64_t elapsed_qpc = qpc() - start_qpc;
      stop_writer.store(true, std::memory_order_relaxed);
      writer.join();

      const double elapsed_ms = static_cast<double>(elapsed_qpc) * 1000.0 / static_cast<double>(own_qpf());
      t.check(elapsed_ms >= 200.0, "duration-ge-200ms", "elapsed=%.1fms", elapsed_ms);
      t.check(writes_done.load() > 0 && consistent_reads > 0, "reads-and-writes-active", "writes=%llu reads=%llu",
              static_cast<unsigned long long>(writes_done.load()), static_cast<unsigned long long>(consistent_reads));
      t.check(torn_leaks == 0, "zero-torn-payload", "torn_leaks=%llu consistent_reads=%llu writes=%llu",
              static_cast<unsigned long long>(torn_leaks), static_cast<unsigned long long>(consistent_reads),
              static_cast<unsigned long long>(writes_done.load()));
      t.end();
    }

    void test_mv_codec(tally_t &t) {
      t.begin("mv-codec");

      // 基本 half<->float 與量化邊界驗證
      {
        const uint16_t h_zero = vr::mv_codec::float_to_half(0.0f);
        const uint16_t h_one = vr::mv_codec::float_to_half(1.0f);
        const uint16_t h_neg = vr::mv_codec::float_to_half(-0.5f);
        t.check(vr::mv_codec::half_to_float(h_zero) == 0.0f &&
                  vr::mv_codec::half_to_float(h_one) == 1.0f &&
                  vr::mv_codec::half_to_float(h_neg) == -0.5f,
                "half-basic");
        t.check(vr::mv_codec::quantize_sbs_uv(std::numeric_limits<float>::quiet_NaN()) == 0 &&
                  vr::mv_codec::quantize_sbs_uv(std::numeric_limits<float>::infinity()) == 0 &&
                  vr::mv_codec::quantize_sbs_uv(-std::numeric_limits<float>::infinity()) == 0,
                "quantize-nan-inf-zero");
        t.check(vr::mv_codec::quantize_sbs_uv(10.0f) == 32767 &&
                  vr::mv_codec::quantize_sbs_uv(-10.0f) == -32767 &&
                  vr::mv_codec::quantize_sbs_uv(0.5f) == 2048 &&
                  vr::mv_codec::quantize_sbs_uv(0.0f) == 0,
                "quantize-clamp");
        t.check(vr::mv_codec::quantize_depth(-0.5f) == 0 &&
                  vr::mv_codec::quantize_depth(0.0f) == 0 &&
                  vr::mv_codec::quantize_depth(1.0f) == 63 &&
                  vr::mv_codec::quantize_depth(0.25f) == 32,
                "quantize-depth");
      }

      // v2 降採樣：2×2 內取逆深度最大（前景）那格的位移；右眼取右眼自己的格子；死區把 |q|≤1 歸 0
      {
        vrmv_desc_t d {};
        d.flags = VRMV_F_BOTH_EYES | VRMV_F_HAS_DEPTH;
        d.grid_w = 8;
        d.grid_h = 2;
        d.payload_bytes = 8 * 2 * VRMV_CELL_BYTES;
        d.mv_units = VRMV_UNITS_EYE_UV;
        std::vector<uint16_t> c(8 * 2 * 4, vr::mv_codec::float_to_half(0.0f));
        auto put = [&](int x, int y, float u, float v, float z) {
          const size_t i = static_cast<size_t>(y) * 8 + x;
          c[i * 4 + 0] = vr::mv_codec::float_to_half(u);
          c[i * 4 + 1] = vr::mv_codec::float_to_half(v);
          c[i * 4 + 2] = vr::mv_codec::float_to_half(z);
        };
        put(0, 0, 0.0f, 0.0f, 0.2f);   // 左眼第一個 2×2：背景
        put(1, 1, 0.25f, -0.125f, 0.9f);  // 同一塊的前景 → 該被選
        put(4, 0, 0.0001f, 0.0f, 0.5f);  // 右眼第一塊：極小位移 → 死區歸 0
        uint16_t ow = 0, oh = 0;
        const bool dims_ok = vr::mv_codec::output_grid_dims(d, ow, oh) && ow == 4 && oh == 1;
        int16_t vx[4] {}, vy[4] {};
        uint8_t dz[4] {};
        const bool q_ok = dims_ok && vr::mv_codec::quantize_rgba16f_to_planes(d, c.data(), d.payload_bytes, vx, vy, dz);
        // 左眼 u=0.25 → SBS 0.125 → ×4096 = 512；v=-0.125 → -512；深度 sqrt(0.9)·63 ≈ 60
        t.check(q_ok && vx[0] == 512 && vy[0] == -512 && dz[0] == 60 && vx[2] == 0 && vy[2] == 0,
                "downsample-foreground-pick", "vx0=%d vy0=%d d0=%d vx2=%d", vx[0], vy[0], dz[0], vx[2]);
      }

      struct grid_shape_t {
        uint16_t w;
        uint16_t h;
      };
      const grid_shape_t shapes[] = {{360, 180}, {540, 270}};
      const char *pattern_names[] = {"random", "smooth", "mostly-zero"};

      uint32_t rng_state = 0x5649504Cu;
      auto next_rng = [&]() -> uint32_t {
        uint32_t x = rng_state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return rng_state = x;
      };

      std::vector<uint8_t> scratch_planar;
      for (const auto &sh : shapes) {
        const size_t n_cells = static_cast<size_t>(sh.w) * static_cast<size_t>(sh.h);
        vrmv_desc_t desc {};
        desc.seq = 2;
        desc.mv_frame_id = 1;
        desc.generation = 1;
        desc.flags = VRMV_F_BOTH_EYES | VRMV_F_HAS_DEPTH;
        desc.grid_w = sh.w;
        desc.grid_h = sh.h;
        desc.payload_bytes = static_cast<uint32_t>(n_cells * VRMV_CELL_BYTES);
        desc.mv_tex_w = static_cast<uint16_t>((sh.w / 2) * 8);
        desc.mv_tex_h = static_cast<uint16_t>(sh.h * 8);
        desc.mv_units = VRMV_UNITS_MVTEX_PIXELS;
        desc.mv_scale[0] = 1.0f;
        desc.mv_scale[1] = 1.0f;
        desc.depth_ref = 1.0f;

        std::vector<uint16_t> rgba16f(n_cells * 4, 0);
        uint16_t out_w = 0, out_h = 0;
        vr::mv_codec::output_grid_dims(desc, out_w, out_h);
        const size_t n_out = static_cast<size_t>(out_w) * out_h;  // v2：1/16 網格
        std::vector<int16_t> ref_vx(n_out, 0);
        std::vector<int16_t> ref_vy(n_out, 0);
        std::vector<uint8_t> ref_depth(n_out, 0);
        std::vector<uint8_t> compressed;

        for (int pat = 0; pat < 3; ++pat) {
          for (uint16_t y = 0; y < sh.h; ++y) {
            for (uint16_t x = 0; x < sh.w; ++x) {
              const size_t idx = static_cast<size_t>(y) * sh.w + x;
              float px = 0.0f;
              float py = 0.0f;
              float pz = 0.0f;
              if (pat == 0) {
                // 隨機網格（含正負與極值）
                const int32_t rx = static_cast<int32_t>(next_rng() % 2001u) - 1000;
                const int32_t ry = static_cast<int32_t>(next_rng() % 2001u) - 1000;
                px = static_cast<float>(rx) * 0.25f;
                py = static_cast<float>(ry) * 0.25f;
                pz = static_cast<float>(next_rng() % 1001u) / 1000.0f;
              } else if (pat == 1) {
                // 平滑網格（模擬鏡頭平移與深度漸層）
                px = 12.5f + 4.0f * std::sin(static_cast<float>(x) * 0.03f) + 2.0f * std::cos(static_cast<float>(y) * 0.04f);
                py = -6.0f + 3.0f * std::cos(static_cast<float>(x) * 0.02f) - 1.5f * std::sin(static_cast<float>(y) * 0.05f);
                pz = std::clamp(0.1f + 0.8f * (static_cast<float>(y) / static_cast<float>(sh.h)), 0.0f, 1.0f);
              } else {
                // 大面積 0 網格（僅中央小區塊有非零位移，其餘皆 0）
                if (x >= sh.w * 3 / 8 && x < sh.w * 5 / 8 && y >= sh.h * 3 / 8 && y < sh.h * 5 / 8) {
                  px = 8.0f;
                  py = -4.0f;
                  pz = 0.5f;
                } else {
                  px = 0.0f;
                  py = 0.0f;
                  pz = 0.0f;
                }
              }
              rgba16f[idx * 4 + 0] = vr::mv_codec::float_to_half(px);
              rgba16f[idx * 4 + 1] = vr::mv_codec::float_to_half(py);
              rgba16f[idx * 4 + 2] = vr::mv_codec::float_to_half(pz);
              rgba16f[idx * 4 + 3] = vr::mv_codec::float_to_half(1.0f);
            }
          }

          const bool q_ok = vr::mv_codec::quantize_rgba16f_to_planes(
            desc, rgba16f.data(), desc.payload_bytes, ref_vx.data(), ref_vy.data(), ref_depth.data()
          );
          const bool enc_ok = vr::mv_codec::encode_rgba16f(
            desc, rgba16f.data(), desc.payload_bytes, compressed, &scratch_planar
          );
          vr::mv_codec::decoded_planes_t dec {};
          const bool dec_ok = enc_ok && vr::mv_codec::decode(compressed.data(), compressed.size(), dec, &scratch_planar);

          const bool match_vx = dec_ok && dec.vx.size() == n_out &&
                                std::memcmp(dec.vx.data(), ref_vx.data(), n_out * sizeof(int16_t)) == 0;
          const bool match_vy = dec_ok && dec.vy.size() == n_out &&
                                std::memcmp(dec.vy.data(), ref_vy.data(), n_out * sizeof(int16_t)) == 0;
          const bool match_d = dec_ok && dec.depth.size() == n_out &&
                               std::memcmp(dec.depth.data(), ref_depth.data(), n_out * sizeof(uint8_t)) == 0;

          char case_name[64];
          snprintf(case_name, sizeof(case_name), "roundtrip-%ux%u-%s", sh.w, sh.h, pattern_names[pat]);
          t.check(q_ok && enc_ok && dec_ok && match_vx && match_vy && match_d, case_name,
                  "compBytes=%zu vx=%d vy=%d d=%d", compressed.size(), match_vx ? 1 : 0, match_vy ? 1 : 0, match_d ? 1 : 0);

          // 量測 360x180 編碼＋解碼時間並印出（目標 < 0.05 ms，超過也如實印出）
          if (sh.w == 360 && sh.h == 180) {
            constexpr int k_iters = 50;
            const int64_t t0 = qpc();
            for (int it = 0; it < k_iters; ++it) {
              vr::mv_codec::encode_rgba16f(desc, rgba16f.data(), desc.payload_bytes, compressed, &scratch_planar);
            }
            const int64_t t1 = qpc();
            for (int it = 0; it < k_iters; ++it) {
              vr::mv_codec::decode(compressed.data(), compressed.size(), dec, &scratch_planar);
            }
            const int64_t t2 = qpc();
            const double enc_ms = static_cast<double>(t1 - t0) * 1000.0 / (static_cast<double>(own_qpf()) * k_iters);
            const double dec_ms = static_cast<double>(t2 - t1) * 1000.0 / (static_cast<double>(own_qpf()) * k_iters);
            const double total_ms = enc_ms + dec_ms;
            line("unit-metric name=mv_codec_360x180 pattern=%s encode_ms=%.4f decode_ms=%.4f total_ms=%.4f comp_bytes=%zu target_ms=0.0500",
                 pattern_names[pat], enc_ms, dec_ms, total_ms, compressed.size());
          }
        }
      }
      t.end();
    }

    void test_mv_pair(tally_t &t) {
      t.begin("mv-pair");
      const int64_t freq = 10'000'000;        // 10 MHz (1 ms = 10,000 ticks)
      const int64_t t_vsync = freq / 90;      // 111,111 ticks (~11.11 ms); 1.5 * T_vsync = 166,666 ticks (~16.67 ms)
      const int64_t drv_qpc = 1'000'000'000;

      // gpu_done 預設＝game_present + 7 ms（Palworld 實測 p50 7.5 ms）
      auto make_cand = [&](uint64_t id, int64_t game_qpc, uint32_t flags = VRMV_F_BOTH_EYES, int64_t gpu_lag = 70'000) {
        vrmv_desc_t d {};
        d.seq = id * 2;
        d.mv_frame_id = id;
        d.generation = 1;
        d.game_present_qpc = game_qpc;
        d.gpu_done_qpc = game_qpc + gpu_lag;
        d.publish_qpc = d.gpu_done_qpc + 800;
        d.flags = flags;
        d.grid_w = 360;
        d.grid_h = 180;
        d.payload_bytes = 360 * 180 * VRMV_CELL_BYTES;
        d.mv_tex_w = 1440;
        d.mv_tex_h = 1440;
        d.mv_units = VRMV_UNITS_EYE_UV;
        d.mv_scale[0] = 1.0f;
        d.mv_scale[1] = 1.0f;
        d.depth_ref = 1.0f;
        return d;
      };

      // 1. 正常命中：GPU 在 Present 前 3 ms 做完
      {
        const vrmv_desc_t cands[] = {make_cand(10, drv_qpc - 100'000)};
        const auto r = vr::mv_pair::match_mv_frame(cands, 1, drv_qpc, 9, t_vsync, freq);
        t.check(r.hit && r.matched.mv_frame_id == 10 && r.k_step == 1 && r.miss_reason == vr::mv_pair::miss_reason_e::none,
                "normal-hit");
      }

      // 1b. GPU 吃滿：Present→driver Present 22 ms（舊規則的 1.5·T_vsync 會判太舊）仍命中
      {
        const vrmv_desc_t cands[] = {make_cand(10, drv_qpc - 220'000, VRMV_F_BOTH_EYES, 110'000)};
        const auto r = vr::mv_pair::match_mv_frame(cands, 1, drv_qpc, 9, t_vsync, freq);
        t.check(r.hit && r.matched.mv_frame_id == 10, "gpu-bound-hit", "reason=%s", vr::mv_pair::miss_reason_name(r.miss_reason));
      }

      // 2. 太舊（> max(6·T_vsync, 50 ms) = 66.7 ms）
      {
        const vrmv_desc_t cands[] = {make_cand(11, drv_qpc - 800'000)};
        const auto r = vr::mv_pair::match_mv_frame(cands, 1, drv_qpc, 10, t_vsync, freq);
        t.check(!r.hit && r.miss_reason == vr::mv_pair::miss_reason_e::too_old,
                "miss-too-old", "reason=%s", vr::mv_pair::miss_reason_name(r.miss_reason));
      }

      // 3. 太新：GPU 在 Present 之後才做完（不可能是被合成的那張）
      {
        const vrmv_desc_t cands[] = {make_cand(12, drv_qpc - 20'000)};  // gpu_done = Present + 5 ms
        const auto r = vr::mv_pair::match_mv_frame(cands, 1, drv_qpc, 10, t_vsync, freq);
        t.check(!r.hit && r.miss_reason == vr::mv_pair::miss_reason_e::too_new,
                "miss-too-new", "reason=%s", vr::mv_pair::miss_reason_name(r.miss_reason));
      }

      // 4. 缺眼（只有左眼或只有右眼）
      {
        const vrmv_desc_t cands_l[] = {make_cand(13, drv_qpc - 100'000, VRMV_F_LEFT_VALID)};
        const vrmv_desc_t cands_r[] = {make_cand(14, drv_qpc - 100'000, VRMV_F_RIGHT_VALID)};
        const auto rl = vr::mv_pair::match_mv_frame(cands_l, 1, drv_qpc, 10, t_vsync, freq);
        const auto rr = vr::mv_pair::match_mv_frame(cands_r, 1, drv_qpc, 10, t_vsync, freq);
        t.check(!rl.hit && rl.miss_reason == vr::mv_pair::miss_reason_e::missing_eye &&
                  !rr.hit && rr.miss_reason == vr::mv_pair::miss_reason_e::missing_eye,
                "miss-missing-eye");
      }

      // 5. 已用過＝合成器重送同一張（最新一筆已配過，不退回去配更舊的）
      {
        const vrmv_desc_t cands[] = {make_cand(14, drv_qpc - 300'000), make_cand(15, drv_qpc - 100'000)};
        const auto r_eq = vr::mv_pair::match_mv_frame(cands, 2, drv_qpc, 15, t_vsync, freq);
        const auto r_lt = vr::mv_pair::match_mv_frame(cands, 2, drv_qpc, 16, t_vsync, freq);
        t.check(!r_eq.hit && r_eq.miss_reason == vr::mv_pair::miss_reason_e::already_used &&
                  !r_lt.hit && r_lt.miss_reason == vr::mv_pair::miss_reason_e::already_used,
                "miss-already-used");
      }

      // 6. 多筆：取 Present 前已做完的最新一筆；GPU 還沒做完的那筆不算
      {
        const vrmv_desc_t cands[] = {
          make_cand(20, drv_qpc - 400'000),  // 做完、較舊
          make_cand(21, drv_qpc - 150'000),  // 做完、最新 → 選它
          make_cand(22, drv_qpc - 30'000),  // gpu_done = Present + 4 ms：還沒做完
        };
        const auto r = vr::mv_pair::match_mv_frame(cands, 3, drv_qpc, 19, t_vsync, freq);
        t.check(r.hit && r.matched.mv_frame_id == 21 && r.index == 1 && r.k_step == 2,
                "pick-newest-completed", "matched=%llu k_step=%u",
                static_cast<unsigned long long>(r.matched.mv_frame_id), r.k_step);
      }

      // 6b. pending：最新一幀 GPU 在 Present 前做完但還沒發布 → 不拿較舊的 21 頂替；pending 的 GPU 在 Present 後才做完 → 照配 21
      {
        const vrmv_desc_t cands[] = {make_cand(20, drv_qpc - 400'000), make_cand(21, drv_qpc - 150'000)};
        const auto r_p = vr::mv_pair::match_mv_frame(cands, 2, drv_qpc, 19, t_vsync, freq, 22, drv_qpc - 5'000);
        const auto r_np = vr::mv_pair::match_mv_frame(cands, 2, drv_qpc, 19, t_vsync, freq, 22, drv_qpc + 5'000);
        const auto r_same = vr::mv_pair::match_mv_frame(cands, 2, drv_qpc, 19, t_vsync, freq, 21, drv_qpc - 80'000);
        t.check(!r_p.hit && r_p.miss_reason == vr::mv_pair::miss_reason_e::pending &&
                  r_np.hit && r_np.matched.mv_frame_id == 21 && r_same.hit && r_same.matched.mv_frame_id == 21,
                "pending-not-substituted", "p=%s np=%d same=%d", vr::mv_pair::miss_reason_name(r_p.miss_reason), r_np.hit ? 1 : 0, r_same.hit ? 1 : 0);
      }

      // 7. K_step = clamp(M_cur - M_prev, 1, 4) 夾限
      {
        t.check(vr::mv_pair::compute_k_step(10, 0) == 1, "kstep-first-frame");
        t.check(vr::mv_pair::compute_k_step(11, 10) == 1, "kstep-1");
        t.check(vr::mv_pair::compute_k_step(12, 10) == 2, "kstep-2");
        t.check(vr::mv_pair::compute_k_step(13, 10) == 3, "kstep-3");
        t.check(vr::mv_pair::compute_k_step(14, 10) == 4, "kstep-4");
        t.check(vr::mv_pair::compute_k_step(25, 10) == 4, "kstep-clamp-max-4");
        t.check(vr::mv_pair::compute_k_step(10, 10) == 1, "kstep-clamp-min-1");
      }

      t.end();
    }

  }  // namespace

  int run_unit(const args_t &a) {
    // T6：先印 ABI 表（selftest 逐字比對 server（GCC）與 vr_probe（MSVC）的版面）
    print_abi_table();

    tally_t t;
    t.verbose = a.unit_verbose;
    test_messages(t);
    test_validation(t);
    test_rings(t);
    test_paths(t);
    test_log_throttle(t);
    test_v4(t);
    test_pose_history(t);
    test_pose_policy(t);
    test_stale_blender(t);
    test_mv_seqlock(t);
    test_mv_codec(t);
    test_mv_pair(t);
    if (a.no_loopback) {
      line("unit-skip case=loop reason=no-loopback");
    } else {
      // loopback 的 ipc_client log 轉到 stdout（診斷用；不計分）
      vrdrv::log::set_sink([](void *, int, const char *text) {
        line("drv %s", text);
      },
                           nullptr);
      test_loopback(t);
      vrdrv::log::set_sink(nullptr, nullptr);
    }
    line("unit pass=%d fail=%d", t.pass, t.fail);
    return t.fail == 0 ? rc_ok : rc_failed;
  }

}  // namespace probe
