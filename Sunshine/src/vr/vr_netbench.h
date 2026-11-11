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
      // §REVIEW-1111：設定檔是 UTF-8。用窄字串直接建 path 在 Windows 會照系統字碼頁解讀，路徑有中文就對不到檔；
      // 相對路徑也改成相對於 config 目錄（和其他路徑設定一致），原本是相對於服務的工作目錄。
      const auto &s = config::vr.netbench_clip;
      std::filesystem::path p {std::u8string(s.begin(), s.end())};
      return p.is_relative() ? platf::appdata() / p : p;
    }
    return platf::appdata() / "netbench" / "clip.mp4";
  }
}  // namespace vr
