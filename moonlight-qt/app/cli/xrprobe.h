// VipleStream §SF-PROBE（M2a）— `viplestream xr-probe`：`[VIPLE-XR-PROBE]`
//
// A 段（instance／system 層，不建 session）：PoC-2 的擴充清單、PoC-F／G-PKG 的「沙箱內
// 能不能載入 OpenXR runtime」、vulkan_enable2 需求與 runtime 指定的 GPU、timespec 轉換、
// system 屬性（含眼動）、PRIMARY_STEREO 的建議 image rect 與 blend mode。
// B 段（--session，M3a X1）：先跑 A 段，再用 XrContext（streaming/xr/xrcontext.h）建 Vulkan
// device 與 XrSession、跑 frame loop --duration 秒（loading quad），回報 session 最高狀態
// （要看到 FOCUSED）、frame 數、漏幀率、refresh rate、每眼 FOV、reference space。
// imageRect 異色測試留到 X2（需要 XrRenderer）。
//
// 只有 `CONFIG+=openxr`（build-steamframe.sh、本機 build_moonlight.cmd --openxr）編進實作；
// 其他建置回 10。
//
// 結束碼：0 runtime 載入成功（--session 時另需 session 跑出 frame）；13 找不到或載不起
// runtime（JSON 內附診斷）；14 session 建不起來或沒有 frame；10 本建置沒有 OpenXR。
// 見 cli/probeutil.h。

#pragma once

#include <QString>

struct XrProbeOptions {
    bool session = false;      // B 段：XrContext session＋frame loop（M3a X1）
    int durationSec = 10;      // B 段 frame loop 秒數
    QString runtimeJson;       // dev：在 xrCreateInstance 前（行程內）設 XR_RUNTIME_JSON
    bool loaderDebug = false;  // dev：行程內設 XR_LOADER_DEBUG=all（loader 的輸出在 stderr）
    QString jsonPath;          // 空 = ProbeUtil::defaultJsonPath("xr-probe")
    bool selftestRay = false;  // M3a X4：只跑射線求交／One-Euro 自測（不需要 runtime）；全過 0、有失敗 14
};

int runXrProbe(const XrProbeOptions& options);
