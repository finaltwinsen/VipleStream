// §HID-PROBE：啟動前 HID 裝置健康探測（實作）。設計背景見 hidprobe.h。
//
// 執行緒模型：
//   main thread ──runOnce()/refresh()──► worker（列舉 + 派工）──► 每個 HID 介面一條 probe 執行緒
//                 │ 最多等 kOverallBudgetMs              │ 最多等 kPerDeviceWaitMs（全部並行起跑，
//                 │                                      │ 所以一個 deadline = 每裝置各 1 s）
//   所有等待都是「等不到就放棄」：卡在核心的執行緒不 join、不 CloseHandle，直接遺棄。
//   本檔不呼叫任何 SDL joystick／hidapi API；SDL_Log 只在主執行緒（run 尾段）呼叫。
//
// 遺棄執行緒的後果（審查結論，已寫進 docs/troubleshooting.md）：行程結束時 Windows 會
// 對卡在同步 IOCTL 的執行緒做 I/O rundown 並等 IRP 完成；minidriver 不完成就等到裝置
// 重插，所以事故狀態下關閉程式後可能留下「無視窗殭屍」直到重插。main.cpp 因此在
// app.exec() 之後主動關閉單一實例 mutex，殘留行程才不會擋下一次啟動。

#include "hidprobe.h"

#include "SDL_compat.h"   // SDL_Log*（只在主執行緒呼叫）

#include <QElapsedTimer>
#include <QRegularExpression>
#include <QStringList>
#include <QtGlobal>

#include <mutex>
#include <utility>   // std::as_const

// 結果：主執行緒寫（runOnce / refresh），任意執行緒讀（AsyncConnectionStartThread 上的
// ScHidPassthrough::start() 也會讀），所以用 mutex 保護、讀取一律拿複本。
static std::mutex s_Mutex;
static bool s_HasRun = false;
static QElapsedTimer s_LastRun;
static QList<HidProbeDevice> s_Unresponsive;

static QString hex4(uint16_t v)
{
    return QString::number(v, 16).toUpper().rightJustified(4, QLatin1Char('0'));
}

// ── dev-only 測試開關 ──
// CLAUDE.md 只允許 env var 當 dev-only debug 開關，本變數正是、不是功能設定。
// VIPLE_HID_PROBE_SIMULATE_HANG 以逗號分隔多組，每組三種寫法：
//   "28DE:1304"        直接視為無回應放進清單，且實體介面「不碰」（連 CreateFile 都不做）；
//                      驗四個閘控點 + UI，不需要實體裝置。
//   "28DE:1304:stall"  對符合的實體介面真的 CreateFile + HidD_GetAttributes，然後改為
//                      Sleep(INFINITE) 取代 HidD_GetProductString——用健康的 Puck 走完
//                      真正的逾時／遺棄執行緒／13 介面去重／殭屍行程路徑（審查要求）。
//   "enumerate:stall"  worker 不回來，走「整個列舉逾時」的 vid=0 分支。
struct SimulatedEntry {
    uint16_t vid = 0;
    uint16_t pid = 0;
    bool stall = false;   // true = 對實體介面做到 Attributes 後永久卡住；false = 直接注入、不碰硬體
};

struct SimulatedConfig {
    QList<SimulatedEntry> entries;
    bool enumerateStall = false;
};

static SimulatedConfig parseSimulatedHangs()
{
    SimulatedConfig cfg;
    const QString raw = QString::fromLocal8Bit(qgetenv("VIPLE_HID_PROBE_SIMULATE_HANG")).trimmed();
    if (raw.isEmpty()) {
        return cfg;
    }
    const QStringList tokens = raw.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& tok : tokens) {
        const QStringList parts = tok.trimmed().split(QLatin1Char(':'));
        if (parts.size() == 2 && parts.at(0).compare(QStringLiteral("enumerate"), Qt::CaseInsensitive) == 0 &&
            parts.at(1).compare(QStringLiteral("stall"), Qt::CaseInsensitive) == 0) {
            cfg.enumerateStall = true;
            continue;
        }
        bool okV = false, okP = false;
        const uint vid = parts.value(0).toUInt(&okV, 16);
        const uint pid = parts.value(1).toUInt(&okP, 16);
        if ((parts.size() == 2 || parts.size() == 3) && okV && okP && vid <= 0xFFFF && pid <= 0xFFFF) {
            SimulatedEntry e;
            e.vid = static_cast<uint16_t>(vid);
            e.pid = static_cast<uint16_t>(pid);
            e.stall = (parts.size() == 3 && parts.at(2).compare(QStringLiteral("stall"), Qt::CaseInsensitive) == 0);
            cfg.entries.append(e);
        }
    }
    return cfg;
}

// 從裝置介面路徑解析 VID/PID（\\?\HID#VID_28DE&PID_1304&MI_02&Col03#...）。
// 在起執行緒前就做，卡在 Open/Attributes 階段的裝置也能被點名（審查要求）。
static bool parseVidPidFromPath(const QString& path, uint16_t& vid, uint16_t& pid)
{
    static const QRegularExpression re(QStringLiteral("VID_([0-9A-Fa-f]{4})&PID_([0-9A-Fa-f]{4})"));
    const QRegularExpressionMatch m = re.match(path);
    if (!m.hasMatch()) {
        return false;
    }
    vid = static_cast<uint16_t>(m.captured(1).toUInt(nullptr, 16));
    pid = static_cast<uint16_t>(m.captured(2).toUInt(nullptr, 16));
    return true;
}

struct ProbeOutcome {
    bool workerLaunched = false;
    bool workerFinished = false;
    bool enumFailed = false;     // 列舉層失敗（非逾時）：不閘控
    QString enumError;
    int enumerated = 0;
    int skippedSimulated = 0;
    qint64 maxRespondedMs = 0;   // 有回應的介面中最慢者
    uint16_t slowestVid = 0;
    uint16_t slowestPid = 0;
    QList<HidProbeDevice> unresponsive;
};

#ifdef Q_OS_WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cfgmgr32.h>
#include <hidsdi.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// 一個 HID 介面的探測槽。由 shared_ptr 持有：被遺棄的執行緒若日後（裝置重插、
// IRP 被 cancel）醒來，寫回自己的槽仍安全，不會踩到已回收的記憶體。
struct ProbeSlot {
    std::wstring path;
    uint16_t vid = 0;             // 起執行緒前由路徑解析
    uint16_t pid = 0;
    bool stallSim = false;        // dev-only：Attributes 之後改為永久卡住
    std::atomic<int> stage{static_cast<int>(HidProbeStage::Open)};   // 目前「還沒回來」的那一步
    std::atomic<bool> done{false};
    std::atomic<long long> elapsedMs{0};
};

struct ProbeBatch {
    std::mutex mtx;
    std::condition_variable cv;
    int doneCount = 0;
};

// 單一介面：CreateFile（0 access + share RW：不需要 I/O 權限就能拿 attributes／
// 字串，也不會和 SDL／SC-HID 之後的開啟互斥；獨占／HidHide 的裝置會很快回
// ACCESS_DENIED，視為有回應——SDL 一樣開不起來、不會卡）→ HidD_GetAttributes
// （走 hidclass 快取的 collection info）→ HidD_GetProductString
// （IOCTL_HID_GET_PRODUCT_STRING → hidusb → USB string descriptor control transfer，
// **這一步**在卡死的裝置上永不返回；與事故 cdb 堆疊
// ntdll!NtDeviceIoControlFile <- hid!HidD_GetProductString 一致）。正常裝置整段 ≤ 1 ms。
static void probeOneInterface(std::shared_ptr<ProbeSlot> slot, std::shared_ptr<ProbeBatch> batch)
{
    // 執行緒命名：下次 cdb 堆疊一眼看出是哪個介面卡住（審查建議）。
    // SetThreadDescription 在 Win10 1607+ 才有，動態取得以免舊系統 link 失敗。
    {
        using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        static SetThreadDescriptionFn s_SetThreadDescription =
            reinterpret_cast<SetThreadDescriptionFn>(
                GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));
        if (s_SetThreadDescription) {
            wchar_t name[64];
            swprintf(name, 64, L"VipleHidProbe %04X:%04X", slot->vid, slot->pid);
            s_SetThreadDescription(GetCurrentThread(), name);
        }
    }

    const auto t0 = std::chrono::steady_clock::now();
    HANDLE h = CreateFileW(slot->path.c_str(), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        slot->stage = static_cast<int>(HidProbeStage::Attributes);
        HIDD_ATTRIBUTES attr;
        attr.Size = sizeof(attr);
        if (HidD_GetAttributes(h, &attr)) {
            // 以裝置回報的為準（路徑解析只是備援）
            slot->vid = attr.VendorID;
            slot->pid = attr.ProductID;
        }
        slot->stage = static_cast<int>(HidProbeStage::ProductString);
        if (slot->stallSim) {
            // dev-only：模擬 Puck 在字串查詢上永久卡住（不 CloseHandle、不設 done，
            // 與真事故一模一樣：執行緒與 handle 一起漏到行程結束）
            Sleep(INFINITE);
        }
        wchar_t product[128];
        HidD_GetProductString(h, product, sizeof(product));   // ← 事故的卡死點
        CloseHandle(h);
    }
    slot->elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
    // 走到這裡 = 裝置有回應（開不起來也算）。done 與計數在同一把鎖內更新，
    // worker 逾時掃描時不會看到「done 已設但計數未加」。
    {
        std::lock_guard<std::mutex> lock(batch->mtx);
        slot->done = true;
        batch->doneCount++;
    }
    batch->cv.notify_all();
}

struct ProbeResult {
    std::mutex mtx;
    std::condition_variable cv;
    bool finished = false;
    ProbeOutcome outcome;
};

static void probeWorker(std::shared_ptr<ProbeResult> result, SimulatedConfig sim)
{
    if (sim.enumerateStall) {
        Sleep(INFINITE);   // dev-only：整個列舉層不回來
    }

    ProbeOutcome out;

    // 1. 列舉「目前存在」的 HID 裝置介面。cfgmgr32 只讀 PnP 樹，不碰裝置。
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);   // = GUID_DEVINTERFACE_HID，免 initguid.h

    std::vector<wchar_t> list;
    bool zeroInterfaces = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        ULONG chars = 0;
        CONFIGRET cr = CM_Get_Device_Interface_List_SizeW(&chars, &hidGuid, nullptr,
                                                          CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr != CR_SUCCESS) {
            out.enumFailed = true;
            out.enumError = QStringLiteral("CM_Get_Device_Interface_List_SizeW cr=%1").arg(cr);
            break;
        }
        if (chars <= 1) {
            // 只有結尾的 NUL = 零個 HID 介面（無 HID 的 VM／測試機是正常情況，不算失敗）
            zeroInterfaces = true;
            break;
        }
        list.assign(chars, L'\0');
        cr = CM_Get_Device_Interface_ListW(&hidGuid, nullptr, list.data(), chars,
                                           CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr == CR_SUCCESS) {
            out.enumFailed = false;
            out.enumError.clear();
            break;
        }
        out.enumFailed = true;
        out.enumError = QStringLiteral("CM_Get_Device_Interface_ListW cr=%1").arg(cr);
        if (cr != CR_BUFFER_SMALL) {
            break;   // CR_BUFFER_SMALL = 列舉中有插拔 → 重試；其他錯誤直接放棄
        }
    }

    if (!out.enumFailed && !zeroInterfaces) {
        // 2. 每個介面一條執行緒、全部同時起跑。Puck 一台就 13 個介面，序列化等
        //    13 × 1 s 會吃光總預算；並行則單一 deadline 就等於「每裝置各 1 s」。
        auto batch = std::make_shared<ProbeBatch>();
        // 注意：不能取名 slots —— 那是 Qt 的關鍵字巨集（#define slots），MSVC 會直接語法錯誤
        std::vector<std::shared_ptr<ProbeSlot>> probeSlots;
        for (const wchar_t* p = list.data(); *p != L'\0'; p += wcslen(p) + 1) {
            const QString qpath = QString::fromWCharArray(p);
            uint16_t vid = 0, pid = 0;
            parseVidPidFromPath(qpath, vid, pid);

            bool skip = false, stall = false;
            for (const SimulatedEntry& s : std::as_const(sim.entries)) {
                if (s.vid == vid && s.pid == pid) {
                    if (s.stall) {
                        stall = true;
                    } else {
                        skip = true;   // 模擬項的實體介面一律不碰（dev-only 語意：不碰硬體）
                    }
                    break;
                }
            }
            if (skip) {
                out.skippedSimulated++;
                continue;
            }
            auto slot = std::make_shared<ProbeSlot>();
            slot->path = p;
            slot->vid = vid;
            slot->pid = pid;
            slot->stallSim = stall;
            probeSlots.push_back(slot);
        }
        out.enumerated = static_cast<int>(probeSlots.size()) + out.skippedSimulated;

        for (auto& slot : probeSlots) {
            try {
                std::thread(probeOneInterface, slot, batch).detach();
            }
            catch (...) {
                // 起不了執行緒（資源耗盡）：不因為我們自己的問題去閘控使用者，視同有回應
                std::lock_guard<std::mutex> lock(batch->mtx);
                slot->done = true;
                batch->doneCount++;
            }
        }

        // 3. 等全部回來，或 kPerDeviceWaitMs 到期
        std::unique_lock<std::mutex> lock(batch->mtx);
        batch->cv.wait_for(lock, std::chrono::milliseconds(HidProbe::kPerDeviceWaitMs),
                           [&] { return batch->doneCount >= static_cast<int>(probeSlots.size()); });
        for (auto& slot : probeSlots) {
            if (slot->done) {
                const long long ms = slot->elapsedMs.load();
                if (ms >= out.maxRespondedMs) {
                    out.maxRespondedMs = ms;
                    out.slowestVid = slot->vid;
                    out.slowestPid = slot->pid;
                }
                continue;
            }
            // 遺棄：這條執行緒卡在核心（NtDeviceIoControlFile 或 CreateFile）。使用者態
            // 沒有任何辦法把它拉回來——CancelSynchronousIo 對 HID minidriver 的同步
            // control transfer 無效；從別條執行緒 CloseHandle 會在 IRP_MJ_CLEANUP 等同一個
            // IRP 完成，等於把第二條執行緒也賠進去。所以：不 join、不關 handle，執行緒＋
            // handle 一起漏到行程結束。裝置被拔掉／重插時 hidclass 會 cancel 那個 IRP，
            // 執行緒自己收尾（probeOneInterface 尾段：CloseHandle + 設 done）。
            HidProbeDevice d;
            d.vid = slot->vid;
            d.pid = slot->pid;
            d.path = QString::fromStdWString(slot->path);
            d.stage = static_cast<HidProbeStage>(slot->stage.load());
            out.unresponsive.append(d);
        }
    }

    {
        std::lock_guard<std::mutex> lock(result->mtx);
        result->outcome = out;
        result->finished = true;
    }
    result->cv.notify_all();
}

static ProbeOutcome runProbe(const SimulatedConfig& sim)
{
    ProbeOutcome out;
    auto result = std::make_shared<ProbeResult>();
    try {
        std::thread(probeWorker, result, sim).detach();
        out.workerLaunched = true;
    }
    catch (...) {
        out.workerLaunched = false;
    }
    if (out.workerLaunched) {
        std::unique_lock<std::mutex> lock(result->mtx);
        const bool finished = result->cv.wait_for(lock, std::chrono::milliseconds(HidProbe::kOverallBudgetMs),
                                                  [&] { return result->finished; });
        if (finished) {
            out = result->outcome;
            out.workerLaunched = true;
        }
        out.workerFinished = finished;
    }
    if (out.workerLaunched && !out.workerFinished) {
        // worker 在總預算內沒回來（列舉層本身卡住？）：指不出裝置，但同樣不能冒險
        // 讓 SDL 去列舉——記一筆未知項（vid=pid=0，stage=Enumerate）讓所有閘控生效。
        // worker 之後若醒來只會寫 result（shared_ptr 持有），不再被讀。
        HidProbeDevice d;
        d.path = QStringLiteral("<probe timed out>");
        d.stage = HidProbeStage::Enumerate;
        out.unresponsive.append(d);
    }
    return out;
}

#else  // !Q_OS_WIN32

// 非 Windows 沒有這條事故路徑（Linux hidraw／macOS IOKit 的列舉不做同步字串查詢），
// 只保留 dev-only 模擬項，讓閘控邏輯在各平台都能測。
static ProbeOutcome runProbe(const SimulatedConfig&)
{
    ProbeOutcome out;
    out.workerLaunched = true;
    out.workerFinished = true;
    return out;
}

#endif  // Q_OS_WIN32

// 共用：跑一次、寫回結果、印 log（主執行緒）
static void runAndPublish(const char* why)
{
    QElapsedTimer timer;
    timer.start();

    const SimulatedConfig sim = parseSimulatedHangs();
    ProbeOutcome out = runProbe(sim);
    for (const SimulatedEntry& s : std::as_const(sim.entries)) {
        if (s.stall) {
            continue;   // stall 模擬走真實路徑，結果已在 out.unresponsive
        }
        HidProbeDevice d;
        d.vid = s.vid;
        d.pid = s.pid;
        d.path = QStringLiteral("<simulated>");
        d.stage = HidProbeStage::Simulated;
        d.simulated = true;
        out.unresponsive.append(d);
    }

    {
        std::lock_guard<std::mutex> lock(s_Mutex);
        s_HasRun = true;
        s_LastRun.start();
        s_Unresponsive = out.unresponsive;
    }

    const qint64 elapsedMs = timer.elapsed();
    if (!out.workerLaunched) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-HID] probe (%s) skipped: worker thread could not be created", why);
    }
    else if (out.enumFailed) {
        // 列舉層失敗（非逾時）：不閘控——不能因為我們自己的錯讓正常機器少手把
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-HID] probe (%s) enumeration failed (%s) in %lld ms - no gating applied",
                    why, out.enumError.toUtf8().constData(), static_cast<long long>(elapsedMs));
    }
    else if (out.unresponsive.isEmpty()) {
        // 正常機器每次啟動就這一行（~ms 級）；附上最慢裝置，日後現場 log 能看出誰接近門檻
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-HID] probe (%s) OK: %d HID interface(s) responded in %lld ms (slowest %04X:%04X %lld ms, limit %d ms)",
                    why, out.enumerated, static_cast<long long>(elapsedMs),
                    out.slowestVid, out.slowestPid, static_cast<long long>(out.maxRespondedMs),
                    HidProbe::kPerDeviceWaitMs);
    }
    if (!out.unresponsive.isEmpty()) {
        for (const HidProbeDevice& d : std::as_const(out.unresponsive)) {
            const char* stage = "";
            switch (d.stage) {
            case HidProbeStage::Enumerate:     stage = "enumeration"; break;
            case HidProbeStage::Open:          stage = "CreateFile"; break;
            case HidProbeStage::Attributes:    stage = "HidD_GetAttributes"; break;
            case HidProbeStage::ProductString: stage = "HidD_GetProductString"; break;
            case HidProbeStage::Simulated:     stage = "simulated"; break;
            }
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-HID] UNRESPONSIVE %s vid=%s pid=%s stuck_at=%s path=%s%s",
                        HidProbe::friendlyName(d.vid, d.pid).toUtf8().constData(),
                        hex4(d.vid).toUtf8().constData(), hex4(d.pid).toUtf8().constData(),
                        stage, d.path.toUtf8().constData(),
                        d.simulated ? " (simulated via VIPLE_HID_PROBE_SIMULATE_HANG, dev-only)" : "");
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-HID] probe (%s): %d unresponsive of %d HID interface(s)%s (per-device wait %d ms, took %lld ms, worker %s): %s - "
                    "gamepad detection and Steam Controller passthrough are DISABLED until the device responds again; "
                    "unplug and replug the device (a restart is not required)",
                    why, static_cast<int>(out.unresponsive.size()), out.enumerated,
                    out.skippedSimulated ? " (+simulated skipped)" : "",
                    HidProbe::kPerDeviceWaitMs, static_cast<long long>(elapsedMs),
                    out.workerFinished ? "finished" : "TIMED OUT",
                    HidProbe::describeUnresponsive().toUtf8().constData());
    }
}

void HidProbe::runOnce()
{
    {
        std::lock_guard<std::mutex> lock(s_Mutex);
        if (s_HasRun) {
            return;
        }
    }
    runAndPublish("startup");
}

void HidProbe::refresh(int minIntervalMs)
{
    {
        std::lock_guard<std::mutex> lock(s_Mutex);
        if (s_HasRun && s_LastRun.isValid() && s_LastRun.elapsed() < minIntervalMs) {
            return;
        }
    }
    runAndPublish("refresh");
}

// ── 各平台共用的唯讀存取 ──

bool HidProbe::hasRun()
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    return s_HasRun;
}

bool HidProbe::anyUnresponsive()
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    return !s_Unresponsive.isEmpty();
}

bool HidProbe::anyUnidentified()
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    for (const HidProbeDevice& d : std::as_const(s_Unresponsive)) {
        if (d.vid == 0 || (d.stage != HidProbeStage::ProductString && d.stage != HidProbeStage::Simulated)) {
            return true;
        }
    }
    return false;
}

QList<HidProbeDevice> HidProbe::unresponsive()
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    return s_Unresponsive;
}

bool HidProbe::isUnresponsiveVid(uint16_t vid)
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    for (const HidProbeDevice& d : std::as_const(s_Unresponsive)) {
        if (d.vid == vid) {
            return true;
        }
    }
    return false;
}

bool HidProbe::isUnresponsive(uint16_t vid, uint16_t pid)
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    for (const HidProbeDevice& d : std::as_const(s_Unresponsive)) {
        if (d.vid == vid && d.pid == pid) {
            return true;
        }
    }
    return false;
}

QString HidProbe::friendlyName(uint16_t vid, uint16_t pid)
{
    if (vid == 0x28DE) {
        // gen-2 Steam Controller PID 家族（對齊 sc_hid.cpp isGen2Pid）
        switch (pid) {
        case 0x1302: return QStringLiteral("Steam Controller (USB)");
        case 0x1303: return QStringLiteral("Steam Controller (Bluetooth)");
        case 0x1304: return QStringLiteral("Steam Controller Puck");
        case 0x1305: return QStringLiteral("Steam Controller Nereid dongle");
        default:     return QStringLiteral("Valve HID device");
        }
    }
    if (vid == 0 && pid == 0) {
        return QStringLiteral("Unknown HID device");
    }
    return QStringLiteral("HID device");
}

QString HidProbe::describeUnresponsive()
{
    const QList<HidProbeDevice> list = unresponsive();
    QStringList parts;
    QList<uint32_t> seen;
    for (const HidProbeDevice& d : std::as_const(list)) {
        if (d.stage == HidProbeStage::Enumerate) {
            if (!parts.contains(QStringLiteral("HID enumeration timed out"))) {
                parts.append(QStringLiteral("HID enumeration timed out"));
            }
            continue;
        }
        const uint32_t key = (static_cast<uint32_t>(d.vid) << 16) | d.pid;
        if (seen.contains(key)) {
            continue;   // Puck 13 個介面只列一次
        }
        seen.append(key);
        parts.append(QStringLiteral("%1 (%2:%3)").arg(friendlyName(d.vid, d.pid), hex4(d.vid), hex4(d.pid)));
    }
    return parts.join(QStringLiteral(", "));
}
