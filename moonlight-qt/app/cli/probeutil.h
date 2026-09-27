// VipleStream §SF-PROBE（M2a）— Steam Frame 探測動作（xr-probe、v4l2-probe、decode-bench）
// 共用的小工具。
//
// 三個探測動作在 main.cpp 早期（log 就緒後、QGuiApplication 之前）以 QCoreApplication
// 派發，完全不碰 QPA／QML／IdentityManager；跑完用 std::_Exit 離開。所以這裡的工具只能
// 依賴 QtCore 與 SDL_Log，不可以用任何 QtGui 物件。
//
// 輸出分三層：
//   1. stdout：給人看的摘要，一行一個事實（經 SSH 執行時直接看得到）；
//   2. log：同一行以 `[VIPLE-…]` tag 寫進 log（Linux release 會寫進 log 檔）；
//   3. JSON：完整結果一律寫檔（--json，預設 Path::getLogDir() 下）。

#pragma once

#include <QJsonObject>
#include <QString>

namespace ProbeUtil {

// 結束碼（docs/steam_frame_client.md 有同一張表）。
enum ExitCode {
    kExitOk = 0,                   // 完成，而且主要能力存在
    kExitCliError = 1,             // 命令列解析錯誤（共用 CommandLineParser::showError）
    kExitNotBuilt = 10,            // 本建置沒編進這個功能，或平台不支援
    kExitBadInput = 11,            // 輸入檔或參數語意錯誤（由 probe 自己判斷）
    kExitNeedsLaterMilestone = 12, // 需要後續里程碑（例如 xr-probe --session 要 M3a）
    kExitCapabilityAbsent = 13,    // 完成，但主要能力不存在（沒有 m2m decoder、沒有 XR runtime、decoder 開不起來）
    kExitRuntimeError = 14,        // 執行中錯誤
    kExitJsonWriteFailed = 15,     // JSON 寫檔失敗
};

// probe 開始時呼叫：stdout 改行緩衝（被 pipe 時 driver 在 ioctl 裡當掉，已印的行也不會掉），
// 並印一行 `<action>: VipleStream <版號> <平台>`。
void beginProbe(const char* action);

// 預設 JSON 路徑：Path::getLogDir()/probe-<action>-<epochMs>-<pid>.json（同一秒連跑不會相撞）。
QString defaultJsonPath(const QString& action);

// 寫 JSON（縮排、UTF-8）。父目錄不存在會建立；失敗回 false 並在 stderr 說明原因
// （Flatpak 內會附上 sandboxPathHint）。
bool writeJson(const QString& path, const QJsonObject& obj);

// 一行事實：原樣印到 stdout，同時以 SDL_LogInfo 寫進 log，log 那份前面加 `[<tag>] `。
// tag 不含方括號，例如 "VIPLE-V4L2-PROBE"。
#if defined(__GNUC__) || defined(__clang__)
void printLine(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#else
void printLine(const char* tag, const char* fmt, ...);
#endif

// 目前是否在 Flatpak 沙箱內（/.flatpak-info 存在）。
bool isInFlatpak();

// 開檔失敗時的提示字串。在 Flatpak 內且 errno 是 ENOENT/EACCES 時，說明沙箱只看得到
// ~/.var/app/<id> 與 finish-args 明列的路徑，建議用 `flatpak run --filesystem=<dir>:ro`；
// 其他情況回傳 strerror 的內容。
QString sandboxPathHint(const QString& path, int err);

// 收尾：jsonPath 非空時在 stderr 印一行 `json: <path>`，再 fflush(stdout/stderr)，回傳 rc。
// 呼叫端（main.cpp）之後直接 std::_Exit(rc)。
int finish(int rc, const QString& jsonPath);

} // namespace ProbeUtil
