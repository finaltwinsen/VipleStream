// ipc_client.h - VipleStream §VR：driver 端 IPC（設計 §B.4、§B.6），vr_probe ipcpeer 共用同一份原始碼。
//
// 執行緒模型：
//  - 一條 ipc 執行緒（start() 建立）處理 pipe（全部 overlapped）、握手、heartbeat、STATE 收送。
//    所有 callback 都在這條執行緒上呼叫；callback 不可阻塞超過數 ms，也不可呼叫 stop()。
//  - 其他執行緒（Present、tracking、RunFrame）只經 current() 取得 generation_t 的 shared_ptr，
//    再用它的讀寫函式存取 shm；generation 結束時最後一個參照放掉才 unmap（不會用到已 unmap 的記憶體）。
//  - driver_status 區塊只由 ipc 執行緒寫（每 100 ms 與送 STATE 之前），其他模組只改本行程的 atomic
//    計數（status()），不直接寫 shm。
//
// 安全規則（§B.1）：
//  - pipe 以 SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION 開啟；連上後先驗 server：pipe 擁有者 == S-1-5-18、
//    DACL protected、GetNamedPipeServerSessionId == 自己的 session；選配：server 映像路徑（peer_mode 時略過）。
//  - WELCOME／TEXTURES 帶來的 handle 在送達時就歸本行程所有，由本模組（或 generation_t）唯一負責關閉。
//  - 絕不在 pipe 上呼叫 FlushFileBuffers；寫入 100 ms 逾時就 CancelIoEx 並 TEARDOWN。
//  - log 絕不含 handle 值。
//
// peer_mode（vr_probe ipcpeer）：HELLO 的 driver_caps 加 VRIPC_DCAP_PEER、略過 server 映像路徑檢查。
// 異常注入掛鉤（peer_hooks_t）只在以 VRDRV_PEER_HOOKS 編譯時存在（Build-SteamVRDriver.ps1 -Target probe
// 才定義）；driver DLL 裡沒有這些程式碼路徑。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <windows.h>

#include "vr_ipc_abi.h"

namespace vrdrv {

  // 一個 generation（一次成功握手）的 per-generation 資源：shm view、兩個 event、本地驗證過的副本。
  // 由 ipc_client 建立；最後一個 shared_ptr 放掉時 unmap 並關 event。
  class generation_t {
  public:
    generation_t(const generation_t &) = delete;
    generation_t &operator=(const generation_t &) = delete;
    ~generation_t();

    uint64_t id() const {
      return welcome_.generation;
    }

    const vripc_welcome_t &welcome() const {
      return welcome_;
    }

    const vripc_session_config_t &config() const {
      return config_;
    }

    bool idle() const {
      return config_.eye_width == 0;
    }

    int64_t qpc_frequency() const {
      return welcome_.qpc_frequency;
    }

    bool dev_pacing() const {
      return (welcome_.welcome_flags & VRIPC_WF_DEV_PACING) != 0;
    }

    // evtTrk（只有 SYNCHRONIZE）：tracking 執行緒等待用。可能是 nullptr（peer 的 close-after-welcome 測試）。
    HANDLE evt_trk() const {
      return evt_trk_;
    }

    // ── 讀 server 寫的區塊（任何執行緒；讀端 wait-free、重試有上限） ──
    // 最新一筆 tracking；回傳 false 表示沒有資料或 torn（torn 另外計數在 *torn）。
    bool read_latest_tracking(vripc_tracking_slot_t &out, uint64_t &idx, bool *torn = nullptr);
    // pacing（單格 seqlock）；失敗由呼叫端沿用上一份。
    bool read_pacing(vripc_pacing_t &out);
    // header 的本地副本（整份複製；呼叫端自己驗）。
    void read_header(vripc_shm_header_t &out) const;
    // consume 區塊的本地副本（server 寫，只供診斷）。
    void read_consume(vripc_consume_status_t &out) const;

    // ── 寫 driver 自己的區塊（內部各有一把鎖＝單一寫入臨界區；本地計數器為準） ──
    // frame descriptor：in.seq 會被忽略；寫完 SetEvent(evtFrm)。
    void publish_frame(const vripc_frame_desc_t &in);
    // SPSC ring；滿了回 false 並計數。
    bool push_haptic(const vripc_haptic_evt_t &evt);
    bool push_timing(const vripc_timing_rec_t &rec);
    bool push_log(uint32_t level, const char *text);

    struct counters_t {
      uint64_t frames_published;
      uint64_t haptic_dropped;
      uint64_t timing_dropped;
      uint64_t log_dropped;
      uint64_t ring_corrupt;  // shm 的 tail 跑到 head 前面
      uint64_t tracking_torn;
    };
    counters_t counters() const;

  private:
    friend class ipc_client_t;
    generation_t() = default;

    vripc_welcome_t welcome_ {};
    vripc_session_config_t config_ {};
    vripc_shm_t *shm_ = nullptr;
    HANDLE evt_trk_ = nullptr;
    HANDLE evt_frm_ = nullptr;

    mutable std::mutex frames_mtx_;
    uint64_t frames_local_index_ = 0;
    std::mutex haptic_mtx_;
    uint64_t haptic_local_head_ = 0;
    std::mutex timing_mtx_;
    uint64_t timing_local_head_ = 0;
    std::mutex log_mtx_;
    uint64_t log_local_head_ = 0;
    uint64_t log_pending_dropped_ = 0;

    std::atomic<uint64_t> frames_published_ {0};
    std::atomic<uint64_t> haptic_dropped_ {0};
    std::atomic<uint64_t> timing_dropped_ {0};
    std::atomic<uint64_t> log_dropped_ {0};
    std::atomic<uint64_t> ring_corrupt_ {0};
    std::atomic<uint64_t> tracking_torn_ {0};
  };

  using generation_ptr = std::shared_ptr<generation_t>;

  // driver 其他模組更新的本行程計數（ipc 執行緒每 100 ms 抄進 shm 的 driver_status）。
  struct driver_status_local_t {
    std::atomic<uint64_t> frames_presented {0};
    std::atomic<uint64_t> frames_composited {0};
    std::atomic<uint64_t> drop_noslot {0};
    std::atomic<uint64_t> drop_acquire_timeout {0};
    std::atomic<uint64_t> drop_format {0};
    std::atomic<uint64_t> stale_pose_count {0};
    std::atomic<uint64_t> outofrange_count {0};
    std::atomic<uint64_t> posehist_miss {0};
    std::atomic<int64_t> last_vsync_qpc {0};
    std::atomic<uint32_t> swapsets_live {0};
    std::atomic<uint32_t> last_layer_count {0};
    std::atomic<int32_t> space_delta_mdeg {0};
    std::atomic<uint32_t> other_hmd {VRIPC_OTHER_HMD_UNKNOWN};
    std::atomic<uint32_t> degraded_code {0};
    std::atomic<uint64_t> next_index_calls {0};
    std::atomic<uint64_t> drop_unknown_layer {0};
    std::atomic<uint32_t> max_layer_count {0};
  };

  // HMD 已 Activate 的不可變參數（K20）。
  struct activated_params_t {
    uint32_t refresh_mhz = 0;
    uint32_t eye_width = 0;
    uint32_t eye_height = 0;
    uint32_t luid_low = 0;
    int32_t luid_high = 0;
  };

  // TEXTURES 處理結果：ok 或要回給 server 的 REJECT 理由。
  struct textures_result_t {
    bool ok = false;
    uint32_t reject_reason = VRIPC_REJ_TEXTURE_INVALID;
    uint32_t reject_detail = 0;  // 例：HRESULT
  };

  struct ipc_callbacks_t {
    // 握手完成（MAPPED）：shm 與 config 已驗證；armed 是 header.armed 的初值。
    std::function<void(const generation_ptr &gen, bool armed)> on_mapped;
    // TEXTURES：handle 是本行程的值。callee 以 OpenSharedResource1／OpenSharedFence 開啟（COM 參照自己保住）；
    // 回傳後 ipc_client 一律關閉這 5 個 handle（不論成功與否）。沒有設定這個 callback → REJECT(TEXTURE_INVALID)。
    std::function<textures_result_t(const generation_ptr &gen, const vripc_textures_t &msg, const HANDLE tex[VRIPC_TEX_COUNT], HANDLE shared_fence, HANDLE consumed_fence)> on_textures;
    // TEARDOWN：callee 在 present 鎖內把輸出目標換成 null、放掉本 generation 的 ring／fence 參照。
    // reason 是靜態字串（例 "bye-reconfig"、"pipe-broken"）。
    std::function<void(uint64_t generation, const char *reason)> on_teardown;
    // server → driver 的 STATE（ARM／DISARM／REQUEST_QUIT／DEV_SET_V2P）；未知 kind 不會送到這裡。
    std::function<void(uint32_t kind, uint32_t arg)> on_server_state;
    // server heartbeat 逾 1 s → true（只進 standby、不斷線）；恢復 → false。
    std::function<void(bool stale)> on_server_heartbeat;
    // 收到 REJECT（握手中或之後）。
    std::function<void(uint32_t reason, uint32_t detail)> on_rejected;
  };

#if defined(VRDRV_PEER_HOOKS)
  // vr_probe ipcpeer 的異常注入（§F.4、§F.5 T2）。只在 peer_mode 時生效。
  struct peer_hooks_t {
    uint32_t hello_abi = VRIPC_ABI_VERSION;  // --abi N
    uint32_t hello_size = (uint32_t) sizeof(vripc_hello_t);  // --hello-size B（24..512）
    bool never_read_after_mapped = false;  // --peer-never-read：MAPPED 後取消 pending read、之後不再讀
    bool close_after_welcome = false;  // --peer-close-after-welcome：MAPPED 後關掉 evtTrk／evtFrm
    bool escalate = false;  // --peer-escalate：對 WELCOME 的 handle 做 DuplicateHandle(self→self, *_ALL_ACCESS)
    bool ignore_dead = false;  // --hello-repeat：REJECT(ABI) 後不進 DEAD，繼續重連
    uint32_t fixed_retry_ms = 0;  // 非 0：每次重連固定間隔（不走退避），給限速測試用
    // close_after_welcome 關掉 handle 後通知 peer（values 是剛關掉的 handle 值，只能拿來比對、不可記 log）。
    std::function<void(uint32_t count, const uint64_t *values)> on_handles_closed;
    // WELCOME 通過 check_welcome 之後、map 之前（ipc 執行緒）：peer 以此查 server 複製進來的 handle 實際權限
    // （NtQueryObject 的 GrantedAccess；§B.1 最小權限複製的證據）。只可查詢、不可關閉或記錄 handle 值。
    std::function<void(const vripc_welcome_t &welcome)> on_welcome;
    // 每一次連線嘗試的結果（限速測試用）：outcome 是靜態字串。
    std::function<void(const char *outcome, uint32_t detail)> on_attempt;
    // vr_probe unit loopback 專用：取代 server 驗證（loopback 的假 server 不是 SYSTEM）。driver 沒有這個欄位。
    std::function<bool(HANDLE pipe, const char *&why)> verify_server_override;
  };
#endif

  struct ipc_client_config_t {
    std::wstring pipe_name;  // 空 → VRIPC_PIPE_NAME_PREFIX + 自己的 session id
    uint32_t driver_version_packed = 0;
    uint32_t driver_caps = 0;
    uint32_t openvr_sdk_packed = 0;
    std::string iface_directmode;  // HELLO 的 iface_directmode（截到 47 字元）
    bool peer_mode = false;
    // 非空才做選配的 server 映像檢查（DOS 路徑；peer_mode 時忽略）。
    std::wstring expected_server_image;
#if defined(VRDRV_PEER_HOOKS)
    peer_hooks_t hooks;
#endif
  };

  class ipc_client_t {
  public:
    enum class first_e {
      pending,  // 還沒有結果（逾時）
      mapped,  // 已 MAPPED
      no_server,  // pipe 不存在或還沒開放（FILE_NOT_FOUND／ACCESS_DENIED）：Init 立即返回
      untrusted,  // server 驗證不過
      rejected,  // 握手被拒
      dead,  // ABI 不同，本行程不再重連
      failed,  // 其他錯誤
    };

    ipc_client_t() = default;
    ipc_client_t(const ipc_client_t &) = delete;
    ipc_client_t &operator=(const ipc_client_t &) = delete;
    // 解構前必須 stop() 成功；stop() 逾時（回 false）時呼叫端不可釋放這個物件（ipc 執行緒還在用）。
    ~ipc_client_t();

    bool start(const ipc_client_config_t &cfg, const ipc_callbacks_t &cb);

    // Init 的同步等待（§B.4 D1–D4、K8）：等第一次連線嘗試有結果，最多 timeout_ms。
    first_e wait_first(uint32_t timeout_ms);

    // Cleanup（§B.6）：停止事件 → CancelIoEx → BYE(DRIVER_CLEANUP)（100 ms）→ join（上限 join_timeout_ms）。
    // 回傳 false 表示 ipc 執行緒沒有在時限內結束（已記一行 log；物件不可釋放）。
    bool stop(uint32_t join_timeout_ms = 500);

    // 目前的 generation（沒有連線時是 nullptr）。任何執行緒可呼叫。
    generation_ptr current();

    // 送 STATE（driver → server）。任何執行緒可呼叫；在 ipc 執行緒上依序送出（先抄一次 driver_status）。
    // 沒有 generation 時丟棄（重連後 server 以 driver_status 取得目前狀態）。
    void send_state(uint32_t kind, uint32_t arg);

    // §B.6 OPERATIONAL 的 device lost 路徑：consumedFence 為 UINT64_MAX 或自己的 device removed 時呼叫。
    // ipc 執行緒依序送 STATE DEVICE_LOST(arg = hresult) → BYE(DEVICE_LOST) → TEARDOWN → 立即重連。任何執行緒可呼叫。
    void report_device_lost(uint32_t hresult);

    // driver_state 的裝置層部分（VRIPC_DRV_HMD_ADDED…DEGRADED；0 = 沒有）。發布值：非 0 時用它，否則用連線層（MAPPED／READY）。
    void set_device_state(uint32_t drv_state);
    void set_activated(const activated_params_t &p);
    void set_other_hmd_system(const char *name);  // 最多 15 字元，只留可列印 ASCII

    driver_status_local_t &status() {
      return status_;
    }

    struct stats_t {
      uint64_t connects;  // 連上 pipe 的次數
      uint64_t handshakes;  // 到達 MAPPED 的次數
      uint64_t operational;  // 到達 OPERATIONAL 的次數
      uint64_t teardowns;
      uint64_t rejects_received;
      uint64_t protocol_errors;
      uint64_t write_timeouts;
      uint64_t last_generation;
      uint32_t last_reject_reason;
      uint32_t last_reject_detail;
      bool dead;
    };
    stats_t stats() const;

    // 由 driver DLL 自己的位置推 server 映像路徑（§B.1：DLL 位置往上 6 層 + \viplestream-server.exe）。
    static std::wstring expected_server_image_from_module(HMODULE module);
    // 純字串版（vr_probe unit 測）：<install>\config\steamvr\<ver>\viplestream\bin\win64\x.dll → <install>\viplestream-server.exe
    static std::wstring expected_server_image_from_dll_path(const std::wstring &dll_path);

  private:
    struct session_t;
    static unsigned __stdcall thread_entry(void *self);
    void run();

    // 連線層
    enum class connect_e {
      ok,
      not_found,
      busy,
      untrusted,
      error,
    };
    connect_e connect_once(HANDLE &pipe, const char *&why, uint32_t &err);
    bool verify_server(HANDLE pipe, const char *&why, uint32_t &err);

    // session 層：回傳下一次重連前要等多久（ms；0 = 立即）與是否進 DEAD／停止
    struct session_end_t {
      uint32_t delay_ms;
      bool dead;
      bool stop;
      bool reset_failures;
    };
    session_end_t run_session(HANDLE pipe);

    void write_status_block(const generation_ptr &gen, uint32_t link_state);
    void signal_first(first_e v);
    void log_connect_problem(const char *what, uint32_t err);

    ipc_client_config_t cfg_;
    ipc_callbacks_t cb_;
    HANDLE thread_ = nullptr;
    HANDLE stop_evt_ = nullptr;
    HANDLE first_evt_ = nullptr;
    HANDLE send_evt_ = nullptr;
    std::atomic<int> first_ {(int) first_e::pending};
    int64_t qpf_ = 0;

    mutable std::mutex gen_mtx_;  // 保護 gen_；持有時不可呼叫 vrdrv::log
    generation_ptr gen_;

    std::mutex send_mtx_;
    struct pending_state_t {
      uint32_t kind;
      uint32_t arg;
    };

    static constexpr uint32_t k_send_queue = 64;
    pending_state_t send_queue_[k_send_queue] {};
    uint32_t send_count_ = 0;
    uint64_t send_dropped_ = 0;
    std::atomic<uint32_t> device_lost_hr_ {0};
    std::atomic<bool> device_lost_pending_ {false};

    std::mutex status_mtx_;  // 保護 activated_、other_hmd_system_
    activated_params_t activated_ {};
    char other_hmd_system_[16] {};
    std::atomic<uint32_t> device_state_ {0};
    driver_status_local_t status_;

    mutable std::mutex stats_mtx_;
    stats_t stats_ {};
    int64_t last_quiet_log_qpc_ = 0;
    uint32_t quiet_suppressed_ = 0;
  };

}  // namespace vrdrv
