/**
 * @file src/file_transfer.h
 * @brief In-stream 雙向檔案傳輸 manager（server side）。
 *
 * VipleStream §N — 透過 server tray menu 觸發 send/receive 流程，命令以
 * per-paired-client queue 形式餵給 nvhttp `/transfer/poll` 長輪詢端點。檔
 * 案 blob 走 `/transfer/blob/<token>` GET/POST。Wire-compat：完全走 HTTPS
 * sidechannel，不擴充 moonlight-common-c 協定。
 */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <vector>

namespace file_transfer {

  /**
   * Server → client 命令種類，序列化成 JSON 由 client 長輪詢拉取。
   */
  enum class command_type {
    list_dir,             ///< 請 client 回傳 Downloads listing
    download_to_client,   ///< server 已備妥檔案，client 來 /transfer/blob/<token> 拉
    upload_from_client,   ///< server 想要 client filename，client POST 到 /transfer/blob/<token>
    cancel,               ///< 中止 token
  };

  struct command {
    command_type type;
    std::string token;       ///< UUID for download/upload/cancel；list_dir 用 query id
    std::string filename;    ///< server→client：display 名稱；client→server：client 端要讀的檔名
    std::uint64_t size;      ///< bytes（download_to_client 才填）
    std::string path;        ///< list_dir / upload_from_client：client 端絕對路徑（空 = Downloads）
    bool is_directory;       ///< upload_from_client：是否是資料夾（要 zip 後再傳）
  };

  enum class direction {
    to_client,     ///< server → client
    from_client,   ///< client → server
  };

  enum class state {
    pending,       ///< 已建立 token，等 client 來 transfer
    running,       ///< 傳輸進行中
    done,
    canceled,
    failed,
  };

  /**
   * 一個 in-flight transfer 的所有狀態。
   *
   * 對 `to_client` 方向：`local_path` 是 server 端要讀的檔；client 來 GET。
   * 對 `from_client` 方向：`local_path` 是 server 端要寫的目標檔；client 來 POST。
   */
  struct transfer {
    std::string token;
    direction dir;
    state st;
    std::filesystem::path local_path;
    std::string display_name;
    std::uint64_t total_bytes;
    std::uint64_t bytes_done;
    std::chrono::steady_clock::time_point started_at;
    std::string error_msg;     ///< 失敗訊息（state==failed 時填）
    std::atomic<bool> cancel_flag;

    // file_transfer 的 transfer 物件透過 shared_ptr 共享，需要可 move/copy 的
    // 持有者；atomic<bool> 不可 copy，所以這個 ctor 套清單明列。
    transfer():
        dir(direction::to_client),
        st(state::pending),
        total_bytes(0),
        bytes_done(0),
        started_at(std::chrono::steady_clock::now()),
        cancel_flag(false) {}
  };

  /**
   * Read-only 快照（給 endpoint 回傳進度用）。`transfer` 含 atomic 不可 copy，
   * 所以 `get_transfer` 改回 snapshot。
   */
  struct transfer_snapshot {
    std::string token;
    direction dir;
    state st;
    std::filesystem::path local_path;
    std::string display_name;
    std::uint64_t total_bytes;
    std::uint64_t bytes_done;
    std::string error_msg;
    bool cancel_flag;
  };

  /**
   * Manager 是單例（per-Sunshine-process），由 tray callbacks + nvhttp
   * endpoints 共享。
   */
  class manager {
  public:
    static manager &instance();

    /**
     * Tray「Send file to client」流程入口：建立 download_to_client 命令、
     * 把 local_path 內容備妥給 client 拉，token 由內部產。
     *
     * @return token，或空 string 代表失敗（無 active client）
     */
    std::string queue_send_to_client(const std::string &client_uuid,
                                     const std::filesystem::path &local_path);

    /**
     * Web UI「Receive from client」流程入口：建立 upload_from_client 命令，
     * 預備一個 server 端的寫入路徑（`Downloads/<sanitized_filename>`）。
     *
     * `path` 是 client 端要讀的絕對路徑（不含檔名 / 資料夾名）。
     * `is_directory=true` 代表是資料夾 → client 會先 zip 再上傳，server 端
     * 落地檔名自動加 `.zip` 後綴。
     */
    std::string queue_receive_from_client(const std::string &client_uuid,
                                          const std::string &path,
                                          const std::string &client_filename,
                                          bool is_directory,
                                          std::uint64_t expected_size);

    /**
     * Tray「Receive file from client」第一步：要 client 回傳目錄列表，server
     * 端收到後存進 `m_last_listing[client_uuid]`，web UI 載入時取得。
     *
     * `path` 空字串表示 client 預設 Downloads，否則 client 端絕對路徑。
     */
    std::string queue_list_dir(const std::string &client_uuid,
                               const std::string &path = {});

    /**
     * 排入 cancel 命令並標記對應 transfer cancel_flag。
     */
    void cancel(const std::string &client_uuid, const std::string &token);

    /**
     * Long-poll：等命令上來或 timeout。回傳 nullopt 代表 timeout。
     */
    std::optional<command> poll(const std::string &client_uuid,
                                std::chrono::milliseconds timeout);

    /**
     * 取得指定 transfer 的狀態快照（給 progress endpoint）。
     */
    std::optional<transfer_snapshot> get_transfer(const std::string &token);

    /**
     * Client 回傳目錄 listing（POST /transfer/result?cmd=list_dir）。
     */
    void store_listing(const std::string &client_uuid, std::string json_listing);

    /**
     * Web UI 載入 transfer 頁時取最近一次 listing。
     */
    std::string take_listing(const std::string &client_uuid);

    /**
     * Client 拉檔（GET /transfer/blob/<token>）— 開啟 ifstream 並推送。
     */
    bool open_for_send(const std::string &token, std::ifstream &out_stream);

    /**
     * Client 推檔（POST /transfer/blob/<token>）— 開啟 ofstream 接收。
     */
    bool open_for_receive(const std::string &token, std::ofstream &out_stream);

    /**
     * Chunk 進度回報；達 5% 或 done 時 atomically update bytes_done。
     */
    void report_progress(const std::string &token, std::uint64_t delta_bytes);

    /**
     * Transfer 結束（client 寫完或拉完）— 標記 done/failed。
     */
    void finalize(const std::string &token, bool success, std::string error_msg = {});

    /**
     * Tray gating：是否有正在進行中的 transfer（避免並行）。
     *
     * §N.5.bug (2026-05-13)：呼叫前會 sweep stale `pending` transfers。
     * 修「client SSL fail 在 `/transfer/blob` 階段，server 從未收到 finalize
     * → `m_active[token]` 一直停在 `pending`，下個 `queue_send_to_client`
     * 被 `busy()` 擋成『another transfer in progress』」的 stale lock bug。
     */
    bool busy() const;

    /**
     * Stream 中止時清除所有 in-flight（呼叫自 system_tray::update_tray_stopped）。
     */
    void abort_all(const std::string &reason);

    /**
     * Sanitize 一個 filename 變成只允許單一檔名（無 ../、無絕對路徑、無空 byte、
     * 無檔系保留字元）。失敗回傳空 string。
     */
    static std::string sanitize_filename(const std::string &raw);

    /**
     * 回傳 server 端 Downloads 路徑（Windows: %USERPROFILE%\Downloads；
     * Linux: $XDG_DOWNLOAD_DIR or ~/Downloads）。建立必要時用。
     */
    static std::filesystem::path downloads_dir();

  private:
    manager() = default;
    manager(const manager &) = delete;
    manager &operator=(const manager &) = delete;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::map<std::string, std::queue<command>> m_queues;      ///< per-client uuid
    std::map<std::string, std::shared_ptr<transfer>> m_active; ///< token → transfer
    std::map<std::string, std::string> m_last_listing;        ///< per-client uuid → JSON

    std::string make_token();

    /**
     * §N.5.bug (2026-05-13) — Sweep `state::pending` transfers 若 `started_at`
     * 超過 STALE_PENDING_TIMEOUT 仍沒進 running 視為 stale (client `/transfer/blob`
     * 沒抵達 / SSL fail / app crash)，標 `state::failed` 釋出 `busy()`.
     *
     * 呼叫者必須已持 `m_mutex`（`_locked` 後綴）。`pending` 是還沒 transition
     * 到 `running` 的 transfer — 一旦 `open_for_send` / `open_for_receive` 被
     * server-side handler invoke 過就會切 running，那階段就改吃 progress
     * timeout（v1 先不做，因為大檔正常 transfer 也可能停滯多秒）。
     */
    void sweep_stale_locked() const;

    /// §N.5.bug — pending state 視為 stale 的時間門檻。
    /// 30s 比 client poll interval (2s) 寬鬆 15×，使用者 retry 也夠時間。
    static constexpr std::chrono::seconds STALE_PENDING_TIMEOUT {30};
  };

}  // namespace file_transfer
