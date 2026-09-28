// ipcpeer.cpp - VipleStream §VR：vr_probe --mode ipcpeer（設計 §F.4、§F.5 T2）。
//
// 以 driver 同一份 ipc_client.cpp（peer_mode，/DVRDRV_PEER_HOOKS）當「假 driver」連 server 的 VR pipe：
//   HELLO → WELCOME（map shm）→ TEXTURES（在 server 指定的 adapter 上開 server 建的 ring／fence）→ READY，
//   之後以 config 的更新率在 ring 上畫測試圖樣（ClearRenderTargetView，不需要 shader）、Signal sharedFence、
//   Flush、發布 frame descriptor（§B.7 driver 端），並讀 tracking ring 檢查撕裂與倒退。
//
// 異常注入（只在 peer；driver DLL 沒有這些程式碼路徑）：
//   --abi N / --hello-size B         HELLO 的 abi_version／大小（ABI 不同、大小不符）
//   --peer-fence-max [--peer-fence-max-after N]   第 N 幀之後把 sharedFence Signal 成 UINT64_MAX（每個行程一次）
//   --peer-stop-signal-after N       第 N 幀之後照常發布 descriptor，但不再 Signal（server 端 fence 逾時）
//   --peer-crash-after N             第 N 幀之後 TerminateProcess（不送 BYE，模擬 driver 當掉）
//   --peer-never-read                MAPPED 之後不再讀 pipe（server 寫入逾時）
//   --peer-close-after-welcome       MAPPED 之後關掉兩個 event、開新物件佔用同值（sec-M1 所有權）
//   --peer-escalate                  對 WELCOME 的 handle 嘗試 DuplicateHandle(self→self, *_ALL_ACCESS)（U30）
//   --peer-no-flush                  Signal 之後不 Flush（E-3 對照）
//   --peer-no-render                 握手後不畫、不發布（只測握手；T6 取 ABI 表時連不存在的 pipe 也用它）
//   --hello-repeat N [--hello-interval-ms M]      固定間隔重連 N 次（server 的限速）
//   --pipe NAME                      連別的 pipe（不存在時一直重試到 --seconds 用完，不提早結束）
//
// stdout（selftest 以 sanitize_probe_line 過濾後解析 key=value）：
//   開頭 abi-row ×N 與 abi-rows count= digest=（T6 逐字比對）；
//   ipcpeer mapped／textures／teardown／state／reject reason=<n> detail=<n>／summary …；
//   ipcpeer tracking reads= torn= backwards= patternChecked= patternBad=（selftest 合成 tracking 的逐欄核對，T2.dual-writer）；
//   drv <ipc_client 的 log 行>（driver_log 的 sink，每秒最多 20 行；行本身以 "ipc " 開頭）。
// 所有行都不含 handle 值（只印 GrantedAccess 這類權限遮罩）。
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
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
#include "probe_common.h"
#include "probe_d3d.h"

namespace probe {
  namespace {

    // ── handle 權限查詢（NtQueryObject，ObjectBasicInformation）──────────────
    // 自己宣告結構（winternl.h 的版本在不同 SDK 間不一致）；以 GetProcAddress 取得，不連 ntdll.lib。
    struct object_basic_info_t {
      ULONG attributes;
      ACCESS_MASK granted_access;
      ULONG handle_count;
      ULONG pointer_count;
      ULONG reserved[10];
    };

    using nt_query_object_t = LONG(NTAPI *)(HANDLE, int, PVOID, ULONG, PULONG);

    // 失敗回 -1（只記錄，不影響情境）
    int64_t granted_access(uint64_t handle_value) {
      static nt_query_object_t fn = []() -> nt_query_object_t {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<nt_query_object_t>(reinterpret_cast<void *>(GetProcAddress(ntdll, "NtQueryObject"))) : nullptr;
      }();
      if (!fn || !vrdrv::proto::plausible_handle_value(handle_value)) {
        return -1;
      }
      object_basic_info_t info {};
      ULONG ret = 0;
      const LONG st = fn((HANDLE) (uintptr_t) handle_value, 0 /* ObjectBasicInformation */, &info, (ULONG) sizeof(info), &ret);
      return st < 0 ? -1 : (int64_t) info.granted_access;
    }

    const char *reject_name(uint32_t r) {
      switch (r) {
        case VRIPC_REJ_ABI_MISMATCH:
          return "abi-mismatch";
        case VRIPC_REJ_IDENTITY:
          return "identity";
        case VRIPC_REJ_BAD_MESSAGE:
          return "bad-message";
        case VRIPC_REJ_ADAPTER_MISMATCH:
          return "adapter-mismatch";
        case VRIPC_REJ_TEXTURE_INVALID:
          return "texture-invalid";
        case VRIPC_REJ_BUSY:
          return "busy";
        case VRIPC_REJ_VR_DISABLED:
          return "vr-disabled";
        case VRIPC_REJ_TIMEOUT:
          return "timeout";
        case VRIPC_REJ_GENERATION:
          return "generation";
        default:
          return "unknown";
      }
    }

    // ── per-generation 輸出目標（TEXTURES 開好的 ring／fence）───────────────
    struct output_t {
      d3d::device_t dev;  ///< 開這個 ring 的 device（ComPtr 複本：TDR 後重建 device 時，舊 ring 仍配舊 context）
      uint64_t generation = 0;
      uint32_t refresh_mhz = 0;
      uint16_t space_epoch = 0;
      uint8_t layout_epoch = 0;
      d3d::opened_ring_t ring;
    };

    struct peer_state_t {
      // direct-mode device 的替身：第一次 TEXTURES 時依 LUID 建立，之後 LUID 不同就 REJECT(ADAPTER_MISMATCH)（K19）
      std::mutex dev_mtx;
      d3d::device_t dev;
      bool have_dev = false;

      std::mutex out_mtx;
      std::shared_ptr<output_t> out;

      // 統計（ipc 執行緒與主執行緒都會寫）
      std::atomic<uint64_t> mapped {0};
      std::atomic<uint64_t> textures_ok {0};
      std::atomic<uint64_t> textures_fail {0};
      std::atomic<uint64_t> teardowns {0};
      std::atomic<uint64_t> rejects {0};
      std::atomic<uint32_t> last_reject {0};
      std::atomic<uint32_t> last_reject_detail {0};
      std::atomic<uint64_t> server_states {0};
      std::atomic<uint64_t> attempts {0};
      std::atomic<int64_t> dup_shm {-2};  // -2 = 沒收到 WELCOME
      std::atomic<int64_t> dup_trk {-2};
      std::atomic<int64_t> dup_frm {-2};

      // close-after-welcome：佔用同值的新物件
      std::mutex occ_mtx;
      std::vector<HANDLE> occupants;
      uint32_t occupied_same_value = 0;
    };

    void log_sink(void *, int level, const char *text) {
      line("drv %s%s", level >= 2 ? "E " : (level == 1 ? "W " : ""), text);
    }

    // 只量化到幀週期的粗略延遲（submit → consumedFence 到值，於下一次輪詢時觀察）
    struct lag_stats_t {
      std::vector<double> ms;

      void add(double v) {
        if (ms.size() < 100000) {
          ms.push_back(v);
        }
      }

      double pct(double q) {
        if (ms.empty()) {
          return 0.0;
        }
        std::vector<double> v = ms;
        std::sort(v.begin(), v.end());
        const size_t i = (size_t) (q * (double) (v.size() - 1) + 0.5);
        return v[i < v.size() ? i : v.size() - 1];
      }
    };

    // selftest T2.dual-writer 的撕裂檢查圖樣（server 端 vr_selftest_win.cpp 的 synth_sample()，兩邊必須相同）：
    // R 控制器角速度 z = 12345 rad/s 是標記（真實資料不會出現）；其餘欄位都由 sampleId 推得。seqlock 讀到的一筆
    // 若混了兩個樣本（撕裂），各欄與 sampleId 就對不上＝pose 不連續。
    constexpr float k_trk_pattern_marker = 12345.0f;

    bool trk_pattern_marked(const vripc_tracking_slot_t &s) {
      return s.pose[2].ang_vel[2] == k_trk_pattern_marker;
    }

    bool trk_pattern_ok(const vripc_tracking_slot_t &s) {
      const uint32_t id = s.sample_id;
      return s.pose[2].ang_vel[0] == (float) (id & 0xFFFFu) && s.pose[2].ang_vel[1] == (float) (id >> 16) && s.input[0].buttons == id &&
             s.input[1].buttons == ~id && s.gaze_yaw_f16 == (uint16_t) (id ^ 0xA5A5u) && s.gaze_pitch_f16 == (uint16_t) (id >> 16);
    }

    // 測試圖樣的顏色：每幀不同（讓 server 端若讀回像素能看出不是同一幀）
    void pattern_color(uint64_t frame, float out[4]) {
      out[0] = (float) (frame % 256) / 255.0f;
      out[1] = (float) ((frame / 256) % 256) / 255.0f;
      out[2] = 0.5f;
      out[3] = 1.0f;
    }

  }  // namespace

  int run_ipcpeer(const args_t &a) {
    // T6：ABI 表一律在連線前印（--pipe 指到不存在的 pipe 也拿得到表）
    print_abi_table();

    // callback 以參照捕捉 st；ipc 執行緒 stop 逾時時 st 刻意洩漏（行程隨即結束），不能放在堆疊上
    auto *st_owner = new peer_state_t();
    peer_state_t &st = *st_owner;
    vrdrv::log::set_sink(&log_sink, nullptr);

    std::wstring out_dir;
    csv_t csv;
    if (prepare_out_dir(a.out, out_dir)) {
      csv.open(out_dir, L"ipcpeer.csv", "generation,frame_id,slot,fence,present_qpc,submit_qpc,signal,flush");
    }

    if (a.gpu_hold_ms != 0) {
      line("ipcpeer warn option=peer-gpu-hold-ms status=not-implemented until=V3");
    }
    if (a.writers > 1) {
      line("ipcpeer info option=peer-writers value=%u (server-side writers; the peer always checks torn/backwards reads)", a.writers);
    }

    vrdrv::ipc_client_config_t cfg;
    cfg.pipe_name = a.pipe;
    cfg.driver_version_packed = VIPLE_DRIVER_VERSION_PACKED;
    cfg.driver_caps = 0;
    cfg.openvr_sdk_packed = vrdrv::proto::pack_sdk(2, 15, 6);
    cfg.iface_directmode = "IVRDriverDirectModeComponent_009";
    cfg.peer_mode = true;
    cfg.hooks.hello_abi = a.abi != 0 ? a.abi : VRIPC_ABI_VERSION;
    cfg.hooks.hello_size = a.hello_size != 0 ? a.hello_size : (uint32_t) sizeof(vripc_hello_t);
    cfg.hooks.never_read_after_mapped = a.never_read;
    cfg.hooks.close_after_welcome = a.close_after_welcome;
    cfg.hooks.escalate = a.escalate;
    cfg.hooks.ignore_dead = a.hello_repeat != 0;
    cfg.hooks.fixed_retry_ms = a.hello_repeat != 0 ? a.hello_interval_ms : 0;
    cfg.hooks.on_attempt = [&st](const char *outcome, uint32_t detail) {
      const uint64_t n = st.attempts.fetch_add(1) + 1;
      line("ipcpeer attempt n=%llu outcome=%s detail=%u", (unsigned long long) n, outcome, detail);
    };
    cfg.hooks.on_welcome = [&st](const vripc_welcome_t &w) {
      st.dup_shm.store(granted_access(w.dup_shm_handle));
      st.dup_trk.store(granted_access(w.dup_evt_trk));
      st.dup_frm.store(granted_access(w.dup_evt_frm));
    };
    cfg.hooks.on_handles_closed = [&st](uint32_t count, const uint64_t *values) {
      // 開新物件：Windows 的 handle table 通常優先重用剛釋放的值；只比對、不記錄值本身
      std::lock_guard<std::mutex> lk(st.occ_mtx);
      for (uint32_t i = 0; i < count; ++i) {
        HANDLE h = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        if (!h) {
          continue;
        }
        st.occupants.push_back(h);
        for (uint32_t k = 0; k < count; ++k) {
          if ((uint64_t) (uintptr_t) h == values[k]) {
            ++st.occupied_same_value;
          }
        }
      }
      line("ipcpeer close-after-welcome closed=%u occupiedSameValue=%u", count, st.occupied_same_value);
    };

    vrdrv::ipc_callbacks_t cb;
    cb.on_mapped = [&st](const vrdrv::generation_ptr &gen, bool armed) {
      st.mapped.fetch_add(1);
      const auto &c = gen->config();
      line("ipcpeer mapped gen=%llu idle=%d ring=%ux%u hz=%u armed=%d devPacing=%d", (unsigned long long) gen->id(), gen->idle() ? 1 : 0, c.packed_width, c.packed_height, c.refresh_mhz / 1000, armed ? 1 : 0, gen->dev_pacing() ? 1 : 0);
    };
    cb.on_textures = [&st](const vrdrv::generation_ptr &gen, const vripc_textures_t &msg, const HANDLE tex[VRIPC_TEX_COUNT], HANDLE shared_fence, HANDLE consumed_fence) {
      vrdrv::textures_result_t r;
      LUID luid {};
      luid.LowPart = msg.adapter_luid_low;
      luid.HighPart = msg.adapter_luid_high;
      std::lock_guard<std::mutex> lk(st.dev_mtx);
      if (st.have_dev && (st.dev.luid.LowPart != luid.LowPart || st.dev.luid.HighPart != luid.HighPart)) {
        r.reject_reason = VRIPC_REJ_ADAPTER_MISMATCH;
        st.textures_fail.fetch_add(1);
        line("ipcpeer textures gen=%llu result=reject reason=adapter-mismatch", (unsigned long long) gen->id());
        return r;
      }
      if (!st.have_dev || st.dev.dev->GetDeviceRemovedReason() != S_OK) {
        const HRESULT hr = d3d::create_device(&luid, st.dev);
        if (FAILED(hr)) {
          r.reject_reason = VRIPC_REJ_TEXTURE_INVALID;
          r.reject_detail = (uint32_t) hr;
          st.textures_fail.fetch_add(1);
          line("ipcpeer textures gen=%llu result=reject reason=create-device hr=0x%08x", (unsigned long long) gen->id(), (unsigned) hr);
          return r;
        }
        st.have_dev = true;
      }
      auto out = std::make_shared<output_t>();
      const char *why = "";
      const HRESULT hr = d3d::open_ring(st.dev, tex, shared_fence, consumed_fence, msg.width, msg.height, out->ring, why);
      if (FAILED(hr)) {
        r.reject_reason = VRIPC_REJ_TEXTURE_INVALID;
        r.reject_detail = (uint32_t) hr;
        st.textures_fail.fetch_add(1);
        line("ipcpeer textures gen=%llu result=reject reason=%s hr=0x%08x", (unsigned long long) gen->id(), why, (unsigned) hr);
        return r;
      }
      out->dev = st.dev;
      out->generation = gen->id();
      out->refresh_mhz = gen->config().refresh_mhz;
      out->space_epoch = gen->config().space_epoch;
      out->layout_epoch = gen->config().layout_epoch;
      {
        std::lock_guard<std::mutex> lk2(st.out_mtx);
        st.out = out;
      }
      st.textures_ok.fetch_add(1);
      line("ipcpeer textures gen=%llu result=ok ring=%ux%u", (unsigned long long) gen->id(), msg.width, msg.height);
      r.ok = true;
      r.reject_reason = 0;
      return r;
    };
    cb.on_teardown = [&st](uint64_t generation, const char *reason) {
      {
        std::lock_guard<std::mutex> lk(st.out_mtx);
        st.out.reset();  // 主執行緒手上的 shared_ptr 放掉後才真的釋放 COM 參照
      }
      st.teardowns.fetch_add(1);
      line("ipcpeer teardown gen=%llu reason=%s", (unsigned long long) generation, reason);
    };
    cb.on_server_state = [&st](uint32_t kind, uint32_t arg) {
      st.server_states.fetch_add(1);
      line("ipcpeer state kind=0x%02x arg=%u", kind, arg);
    };
    cb.on_server_heartbeat = [](bool stale) {
      line("ipcpeer server-heartbeat %s", stale ? "stale" : "recovered");
    };
    cb.on_rejected = [&st](uint32_t reason, uint32_t detail) {
      st.rejects.fetch_add(1);
      st.last_reject.store(reason);
      st.last_reject_detail.store(detail);
      line("ipcpeer reject reason=%u detail=%u name=%s", reason, detail, reject_name(reason));
    };

    // client 物件活到 stop() 成功為止；stop 逾時時刻意洩漏（ipc 執行緒還在用它），行程隨即結束
    auto *client = new vrdrv::ipc_client_t();
    if (!client->start(cfg, cb)) {
      line("ipcpeer error reason=start-failed");
      delete client;  // 執行緒沒起來：可以安全釋放
      delete st_owner;
      vrdrv::log::set_sink(nullptr, nullptr);
      return rc_failed;
    }

    // tracking 讀取執行緒：等 evtTrk、讀最新一筆，檢查撕裂與 sampleId 倒退（雙寫入者情境的 peer 端證據）
    std::atomic<bool> quit {false};
    std::atomic<uint64_t> trk_reads {0};
    std::atomic<uint64_t> trk_torn {0};
    std::atomic<uint64_t> trk_backwards {0};
    std::atomic<uint64_t> trk_pattern_checked {0};
    std::atomic<uint64_t> trk_pattern_bad {0};
    std::atomic<uint32_t> trk_last_id {0};
    std::thread trk_thread([&]() {
      uint64_t cur_gen = 0;
      uint32_t last_id = 0;
      bool have_last = false;
      while (!quit.load()) {
        auto gen = client->current();
        if (!gen || gen->idle() || !gen->evt_trk()) {
          Sleep(20);
          continue;
        }
        if (gen->id() != cur_gen) {
          cur_gen = gen->id();
          have_last = false;
        }
        if (WaitForSingleObject(gen->evt_trk(), 50) != WAIT_OBJECT_0) {
          continue;
        }
        vripc_tracking_slot_t s {};
        uint64_t idx = 0;
        bool torn = false;
        if (gen->read_latest_tracking(s, idx, &torn)) {
          trk_reads.fetch_add(1);
          if (trk_pattern_marked(s)) {
            trk_pattern_checked.fetch_add(1);
            if (!trk_pattern_ok(s)) {
              trk_pattern_bad.fetch_add(1);
            }
          }
          if (have_last) {
            const int32_t d = (int32_t) (s.sample_id - last_id);
            if (d < 0) {
              trk_backwards.fetch_add(1);
            }
          }
          last_id = s.sample_id;
          have_last = true;
          trk_last_id.store(s.sample_id);
        } else if (torn) {
          trk_torn.fetch_add(1);
        }
      }
    });

    // 主迴圈：以 config 的更新率畫圖、Signal、發布（§B.7 driver 端）
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
      timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    const int64_t f = qpf();
    const int64_t t_end = qpc() + (int64_t) (a.seconds * (double) f);

    uint64_t frames_total = 0;
    uint64_t noslot = 0;
    uint64_t signal_skipped = 0;
    uint64_t consumed_lost = 0;
    uint64_t gens_rendered = 0;
    bool fence_max_sent = false;
    lag_stats_t lag;

    uint64_t cur_gen = 0;
    uint64_t frame_id = 0;
    uint64_t fence_n = 0;
    uint64_t last_fence_of[VRIPC_TEX_COUNT] = {};
    int64_t submit_of[VRIPC_TEX_COUNT] = {};
    bool lag_pending[VRIPC_TEX_COUNT] = {};
    int64_t next_frame = qpc();

    while (qpc() < t_end) {
      if (a.hello_repeat != 0 && st.attempts.load() >= a.hello_repeat) {
        break;
      }
      auto gen = client->current();
      std::shared_ptr<output_t> out;
      {
        std::lock_guard<std::mutex> lk(st.out_mtx);
        out = st.out;
      }
      if (a.no_render || !gen || !out || out->generation != gen->id()) {
        Sleep(10);
        next_frame = qpc();
        continue;
      }
      if (out->generation != cur_gen) {
        // 新 generation：fence 是新物件（從 0 起）、frame_id 從 1 起
        cur_gen = out->generation;
        frame_id = 0;
        fence_n = 0;
        std::fill(std::begin(last_fence_of), std::end(last_fence_of), 0ull);
        std::fill(std::begin(lag_pending), std::end(lag_pending), false);
        ++gens_rendered;
      }

      const uint32_t mhz = out->refresh_mhz >= 30000 ? out->refresh_mhz : 90000;
      const int64_t period = (f * 1000) / (int64_t) mhz;
      const int64_t now0 = qpc();
      if (now0 < next_frame) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG) (((next_frame - now0) * 10000000) / f);  // 100 ns，相對
        if (due.QuadPart == 0) {
          due.QuadPart = -1;
        }
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
          WaitForSingleObject(timer, 100);
        } else {
          Sleep(1);
        }
        continue;
      }
      next_frame += period;
      if (next_frame < now0 - period) {
        next_frame = now0 + period;  // 落後太多（例如剛換 generation）：不追趕
      }

      // slot：consumedFence 已放回的第一個（§B.7）；UINT64_MAX → device lost 路徑（§B.6）
      const uint64_t consumed = out->ring.consumed_fence->GetCompletedValue();
      if (consumed == UINT64_MAX) {
        ++consumed_lost;
        line("ipcpeer consumed-fence-lost gen=%llu", (unsigned long long) cur_gen);
        client->report_device_lost(0x887A0005u /* DXGI_ERROR_DEVICE_REMOVED */);
        {
          std::lock_guard<std::mutex> lk(st.out_mtx);
          if (st.out == out) {
            st.out.reset();
          }
        }
        continue;
      }
      const int64_t t_poll = qpc();
      for (uint32_t i = 0; i < VRIPC_TEX_COUNT; ++i) {
        if (lag_pending[i] && consumed >= last_fence_of[i]) {
          lag.add(qpc_to_ms(t_poll - submit_of[i]));
          lag_pending[i] = false;
        }
      }
      int slot = -1;
      for (uint32_t i = 0; i < VRIPC_TEX_COUNT; ++i) {
        if (consumed >= last_fence_of[i]) {
          slot = (int) i;
          break;
        }
      }
      auto &status = client->status();
      status.frames_presented.fetch_add(1, std::memory_order_relaxed);
      if (slot < 0) {
        ++noslot;
        status.drop_noslot.fetch_add(1, std::memory_order_relaxed);
        continue;
      }

      const int64_t present_qpc = qpc();
      ++frame_id;
      ++frames_total;
      float color[4];
      pattern_color(frame_id, color);
      auto &ctx = out->dev.ctx;
      auto &ctx4 = out->dev.ctx4;
      ctx->ClearRenderTargetView(out->ring.rtv[slot].Get(), color);

      bool signal = true;
      uint64_t signal_value = fence_n + 1;
      if (a.stop_signal_after != 0 && frames_total > a.stop_signal_after) {
        signal = false;
      }
      if (a.fence_max && !fence_max_sent && frames_total > a.fence_max_after) {
        signal_value = UINT64_MAX;
        fence_max_sent = true;
      }
      fence_n += 1;  // descriptor 的 fence 值照常遞增（fence-max 時 server 端讀到的完成值是 UINT64_MAX）
      if (signal) {
        const HRESULT hr = ctx4->Signal(out->ring.shared_fence.Get(), signal_value);
        if (FAILED(hr)) {
          line("ipcpeer warn signal-failed hr=0x%08x value=%s", (unsigned) hr, signal_value == UINT64_MAX ? "max" : "n");
        } else if (signal_value == UINT64_MAX) {
          line("ipcpeer fence-max-signaled gen=%llu frame=%llu", (unsigned long long) cur_gen, (unsigned long long) frame_id);
        }
      } else {
        ++signal_skipped;
      }
      if (!a.no_flush) {
        ctx->Flush();  // 跨行程 CPU 觀察需要（§B.7）
      }
      const int64_t submit_qpc = qpc();
      last_fence_of[slot] = fence_n;
      submit_of[slot] = submit_qpc;
      lag_pending[slot] = true;

      vripc_frame_desc_t d {};
      d.frame_id = frame_id;
      d.generation = cur_gen;
      d.fence_value = fence_n;
      d.tex_idx = (uint32_t) slot;
      d.flags = VRIPC_FRM_POSE_VALID;
      const uint32_t echo = trk_last_id.load();
      if (echo != 0) {
        d.echo_sample_id = echo;
        d.flags |= VRIPC_FRM_ECHO_MATCHED;
      } else {
        d.flags |= VRIPC_FRM_POSE_FALLBACK;
      }
      d.space_epoch = out->space_epoch;
      d.layout_epoch = out->layout_epoch;
      d.render_pos[0] = 0.0f;
      d.render_pos[1] = 1.6f;
      d.render_pos[2] = 0.0f;
      d.render_rot[0] = 0.0f;
      d.render_rot[1] = 0.0f;
      d.render_rot[2] = 0.0f;
      d.render_rot[3] = 1.0f;
      d.layer_count = 1;
      d.t_target_qpc = present_qpc + period;
      d.present_qpc = present_qpc;
      d.submit_qpc = submit_qpc;
      gen->publish_frame(d);
      status.frames_composited.fetch_add(1, std::memory_order_relaxed);
      csv.row("%llu,%llu,%d,%llu,%lld,%lld,%d,%d", (unsigned long long) cur_gen, (unsigned long long) frame_id, slot, (unsigned long long) fence_n, (long long) present_qpc, (long long) submit_qpc, signal ? 1 : 0, a.no_flush ? 0 : 1);

      if (a.crash_after != 0 && frames_total >= a.crash_after) {
        line("ipcpeer crash-now frames=%llu", (unsigned long long) frames_total);
        csv.close();
        TerminateProcess(GetCurrentProcess(), 3);  // 不送 BYE：模擬 driver 當掉（kernel 關 pipe）
      }
    }

    quit.store(true);
    trk_thread.join();
    if (timer) {
      CloseHandle(timer);
    }

    const bool stopped = client->stop(500);
    const auto s = client->stats();
    {
      std::lock_guard<std::mutex> lk(st.out_mtx);
      st.out.reset();
    }
    uint32_t intact = 0;
    {
      std::lock_guard<std::mutex> lk(st.occ_mtx);
      for (HANDLE h : st.occupants) {
        DWORD flags = 0;
        // 新物件仍是我們的 event、而且還是 signaled（server 沒有從遠端關掉或改動它）
        if (GetHandleInformation(h, &flags) && WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
          ++intact;
        }
        CloseHandle(h);
      }
      if (!st.occupants.empty()) {
        line("ipcpeer close-after-welcome-check occupants=%u intact=%u", (unsigned) st.occupants.size(), intact);
      }
    }

    char dup[128] = "";
    if (st.dup_shm.load() >= 0 || st.dup_trk.load() >= 0 || st.dup_frm.load() >= 0) {
      snprintf(dup, sizeof(dup), " dup_shm=0x%llx dup_trk=0x%llx dup_frm=0x%llx", (unsigned long long) st.dup_shm.load(), (unsigned long long) st.dup_trk.load(), (unsigned long long) st.dup_frm.load());
    }
    line("ipcpeer summary connects=%llu handshakes=%llu operational=%llu teardowns=%llu rejects=%llu lastReject=%u lastRejectDetail=%u dead=%d frames=%llu gens=%llu noslot=%llu signalSkipped=%llu consumedLost=%llu fenceMax=%d%s",
         (unsigned long long) s.connects, (unsigned long long) s.handshakes, (unsigned long long) s.operational, (unsigned long long) s.teardowns,
         (unsigned long long) s.rejects_received, s.last_reject_reason, s.last_reject_detail, s.dead ? 1 : 0, (unsigned long long) frames_total,
         (unsigned long long) gens_rendered, (unsigned long long) noslot, (unsigned long long) signal_skipped, (unsigned long long) consumed_lost, fence_max_sent ? 1 : 0, dup);
    line("ipcpeer tracking reads=%llu torn=%llu backwards=%llu patternChecked=%llu patternBad=%llu", (unsigned long long) trk_reads.load(), (unsigned long long) trk_torn.load(), (unsigned long long) trk_backwards.load(), (unsigned long long) trk_pattern_checked.load(), (unsigned long long) trk_pattern_bad.load());
    line("ipcpeer consumed-lag frames=%u p50Ms=%.3f p95Ms=%.3f flush=%d (frame-period resolution)", (unsigned) lag.ms.size(), lag.pct(0.5), lag.pct(0.95), a.no_flush ? 0 : 1);
    csv.close();
    vrdrv::log::set_sink(nullptr, nullptr);
    if (stopped) {
      delete client;
      delete st_owner;
    } else {
      line("ipcpeer warn stop-timeout (ipc thread still running; exiting anyway)");
    }
    return rc_ok;
  }

}  // namespace probe
