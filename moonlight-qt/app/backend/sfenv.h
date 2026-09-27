// VipleStream §SF-ENV（M2a）— Steam Frame 執行環境摘要：`[VIPLE-SF-ENV]`
//
// 目的：Frame 上的每一份 log 與探測 JSON 都能自己說明「在什麼環境跑的」——打包形態
// （Flatpak／AppImage／plain）、runtime、架構、host 作業系統、Wayland／gamescope、
// V4L2 與 DRM 裝置、OpenXR active runtime。實機關卡（G-α／G-PKG）的判讀靠它歸因。
//
// 一律編譯；非 Linux 平台 logOnce()/summaryLine() 為 no-op／空字串，snapshot() 只含平台欄位。
//
// 成本規則（不變式 5 精神：不拖慢一般啟動）：
//   - logOnce()／summaryLine() 只收「便宜」的資訊：讀檔（/.flatpak-info、os-release、
//     sysfs）、環境變數白名單、WMUtils::getDrmDriverNames()（已快取）。不 dlopen Vulkan、
//     不對 /dev/video* 做 ioctl。
//   - snapshot() 另外收 Vulkan 裝置（dlopen libvulkan.so.1）與 V4L2 QUERYCAP；只有探測動作呼叫。
//
// 隱私：環境變數只記白名單；STEAM_* 只記「有／無」，不印值（可能含 token）。

#pragma once

#include <QJsonObject>
#include <QString>

namespace SfEnv {

// main() 最前面（任何 qputenv 之前）呼叫一次：記下 QT_QPA_PLATFORM、SDL_VIDEODRIVER 的
// 原始值。之後 main.cpp 可能為了 SSH／eglfs 陷阱改寫它們，log 裡要看得到使用者原本的設定。
void captureOriginalEnv();

// 印兩行 `[VIPLE-SF-ENV]`（便宜資訊）。每個行程只印一次；非 Linux 為 no-op。
void logOnce();

// 一行精簡摘要（快取），給 session 開場的 `[VIPLE-SF-ENV] session:` 行；非 Linux 回傳空字串。
// 尾端的 `sdl-driver=<SDL_GetCurrentVideoDriver()> sdl-app-id=<SDL_GetHint("SDL_APP_ID")>` 每次現取
// （設計 v2 R3），呼叫端不必另外印這兩個值。
QString summaryLine();

// 完整環境（含 Vulkan 裝置名、driver、API 版本，與 /dev/video* 的 QUERYCAP 結果）。
// 只有探測動作呼叫；結果嵌進各探測的 JSON。
QJsonObject snapshot();

} // namespace SfEnv
