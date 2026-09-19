#pragma once

// §HID-PROBE：啟動前 HID 裝置健康探測（Windows 專用；其他平台為 no-op stub）
//
// 事故（2026-09-19，v1.5.271，cdb 實堆疊）：Steam Controller Puck（USB VID_28DE
// PID_1304，13 個 HID 介面）停止回應 control transfer，但 PnP 仍報告裝置 OK/D0。
// SDL3 的 hidapi 在 SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) 時會對「所有」HID
// 介面同步呼叫 HidD_GetProductString；卡死的裝置讓那個 IOCTL 在核心永不返回，
// 主執行緒因而掛在 SdlInputHandler::getUnmappedGamepads()（gamepad.cpp），視窗
// 永遠出不來。重插 Puck 才恢復；SC-HID 轉發場景會再發生。
//
// 解法：main() 早期（log 已開、QML 尚未載入）用純 Win32 對每個 HID 介面做一次
// 有逾時的探測；無回應的裝置記進清單，之後所有會觸發 SDL joystick／hidapi 列舉
// 的進入點都以此清單閘控：
//   1. SdlInputHandler::getUnmappedGamepads()（GUI 路徑最先到這裡：main.qml
//      ApplicationWindow.onCompleted → startAsyncLoad；每場 validateLaunch 也會）
//   2. SdlGamepadKeyNavigation::enable()（StackView.onCompleted → doEarlyInit；
//      CLI stream/quit 路徑最先到這裡）
//   3. SdlInputHandler 建構子（Session::start，每場串流）
//   4. ScHidPassthrough::start()（SDL_hid_enumerate / open / feature report，鎖內）
// 另外 refresh() 讓「串流中才卡死」的情境在下一次 validateLaunch / enable() 前重測，
// 也讓使用者重插裝置後不必重啟程式。
//
// 本類別不呼叫任何 SDL joystick／hidapi API（純 Win32 + Qt 字串），符合 SDL 執行緒規則。
// 刻意比 SDL 保守：SDL3 預設 SDL_HIDAPI_ENUMERATE_ONLY_CONTROLLERS=1 只對手把類
// 裝置查字串，本探測對「所有」HID 介面查——一支卡死的普通鍵盤也會讓本程式停用
// 手把偵測。這是選擇，不是 bug。

#include <QList>
#include <QString>
#include <cstdint>

// 裝置在哪一步不再回應。hidapi 對每個介面（不分 VID）都會做 Open + Attributes，
// 只有 VID 相符（或列舉全部）時才做 ProductString——sc_hid.cpp 的閘控條件據此區分。
enum class HidProbeStage : uint8_t {
    Enumerate,       // 整個列舉層（cfgmgr32 / worker）沒在預算內回來，指不出裝置
    Open,            // CreateFile 沒回來
    Attributes,      // HidD_GetAttributes 沒回來
    ProductString,   // HidD_GetProductString 沒回來（事故的卡死點）
    Simulated,       // VIPLE_HID_PROBE_SIMULATE_HANG 注入（dev-only）
};

struct HidProbeDevice {
    uint16_t vid = 0;        // 由介面路徑 VID_xxxx&PID_xxxx 解析；0 = 路徑無此欄位或整體逾時
    uint16_t pid = 0;
    QString path;            // 裝置介面路徑 \\?\HID#VID_28DE&PID_1304&MI_00#...；模擬項為 "<simulated>"
    HidProbeStage stage = HidProbeStage::ProductString;
    bool simulated = false;  // 由 VIPLE_HID_PROBE_SIMULATE_HANG 注入（dev-only）
};

class HidProbe {
public:
    // 主執行緒呼叫、整個行程只跑一次（重複呼叫直接返回）。
    // 內部起 worker 執行緒做列舉＋逐介面並行探測；主執行緒最多等 kOverallBudgetMs。
    // 正常機器 ~ms；有裝置卡死時最多 kPerDeviceWaitMs 後帶著清單返回。
    static void runOnce();

    // 主執行緒呼叫：距上次探測超過 minIntervalMs 才重跑一次（正常機器 ~ms）。
    // 用在每場 validateLaunch 與手把 UI 導覽重新啟用前，涵蓋「串流中才卡死」與
    // 「使用者已重插」兩種狀態改變。
    static void refresh(int minIntervalMs = 2000);

    static bool hasRun();
    static bool anyUnresponsive();
    // vid == 0 或卡在 ProductString 之前的階段：hidapi 對任何 VID 都會做那幾步，
    // 所以連只列舉 0x28DE 的 SDL_hid_enumerate 都會撞到。
    static bool anyUnidentified();
    static QList<HidProbeDevice> unresponsive();   // 複本（可從任意執行緒讀）
    static bool isUnresponsiveVid(uint16_t vid);
    static bool isUnresponsive(uint16_t vid, uint16_t pid);

    // 人類可讀清單，同 VID:PID 去重（Puck 13 個介面只列一次）：
    //   "Steam Controller Puck (28DE:1304)"，多個以 ", " 連接；沒有則為空字串。
    static QString describeUnresponsive();

    // 單一裝置的友善名稱（Valve gen-2 PID 家族有對照表，其餘 "HID device"）
    static QString friendlyName(uint16_t vid, uint16_t pid);

    // 逾時（審查結論）：全部介面並行探測，所以拉長 per-device wait 對健康機器零成本
    // （全部回來 cv 立刻醒）；1000 ms 給 USB selective-suspend 喚醒、多層 hub、
    // 藍牙／BLE HID 字串查詢足夠的裕度，只有真卡死才多等這 1 秒。
    // 總預算必須 > per-device wait，否則 worker 會在完成前被主執行緒放棄。
    static constexpr int kPerDeviceWaitMs = 1000;
    static constexpr int kOverallBudgetMs = 2500;
};
