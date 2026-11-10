/**
 * @file src/vr/vr_netbench.h
 * @brief VipleStream 2.0 §VR-NETBENCH（2026-11-08）：設定頁「測試網路」用的 host 端片段播放（VR 網路測試）。
 *
 * 頭盔在設定頁按「測試網路」→ 對「VR Network Test」這個 VR 應用開一般的 PCVR 串流，host 不啟動 SteamVR，
 * 改由 display_vr_clip_t 播放預錄的桌球片段（每次內容相同），其餘編碼、傳送、碼率控制、補包全部照一般 VR 串流。
 * 每一階（每眼解析度）重開一次串流；網路狀態由頭盔切換。
 */
#pragma once

#include <filesystem>

#include "src/config.h"
#include "src/platform/common.h"

namespace vr {
  /// 片段檔：設定 vr_netbench_clip，沒設就是 config 目錄的 netbench/clip.mp4
  inline std::filesystem::path netbench_clip_path() {
    if (!config::vr.netbench_clip.empty()) {
      return std::filesystem::path {config::vr.netbench_clip};
    }
    return platf::appdata() / "netbench" / "clip.mp4";
  }
}  // namespace vr
