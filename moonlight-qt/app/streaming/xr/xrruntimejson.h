// VipleStream §SF-PROBE（M2a）— OpenXR active runtime JSON 的探索（不載入 loader）
//
// 一律編譯（純 QtCore 檔案操作）：SfEnv 記 log、xr-probe 的失敗診斷、之後 M3a 的
// XrContext 都用它。
//
// 為什麼要自己找：Flatpak 會把 XDG_CONFIG_HOME 改成 ~/.var/app/<id>/config，OpenXR loader
// 只看 $XDG_CONFIG_HOME/openxr/1/active_runtime[.<arch>].json、XDG_CONFIG_DIRS、SYSCONFDIR
// （/app/etc），看不到 host 的 ~/.config/openxr。所以在沙箱內要另外查 host 的路徑，找到後
// 由呼叫端在 xrCreateInstance 前設 XR_RUNTIME_JSON（行程內，不是使用者設定）。
//
// 探索順序（先找到先用；每一條都記來源，JSON 報告全部列出）：
//   1. XR_RUNTIME_JSON（環境變數，dev 覆寫）
//   2. $XDG_CONFIG_HOME/openxr/1/active_runtime.<arch>.json、active_runtime.json
//   3. Flatpak 內：$HOST_XDG_CONFIG_HOME（有設時）與 $HOME/.config 下的同名檔；再來是 host 的
//      /run/host/etc/xdg、/run/host/etc（要 --filesystem=host-etc 才看得到）。來源都記 "host-config"。
//   4. $XDG_CONFIG_DIRS（預設 /etc/xdg）下的同名檔
//   5. /etc/openxr/1/ 下的同名檔（Flatpak 內先查 loader 編譯時的 SYSCONFDIR：/app/etc/openxr/1/）
// <arch> 依 OpenXR loader 規格：x86_64、aarch64 等（loader 用的是 uname 風格名稱）。
// （已對照 OpenXR-SDK release-1.1.63 src/common/platform_utils.hpp 的 XR_ARCH_ABI：x86_64、aarch64、
//  i686、armv7a-vfp…；實作照抄同一串判斷。）

#pragma once

#include <QList>
#include <QString>

namespace XrRuntimeJson {

struct Candidate {
    QString path;
    QString source;     // "XR_RUNTIME_JSON"、"XDG_CONFIG_HOME"、"host-config"、"XDG_CONFIG_DIRS"、"etc"
    bool exists = false;
    bool readable = false;
};

// 依上面的順序列出所有候選（含不存在的），給診斷用。非 Linux 回空清單。
QList<Candidate> candidates();

struct Resolved {
    bool found = false;
    QString jsonPath;
    QString source;          // 同 Candidate::source
    QString runtimeName;     // runtime.name（可能為空）
    QString libraryPath;     // runtime.library_path；相對路徑已以 JSON 所在目錄解析成絕對路徑
    bool libraryExists = false;
    bool loaderWouldFind = false; // true = loader 自己的搜尋就會找到（來源是 1、2、4、5）；
                                  // false = 只有 host-config 找得到，呼叫端需要設 XR_RUNTIME_JSON
    QString parseError;      // JSON 解析失敗時的訊息
};

// 第一個存在而且可讀的候選，解析其內容。非 Linux 回 found=false。
// XR_RUNTIME_JSON 有設時與 loader 一樣只看它：檔案不存在或讀不到時 found=false，但 jsonPath／source／
// parseError 仍填那個路徑，呼叫端看得出是覆寫本身有問題。
Resolved resolveActive();

} // namespace XrRuntimeJson
