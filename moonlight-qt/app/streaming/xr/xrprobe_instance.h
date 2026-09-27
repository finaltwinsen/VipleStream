// VipleStream §SF-PROBE（M2a）— xr-probe A 段的 OpenXR 實作（只在 CONFIG+=openxr 編譯，
// 定義 HAVE_OPENXR）。CLI 包裝在 cli/xrprobe.cpp。

#pragma once

#include <QJsonObject>

struct XrProbeOptions;

// 跑 A 段：API 1.1→1.0 退回、列舉擴充後才啟用、timespec 來回差、system 與 view
// configuration、vulkan_enable2（或 enable）的需求與 runtime 指定的 GPU，最後一定
// xrDestroyInstance。結果填進 out，重點同時以 `[VIPLE-XR-PROBE]` 印 stdout 與 log。
// 回傳 ProbeUtil 的結束碼（0 或 13 或 14）。
int xrProbeInstance(const XrProbeOptions& options, QJsonObject& out);
