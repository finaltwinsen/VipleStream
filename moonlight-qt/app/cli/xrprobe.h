// VipleStream §SF-PROBE（M2a）— `viplestream xr-probe`：`[VIPLE-XR-PROBE]`
//
// A 段（instance／system 層，不建 session）：PoC-2 的擴充清單、PoC-F／G-PKG 的「沙箱內
// 能不能載入 OpenXR runtime」、vulkan_enable2 需求與 runtime 指定的 GPU、timespec 轉換、
// system 屬性（含眼動）、PRIMARY_STEREO 的建議 image rect 與 blend mode。
// B 段（--session：refresh rate、FOV、reference space、imageRect 異色測試）需要 Vulkan
// device 與 XrSession，和 M3a 的 XrContext 重疊，這一版回 12。
//
// 只有 `CONFIG+=openxr`（build-steamframe.sh）編進 A 段實作；其他建置回 10。
//
// 結束碼：0 runtime 載入成功；13 找不到或載不起 runtime（JSON 內附診斷）；12 --session；
// 10 本建置沒有 OpenXR。見 cli/probeutil.h。

#pragma once

#include <QString>

struct XrProbeOptions {
    bool session = false;      // B 段：需要 M3a，回 12
    QString runtimeJson;       // dev：在 xrCreateInstance 前（行程內）設 XR_RUNTIME_JSON
    bool loaderDebug = false;  // dev：行程內設 XR_LOADER_DEBUG=all（loader 的輸出在 stderr）
    QString jsonPath;          // 空 = ProbeUtil::defaultJsonPath("xr-probe")
};

int runXrProbe(const XrProbeOptions& options);
