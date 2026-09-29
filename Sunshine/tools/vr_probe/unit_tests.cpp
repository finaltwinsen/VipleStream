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

#include <windows.h>

#include "driver_log.h"
#include "driver_version.h"
#include "ipc_client.h"
#include "ipc_proto.h"
#include "pose_history.h"
#include "probe_common.h"
#include "probe_d3d.h"
#include "virtual_vsync.h"
#include "vr_math.h"
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

      void check(bool ok, const char *name, const char *fmt = nullptr, ...) {
        if (ok) {
          ++pass;
          ++group_pass;
          return;
        }
        ++fail;
        ++group_fail;
        char detail[200] = "";
        if (fmt) {
          va_list ap;
          va_start(ap, fmt);
          vsnprintf(detail, sizeof(detail), fmt, ap);
          va_end(ap);
        }
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
        w.abi_version = 2;
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

  }  // namespace

  int run_unit(const args_t &a) {
    // T6：先印 ABI 表（selftest 逐字比對 server（GCC）與 vr_probe（MSVC）的版面）
    print_abi_table();

    tally_t t;
    test_messages(t);
    test_validation(t);
    test_rings(t);
    test_paths(t);
    test_log_throttle(t);
    test_v4(t);
    test_pose_history(t);
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
