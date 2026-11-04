/**
 * @file src/vr/vr_padmode.h
 * @brief §VR-PADMODE：不使用 VR 手把的遊戲自動切成「Xbox 手把模式」。
 *
 * 依據：Steam 商店的分類「Tracked Controller Support」（id 52）。SteamVR 的場景程式是 Steam 遊戲、而且商店沒有
 * 這個標籤（例：Assetto Corsa、ACC、GRIP）就插一支虛擬 Xbox 360 手把，把 Frame 兩手的狀態合成過去；
 * VR 手把照常送給 SteamVR（手的追蹤與系統鍵不受影響）。兩支選單鍵同時長按 2 秒手動切換，並記住這款遊戲。
 * 商店查詢與手動選擇存在 appdata 的 vr_padmode.json；查不到（離線、下架、非 Steam 遊戲）一律維持 VR 手把。
 */
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <optional>
#include <string>

#include "vr_padmode_map.h"

namespace vr::padmode {

  /// S→C NOTICE 的代碼（VIPLE_VR_NOTICE_*，同值）
  enum class notice_e : uint8_t {
    pad_on_auto = 1,  ///< 這款遊戲不使用 VR 手把，已切成 Xbox 手把模式
    pad_on_manual = 2,  ///< 手動切成 Xbox 手把模式
    pad_off_manual = 3,  ///< 手動切回 VR 手把
    pad_unavailable = 4,  ///< 要切成手把模式但 host 建不了虛擬手把（ViGEmBus 沒裝）
  };

  /// 場景程式的身分。key：Steam 遊戲是 appid、其他是 "exe:<小寫檔名>"
  struct app_t {
    std::string key;
    std::string appid;  ///< 空字串＝不是 Steam 遊戲
    std::string name;
  };

  struct hooks_t {
    std::function<uint32_t()> scene_pid;  ///< SteamVR 目前的場景程式 PID；0＝沒有
    std::function<std::optional<app_t>(uint32_t)> resolve;  ///< PID → 遊戲
    std::function<std::optional<bool>(const std::string &)> tracked;  ///< appid → 商店有沒有 Tracked Controller Support；nullopt＝還不知道
    std::function<std::optional<bool>(const std::string &)> get_override;  ///< key → 手動選擇（true＝手把模式）
    std::function<void(const std::string &, bool)> set_override;
    std::function<int()> pad_alloc;  ///< 建虛擬手把，回傳 id；< 0＝失敗
    std::function<void(int, const xpad_t &)> pad_update;
    std::function<void(int)> pad_free;
    std::function<void(notice_e)> notice;
  };

  /**
   * @brief 每個 PCVR session 一份。場景程式的偵測、決策、建立／移除虛擬手把都在自己的背景執行緒（每 k_poll_ms 一次，
   *        手動切換時立刻），收 tracking 的執行緒只做按鍵對應與長按偵測，不會被 ViGEm 或 Steam 遊戲庫掃描卡住。
   */
  class controller_t {
  public:
    static constexpr int64_t k_poll_ms = 500;  ///< 多久查一次場景程式
    static constexpr int64_t k_hold_ms = 2000;  ///< 兩支選單鍵長按多久切換

    /// start_thread＝false 給單元測試：不開背景執行緒，改由測試呼叫 poll_once()
    explicit controller_t(hooks_t hooks, bool start_thread = true);
    ~controller_t();
    controller_t(const controller_t &) = delete;
    controller_t &operator=(const controller_t &) = delete;

    /// 每個被採用的 tracking 樣本呼叫一次（任何執行緒）。in[0]＝左手、in[1]＝右手
    void on_sample(const VIPLE_VR_CONTROLLER_INPUT in[2], int64_t now_ms);

    /// 背景執行緒每一輪做的事（測試直接呼叫）
    void poll_once();

    bool pad_active() const;

  private:
    enum class source_e { none, store, manual };

    void apply(bool want_pad, source_e source);

    hooks_t h_;
    mutable std::mutex mtx_;  ///< 保護下面標 (m) 的欄位
    std::condition_variable cv_;
    bool stop_ = false;  // (m)
    bool toggle_req_ = false;  // (m) on_sample 偵測到長按，背景執行緒處理
    int pad_id_ = -1;  // (m) on_sample 讀來送狀態
    combo_t combo_;  // (m)
    xpad_t last_ {};  // (m)
    bool sent_ = false;  // (m)

    // 以下只有背景執行緒（或測試的 poll_once）碰
    uint32_t pid_ = 0;
    std::optional<app_t> app_;
    bool decided_ = true;  ///< false＝商店結果還沒回來，每一輪重試
    bool session_override_set_ = false;  ///< 沒有 key 的場景（例：沒有遊戲）手動切換只在這一場有效
    bool session_override_ = false;
    bool unavailable_noticed_ = false;

    std::thread worker_;
  };

  // ── 平台與持久化（vr_padmode.cpp）──

  /// PID → 遊戲（Windows：比對 Steam 遊戲庫的安裝目錄）；其他平台回 nullopt
  std::optional<app_t> resolve_pid(uint32_t pid);
  /// 商店分類（記憶體＋檔案快取；第一次在背景查，查到之前回 nullopt）
  std::optional<bool> tracked_controller(const std::string &appid);
  std::optional<bool> manual_override(const std::string &key);
  void set_manual_override(const std::string &key, bool pad);

}  // namespace vr::padmode
