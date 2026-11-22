#include "session.h"
#include <chrono>
#include <cstdlib>
#include <thread>
#include "backend/nvcomputer.h"
#include "backend/identitymanager.h"
#include "backend/relaylookup.h"
#include "backend/relaytcptunnel.h"
#include "backend/relayudptunnel.h"
#include "settings/streamingpreferences.h"
#include "streaming/streamutils.h"
#include "streaming/transfer/filetransferclient.h"
#include "streaming/video/overlaymanager.h"
#include "backend/richpresencemanager.h"
#include "backend/hidprobe.h"   // §HID-PROBE
#include "backend/sfenv.h"      // §SF-ENV（M2a）
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#ifdef HAVE_OPENXR
#include "streaming/xr/xrcontext.h"  // §VR M3a X1
#include <QFile>
#endif

#include <Limelight.h>
#include "HolePunch.h"  // VipleStream: LocalControlPort global
#include "SDL_compat.h"
#include "utils.h"

#include <map>
#include <memory>
#include <tuple>

#ifdef HAVE_FFMPEG
#include "video/ffmpeg.h"
#include "backend/tailscalepath.h"
#include "streaming/batterymonitor.h"  // §VR-LOWBATT
#include "streaming/vr/vrnetmode.h"  // §VR-NETSEL
#include "streaming/vr/vrnetbench.h"  // §VR-NETBENCH
#include "streaming/audio/miccapture.h"  // §VR-MIC
#endif

#ifdef HAVE_SLVIDEO
#include "video/slvid.h"
#endif

#ifdef Q_OS_WIN32
#include <windows.h>
#include <SDL_syswm.h>   // §INPUT-STALL watchdog 需要 HWND
#include <dbghelp.h>     // §INPUT-STALL 主執行緒堆疊擷取
#include <wct.h>         // §INPUT-STALL 等待鏈（Wait Chain Traversal，advapi32）
#include <cstdarg>
#include <string>
#include <QDateTime>
#include <QCoreApplication>
#include "path.h"        // §INPUT-STALL 不輪替的紀錄檔放在 log 目錄
#ifdef _MSC_VER
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "advapi32.lib")
#endif
// Scaling the icon down on Win32 looks dreadful, so render at lower res
#define ICON_SIZE 32
#else
#define ICON_SIZE 64
#endif

// §INPUT-STALL 診斷（2026-09-21 鍵鼠斷續事故）：主執行緒是鍵鼠事件唯一入口，
// 影像卻由 Pacer 執行緒繪製——主執行緒短暫卡住時畫面照常、鍵鼠「短暫失控後
// 恢復」。既有 log 沒有主迴圈節拍，這裡用獨立 watchdog 執行緒量測：主迴圈
// 每圈更新 s_LoopBeat / s_LoopPhase（-1 = 在 SDL_WaitEventTimeout 內；否則為
// 正在分派的 SDL 事件型別，SDL_USEREVENT 以 0x10000|code 表示）。watchdog 每
// 10 ms 檢查，節拍停滯 ≥50 ms 且「停滯期間有使用者輸入」（GetLastInputInfo，
// 合成輸入也算）且本視窗在前景時記一筆 [VIPLE-INPUT-STALL]，恢復時再記總長。
// 停在 wait 相位＝卡在 SDL 內部（PumpEvents／joystick／hidapi 列舉）；停在
// 事件相位＝我們自己的 handler。純閒置不會誤報。
// 只在 Windows 啟動：「停滯期間有使用者輸入」與「本視窗在前景」都靠 Win32 API 判斷，堆疊擷取也是。其他平台
// 沒有這兩個條件，只能退成「事件處理 ≥50 ms 就報」——重建解碼器、XR session 重建這類本來就會超過 50 ms 的
// 正常處理全被印成 "with user input pending"，又沒有堆疊可查，是純誤報。所以非 Windows 不開 watchdog 執行緒
// （節拍與相位照樣更新，只是兩次 atomic 寫入，沒有人讀）。
#include <atomic>
namespace {
std::atomic<uint32_t> s_LoopBeat {0};
std::atomic<int32_t>  s_LoopPhase {-2};   // -3 迴圈開頭（等待之前的輪詢）, -2 未啟動, -1 wait, >=0 事件
std::atomic<bool>     s_StallWatchdogRun {false};
#ifdef Q_OS_WIN32
HWND                  s_StallWatchdogHwnd = nullptr;
DWORD                 s_StallMainThreadId = 0;
static uint32_t stallNowMs() { return GetTickCount(); }   // 與 GetLastInputInfo 同時基

// §INPUT-STALL 堆疊擷取（2026-09-22；2026-11-20 改寫）：儀器化第一晚抓到 26 次主迴圈停頓，每次都
// 固定 ≈2016 ms、phase 全是 SDL_WaitEventTimeout（卡在 SDL 內部），log 前後沒有任何
// 觸發事件。要知道是 SDL 的哪條路（hidapi 列舉／DirectInput／WGI／訊息泵）只能看堆疊。
// 兩個月下來證據一筆都沒留住（log 只保留幾份、CLI 串流只寫 stdout），所以 11-20 起：
//   - 一次停頓抄兩次（≥300 ms 與 ≥1000 ms，兩次至少相隔 700 ms）：最上面幾格相同＝卡在同一個呼叫；不同＝一連串慢呼叫。
//   - 每一格都印「模組+RVA」，SDL3.dll 沒有 PDB 也能拿同版符號離線解；解得到符號時一併印。
//   - 印主執行緒的等待鏈（Wait Chain Traversal）：跨行程的等待（輸入法／hook／SendMessage／ALPC）
//     鏈上會出現別的行程；卡在同步 IOCTL 時鏈上只有本執行緒。
//   - 同樣內容附加寫進 log 目錄下不輪替的 input-stall.log（上限 1 MB，滿了只留後半）。
// 全部在 watchdog 執行緒上做。主執行緒被暫停的那一小段只做三件不拿使用者態鎖、不配置記憶體的事：
// GetThreadContext、VirtualQuery（都是系統呼叫）、把堆疊記憶體 memcpy 到靜態緩衝區。展開堆疊
// （StackWalk64 會進 dbghelp，可能配置記憶體、拿 loader 相關的鎖）一律等恢復主執行緒之後，
// 對著那份副本做——主執行緒若正拿著 heap 鎖被暫停，在暫停期間配置記憶體就是死結。
// dbghelp 非 thread-safe，只在 watchdog 執行緒使用。
// §REVIEW-1120 第三輪 C43：等待鏈在恢復主執行緒後立刻抓（展開堆疊、解符號之前），節點先放靜態緩衝區、晚點才格式化
struct StallWaitChain {
    bool ok = false;
    const char* failed = nullptr;  // 失敗的 API 名稱
    DWORD err = 0;
    DWORD count = 0;
    BOOL cycle = FALSE;
    bool beatMoved = false;        // 抓完時主迴圈已經恢復（這份等待鏈可能已經不是停頓當下的狀態）
};

struct StallStack {
    DWORD64 pcs[32];
    int n;
    bool movedAtSuspend;  // §REVIEW-1120 第三輪 C43：暫停主執行緒當下節拍已經變了＝抄到的不是停頓現場
    StallWaitChain wct;
};

static constexpr uint32_t kStallCapture1Ms = 300;
static constexpr uint32_t kStallCapture2Ms = 1000;
// §REVIEW-1120 第三輪 C38：第二次至少要比第一次晚這麼久。停頓常在主迴圈閒置（SDL_WaitEventTimeout 最多約 1 秒）之後才被
// 回報，那時 stale 早就過了 300 ms，兩個門檻會在相鄰兩輪（約 10 ms）先後成立，兩份堆疊必然相同，看不出是不是同一個呼叫
static constexpr uint32_t kStallCaptureGapMs = 700;
// 等待鏈的節點（只有 watchdog 執行緒讀寫）
static WAITCHAIN_NODE_INFO s_StallWctNodes[WCT_MAX_NODE_COUNT];
static constexpr qint64 kStallFileMaxBytes = 1024 * 1024;

// 主執行緒堆疊的副本（只有 watchdog 執行緒讀寫；256 KB 足夠涵蓋 32 格）
static BYTE    s_StallStackCopy[256 * 1024];
static DWORD64 s_StallStackCopyBase = 0;
static SIZE_T  s_StallStackCopyLen = 0;

// StackWalk64 的記憶體讀取：落在副本範圍內的從副本讀（主執行緒已經繼續跑，現場的堆疊內容可能變了），
// 其餘（程式碼、unwind 資料）直接讀行程本身。
static BOOL CALLBACK stallReadMemory(HANDLE hProcess, DWORD64 addr, PVOID buf, DWORD size, LPDWORD bytesRead)
{
    if (addr >= s_StallStackCopyBase) {
        const DWORD64 off = addr - s_StallStackCopyBase;
        if (off < s_StallStackCopyLen && size <= s_StallStackCopyLen - off) {
            memcpy(buf, s_StallStackCopy + off, size);
            if (bytesRead) *bytesRead = size;
            return TRUE;
        }
    }
    SIZE_T got = 0;
    const BOOL ok = ReadProcessMemory(hProcess, (LPCVOID)(uintptr_t)addr, buf, size, &got);
    if (bytesRead) *bytesRead = (DWORD)got;
    return ok;
}

// 印一行到一般 log，同時（rec 不為空時）累積到要寫進 input-stall.log 的文字
static void stallEmit(std::string* rec, const char* fmt, ...)
{
    char line[640];
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-INPUT-STALL] %s", line);
    if (rec != nullptr) {
        rec->append(line);
        rec->push_back('\n');
    }
}

// 附加寫入 input-stall.log（watchdog 執行緒；主執行緒此時沒有被暫停）。超過上限時只留後半。
static void stallAppendToFile(const std::string& rec)
{
    if (rec.empty()) return;
    static const QString s_path = QDir(Path::getLogDir()).filePath(QStringLiteral("input-stall.log"));
    QFile f(s_path);
    if (f.size() > kStallFileMaxBytes) {
        QByteArray tail;
        if (f.open(QIODevice::ReadOnly)) {
            f.seek(f.size() - kStallFileMaxBytes / 2);
            tail = f.readAll();
            f.close();
            const int nl = tail.indexOf('\n');
            if (nl >= 0) tail.remove(0, nl + 1);   // 從完整的一行開始
        }
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write("[older entries trimmed]\n");
            f.write(tail);
            f.close();
        }
    }
    if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
        f.write(rec.data(), (qint64)rec.size());
        f.close();
    }
}

// 主執行緒的等待鏈（同步查詢、唯讀；不需要暫停主執行緒）。只抓、不格式化（節點放 s_StallWctNodes）：
// 要在主執行緒剛恢復、還沒做任何慢的事（載入符號、展開堆疊）之前呼叫，才是停頓當下的狀態。
// COM 的等待要另外註冊 ole32 的 callback 才展得開，這裡沒有做：鏈上會停在 COM 節點，看得到「在等 COM」但看不到對方是誰。
static void stallReadWaitChain(StallWaitChain* w, uint32_t stalledBeat)
{
    *w = StallWaitChain();
    if (s_StallMainThreadId == 0) return;
    HWCT wct = OpenThreadWaitChainSession(0, nullptr);
    if (wct == nullptr) {
        w->failed = "OpenThreadWaitChainSession";
        w->err = GetLastError();
        return;
    }
    DWORD count = WCT_MAX_NODE_COUNT;
    if (!GetThreadWaitChain(wct, 0, WCTP_GETINFO_ALL_FLAGS, s_StallMainThreadId, &count, s_StallWctNodes, &w->cycle)) {
        w->failed = "GetThreadWaitChain";
        w->err = GetLastError();
    }
    else {
        w->ok = true;
        w->count = count > WCT_MAX_NODE_COUNT ? WCT_MAX_NODE_COUNT : count;   // 鏈比緩衝區長時 count 回報實際長度
    }
    CloseThreadWaitChainSession(wct);
    w->beatMoved = s_LoopBeat.load() != stalledBeat;
}

// 暫停主執行緒、抄暫存器與堆疊記憶體、恢復，之後才展開。回傳格數（0 = 沒抄到）。
// stalledBeat＝停頓中的節拍值：暫停當下（ResumeThread 之前）就比對，抄到的是不是停頓現場以那一刻為準（C43）
static void stallCaptureMainThreadStack(StallStack* out, uint32_t stalledBeat)
{
    out->n = 0;
    out->movedAtSuspend = false;
    out->wct = StallWaitChain();
    if (s_StallMainThreadId == 0) return;
    HANDLE hThread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
                                FALSE, s_StallMainThreadId);
    if (hThread == nullptr) {
        stallReadWaitChain(&out->wct, stalledBeat);  // 堆疊抄不到，等待鏈照樣抓
        return;
    }

    // 符號引擎先初始化好（會配置記憶體、載入模組清單），不可留到暫停期間
    static bool s_symInit = false;
    if (!s_symInit) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_NO_PROMPTS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        s_symInit = true;
    }

    CONTEXT ctx;
    ZeroMemory(&ctx, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    bool haveCtx = false;
    s_StallStackCopyBase = 0;
    s_StallStackCopyLen = 0;

    // ── 暫停期間：不配置記憶體、不印 log、不呼叫 dbghelp ──
    if (SuspendThread(hThread) != (DWORD)-1) {
        if (GetThreadContext(hThread, &ctx)) {
            haveCtx = true;
#if defined(_M_X64)
            const uintptr_t sp = (uintptr_t)ctx.Rsp;
#elif defined(_M_ARM64)
            const uintptr_t sp = (uintptr_t)ctx.Sp;
#else
            const uintptr_t sp = (uintptr_t)ctx.Esp;
#endif
            // 堆疊已提交的部分（sp 所在頁到堆疊底）是同一個區域；只抄到區域結尾，不會讀到未提交的頁
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery((LPCVOID)sp, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_COMMIT &&
                    !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
                const uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
                SIZE_T len = end > sp ? (SIZE_T)(end - sp) : 0;
                if (len > sizeof(s_StallStackCopy)) len = sizeof(s_StallStackCopy);
                memcpy(s_StallStackCopy, (const void*)sp, len);
                s_StallStackCopyBase = (DWORD64)sp;
                s_StallStackCopyLen = len;
            }
        }
        out->movedAtSuspend = s_LoopBeat.load() != stalledBeat;   // atomic 讀取：不拿鎖、不配置記憶體
        ResumeThread(hThread);
    }
    // ── 主執行緒已恢復 ──
    // 等待鏈立刻抓：下面的 SymRefreshModuleList／StackWalk64 第一次碰到某個模組時要載入符號，可能花上幾百毫秒
    stallReadWaitChain(&out->wct, stalledBeat);

    if (haveCtx && s_StallStackCopyLen > 0) {
        // §REVIEW-1120：SymInitialize 只在第一次擷取時列模組，之後才載入的 DLL（驅動的 user-mode 元件、輸入法、
        // 外掛）不在清單裡，StackWalk64 查不到 unwind 資料就在那一格斷掉。每次展開前補一次。
        // 放在恢復主執行緒之後：暫停期間不可呼叫 dbghelp（它會配置記憶體、可能等 loader lock）；暫存器與堆疊也已經
        // 抄好，這裡就算被擋住也只是延後展開，不影響擷取時刻。失敗就照舊用舊清單，回傳值不看。
        SymRefreshModuleList(GetCurrentProcess());
        STACKFRAME64 sf;
        ZeroMemory(&sf, sizeof(sf));
#if defined(_M_X64)
        const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
        sf.AddrPC.Offset = ctx.Rip;
        sf.AddrFrame.Offset = ctx.Rbp;
        sf.AddrStack.Offset = ctx.Rsp;
#elif defined(_M_ARM64)
        const DWORD machine = IMAGE_FILE_MACHINE_ARM64;
        sf.AddrPC.Offset = ctx.Pc;
        sf.AddrFrame.Offset = ctx.Fp;
        sf.AddrStack.Offset = ctx.Sp;
#else
        const DWORD machine = IMAGE_FILE_MACHINE_I386;
        sf.AddrPC.Offset = ctx.Eip;
        sf.AddrFrame.Offset = ctx.Ebp;
        sf.AddrStack.Offset = ctx.Esp;
#endif
        sf.AddrPC.Mode = sf.AddrFrame.Mode = sf.AddrStack.Mode = AddrModeFlat;
        while (out->n < (int)(sizeof(out->pcs) / sizeof(out->pcs[0])) &&
               StackWalk64(machine, GetCurrentProcess(), hThread, &sf, &ctx, stallReadMemory,
                           SymFunctionTableAccess64, SymGetModuleBase64, nullptr) &&
               sf.AddrPC.Offset != 0) {
            out->pcs[out->n++] = sf.AddrPC.Offset;
        }
    }
    CloseHandle(hThread);
}

// 每一格印「模組+RVA」，解得到符號再加「(符號+位移)」。prev 不為空時在標題列註明和上一次相同的格數。
static void stallLogStack(std::string* rec, const StallStack& st, uint32_t staleMs, int index, const StallStack* prev)
{
    char same[64] = "";
    if (prev != nullptr) {
        int top = 0;
        while (top < st.n && top < prev->n && st.pcs[top] == prev->pcs[top]) top++;
        // 由最外層往內比對：兩次堆疊深度不同時，共同的呼叫者也看得出來
        int bottom = 0;
        while (bottom < st.n && bottom < prev->n &&
               st.pcs[st.n - 1 - bottom] == prev->pcs[prev->n - 1 - bottom]) bottom++;
        SDL_snprintf(same, sizeof(same), ", same as capture 1: top %d, outermost %d", top, bottom);
    }
    stallEmit(rec, "main thread stack %d at %u ms (%d frames%s):", index, staleMs, st.n, same);
    for (int i = 0; i < st.n; i++) {
        char modName[MAX_PATH] = "?";
        HMODULE hMod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)st.pcs[i], &hMod) && hMod != nullptr) {
            char full[MAX_PATH];
            const DWORD n = GetModuleFileNameA(hMod, full, sizeof(full));
            if (n > 0 && n < sizeof(full)) {
                const char* base = strrchr(full, '\\');
                SDL_strlcpy(modName, base ? base + 1 : full, sizeof(modName));
            }
        }
        const unsigned long long rva =
            (unsigned long long)(hMod ? st.pcs[i] - (DWORD64)(uintptr_t)hMod : st.pcs[i]);
        char symBuf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* sym = (SYMBOL_INFO*)symBuf;
        ZeroMemory(symBuf, sizeof(symBuf));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(GetCurrentProcess(), st.pcs[i], &disp, sym)) {
            stallEmit(rec, "  #%02d %s+0x%llx (%s+0x%llx)", i, modName, rva, sym->Name, (unsigned long long)disp);
        }
        else {
            stallEmit(rec, "  #%02d %s+0x%llx", i, modName, rva);
        }
    }
}

static const char* stallWctTypeName(int t)
{
    switch (t) {
    case WctCriticalSectionType: return "CriticalSection";
    case WctSendMessageType:     return "SendMessage";
    case WctMutexType:           return "Mutex";
    case WctAlpcType:            return "ALPC";
    case WctComType:             return "COM";
    case WctThreadWaitType:      return "ThreadWait";
    case WctProcessWaitType:     return "ProcessWait";
    case WctThreadType:          return "Thread";
    case WctComActivationType:   return "ComActivation";
    case WctSocketIoType:        return "SocketIo";
    case WctSmbIoType:           return "SmbIo";
    default:                     return "Unknown";
    }
}

static const char* stallWctStatusName(int s)
{
    switch (s) {
    case WctStatusNoAccess:     return "no-access";
    case WctStatusRunning:      return "running";
    case WctStatusBlocked:      return "blocked";
    case WctStatusPidOnly:      return "pid-only";
    case WctStatusPidOnlyRpcss: return "pid-only-rpcss";
    case WctStatusOwned:        return "owned";
    case WctStatusNotOwned:     return "not-owned";
    case WctStatusAbandoned:    return "abandoned";
    case WctStatusError:        return "error";
    default:                    return "unknown";
    }
}

// 印 stallReadWaitChain 抓到的等待鏈（節點在 s_StallWctNodes；這一筆紀錄格式化完之前不可以再抓下一次）
static void stallLogWaitChain(std::string* rec, const StallWaitChain& w)
{
    if (s_StallMainThreadId == 0) return;
    if (!w.ok) {
        if (w.failed != nullptr) {
            stallEmit(rec, "wait chain: %s failed (%lu)", w.failed, w.err);
        }
        return;
    }
    const DWORD count = w.count;
    stallEmit(rec, "wait chain (%lu node(s)%s)%s:", count, w.cycle ? ", DEADLOCK CYCLE" : "",
              w.beatMoved ? " (main loop resumed before the wait chain was read)" : "");
    const DWORD myPid = GetCurrentProcessId();
    for (DWORD i = 0; i < count; i++) {
        const WAITCHAIN_NODE_INFO& n = s_StallWctNodes[i];
        if (n.ObjectType == WctThreadType) {
            char procName[MAX_PATH] = "";
            const DWORD pid = n.ThreadObject.ProcessId;
            if (pid == myPid) {
                SDL_strlcpy(procName, "self", sizeof(procName));
            }
            else if (HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                char full[MAX_PATH];
                DWORD len = sizeof(full);
                if (QueryFullProcessImageNameA(hp, 0, full, &len)) {
                    const char* base = strrchr(full, '\\');
                    SDL_strlcpy(procName, base ? base + 1 : full, sizeof(procName));
                }
                CloseHandle(hp);
            }
            stallEmit(rec, "  [%lu] Thread pid=%lu (%s) tid=%lu %s wait=%lu ms ctxsw=%lu",
                      i, pid, procName[0] ? procName : "?", n.ThreadObject.ThreadId,
                      stallWctStatusName(n.ObjectStatus), n.ThreadObject.WaitTime, n.ThreadObject.ContextSwitches);
        }
        else {
            // ObjectName 不保證有結尾的 0：先抄到自己的緩衝區補上
            WCHAR wname[WCT_OBJNAME_LENGTH + 1];
            memcpy(wname, n.LockObject.ObjectName, sizeof(n.LockObject.ObjectName));
            wname[WCT_OBJNAME_LENGTH] = 0;
            char objName[WCT_OBJNAME_LENGTH * 3 + 1] = "";
            WideCharToMultiByte(CP_UTF8, 0, wname, -1, objName, sizeof(objName), nullptr, nullptr);
            objName[sizeof(objName) - 1] = '\0';
            stallEmit(rec, "  [%lu] %s %s name=\"%s\"%s",
                      i, stallWctTypeName(n.ObjectType), stallWctStatusName(n.ObjectStatus), objName,
                      n.LockObject.Alertable ? " alertable" : "");
        }
    }
}

static void stallPhaseName(int32_t phase, char* buf, size_t len)
{
    if (phase == -2) SDL_snprintf(buf, len, "not-started");
    else if (phase == -3) SDL_snprintf(buf, len, "loop-top");
    else if (phase == -1) SDL_snprintf(buf, len, "SDL_WaitEventTimeout");
    else if (phase & 0x10000) SDL_snprintf(buf, len, "SDL_USEREVENT code=%d", phase & 0xFFFF);
    else SDL_snprintf(buf, len, "event 0x%x", phase);
}

static int inputStallWatchdogProc(void*)
{
    uint32_t lastBeat = s_LoopBeat.load();
    uint32_t lastBeatChangeMs = stallNowMs();
    bool reported = false;
    int captures = 0;          // 這次停頓已經抄了幾次堆疊（0～2）
    uint32_t cap1Ms = 0;       // 第一次抄的時刻（C38：第二次至少晚 kStallCaptureGapMs）
    StallStack firstStack = {};
    int32_t reportedPhase = -2;
    uint32_t stallCount = 0;
    while (s_StallWatchdogRun.load()) {
        SDL_Delay(10);
        uint32_t now = stallNowMs();
        uint32_t beat = s_LoopBeat.load();
        if (beat != lastBeat) {
            if (reported) {
                char pn[48]; stallPhaseName(reportedPhase, pn, sizeof(pn));
                // 有抄過堆疊的停頓，把總長也寫進 input-stall.log，一筆紀錄才完整
                std::string rec;
                stallEmit(captures > 0 ? &rec : nullptr,
                          "recovered: main loop was unresponsive %u ms (phase=%s) #%u",
                          now - lastBeatChangeMs, pn, stallCount);
                stallAppendToFile(rec);
                reported = false;
                captures = 0;
            }
            lastBeat = beat;
            lastBeatChangeMs = now;
            continue;
        }
        uint32_t stale = now - lastBeatChangeMs;
        if (reported && ((captures == 0 && stale >= kStallCapture1Ms) ||
                         (captures == 1 && stale >= kStallCapture2Ms && now - cap1Ms >= kStallCaptureGapMs))) {
            if (captures == 0) cap1Ms = now;
            StallStack st;
            stallCaptureMainThreadStack(&st, lastBeat);
            // §REVIEW-1120 第三輪 C43：是不是停頓現場看「暫停主執行緒的當下」節拍有沒有變（抄完、展開之後才看會把
            // 展開堆疊、載入符號那段時間內恢復的停頓也標成不可信——暫存器與堆疊副本是暫停時抄的，一定是現場）
            const bool stillStalled = !st.movedAtSuspend;
            char pn[48]; stallPhaseName(reportedPhase, pn, sizeof(pn));
            std::string rec;
            if (captures == 0) {
                rec = "==== " + QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toStdString() +
                      " v" + QCoreApplication::applicationVersion().toStdString() +
                      " pid=" + std::to_string(GetCurrentProcessId()) + " ====\n";
                stallEmit(&rec, "stall #%u: main loop unresponsive %u ms so far (phase=%s)", stallCount, stale, pn);
            }
            else {
                rec = "---- " + QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toStdString() + " ----\n";
            }
            stallLogStack(&rec, st, stale, captures + 1, captures == 1 ? &firstStack : nullptr);
            if (!stillStalled) {
                stallEmit(&rec, "  (main loop resumed before the thread was suspended; this stack is past the stall)");
            }
            stallLogWaitChain(&rec, st.wct);
            stallAppendToFile(rec);
            if (captures == 0) firstStack = st;
            captures++;
        }
        if (stale < 50 || reported) continue;
        bool inputPending = true;
        LASTINPUTINFO lii; lii.cbSize = sizeof(lii); lii.dwTime = 0;
        if (GetLastInputInfo(&lii)) {
            // 只有「節拍停滯後仍有使用者輸入且已等 ≥50 ms」才算鍵鼠受影響
            // 嚴格晚於節拍停滯起點（GetTickCount 15.6 ms 粒度下 >=0 會把「最後一筆
            // 輸入剛處理完就閒置」誤判成停頓——首輪合成測試結尾就出現過一次）
            inputPending = ((int32_t)(lii.dwTime - lastBeatChangeMs) > 0) && (now - lii.dwTime) >= 50;
        }
        if (inputPending && s_StallWatchdogHwnd != nullptr && GetForegroundWindow() != s_StallWatchdogHwnd) {
            inputPending = false;   // 使用者在別的視窗操作，主迴圈閒置是正常的
        }
        if (!inputPending) continue;
        reported = true;
        reportedPhase = s_LoopPhase.load();
        stallCount++;
        char pn[48]; stallPhaseName(reportedPhase, pn, sizeof(pn));
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-INPUT-STALL] main loop unresponsive %u ms with user input pending (phase=%s) #%u",
                    stale, pn, stallCount);
    }
    return 0;
}
#endif // Q_OS_WIN32
} // namespace


#define SDL_CODE_FLUSH_WINDOW_EVENT_BARRIER 100
#define SDL_CODE_GAMECONTROLLER_RUMBLE 101
#define SDL_CODE_GAMECONTROLLER_RUMBLE_TRIGGERS 102
#define SDL_CODE_GAMECONTROLLER_SET_MOTION_EVENT_STATE 103
#define SDL_CODE_GAMECONTROLLER_SET_CONTROLLER_LED 104
#define SDL_CODE_GAMECONTROLLER_SET_ADAPTIVE_TRIGGERS 105
#define SDL_CODE_VR_MESSAGE 106  // §VR：data1＝長度、data2＝SDL_malloc 的 TLV 副本
// §REVIEW-1119：還在 SDL 佇列裡、沒被主迴圈處理的 VR 訊息數（見 clVrMessage）
static std::atomic<int> s_VrMsgQueued{0};
static constexpr int kVrMsgQueueMax = 256;
// §VR M3a X3：XR 事件（X4 的 xrinput 會從 XR thread 推這些事件到 main loop）
#define SDL_CODE_XR_RECENTER 107
// §VR M3a X4：XR 射線滑鼠（XR frame thread → main loop → SdlInputHandler::handleXr*）
#define SDL_CODE_XR_POINTER 108  // data1＝(u16 << 16) | v16
#define SDL_CODE_XR_BUTTON 109   // data1＝Limelight BUTTON_*、data2＝1 按下／0 放開
#define SDL_CODE_XR_SCROLL 110   // data1＝高解析捲動量（intptr_t）
// §VR M3a X5：XR session 被 runtime 結束（data1＝XrContext::kEndedLoss／kEndedExit）；
// 重建計時到期（data1＝generation，過期的忽略）
#define SDL_CODE_XR_ENDED 111
#define SDL_CODE_XR_REBUILD 112
// §VR M3a 虛擬鍵盤：data1＝VK | (modifiers << 16)、data2＝1 按下／0 放開
#define SDL_CODE_XR_KEY 113
// §VR-WHEEL：快捷鍵切換了方向盤模式（XR frame thread 推），data1＝1 開／0 關
#define SDL_CODE_XR_WHEEL 114
// §XR-PADMODE：2D 串流的手把模式。PAD＝手把狀態（data1＝SDL_malloc 的 XrPadState，收的那一邊釋放）；
// PADMODE＝模式切換了（data1＝1 開／0 關）
#define SDL_CODE_XR_PAD 115
#define SDL_CODE_XR_PADMODE 116

#include <openssl/rand.h>
#include <algorithm>
#include <QNetworkInterface>  // §FLAT-ML
#include <cmath>

#include <QtEndian>
#include <QCoreApplication>
#include <QThreadPool>
#include <QSvgRenderer>
#include <QPainter>
#include <QImage>
#include <QGuiApplication>
#include <QCursor>
#include <QScreen>
#include <QStandardPaths>
#include <QDir>
#include <QHostAddress>

#ifdef VIPLE_MPQUIC
// §Q Phase 5 (5f): 0-RTT ticket store setter from moonlight-common-c.
// Forward-declared to avoid pulling QuicTransport.h (which transitively
// includes picoquic platform headers that conflict with Qt's net stack).
extern "C" void quicSetTicketStorePath(const char* path);
#endif

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QQuickOpenGLUtils>
#endif

// §VR-HALF-RATE：頭盔的顯示更新率（追蹤樣本的送出頻率用；半速串流時 m_StreamConfig.fps 是一半）
static int s_VrTrackingHz = 0;

// §VR-10BIT：要不要向 host 要 10-bit 的格式。一般串流看「HDR」設定；PCVR 沒有 HDR，10-bit 只是編碼精度，
// 由自己的設定（vr10bit）決定——HDR 開著不會讓 PCVR 變成 10-bit，反過來也不會。
//
// Steam Frame（Linux arm64）目前不行：iris 解碼器（V4L2）在 FFmpeg 的 v4l2m2m 路徑上只協商得到 8-bit 的輸出格式
// （NV12），Main10 的串流解出來的影格版面和 dmabuf 匯入對不上，client 會在第一幀崩潰（2026-10-08 頭盔實測）。
// 所以真的頭盔上一律不要求 10-bit；--vr-emulate（不解到 XR）不受限，用來驗 host 端。
static bool wants10Bit(const StreamingPreferences* prefs)
{
    if (prefs->displayTarget != StreamingPreferences::DT_PCVR) {
        return prefs->enableHdr;
    }
#if defined(Q_OS_LINUX) && defined(Q_PROCESSOR_ARM_64)
    if (!prefs->vrEmulate) {
        return false;
    }
#endif
    return prefs->vr10Bit;
}

#define CONN_TEST_SERVER "qt.conntest.moonlight-stream.org"

// [VIPLE-STARTUP]（A44，量測）：client log 的時間戳只到秒，開場各階段的時間點自己帶毫秒。steady clock 的絕對值：
// 和 xrcontext.cpp 的 bring-up 行、同一台機器上的另一個行程（VR 介面與它生出的串流子行程）都是同一個時鐘，可以直接相減
static long long startupNowMs()
{
    return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch()).count());
}

CONNECTION_LISTENER_CALLBACKS Session::k_ConnCallbacks = {
    Session::clStageStarting,
    nullptr,
    Session::clStageFailed,
    nullptr,
    Session::clConnectionTerminated,
    Session::clLogMessage,
    Session::clRumble,
    Session::clConnectionStatusUpdate,
    Session::clSetHdrMode,
    Session::clRumbleTriggers,
    Session::clSetMotionEventState,
    Session::clSetControllerLED,
    Session::clSetAdaptiveTriggers,
    Session::clScHidFeatureRequest,  // §SC-HID feature tunnel
    Session::clVrMessage             // §VR 0x5508（Limelight.h 的 struct 尾端）
};

Session* Session::s_ActiveSession;
QSemaphore Session::s_ActiveSessionSemaphore(1);

void Session::clStageStarting(int stage)
{
    // [VIPLE-STARTUP]：連線各階段開始的時刻（下一個階段開始＝這一個結束；最後一個結束看「LiStartConnection returned」）
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-STARTUP] t=%lld stage %s", startupNowMs(), LiGetStageName(stage));

    // We know this is called on the same thread as LiStartConnection()
    // which happens to be the main thread, so it's cool to interact
    // with the GUI in these callbacks.
    emit s_ActiveSession->stageStarting(QString::fromLocal8Bit(LiGetStageName(stage)));
}

void Session::clStageFailed(int stage, int errorCode)
{
    // Perform the port test now, while we're on the async connection thread and not blocking the UI.
    unsigned int portFlags = LiGetPortFlagsFromStage(stage);
    s_ActiveSession->m_PortTestResults = LiTestClientConnectivity(CONN_TEST_SERVER, 443, portFlags);

    char failingPorts[128];
    LiStringifyPortFlags(portFlags, ", ", failingPorts, sizeof(failingPorts));
    emit s_ActiveSession->stageFailed(QString::fromLocal8Bit(LiGetStageName(stage)), errorCode, QString(failingPorts));
}

void Session::clConnectionTerminated(int errorCode)
{
    unsigned int portFlags = LiGetPortFlagsFromTerminationErrorCode(errorCode);
    s_ActiveSession->m_PortTestResults = LiTestClientConnectivity(CONN_TEST_SERVER, 443, portFlags);

    // Display the termination dialog if this was not intended
    //
    // 使用者已主動退出（quit combo／關窗／UI 中斷）後才到達的終止錯誤，
    // 是 teardown 期間的預期副作用（例如畫面凍結按 quit 後 ENet 逾時），
    // 只記 log、不彈錯誤對話框，避免主動退出卻看到誤導性的錯誤訊息。
    if (errorCode != ML_ERROR_GRACEFUL_TERMINATION &&
        s_ActiveSession->m_UserInitiatedQuit.load()) {
        // 不設 m_UnexpectedTermination：使用者主動退出＝預期終止，讓收尾
        // 照送 HTTP /cancel（HTTP 通道與壞掉的串流 socket 獨立，凍結時通常
        // 仍可達），否則 host 端 ownership 不清、下一台裝置吃 §M.1 503。
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Suppressing termination dialog after user-initiated quit (error: %d)",
                    errorCode);
    }
    else switch (errorCode) {
    case ML_ERROR_GRACEFUL_TERMINATION:
        break;

    case ML_ERROR_NO_VIDEO_TRAFFIC:
        s_ActiveSession->m_UnexpectedSeen = true; s_ActiveSession->m_UnexpectedTermination = true;

        char ports[128];
        SDL_assert(portFlags != 0);
        LiStringifyPortFlags(portFlags, ", ", ports, sizeof(ports));
        emit s_ActiveSession->displayLaunchError(tr("No video received from host.") + "\n\n"+
                                                 tr("Check your firewall and port forwarding rules for port(s): %1").arg(ports));
        break;

    case ML_ERROR_NO_VIDEO_FRAME:
        s_ActiveSession->m_UnexpectedSeen = true; s_ActiveSession->m_UnexpectedTermination = true;
        emit s_ActiveSession->displayLaunchError(tr("Your network connection isn't performing well. Reduce your video bitrate setting or try a faster connection."));
        break;

    case ML_ERROR_PROTECTED_CONTENT:
    case ML_ERROR_UNEXPECTED_EARLY_TERMINATION:
        s_ActiveSession->m_UnexpectedSeen = true; s_ActiveSession->m_UnexpectedTermination = true;
        emit s_ActiveSession->displayLaunchError(tr("Something went wrong on your host PC when starting the stream.") + "\n\n" +
                                                 tr("Make sure you don't have any DRM-protected content open on your host PC. You can also try restarting your host PC."));
        break;

    case ML_ERROR_FRAME_CONVERSION:
        s_ActiveSession->m_UnexpectedSeen = true; s_ActiveSession->m_UnexpectedTermination = true;
        emit s_ActiveSession->displayLaunchError(tr("The host PC reported a fatal video encoding error.") + "\n\n" +
                                                 tr("Try disabling HDR mode, changing the streaming resolution, or changing your host PC's display resolution."));
        break;

    case ML_ERROR_CONTROL_STREAM_DISCONNECT:
        // 原本 -1 落入 default 只給 generic 錯誤碼，使用者無從行動。
        // （qmake mkspec 已注入 -utf-8，中文字面量可直寫。）
        s_ActiveSession->m_UnexpectedSeen = true; s_ActiveSession->m_UnexpectedTermination = true;
        emit s_ActiveSession->displayLaunchError(tr("The control connection to the host was lost. The network may have dropped briefly or the host went offline. You can try to resume the stream later."));
        break;

    default:
        s_ActiveSession->m_UnexpectedSeen = true; s_ActiveSession->m_UnexpectedTermination = true;

        // We'll assume large errors are hex values
        // §REVIEW-1119：qAbs(INT_MIN) 是有號溢位。錯誤碼來自終止封包，用範圍比較就好。
        bool hexError = errorCode > 1000 || errorCode < -1000;
        emit s_ActiveSession->displayLaunchError(tr("Connection terminated") + "\n\n" +
                                                 tr("Error code: %1").arg(errorCode, hexError ? 8 : 0, hexError ? 16 : 10, QChar('0')));
        break;
    }

    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "Connection terminated: %d",
                 errorCode);

    // Push a quit event to the main loop
    SDL_Event event;
    event.type = SDL_QUIT;
    event.quit.timestamp = SDL_GetTicks();
    SDL_PushEvent(&event);
}

void Session::clLogMessage(const char* format, ...)
{
    va_list ap;

    va_start(ap, format);
    SDL_LogMessageV(SDL_LOG_CATEGORY_APPLICATION,
                    SDL_LOG_PRIORITY_INFO,
                    format,
                    ap);
    va_end(ap);
}

void Session::clRumble(unsigned short controllerNumber, unsigned short lowFreqMotor, unsigned short highFreqMotor)
{
    // We push an event for the main thread to handle in order to properly synchronize
    // with the removal of game controllers that could result in our game controller
    // going away during this callback.
    SDL_Event rumbleEvent = {};
    rumbleEvent.type = SDL_USEREVENT;
    rumbleEvent.user.code = SDL_CODE_GAMECONTROLLER_RUMBLE;
    rumbleEvent.user.data1 = (void*)(uintptr_t)controllerNumber;
    // §REVIEW-1111（第二輪）：先轉無號再位移；原本 ≥0x8000 的值位移後是負的 int，轉成指標寬度時高位全是 1
    rumbleEvent.user.data2 = (void*)(uintptr_t)(((uint32_t)lowFreqMotor << 16) | highFreqMotor);
    SDL_PushEvent(&rumbleEvent);
}

void Session::clConnectionStatusUpdate(int connectionStatus)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Connection status update: %d",
                connectionStatus);

    if (!s_ActiveSession->m_Preferences->connectionWarnings) {
        return;
    }

    if (s_ActiveSession->m_MouseEmulationRefCount > 0) {
        // Don't display the overlay if mouse emulation is already using it
        return;
    }

    switch (connectionStatus)
    {
    case CONN_STATUS_POOR:
        s_ActiveSession->m_ConnectionPoor.store(true);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-NET] Connection status: POOR — FRUC will reduce latency mode");
        s_ActiveSession->m_OverlayManager.updateOverlayText(Overlay::OverlayStatusUpdate,
                                                            s_ActiveSession->m_StreamConfig.bitrate > 5000 ?
                                                                "Slow connection to PC\nReduce your bitrate" : "Poor connection to PC");
        s_ActiveSession->m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, true);
        break;
    case CONN_STATUS_OKAY:
        s_ActiveSession->m_ConnectionPoor.store(false);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-NET] Connection status: OKAY — FRUC resuming normal mode");
        s_ActiveSession->m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, false);
        break;
    }
}

void Session::clSetHdrMode(bool enabled)
{
    // If we're in the process of recreating our decoder when we get
    // this callback, we'll drop it. The main thread will make the
    // callback when it finishes creating the new decoder.
    if (SDL_TryLockMutex(s_ActiveSession->m_DecoderLock) == 0) {
        IVideoDecoder* decoder = s_ActiveSession->m_VideoDecoder;
        if (decoder != nullptr) {
            decoder->setHdrMode(enabled);
        }
        SDL_UnlockMutex(s_ActiveSession->m_DecoderLock);
    }
}

void Session::clRumbleTriggers(uint16_t controllerNumber, uint16_t leftTrigger, uint16_t rightTrigger)
{
    // We push an event for the main thread to handle in order to properly synchronize
    // with the removal of game controllers that could result in our game controller
    // going away during this callback.
    SDL_Event rumbleEvent = {};
    rumbleEvent.type = SDL_USEREVENT;
    rumbleEvent.user.code = SDL_CODE_GAMECONTROLLER_RUMBLE_TRIGGERS;
    rumbleEvent.user.data1 = (void*)(uintptr_t)controllerNumber;
    rumbleEvent.user.data2 = (void*)(uintptr_t)(((uint32_t)leftTrigger << 16) | rightTrigger);
    SDL_PushEvent(&rumbleEvent);
}

void Session::clSetMotionEventState(uint16_t controllerNumber, uint8_t motionType, uint16_t reportRateHz)
{
    // We push an event for the main thread to handle in order to properly synchronize
    // with the removal of game controllers that could result in our game controller
    // going away during this callback.
    SDL_Event setMotionEventStateEvent = {};
    setMotionEventStateEvent.type = SDL_USEREVENT;
    setMotionEventStateEvent.user.code = SDL_CODE_GAMECONTROLLER_SET_MOTION_EVENT_STATE;
    setMotionEventStateEvent.user.data1 = (void*)(uintptr_t)controllerNumber;
    setMotionEventStateEvent.user.data2 = (void*)(uintptr_t)((motionType << 16) | reportRateHz);
    SDL_PushEvent(&setMotionEventStateEvent);
}

void Session::clSetControllerLED(uint16_t controllerNumber, uint8_t r, uint8_t g, uint8_t b)
{
    // We push an event for the main thread to handle in order to properly synchronize
    // with the removal of game controllers that could result in our game controller
    // going away during this callback.
    SDL_Event setControllerLEDEvent = {};
    setControllerLEDEvent.type = SDL_USEREVENT;
    setControllerLEDEvent.user.code = SDL_CODE_GAMECONTROLLER_SET_CONTROLLER_LED;
    setControllerLEDEvent.user.data1 = (void*)(uintptr_t)controllerNumber;
    setControllerLEDEvent.user.data2 = (void*)(uintptr_t)(r << 16 | g << 8 | b);
    SDL_PushEvent(&setControllerLEDEvent);
}

void Session::clSetAdaptiveTriggers(uint16_t controllerNumber, uint8_t eventFlags, uint8_t typeLeft, uint8_t typeRight, uint8_t *left, uint8_t *right){
    // We push an event for the main thread to handle in order to properly synchronize
    // with the removal of game controllers that could result in our game controller
    // going away during this callback.
    SDL_Event setControllerLEDEvent = {};
    setControllerLEDEvent.type = SDL_USEREVENT;
    setControllerLEDEvent.user.code = SDL_CODE_GAMECONTROLLER_SET_ADAPTIVE_TRIGGERS;
    setControllerLEDEvent.user.data1 = (void*)(uintptr_t)controllerNumber;

    // Based on the following SDL code:
    // https://github.com/libsdl-org/SDL/blob/120c76c84bbce4c1bfed4e9eb74e10678bd83120/test/testgamecontroller.c#L286-L307
    DualSenseOutputReport *state = (DualSenseOutputReport *) SDL_malloc(sizeof(DualSenseOutputReport));
    if (state == nullptr) {
        return;
    }
    SDL_zero(*state);
    state->validFlag0 = (eventFlags & DS_EFFECT_RIGHT_TRIGGER) | (eventFlags & DS_EFFECT_LEFT_TRIGGER);
    state->rightTriggerEffectType = typeRight;
    SDL_memcpy(state->rightTriggerEffect, right, sizeof(state->rightTriggerEffect));
    state->leftTriggerEffectType = typeLeft;
    SDL_memcpy(state->leftTriggerEffect, left, sizeof(state->leftTriggerEffect));

    setControllerLEDEvent.user.data2 = (void *) state;
    // 佇列滿或被過濾時事件沒進去，report 要自己收（opt B045；同 clVrMessage）
    if (SDL_PushEvent(&setControllerLEDEvent) <= 0) {
        SDL_free(state);
    }
}

// §REVIEW-1111：clVrMessage 收下的最新震動 eventId（分左右手；bit 32＝已有值）。回呼是靜態函式、會在收影像的
// 執行緒與 async 回呼執行緒上被呼叫，所以放檔案層級；每一場開始時（startConnectionAsync）歸零。
static std::atomic<uint64_t> s_VrHapticSeenCb[2];

void Session::clVrMessage(const uint8_t* tlv, int length)
{
    // common-c 的 async callback 執行緒：複製 payload，一律包成 SDL_USEREVENT
    // 交給主執行緒處理（docs/vr_protocol.md §4.5）
    // §REVIEW-1119：呼叫端目前有擋 251，這裡再擋一次。長度來自封包，不能直接拿去 SDL_malloc。
    if (tlv == nullptr || length <= 0 || length > VIPLE_VR_MAX_CTRL_PAYLOAD) {
        return;
    }
    // §REVIEW-1119：主執行緒停住（或 host 灌訊息）時，這裡每一則配置一塊、SDL 佇列可以堆到六萬多筆。
    // 佇列裡已經有 kVrMsgQueueMax 則還沒處理就丟掉新的（震動、狀態通知晚到也沒有用）。只多一次原子讀。
    if (s_VrMsgQueued.load(std::memory_order_relaxed) >= kVrMsgQueueMax) {
        return;
    }
    // §REVIEW-1111：震動的重複副本在這裡就丟掉。host 預設每一則震動送三份（0／8／20 ms），每一份又走視訊連線與 ENet
    // 各一次；原本六份都在這裡配置、複製、推 SDL 事件（其中一半發生在收影像的執行緒上），到主執行緒才用 eventId
    // 丟掉五份。兩手連續震動時是每秒上千次配置與事件。這裡只看「整則訊息就是一個震動 TLV」的情況，序號比對與
    // 主執行緒的去重相同（分左右手、32-bit 迴繞安全）；第一份照常往下送。
    if (length == 2 + (int)sizeof(VIPLE_VR_TLV_HAPTIC) && tlv[0] == VIPLE_VR_S2C_HAPTIC && tlv[1] == sizeof(VIPLE_VR_TLV_HAPTIC)) {
        VIPLE_VR_TLV_HAPTIC h;
        memcpy(&h, tlv + 2, sizeof(h));
        const unsigned dev = (unsigned)h.device - VIPLE_VR_POSE_LEFT;
        if (dev < 2) {
            uint64_t seen = s_VrHapticSeenCb[dev].load(std::memory_order_relaxed);
            for (;;) {
                if ((seen >> 32) != 0 && (int32_t)(h.eventId - (uint32_t)seen) <= 0) {
                    return;  // 已經收下過這一則（或更新的）
                }
                if (s_VrHapticSeenCb[dev].compare_exchange_weak(seen, (1ull << 32) | h.eventId, std::memory_order_relaxed)) {
                    break;
                }
            }
        }
    }
    void* copy = SDL_malloc((size_t)length);
    if (copy == nullptr) {
        return;
    }
    memcpy(copy, tlv, (size_t)length);

    // §REVIEW-1119：沒清掉的話 windowID 等欄位是堆疊殘值，SDL 會整筆複製進佇列。
    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = SDL_CODE_VR_MESSAGE;
    event.user.data1 = (void*)(uintptr_t)length;
    event.user.data2 = copy;
    if (SDL_PushEvent(&event) <= 0) {
        SDL_free(copy);
    }
    else {
        s_VrMsgQueued.fetch_add(1, std::memory_order_relaxed);
    }
}

void Session::handleVrMessage(const uint8_t* tlv, int length)
{
    int off = 0;
    while (off + 2 <= length) {
        const uint8_t type = tlv[off];
        const uint8_t len = tlv[off + 1];
        if (off + 2 + len > length) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-SESSION] truncated VR_S2C TLV (type=%u len=%u remaining=%d)",
                        type, len, length - off - 2);
            break;
        }
        const uint8_t* payload = tlv + off + 2;

        switch (type) {
        case VIPLE_VR_S2C_NOTICE:
            // §VR-PADMODE：server 切換了 Xbox 手把模式（或建不了虛擬手把），頭盔裡提示 3 秒
            if (len >= 1) {
                QString text;
                switch (payload[0]) {
                case VIPLE_VR_NOTICE_PAD_ON_AUTO:
                    text = tr("This game doesn't use VR controllers - switched to Xbox controller mode");
                    m_VrPadModeOn = true;
                    break;
                case VIPLE_VR_NOTICE_PAD_ON_MANUAL:
                    text = tr("Xbox controller mode: on");
                    m_VrPadModeOn = true;
                    break;
                case VIPLE_VR_NOTICE_PAD_OFF_MANUAL:
                    text = tr("Xbox controller mode: off (VR controllers)");
                    m_VrPadModeOn = false;
                    break;
                case VIPLE_VR_NOTICE_PAD_UNAVAILABLE:
                    text = tr("Xbox controller mode unavailable: install ViGEmBus on the host");
                    break;
                case VIPLE_VR_NOTICE_MIC_UNAVAILABLE:
                    text = tr("Microphone not available on the host: Steam's \"Steam Streaming Microphone\" device was not found");
                    break;
                default:
                    break;
                }
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-PADMODE] server notice %u", payload[0]);
#ifdef HAVE_OPENXR
                if (!text.isEmpty() && m_XrContext != nullptr) {
                    m_XrContext->showToast(text, 3000);
                }
#endif
            }
            break;
        case VIPLE_VR_S2C_STATE:
            if (len >= sizeof(VIPLE_VR_TLV_STATE)) {
                VIPLE_VR_TLV_STATE st;
                memcpy(&st, payload, sizeof(st));
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VR-SESSION] server state=%u progress=%u code=%u",
                            st.state, st.progress, st.code);
                // §VR-LAUNCHER：編排器失敗（例：host 上有 VR 遊戲在跑、SteamVR 要重啟）時 server 只送 STATE ERROR、
                // 串流照常送黑幀，client 會一直停在 loading。顯示原因，經 SDL_QUIT 走正常退出（送 /cancel）。
                if (st.state == VIPLE_VR_STATE_ERROR && !m_VrServerErrorHandled) {
                    m_VrServerErrorHandled = true;
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "[VIPLE-VR-SESSION] server could not start VR (code=%u) - ending the stream", st.code);
                    emit displayLaunchError(vrServerErrorText(st.code));
                    SDL_Event quit = {};
                    quit.type = SDL_QUIT;
                    SDL_PushEvent(&quit);
                }
            }
            break;
        case VIPLE_VR_S2C_RATE:
            // §VR-RATE-AUTO：host 套用（或拒絕）了「每幾張送一張」。只有 OpenXR 的 PCVR 會送要求；沒有 OpenXR 的建置
            // （Windows、Linux x64）XrContext 只有前向宣告，不能碰它的成員（2.0.96～2.0.121 這裡少了保護，那幾版在這些平台編不過）
#ifdef HAVE_OPENXR
            if (len >= sizeof(VIPLE_VR_TLV_RATE_APPLIED) && m_XrContext != nullptr) {
                VIPLE_VR_TLV_RATE_APPLIED a;
                memcpy(&a, payload, sizeof(a));
                m_XrContext->onPcvrRateApplied(a.div, a.accepted != 0, a.seq);
            }
#endif
            break;
        case VIPLE_VR_S2C_STATS: {
            // 欄位只往後加：依 len 能讀多少算多少。計數欄位是 session 累計值
            //（STATS 走 UNSEQUENCED，掉一筆不會漏算），區間值 = 兩個快照相減
            VIPLE_VR_TLV_STATS st;
            memset(&st, 0, sizeof(st));
            memcpy(&st, payload, (size_t)len < sizeof(st) ? (size_t)len : sizeof(st));
            m_VrServerStatsLatest = st;
            m_VrServerStatsCount++;
            const uint32_t now = SDL_GetTicks();
            if (m_VrServerStatsLastLogMs == 0) {
                m_VrServerStatsAtLog = st;
                m_VrServerStatsLastLogMs = now;
            }
            else if (now - m_VrServerStatsLastLogMs >= 10000) {
                const VIPLE_VR_TLV_STATS& a = m_VrServerStatsAtLog;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VR-STATS] server %.1fs (%u msgs): poseRx=%u ooo=%u gap=%u tagged=%u fallback=%u "
                            "lossRx=%u waves=%u | refreshStartRx=%u | haptics rx=%u queued=%u dupDrop=%u expiredDrop=%u",
                            (now - m_VrServerStatsLastLogMs) / 1000.0, m_VrServerStatsCount,
                            st.poseRx - a.poseRx, st.poseOutOfOrder - a.poseOutOfOrder,
                            st.poseGap - a.poseGap, st.framesTagged - a.framesTagged,
                            st.framesFallback - a.framesFallback, st.lossRx - a.lossRx,
                            st.refreshWaves - a.refreshWaves, m_VrRefreshStartRx,
                            m_VrHapticRx, m_VrHapticQueued, m_VrHapticDroppedDup, m_VrHapticDroppedExpired);
                m_VrServerStatsAtLog = st;
                m_VrServerStatsCount = 0;
                m_VrRefreshStartRx = 0;
                m_VrServerStatsLastLogMs = now;
            }
            break;
        }
        case VIPLE_VR_S2C_REFRESH_START:
            // 恢復狀態機已在 common-c 的控制接收執行緒處理，這裡只計數
            m_VrRefreshStartRx++;
            break;
        case VIPLE_VR_S2C_HAPTIC:
            // M4a R2：控制器震動 → XrContext haptic 佇列 → XR frame thread xrApplyHapticFeedback。
            // 本函式與 destroyXrContext 都在 main thread，不會與拆除競爭
            if (len >= 20) {
                VIPLE_VR_TLV_HAPTIC h;
                memset(&h, 0, sizeof(h));
                memcpy(&h, payload, (std::min)((size_t)len, sizeof(h)));
                m_VrHapticRx++;

                // §VR-HAPTIC-REDUNDANT：去重（分左右手，32-bit 序號安全迴繞比對）。device 是 VIPLE_VR_POSE_LEFT(1)／RIGHT(2)
                const unsigned dev = (unsigned)h.device - VIPLE_VR_POSE_LEFT;
                if (dev >= 2) {
                    break;  // §REVIEW-1111（第二輪）：不是左右手的裝置編號，不往下送（也不經過去重）
                }
                if (dev < 2) {
                    if (m_VrHapticHaveEventId[dev]) {
                        if ((int32_t)(h.eventId - m_VrHapticLastEventId[dev]) <= 0) {
                            m_VrHapticDroppedDup++;
                            break;
                        }
                    }
                    m_VrHapticLastEventId[dev] = h.eventId;
                    m_VrHapticHaveEventId[dev] = true;
                }

                // §VR-HAPTIC-REDUNDANT：過期判定（超過 durationUs + 50 ms 寬限丟棄）。寬限要蓋過正常的單程延遲與時鐘
                // 估計誤差，否則短促的震動（幾 ms）在一般網路下第一份就會被丟掉
                if (h.timestampNs != 0) {
                    const uint64_t nowNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    if (nowNs > h.timestampNs) {
                        const uint64_t ageNs = nowNs - h.timestampNs;
                        const uint64_t maxAgeNs = ((uint64_t)h.durationUs + 50000ull) * 1000ull;
                        if (ageNs > maxAgeNs) {
                            m_VrHapticDroppedExpired++;
                            break;
                        }
                    }
                }
#ifdef HAVE_OPENXR
                if (m_XrPcvr && m_XrContext != nullptr) {
                    m_XrContext->queueHaptic(h.device, h.durationUs, h.frequencyHz, h.amplitude);
                    m_VrHapticQueued++;
                }
#endif
                if (m_VrHapticRx <= 3 || m_VrHapticRx % 50 == 0) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-VR-HAPTIC] rx=%u queued=%u dupDrop=%u expiredDrop=%u device=%u dur=%u us freq=%.0f Hz amp=%.2f id=%u",
                                m_VrHapticRx, m_VrHapticQueued, m_VrHapticDroppedDup, m_VrHapticDroppedExpired, h.device, h.durationUs,
                                (double)h.frequencyHz, (double)h.amplitude, h.eventId);
                }
            }
            break;
        default:
            // 不認得的 subtype 依 len 跳過（前向相容）
            break;
        }
        off += 2 + len;
    }
}

bool Session::pcvrCannotFallBackToFlat() const
{
#if defined(Q_OS_LINUX) && !defined(STEAM_LINK)
    return m_Preferences->displayTarget == StreamingPreferences::DT_PCVR && !m_Preferences->vrEmulate &&
           qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
#else
    return false;
#endif
}

void Session::vrFallBackToFlat(const QString& userMessage)
{
    if (m_VrNoFlatError.isEmpty() && pcvrCannotFallBackToFlat()) {
        m_VrNoFlatError = userMessage;
    }
}

QString Session::vrServerErrorText(unsigned code) const
{
    const QString host = m_Computer->name;
    switch (code) {
    case VIPLE_VR_STATE_CODE_VRLINK_ACTIVE:
    case VIPLE_VR_STATE_CODE_OTHER_HMD_ACTIVE:
        return tr("Another headset is using SteamVR on %1 (for example Steam Link). Disconnect it, then try again.").arg(host);
    case VIPLE_VR_STATE_CODE_VR_APP_RUNNING:
        return tr("A VR game is already running on %1 without VipleStream. Quit it, then try again.").arg(host);
    case VIPLE_VR_STATE_CODE_STEAMVR_RESTART_REQUIRED:
        return tr("SteamVR on %1 has to restart, but a VR game is still running there. Quit the game first "
                  "(Quit Game in the app list), then try again.").arg(host);
    case VIPLE_VR_STATE_CODE_SAFE_MODE:
        return tr("SteamVR on %1 started in safe mode and blocked the VipleStream driver. Restart SteamVR on the host, then try again.").arg(host);
    case VIPLE_VR_STATE_CODE_STEAMVR_NOT_INSTALLED:
        return tr("SteamVR is not installed on %1.").arg(host);
    case VIPLE_VR_STATE_CODE_STEAM_NOT_LOGGED_IN:
        return tr("Steam is not signed in on %1. Sign in to Steam on the host (for example in desktop mode), then try again.").arg(host);
    case VIPLE_VR_STATE_CODE_NO_USER_SESSION:
        return tr("Nobody is signed in to Windows on %1.").arg(host);
    case VIPLE_VR_STATE_CODE_HOST_LOCKED:
        return tr("%1 is on the Windows sign-in or lock screen. Sign in on the host, then try again.").arg(host);
    case VIPLE_VR_STATE_CODE_HMD_TIMEOUT:
    case VIPLE_VR_STATE_CODE_STEAMVR_LAUNCH_FAILED:
        return tr("SteamVR on %1 did not start in time. Try again.").arg(host);
    default:
        return tr("%1 could not start VR (error %2).").arg(host).arg(code);
    }
}

void Session::decideVrRequest()
{
    m_VrRequested = false;
    if (m_Preferences->displayTarget != StreamingPreferences::DT_PCVR) {
        return;
    }

    // 不變式 5：XR 在 /launch 之前就不可用 → 退回平面模式，不中斷串流
#ifndef HAVE_OPENXR
    if (!m_Preferences->vrEmulate) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-SESSION] display target PCVR needs an XR runtime, which this build "
                    "does not have (CONFIG+=openxr); use --vr-emulate. Streaming flat.");
        return;
    }
#endif
    if (!(m_Computer->vipleStreamVr & VIPLE_VR_SERVER_CAP_PCVR) ||
            m_Computer->vipleStreamVrProto < VIPLE_VR_PROTO_VERSION) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-SESSION] host does not offer PCVR (VipleStreamVR=0x%x proto=%d) — streaming flat",
                    m_Computer->vipleStreamVr, m_Computer->vipleStreamVrProto);
        vrFallBackToFlat(tr("%1 doesn't offer VR streaming. It needs VipleStream Server 2.0 or later with PCVR enabled.")
                             .arg(m_Computer->name));
        return;
    }
    // 下面兩種情況在 /launch 之後才退回平面的話，平面串流會留著 VR 的形狀（2W×H@Hz、
    // 沒做 FRUC 減半），所以在套用 VR 形狀之前就擋下
    if (m_Computer->onlineViaRelay || m_Preferences->forceRelayStream) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-SESSION] host is reachable only via relay — VR not requested "
                    "(transport unsupported before GA), streaming flat");
        vrFallBackToFlat(tr("%1 is only reachable through the relay. VR streaming needs a direct connection.")
                             .arg(m_Computer->name));
        return;
    }
    if (m_Preferences->videoCodecConfig == StreamingPreferences::VCC_FORCE_AV1) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-SESSION] video codec preference forces AV1; PCVR needs HEVC or H.264 "
                    "(AV1 waits for L1) — streaming flat");
        vrFallBackToFlat(tr("The video codec setting forces AV1, but VR streaming needs HEVC or H.264. "
                            "Change the video codec in Settings."));
        return;
    }

    m_VrLaunch = VrLaunchConfig();
    m_VrLaunch.eyeWidth = m_Preferences->vrEyeWidth;
    m_VrLaunch.eyeHeight = m_Preferences->vrEyeHeight;
    m_VrLaunch.refreshHz = m_Preferences->vrRefreshHz;
    m_VrLaunch.fovea = m_Preferences->vrFoveation;  // §VR-FOVEA
    // §VR-FOVEA-TILES：注視點開著時帶上設定頁選的大小（1 小、2 中、3 大）；dev 的 --vr-fovea-tiles 優先（0＝這一場不分塊）。
    // 注視點關著就不帶。舊 host 不認得這個參數（略過），照它原本的注視點做法
    m_VrLaunch.foveaTilesReq = m_Preferences->vrFoveaTilesReq >= 0 ? m_Preferences->vrFoveaTilesReq
                               : (m_Preferences->vrFoveation > 100 ? qBound(1, m_Preferences->vrFoveaTiles, 3) : -1);
#ifdef HAVE_OPENXR
    if (!m_Preferences->vrEmulate && !setupXrPcvr()) {
        return;  // 已記原因；不變式 5（/launch 前）→ 平面
    }
#endif
    m_VrRequested = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VR-SESSION] requesting PCVR (%s): eye=%dx%d hz=%d serverCaps=0x%x",
                m_Preferences->vrEmulate ? "emulated pose" : "XR runtime",
                m_VrLaunch.eyeWidth, m_VrLaunch.eyeHeight, m_VrLaunch.refreshHz,
                m_Computer->vipleStreamVr);
}

void Session::clScHidFeatureRequest(uint8_t reportId, uint8_t op, uint8_t seq,
                                    const uint8_t* query, uint8_t queryLen) {
    // §SC-HID Phase 2C: host relays a Steam feature op to the real SC (transparent
    // proxy). forwardFeatureRequest replays it and returns the SC's response via
    // LiSendScHidFeatureReport (0x55000008), tagged with the same seq.
    if (s_ActiveSession == nullptr) {
        return;
    }
    // §REVIEW-1119：sc_hid 只截長度就對實體裝置下 feature。
    // op 不是 GET(1)/SET(2)、或 SET 卻沒有 type 位元組的，不要送出去。
    // report id 不擋：不同世代的 Steam 手把與接收器用的 id 沒有實機清單，擋錯會讓手把失效。
    // feature type 的白名單不在這裡做：Steam 正常會送的 opcode 沒有完整清單，擋錯會讓手把失效。
    if ((op != 1 && op != 2) ||
            (op == 2 && (query == nullptr || queryLen < 2))) {
        static bool s_rejectLogged = false;
        static Uint32 s_lastRejectLog = 0;
        const Uint32 now = SDL_GetTicks();
        if (!s_rejectLogged || (Uint32)(now - s_lastRejectLog) >= 1000) {
            s_rejectLogged = true;
            s_lastRejectLog = now;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[SC-HID] dropping feature request id=0x%02x op=%u seq=%u qlen=%u",
                        reportId, (unsigned)op, seq, (unsigned)queryLen);
        }
        return;
    }
    s_ActiveSession->m_ScHid.forwardFeatureRequest(reportId, op, seq, query, queryLen);
}


bool Session::chooseDecoder(StreamingPreferences::VideoDecoderSelection vds,
                            SDL_Window* window, int videoFormat, int width, int height,
                            int frameRate, bool enableVsync, bool enableFramePacing, bool testOnly, IVideoDecoder*& chosenDecoder,
                            XrContext* xr)
{
    DECODER_PARAMETERS params;

    // We should never have vsync enabled for test-mode.
    // It introduces unnecessary delay for renderers that may
    // block while waiting for a backbuffer swap.
    SDL_assert(!enableVsync || !testOnly);

    params.width = width;
    params.height = height;
    params.frameRate = frameRate;
    params.videoFormat = videoFormat;
    params.window = window;
    params.enableVsync = enableVsync;
    params.enableFramePacing = enableFramePacing;
    params.testOnly = testOnly;
    params.vds = vds;
    // §VR M3a X2：只有真正串流的 decoder 才接 XR（能力探測的 testOnly 實例一律平面）
    params.xr = testOnly ? nullptr : xr;

    // [VIPLE-STARTUP]：這一次（探測或真的解碼器）花多久，出函式時印，成功與失敗都印
    struct StartupTimer {
        long long t0Ms;
        bool testOnly;
        int fmt, w, h, fps;
        IVideoDecoder** chosen;  // 呼叫端的變數：成功時是新的解碼器，失敗時下面會設成 nullptr
        ~StartupTimer()
        {
            const long long nowMs = startupNowMs();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-STARTUP] t=%lld chooseDecoder testOnly=%d fmt=0x%x %dx%d@%d ok=%d took=%lld ms",
                        nowMs, testOnly ? 1 : 0, fmt, w, h, fps, *chosen != nullptr ? 1 : 0, nowMs - t0Ms);
        }
    } startupTimer{startupNowMs(), testOnly, videoFormat, width, height, frameRate, &chosenDecoder};

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "V-sync %s",
                enableVsync ? "enabled" : "disabled");

#ifdef HAVE_SLVIDEO
    chosenDecoder = new SLVideoDecoder(testOnly);
    if (chosenDecoder->initialize(&params)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SLVideo video decoder chosen");
        return true;
    }
    else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to load SLVideo decoder");
        delete chosenDecoder;
        chosenDecoder = nullptr;
    }
#endif

#ifdef HAVE_FFMPEG
    chosenDecoder = new FFmpegVideoDecoder(testOnly);
    if (chosenDecoder->initialize(&params)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "FFmpeg-based video decoder chosen");
        return true;
    }
    else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to load FFmpeg decoder");
        delete chosenDecoder;
        chosenDecoder = nullptr;
    }
#endif

#if !defined(HAVE_FFMPEG) && !defined(HAVE_SLVIDEO)
#error No video decoding libraries available!
#endif

    // If we reach this, we didn't initialize any decoders successfully
    return false;
}

int Session::drSetup(int videoFormat, int width, int height, int frameRate, void *, int)
{
    s_ActiveSession->m_ActiveVideoFormat = videoFormat;
    s_ActiveSession->m_ActiveVideoWidth = width;
    s_ActiveSession->m_ActiveVideoHeight = height;
    s_ActiveSession->m_ActiveVideoFrameRate = frameRate;

    // Defer decoder setup until we've started streaming so we
    // don't have to hide and show the SDL window (which seems to
    // cause pointer hiding to break on Windows).

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Video stream is %dx%dx%d (format 0x%x)",
                width, height, frameRate, videoFormat);

    return 0;
}

int Session::drSubmitDecodeUnit(PDECODE_UNIT du)
{
    // Use a lock since we'll be yanking this decoder out
    // from underneath the session when we initiate destruction.
    // We need to destroy the decoder on the main thread to satisfy
    // some API constraints (like DXVA2). If we can't acquire it,
    // that means the decoder is about to be destroyed, so we can
    // safely return DR_OK and wait for the IDR frame request by
    // the decoder reinitialization code.

    if (SDL_TryLockMutex(s_ActiveSession->m_DecoderLock) == 0) {
        IVideoDecoder* decoder = s_ActiveSession->m_VideoDecoder;
        if (decoder != nullptr) {
            int ret = decoder->submitDecodeUnit(du);
            SDL_UnlockMutex(s_ActiveSession->m_DecoderLock);
            return ret;
        }
        else {
            SDL_UnlockMutex(s_ActiveSession->m_DecoderLock);
            return DR_OK;
        }
    }
    else {
        // Decoder is going away. Ignore anything coming in until
        // the lock is released.
        return DR_OK;
    }
}

void Session::getDecoderInfo(SDL_Window* window,
                             bool& isHardwareAccelerated, bool& isFullScreenOnly,
                             bool& isHdrSupported, QSize& maxResolution)
{
    IVideoDecoder* decoder;

    // VIPLE patch (§K.dd.probe.1) — default-assume HW.
    //
    // 修兩個 historical bugs 造成 AMD APU / Intel new GPU client 啟動時誤出
    // "No functioning hardware accelerated video decoder" popup (即使 stream
    // 實際用 HW decode 順暢):
    //
    //   (a) DecoderInfoThread::run() 的 stack-local `bool hasHardwareAcceleration;`
    //       沒初始化 (systemproperties.cpp:28). 若 getDecoderInfo cascade 任何
    //       path 沒 explicit set `isHardwareAccelerated` (如所有 HEVC HW probe
    //       fail 然後 fallthrough), local var 是 UB garbage, 可能 false.
    //
    //   (b) 原本 cascade design 中 AV1 HW probe success 只 set isHdrSupported,
    //       *不* set isHardwareAccelerated (line 431). 對 AMD 780M 這種 HEVC HW
    //       缺 ext 但 AV1/H.264 Vulkan video work 的 GPU, AV1 succeed 不算分,
    //       最後落到 line 481 H.264 VDS_AUTO. 若 H.264 cascade race 到 SW path,
    //       明確 set false → emit signal → popup 跳出.
    //
    // 觀察根據: AMD Radeon 780M (v1.4.158) client log 顯示 H.264 + AV1 Vulkan
    // video HW 都 work + stream 順暢, 但 popup 還是出來.
    //
    // Patch:
    //   1. 開頭 default isHardwareAccelerated = true (修 bug (a) 的 UB).
    //   2. AV1 Main10 HW probe success 順便 set true (修 bug (b)).
    // 兩道防線, 即使其中一道沒擋到 (e.g., AV1 在 AMD APU probe fail) 另一道
    // (default true) 仍能保住 final value 是 true unless 真的全 HW path 都
    // explicit set false.
    isHardwareAccelerated = true;

    // Since AV1 support on the host side is in its infancy, let's not consider
    // _only_ a working AV1 decoder to be acceptable and still show the warning
    // dialog indicating lack of hardware decoding support.

    // Try an HEVC Main10 decoder first to see if we have HDR support
    if (chooseDecoder(StreamingPreferences::VDS_FORCE_HARDWARE,
                      window, VIDEO_FORMAT_H265_MAIN10, 1920, 1080, 60,
                      false, false, true, decoder)) {
        isHardwareAccelerated = decoder->isHardwareAccelerated();
        isFullScreenOnly = decoder->isAlwaysFullScreen();
        isHdrSupported = decoder->isHdrSupported();
        maxResolution = decoder->getDecoderMaxResolution();
        delete decoder;

        return;
    }

    // Try an AV1 Main10 decoder next to see if we have HDR support
    if (chooseDecoder(StreamingPreferences::VDS_FORCE_HARDWARE,
                      window, VIDEO_FORMAT_AV1_MAIN10, 1920, 1080, 60,
                      false, false, true, decoder)) {
        // If we've got a working AV1 Main 10-bit decoder, we'll enable the HDR checkbox
        // but we will still continue probing to get other attributes for HEVC or H.264
        // decoders. See the AV1 comment at the top of the function for more info.
        isHdrSupported = decoder->isHdrSupported();
        // VIPLE patch (§K.dd.probe.1) bug (b) fix: AV1 HW probe success 也算
        // HW acceleration. 對 AMD 780M / Intel new GPU 這種 HEVC HW 缺 ext
        // 但 AV1 HW work 的 case, AV1 succeed 是真實 HW decode 證據.
        if (decoder->isHardwareAccelerated()) {
            isHardwareAccelerated = true;
        }
        delete decoder;
    }
    else {
        // If we found no hardware decoders with HDR, check for a renderer
        // that supports HDR rendering with software decoded frames.
        if (chooseDecoder(StreamingPreferences::VDS_FORCE_SOFTWARE,
                          window, VIDEO_FORMAT_H265_MAIN10, 1920, 1080, 60,
                          false, false, true, decoder) ||
            chooseDecoder(StreamingPreferences::VDS_FORCE_SOFTWARE,
                          window, VIDEO_FORMAT_AV1_MAIN10, 1920, 1080, 60,
                          false, false, true, decoder)) {
            isHdrSupported = decoder->isHdrSupported();
            delete decoder;
        }
        else {
            // We weren't compiled with an HDR-capable renderer or we don't
            // have the required GPU driver support for any HDR renderers.
            isHdrSupported = false;
        }
    }

    // Try a regular hardware accelerated HEVC decoder now
    if (chooseDecoder(StreamingPreferences::VDS_FORCE_HARDWARE,
                      window, VIDEO_FORMAT_H265, 1920, 1080, 60,
                      false, false, true, decoder)) {
        isHardwareAccelerated = decoder->isHardwareAccelerated();
        isFullScreenOnly = decoder->isAlwaysFullScreen();
        maxResolution = decoder->getDecoderMaxResolution();
        delete decoder;

        return;
    }


#if 0 // See AV1 comment at the top of this function
    if (chooseDecoder(StreamingPreferences::VDS_FORCE_HARDWARE,
                      window, VIDEO_FORMAT_AV1_MAIN8, 1920, 1080, 60,
                      false, false, true, decoder)) {
        isHardwareAccelerated = decoder->isHardwareAccelerated();
        isFullScreenOnly = decoder->isAlwaysFullScreen();
        maxResolution = decoder->getDecoderMaxResolution();
        delete decoder;

        return;
    }
#endif

    // If we still didn't find a hardware decoder, try H.264 now.
    // This will fall back to software decoding, so it should always work.
    if (chooseDecoder(StreamingPreferences::VDS_AUTO,
                      window, VIDEO_FORMAT_H264, 1920, 1080, 60,
                      false, false, true, decoder)) {
        // VIPLE patch (§K.dd.probe.1) bug (c) fix: H.264 VDS_AUTO 可能 cascade
        // 走到 SW path (VDS_AUTO 容許 SW fallback). 原本直接覆寫:
        //   isHardwareAccelerated = decoder->isHardwareAccelerated();
        // 會把前面 AV1 / HEVC HW probe success set 的 true 覆寫成 false.
        // 改成 only-upgrade (HW set true, SW 不 demote), 保留前面 cascade 結果.
        if (decoder->isHardwareAccelerated()) {
            isHardwareAccelerated = true;
        }
        isFullScreenOnly = decoder->isAlwaysFullScreen();
        maxResolution = decoder->getDecoderMaxResolution();
        delete decoder;

        return;
    }

    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "Failed to find ANY working H.264 or HEVC decoder!");
}

Session::DecoderAvailability
Session::getDecoderAvailability(SDL_Window* window,
                                StreamingPreferences::VideoDecoderSelection vds,
                                int videoFormat, int width, int height, int frameRate)
{
    // §STARTUP-PERF: cache probe results to avoid redundant VkFrucRenderer
    // init/teardown cycles. Key = {videoFormat, vds} — resolution/fps don't
    // change decoder availability within a session.
    struct ProbeKey {
        int format; int vds;
        bool operator<(const ProbeKey& o) const {
            return std::tie(format, vds) < std::tie(o.format, o.vds);
        }
    };
    static std::map<ProbeKey, DecoderAvailability> s_probeCache;

    ProbeKey key{videoFormat, (int)vds};
    auto it = s_probeCache.find(key);
    if (it != s_probeCache.end()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-CACHE] decoder probe cache hit format=0x%x vds=%d -> %s",
                    videoFormat, (int)vds,
                    it->second == DecoderAvailability::Hardware ? "HARDWARE" :
                    it->second == DecoderAvailability::Software ? "SOFTWARE" : "None");
        return it->second;
    }

    IVideoDecoder* decoder;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-DIAG] getDecoderAvailability: probing format=0x%x %dx%d@%dfps vds=%d",
                videoFormat, width, height, frameRate, vds);

    if (!chooseDecoder(vds, window, videoFormat, width, height, frameRate, false, false, true, decoder)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-DIAG] getDecoderAvailability: chooseDecoder returned FALSE for format=0x%x -> None",
                    videoFormat);
        s_probeCache[key] = DecoderAvailability::None;
        return DecoderAvailability::None;
    }

    bool hw = decoder->isHardwareAccelerated();

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-DIAG] getDecoderAvailability: format=0x%x -> %s",
                videoFormat, hw ? "HARDWARE" : "SOFTWARE");

    // §PERF-1113 A5：趁這個測試解碼器還在，把 populateDecoderProperties() 要讀的屬性記在這個 Session 上
    // （不放進上面行程層級的 s_probeCache，理由見 session.h 的 m_ProbedDecoderProps）
    {
        ProbedDecoderProps props;
        props.videoFormat = videoFormat;
        props.vds = (int)vds;
        props.width = width;
        props.height = height;
        props.frameRate = frameRate;
        props.capabilities = decoder->getDecoderCapabilities();
        props.colorSpace = decoder->getDecoderColorspace();
        props.colorRange = decoder->getDecoderColorRange();
        props.alwaysFullScreen = decoder->isAlwaysFullScreen();
        m_ProbedDecoderProps.push_back(props);
    }

    delete decoder;

    auto result = hw ? DecoderAvailability::Hardware : DecoderAvailability::Software;
    s_probeCache[key] = result;
    return result;
}

bool Session::populateDecoderProperties(SDL_Window* window)
{
    const int videoFormat = m_SupportedVideoFormats.first();
    const int vds = (int)m_Preferences->videoDecoderSelection;

    // §PERF-1113 A5：這一次 initialize() 的探測若已經用一模一樣的參數建過測試解碼器（同樣是 testOnly、沒有 V-sync、
    // 沒有節拍），就沿用它的屬性。沒有的情況照舊建一次：探測命中行程層級的快取（同行程的第二場起）、
    // 或驗證時改過解析度
    ProbedDecoderProps props {};
    bool reused = false;
    for (const ProbedDecoderProps& p : m_ProbedDecoderProps) {
        if (p.videoFormat == videoFormat && p.vds == vds &&
                p.width == m_StreamConfig.width && p.height == m_StreamConfig.height &&
                p.frameRate == m_StreamConfig.fps) {
            props = p;
            reused = true;
            break;
        }
    }

    if (!reused) {
        IVideoDecoder* decoder;

        if (!chooseDecoder(m_Preferences->videoDecoderSelection,
                           window,
                           videoFormat,
                           m_StreamConfig.width,
                           m_StreamConfig.height,
                           m_StreamConfig.fps,
                           false, false, true, decoder)) {
            return false;
        }

        props.videoFormat = videoFormat;
        props.vds = vds;
        props.width = m_StreamConfig.width;
        props.height = m_StreamConfig.height;
        props.frameRate = m_StreamConfig.fps;
        props.capabilities = decoder->getDecoderCapabilities();
        props.colorSpace = decoder->getDecoderColorspace();
        props.colorRange = decoder->getDecoderColorRange();
        props.alwaysFullScreen = decoder->isAlwaysFullScreen();

        delete decoder;
    }

    // 沿用與實建印同一個格式，兩條路徑的數值可以直接比
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-STARTUP] decoder props src=%s fmt=0x%x %dx%d@%d vds=%d caps=0x%x colorspace=%d range=%d alwaysFullScreen=%d",
                reused ? "probe" : "built", props.videoFormat, props.width, props.height, props.frameRate, props.vds,
                props.capabilities, props.colorSpace, props.colorRange, props.alwaysFullScreen ? 1 : 0);

    m_VideoCallbacks.capabilities = props.capabilities;
    if (m_VideoCallbacks.capabilities & CAPABILITY_PULL_RENDERER) {
        // It is an error to pass a push callback when in pull mode
        m_VideoCallbacks.submitDecodeUnit = nullptr;
    }
    else {
        m_VideoCallbacks.submitDecodeUnit = drSubmitDecodeUnit;
    }

    if (Utils::getEnvironmentVariableOverride("COLOR_SPACE_OVERRIDE", &m_StreamConfig.colorSpace)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Using colorspace override: %d",
                    m_StreamConfig.colorSpace);
    }
    else {
        m_StreamConfig.colorSpace = props.colorSpace;
    }

    if (Utils::getEnvironmentVariableOverride("COLOR_RANGE_OVERRIDE", &m_StreamConfig.colorRange)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Using color range override: %d",
                    m_StreamConfig.colorRange);
    }
    else {
        m_StreamConfig.colorRange = props.colorRange;
    }

    if (props.alwaysFullScreen) {
        m_IsFullScreen = true;
    }

    return true;
}

Session::Session(NvComputer* computer, NvApp& app, StreamingPreferences *preferences,
                 bool takeover)
    : m_Preferences(preferences ? preferences : StreamingPreferences::get()),
      m_IsFullScreen(m_Preferences->windowMode != StreamingPreferences::WM_WINDOWED || !WMUtils::isRunningDesktopEnvironment()),
      m_Computer(computer),
      m_App(app),
      m_Window(nullptr),
      m_VideoDecoder(nullptr),
      m_DecoderLock(SDL_CreateMutex()),
      m_AudioMuted(false),
      m_QtWindow(nullptr),
      m_UnexpectedTermination(true), // Failure prior to streaming is unexpected
      m_InputHandler(nullptr),
      m_MouseEmulationRefCount(0),
      m_FlushingWindowEventsRef(0),
      m_ShouldExit(false),
      m_AsyncConnectionSuccess(false),
      m_PortTestResults(0),
      m_OpusDecoder(nullptr),
      m_AudioRenderer(nullptr),
      m_AudioSampleCount(0),
      m_DropAudioEndTime(0),
      m_Takeover(takeover)
{
}

Session::~Session()
{
    // NB: This may not get destroyed for a long time! Don't put any non-trivial cleanup here.
    // Use Session::exec() or DeferredSessionCleanupTask instead.

    // §VR M3a X1：正常情況 exec() 已拆掉；這裡只防 exec() 沒跑到的路徑讓 XR session 殘留
    destroyXrContext();
    releaseXrOwner();

    // VipleStream: safety net for the relay UDP tunnel — normally torn
    // down in the deferred cleanup task after LiStopConnection, but
    // cover the early-failure path here too.
    if (m_UdpTunnel) {
        m_UdpTunnel->stop();
        m_UdpTunnel->wait(2000);
        delete m_UdpTunnel;
        m_UdpTunnel = nullptr;
    }

    SDL_DestroyMutex(m_DecoderLock);
}

// §REVIEW-1119：面積比較會把 1×16384 這類細長模式當成比 1080p 小，整組覆寫串流尺寸。
// 單邊低於一般桌面，或長寬比超過 8（比三個 16:9 並排還寬）的不拿來當上限。
// 偶數沒有強制：自訂解析度可以是奇數，這裡不改那種模式的行為。
static bool usableHostDisplayMode(int width, int height)
{
    if (width < 320 || height < 240 || width > 16384 || height > 16384) {
        return false;
    }
    if (width > height * 8 || height > width * 8) {
        return false;
    }
    return true;
}

// §REVIEW-1119：LiHolePunch 會對字串呼叫 getaddrinfo。網域名稱會發 DNS，
// 迴環、私網、鏈路本地、CGNAT、多播會對本機或內網送 UDP。打洞只留公網單播 IPv4。
static bool publicUnicastStunIpv4(const QString& text)
{
    if (text.size() > 64) {
        return false;
    }
    QHostAddress ha;
    if (!ha.setAddress(text) || !ha.scopeId().isEmpty()) {
        return false;
    }
    if (ha.protocol() == QAbstractSocket::IPv6Protocol) {
        bool isV4 = false;
        const quint32 v4 = ha.toIPv4Address(&isV4);
        if (!isV4) {
            return false;
        }
        ha = QHostAddress(v4);
    }
    if (ha.protocol() != QAbstractSocket::IPv4Protocol || ha.isLoopback() || ha.isLinkLocal() ||
            ha.isMulticast() || ha.isPrivateUse() ||
            ha == QHostAddress::Any || ha == QHostAddress::AnyIPv4 || ha == QHostAddress::Broadcast ||
            ha.isInSubnet(QHostAddress(QStringLiteral("0.0.0.0")), 8) ||
            ha.isInSubnet(QHostAddress(QStringLiteral("100.64.0.0")), 10)) {
        return false;
    }
    return true;
}

// §REVIEW-1119：alt peer 會交給 common-c 探測。迴環與多播會打到本機。
// 私網位址要留（區網第二張網卡）。不改寫成 toString()，送出去的字串維持原來的寫法。
static bool usableQuicAltPeer(const QString& ip)
{
    if (ip.size() > 64) {
        return false;
    }
    QHostAddress ha;
    if (!ha.setAddress(ip)) {
        // 不是位址字面值＝主機名稱（使用者手動加入的 DDNS 名稱會走到這裡）。照原本的行為放行，
        // 只擋掉不可能是主機名稱的字元。
        for (const QChar ch : ip) {
            if (!(ch.isLetterOrNumber() && ch.unicode() < 0x80) && ch != QLatin1Char('.') && ch != QLatin1Char('-')) {
                return false;
            }
        }
        return true;
    }
    if (!ha.scopeId().isEmpty()) {
        return false;
    }
    if (ha.protocol() == QAbstractSocket::IPv6Protocol) {
        bool isV4 = false;
        const quint32 v4 = ha.toIPv4Address(&isV4);
        if (isV4) {
            ha = QHostAddress(v4);
        }
    }
    if (ha.isLoopback() || ha.isLinkLocal() || ha.isMulticast() ||
            ha == QHostAddress::Any || ha == QHostAddress::AnyIPv4 ||
            ha == QHostAddress::AnyIPv6 || ha == QHostAddress::Broadcast ||
            (ha.protocol() == QAbstractSocket::IPv4Protocol &&
             ha.isInSubnet(QHostAddress(QStringLiteral("0.0.0.0")), 8))) {
        return false;
    }
    return ha.protocol() == QAbstractSocket::IPv4Protocol ||
           ha.protocol() == QAbstractSocket::IPv6Protocol;
}

bool Session::initialize(QQuickWindow* qtWindow)
{
    m_QtWindow = qtWindow;

    // [VIPLE-STARTUP]：initialize 的起點與終點（XR bring-up、解碼器探測、音訊測試都在這中間）；出函式時印，失敗的路徑也印
    struct StartupTimer {
        long long t0Ms;
        bool ok;
        ~StartupTimer()
        {
            const long long nowMs = startupNowMs();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-STARTUP] t=%lld initialize done ok=%d took=%lld ms",
                        nowMs, ok ? 1 : 0, nowMs - t0Ms);
        }
    } startupTimer{startupNowMs(), false};
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-STARTUP] t=%lld initialize begin", startupTimer.t0Ms);

    // §PERF-1113 A5：探測到的解碼器屬性只在這一次 initialize() 裡有效
    m_ProbedDecoderProps.clear();

#ifdef Q_OS_DARWIN
    if (qEnvironmentVariableIntValue("I_WANT_BUGGY_FULLSCREEN") == 0) {
        // If we have a notch and the user specified one of the two native display modes
        // (notched or notchless), override the fullscreen mode to ensure it works as expected.
        // - SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES=0 will place the video underneath the notch
        // - SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES=1 will place the video below the notch
        bool shouldUseFullScreenSpaces = m_Preferences->windowMode != StreamingPreferences::WM_FULLSCREEN;
        SDL_DisplayMode desktopMode;
        SDL_Rect safeArea;
        for (int displayIndex = 0; StreamUtils::getNativeDesktopMode(displayIndex, &desktopMode, &safeArea); displayIndex++) {
            // Check if this display has a notch (safeArea != desktopMode)
            if (desktopMode.h != safeArea.h || desktopMode.w != safeArea.w) {
                // Check if we're trying to stream at the full native resolution (including notch)
                if (m_Preferences->width == desktopMode.w && m_Preferences->height == desktopMode.h) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Overriding default fullscreen mode for native fullscreen resolution");
                    shouldUseFullScreenSpaces = false;
                    break;
                }
                else if (m_Preferences->width == safeArea.w && m_Preferences->height == safeArea.h) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Overriding default fullscreen mode for native safe area resolution");
                    shouldUseFullScreenSpaces = true;
                    break;
                }
            }
        }

        // Using modesetting on modern versions of macOS is extremely unreliable
        // and leads to hangs, deadlocks, and other nasty stuff. The only time
        // people seem to use it is to get the full screen on notched Macs,
        // which setting SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES=1 also accomplishes
        // with much less headache.
        //
        // https://github.com/moonlight-stream/moonlight-qt/issues/973
        // https://github.com/moonlight-stream/moonlight-qt/issues/999
        // https://github.com/moonlight-stream/moonlight-qt/issues/1211
        // https://github.com/moonlight-stream/moonlight-qt/issues/1218
        SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, shouldUseFullScreenSpaces ? "1" : "0");
    }
#endif

#if defined(Q_OS_LINUX) && defined(HAVE_OPENXR)
    // §VR M3a X5（設計 §2.4 ②）：XR 模式且沒有 Wayland／X11 顯示（例如 Frame 上從 SSH 啟動，
    // 或 Steam 以 OpenXR app 直接啟動）時改用 SDL offscreen driver，影像全在 XR 裡。
    if ((m_Preferences->displayTarget == StreamingPreferences::DT_XR_DESKTOP ||
         (m_Preferences->displayTarget == StreamingPreferences::DT_PCVR && !m_Preferences->vrEmulate)) &&
        qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") && qEnvironmentVariableIsEmpty("DISPLAY")) {
        qputenv("SDL_VIDEODRIVER", "offscreen");
        qputenv("SDL_VIDEO_DRIVER", "offscreen");
        SDL_SetHintWithPriority("SDL_VIDEODRIVER", "offscreen", SDL_HINT_OVERRIDE);
        // §VR-HEADLESS：gamescope 裡的 PCVR 是 main() 刻意清掉 DISPLAY／WAYLAND_DISPLAY（不開平面視窗）
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] no Wayland/X11 display - using the SDL offscreen video driver%s",
                    !qEnvironmentVariableIsEmpty("GAMESCOPE_WAYLAND_DISPLAY") ? " (PCVR under gamescope: headless, no flat window)" : "");
    }
#endif

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_VIDEO) failed: %s",
                     SDL_GetError());
        // §REVIEW-1104：SDL_InitSubSystem 失敗時補發 displayLaunchError，讓 GUI 與 CLI 都能拿到啟動錯誤原因
        emit displayLaunchError(tr("Failed to initialize video subsystem: %1").arg(QString::fromUtf8(SDL_GetError())));
        return false;
    }

    LiInitializeStreamConfiguration(&m_StreamConfig);
    m_StreamConfig.width = m_Preferences->width;
    m_StreamConfig.height = m_Preferences->height;
    // §RES-BITRATE：畫面 fps 與碼率的來源在任何改寫之前定下來；之後只改 m_StreamConfig 與這幾個成員，不寫 m_Preferences
    m_OriginalFps = m_Preferences->fps;
    m_BitrateYuv444 = m_Preferences->enableYUV444;
    m_BitrateFruc = false;  // §RES-BITRATE（C08）：下面補幀分支定案時才設
    m_BitrateCapKbps = 0;
    m_BitrateVrKbps = 0;
    decideBitrateFollowsDefault();

    // §VR：PCVR 串流的是 SBS 打包畫面（2W×H），不受 host 顯示器解析度限制
    decideVrRequest();
    if (!m_VrNoFlatError.isEmpty()) {
        // §VR-LAUNCHER：無頭 PCVR 不能退回平面（看不到）——以啟動錯誤結束（CLI 印 "Stream error:"、rc 1）
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VR-SESSION] PCVR has no flat window to fall back to: %s", qUtf8Printable(m_VrNoFlatError));
        emit displayLaunchError(m_VrNoFlatError);
        // §REVIEW-1119：decideVrRequest 可能已經佔住 XR。失敗直接 return 要等 QML 回收才還，下一場會拿不到。
        destroyXrContext();
        m_XrPcvr = false;
        releaseXrOwner();
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }
    if (m_VrRequested) {
        m_StreamConfig.width = 2 * m_VrLaunch.eyeWidth;
        m_StreamConfig.height = m_VrLaunch.eyeHeight;
    }

    // §H.4-AUTO：「自動（本機螢幕最佳）」在這裡換成實際值（PCVR 不經過）
    resolveAutoDisplayMode();

    // VipleStream §H.4 — host-display-aware resolution clamp.
    //
    // The settings page is global (no per-host context), so the user can pick
    // 1920×1200 even when the only paired host's primary display is 1920×1080.
    // Without this guard the host silently captures+encodes at 1080 and the user
    // is left wondering why their "1200p" stream is letterboxed.
    //
    // Sunshine builds advertise host-display modes in /serverinfo (see VipleStream
    // §H.4 patch in nvhttp.cpp); GFE / NVIDIA servers advertise encoder-capable
    // modes instead, so the clamp is gated on the non-NV path.
    // §REVIEW-1119：isEmpty() 與 isNvidiaServerSoftware 原本在鎖外讀。輪詢執行緒在寫鎖裡整份換掉
    // displayModes，鎖外讀 QVector 內部指標會用到已釋放的緩衝。先複製再判斷。
    // gpuModel／gfeVersion 也是 QString，這個函式後面還會讀；鎖外讀一樣會用到已釋放的緩衝，同一把鎖裡一起複製。
    bool hostIsNvidia = false;
    bool hostSwitchable = false;
    QVector<NvDisplayMode> hostModes;
    QString hostGpuModel;
    QString hostGfeVersion;
    int hostCodecModes = 0;
    {
        QReadLocker computerLock(&m_Computer->lock);
        hostIsNvidia = m_Computer->isNvidiaServerSoftware;
        hostCodecModes = m_Computer->serverCodecModeSupport;
        hostGpuModel = m_Computer->gpuModel;
        hostGfeVersion = m_Computer->gfeVersion;
        if (!m_VrRequested) {
            hostSwitchable = m_Computer->displayModeSwitchable;
            hostModes = m_Computer->displayModes;
        }
    }
    if (!m_VrRequested && !hostIsNvidia && !hostModes.isEmpty()) {
        int hostMaxW = 0;
        int hostMaxH = 0;
        for (const NvDisplayMode &mode : std::as_const(hostModes)) {
            // §REVIEW-1119：<=0 與 >16384 在 nvcomputer 已拿掉。這裡再擋細長與過小的模式。
            if (!usableHostDisplayMode(mode.width, mode.height)) {
                continue;
            }
            if ((long long)mode.width * mode.height > (long long)hostMaxW * hostMaxH) {
                hostMaxW = mode.width;
                hostMaxH = mode.height;
            }
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-RES] Stream config requested: %dx%d; host advertises max: %dx%d (%s)",
                    m_StreamConfig.width, m_StreamConfig.height, hostMaxW, hostMaxH,
                    hostSwitchable ? "switchable max" : "current mode");
        if (hostMaxW > 0 && hostMaxH > 0 &&
            m_StreamConfig.width > 0 && m_StreamConfig.height > 0 &&
            (long long)m_StreamConfig.width * m_StreamConfig.height >
                (long long)hostMaxW * hostMaxH) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-RES] Requested %dx%d exceeds host display max %dx%d — "
                        "clamping stream to host maximum to avoid silent letterbox",
                        m_StreamConfig.width, m_StreamConfig.height, hostMaxW, hostMaxH);
            m_StreamConfig.width = hostMaxW;
            m_StreamConfig.height = hostMaxH;
        }
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-RES] Stream config requested: %dx%d (host-display clamp skipped: %s)",
                    m_StreamConfig.width, m_StreamConfig.height,
                    m_VrRequested ? "VR packed frame" :
                    (hostIsNvidia ? "NV server"
                                  : "host did not advertise DisplayMode"));
    }

    int x, y, width, height;
    getWindowDimensions(x, y, width, height);

    // Create a hidden window to use for decoder initialization tests
    SDL_Window* testWindow = SDL_CreateWindow("", x, y, width, height,
                                              SDL_WINDOW_HIDDEN | StreamUtils::getPlatformWindowFlags());
    if (!testWindow) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to create test window with platform flags: %s",
                    SDL_GetError());

        testWindow = SDL_CreateWindow("", x, y, width, height, SDL_WINDOW_HIDDEN);
        if (!testWindow) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed to create window for hardware decode test: %s",
                         SDL_GetError());
            // §REVIEW-1104：測試視窗兩次建立都失敗時補發 displayLaunchError
            emit displayLaunchError(tr("Failed to create window for hardware decode test: %1").arg(QString::fromUtf8(SDL_GetError())));
            // §REVIEW-1119：同上，PCVR 的 XR 已經建好就不能留到 QML 回收。
            destroyXrContext();
            m_XrPcvr = false;
            releaseXrOwner();
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return false;
        }
    }

    // §REVIEW-1119：這兩個字串來自 /serverinfo。換行與其他控制字元會把 log 拆成假的一行。
    auto scrubServerLog = [](QString s) {
        for (int i = 0; i < s.size(); ++i) {
            if (s.at(i).unicode() < 0x20 || s.at(i).unicode() == 0x7F) {
                s[i] = QChar(' ');
            }
        }
        return s;
    };
    qInfo() << "Server GPU:" << scrubServerLog(hostGpuModel);
    qInfo() << "Server GFE version:" << scrubServerLog(hostGfeVersion);

    LiInitializeVideoCallbacks(&m_VideoCallbacks);
    m_VideoCallbacks.setup = drSetup;

    // §K.15: checkpoint-save preferences before streaming starts.
    // If the app crashes during streaming, QSettings' lazy flush may
    // lose the user's last Settings-page changes (fps, FRUC toggle,
    // etc.).  Saving here guarantees the registry reflects the values
    // we are about to use for this session.
    // §VR-NETBENCH：網路測試的子行程用命令列帶的解析度，不能寫回使用者的設定
    if (!NetBench::config().active()) {
        m_Preferences->save();
    }

    // §RES-BITRATE：m_OriginalFps＝這一場的畫面 fps（自動模式由 resolveAutoDisplayMode 解析；否則＝偏好值）
    m_StreamConfig.fps = m_OriginalFps;

    // §B-DUMP — VIPLE_VKFRUC_DUMP_DIR forces enableFrameInterpolation so the
    // dump path actually has interp frames to capture.  Without this, server
    // sends full fps + client shows real-only → FRUC compute chain never runs
    // → dump captures nothing.  This override survives until session ends.
    if (!qgetenv("VIPLE_VKFRUC_DUMP_DIR").isEmpty()
        && !m_Preferences->enableFrameInterpolation) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-DUMP] forcing enableFrameInterpolation=true "
                    "(VIPLE_VKFRUC_DUMP_DIR set, otherwise server sends full fps "
                    "and FRUC chain never runs)");
        m_Preferences->enableFrameInterpolation = true;
    }

    // VipleStream v1.4.151 §R2-α-1: 還原 v1.4.145 半切邏輯 (R1-1 revert).
    // FRUC dual-present 數學上要求 server_fps × ratio = display_fps.
    //
    // VipleStream v1.4.152 §R2-β-1: smart initial ratio.
    // VipleStream v1.4.153 §R2-γ-3: 主動 / 被動 兩條 server fps 路徑.
    //   主動 (default): server fps = UI / ratio, always-on dual/triple present.
    //   被動: server fps = UI (全推), client 端依 recv% 動態升 ratio.
    // §REVIEW-1119：prefs 指標是行程位址，寫進會被分享的 log 就洩漏 ASLR。
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-PREFS] session FRUC gate: enableFrameInterpolation=%d",
                (int)m_Preferences->enableFrameInterpolation);
    if (m_VrRequested) {
        // §VR：fps 一個 session 固定為 HMD 顯示 Hz；VR 不做 FRUC（renderer 端也以
        // LiGetVrFlags() 關掉），偏好設定本身不動（會被 save() 寫回）
        m_StreamConfig.fps = m_VrLaunch.refreshHz;
        m_OriginalFps = m_VrLaunch.refreshHz;
        m_BitrateFruc = false;  // §RES-BITRATE（C08）：VR 不補幀（PCVR 碼率也不走 refreshSessionBitrate）
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-SESSION] VR request: stream %dx%d@%d (FRUC %s)",
                    m_StreamConfig.width, m_StreamConfig.height, m_StreamConfig.fps,
                    m_Preferences->enableFrameInterpolation ? "ignored" : "off");
    }
    else if (m_Preferences->enableFrameInterpolation && m_Preferences->enableYUV444 && m_Computer != nullptr &&
             (hostCodecModes &
              (SCM_H264_HIGH8_444 | SCM_HEVC_REXT8_444 | SCM_HEVC_REXT10_444 | SCM_AV1_HIGH8_444 | SCM_AV1_HIGH10_444)) != 0) {
        // §FRUC-444-FULLFPS（2026-11-12）：4:4:4 的畫面由 libplacebo 顯示，那條路沒有補幀（§FRUC-444-NOTICE 會提示）。
        // 原本照樣把 host 的幀率砍半，結果是「只有一半的幀率、也沒有補幀」（設定 60 實際 30 fps）。
        // 補不了就請 host 送完整幀率；兩個設定本身不動。
        m_StreamConfig.fps = m_OriginalFps;
        // §RES-BITRATE（C08）：host 送全幀，預設碼率不砍半、不加補幀 headroom。之後就算 4:4:4 談不到
        // （"no 4:4:4 this session"），fps 已經是全幀，維持 false。
        m_BitrateFruc = false;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] YUV 4:4:4 is on and this host offers it: frame interpolation cannot run on a 4:4:4 stream, "
                    "so the host is asked for the full %d fps instead of half",
                    m_StreamConfig.fps);
    }
    else if (m_Preferences->enableFrameInterpolation) {
        // §REVIEW-1104：只有 rendererSelection == RS_VULKAN 才啟用被動補幀，其他 renderer 視為關閉以與預設位元率計算一致
        const bool isVulkan = m_Preferences && m_Preferences->rendererSelection == StreamingPreferences::RS_VULKAN;
        const bool passive = isVulkan && m_Preferences->vkfrucPassiveMode;
        // §RES-BITRATE（C08）：主動與被動都交給 getDefaultBitrate 判斷（被動＝全幀、不加 headroom；主動＝÷2／÷3）
        m_BitrateFruc = true;
        if (passive) {
            m_StreamConfig.fps = m_OriginalFps;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] mode=PASSIVE — server fps = %d (UI 全推), "
                        "client 依 recv%% 動態升 ratio (≥70%% 1x, 40-70%% 2x, <40%% 3x)",
                        m_StreamConfig.fps);
        } else {
            // §REVIEW-1104：只有 RS_VULKAN 且啟用 triple 時才會 Present 三張，移除未開 triple 的 240Hz 自動 /3 並限制 tripleEnv 於 Vulkan
            const bool tripleEnv = isVulkan
                                  && ((qEnvironmentVariableIntValue("VIPLE_VKFRUC_TRIPLE") != 0)
                                      || m_Preferences->vkfrucEnableTriple);
            // 抓 primary display refresh (best-effort, multi-monitor 可能不準).
            SDL_DisplayMode dispMode;
            int displayHz = 0;
            if (SDL_GetCurrentDisplayMode(0, &dispMode) == 0 && dispMode.refresh_rate > 0) {
                displayHz = dispMode.refresh_rate;
            }
            int frucRatio = 2;
            const char* reason = "default dual";
            if (tripleEnv) {
                frucRatio = 3;
                reason = "user override (env/pref)";
            }
            // Floor: server fps < 30 不允許, 改回 dual.
            if (m_OriginalFps / frucRatio < 30 && frucRatio == 3) {
                frucRatio = 2;
                reason = "ratio floor (server fps < 30)";
            }
            m_StreamConfig.fps = m_OriginalFps / frucRatio;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] mode=ACTIVE — server fps = %d "
                        "(UI %d fps target ÷ %dx FRUC, display=%dHz, reason: %s)",
                        m_StreamConfig.fps, m_OriginalFps, frucRatio,
                        displayHz, reason);
        }
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] FRUC disabled — requesting %d FPS from server (pass-through).",
                    m_StreamConfig.fps);
    }
    // §REVIEW-1104：算出的 server fps <= 0 時（含主動補幀整除變 0 或被動／直通偏好本身 <= 0）拒絕開串流
    if (m_StreamConfig.fps <= 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Invalid stream FPS: %d (preference FPS: %d)",
                     m_StreamConfig.fps, m_OriginalFps);
        emit displayLaunchError(tr("Invalid stream frame rate (%1 FPS).").arg(m_StreamConfig.fps));
        // §REVIEW-1119：同上，失敗就還 XR，不要等 QML 回收 Session。
        destroyXrContext();
        m_XrPcvr = false;
        releaseXrOwner();
        SDL_DestroyWindow(testWindow);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }
    // §J.3.e.2.i.8 Phase 1.5c — note: ONLY mode + FRUC pref ON → FRUC pref
    // gives us 30fps stream (FRUC 2x halving above), which happens to match
    // ONLY mode's max stable submission rate on NV 596.84 + RTX 3060.  60fps
    // ONLY mode reproduces graphics-queue DEVICE_LOST on first render submit
    // (v1.3.282 testing).  Phase 1.5c-final needs Pacer-level submit rate
    // throttling to match swapchain/display rate; deferred.

    m_StreamConfig.bitrate = m_Preferences->bitrateKbps;
    // §ABR-AUTO：送給 server 的是「串流中依網路調整」，不是滑桿跟預設值的那個旗標
    m_StreamConfig.autoAdjustBitrate = m_Preferences->adaptiveBitrate ? 1 : 0;
    if (m_VrRequested) {
        // M4a 收尾（Frame 實測）：平面串流的位元率（預設 20 Mbps 上下）對 3456x1728@72～120 太低（清晰度差）。
        // VR 用自己的預設：以 3456x1728@90＝150 Mbps 按像素率線性縮放，上限 200 Mbps；只有 CLI 明確帶
        // --bitrate 才照指定值。ABR 照常可往下調。
        //
        // §VR-BITRATE（2026-10-02）：原本還要求「平面位元率偏好＝該解析度的預設值」才套 VR 預設，把平面設定
        // 當成使用者對 VR 的意思。Frame 實測那個偏好是 23000（平面 1080p120 的預設是 28000；CLI 參數會被
        // 存回設定，值對不上就被當成「使用者改過」），結果 VR 又用 23 Mbps。平面設定與 VR 無關，不再參考。
        const int flatDefault = StreamingPreferences::getDefaultBitrate(m_Preferences->width, m_Preferences->height,
                                                                        m_Preferences->fps, m_Preferences->enableYUV444,
                                                                        m_Preferences->enableFrameInterpolation);
        const char* source = "--bitrate";
        if (!m_Preferences->bitrateFromCli) {
            const double pixRate = static_cast<double>(m_StreamConfig.width) * m_StreamConfig.height * m_StreamConfig.fps;
            const double ref = 3456.0 * 1728.0 * 90.0;
            const double baseKbps = (m_Preferences->videoCodecConfig == StreamingPreferences::VCC_FORCE_PYROWAVE) ? 250000.0 : 150000.0;
            const int vrDefault = static_cast<int>(std::lround(baseKbps * pixRate / ref));
            const int maxKbps = (m_Preferences->videoCodecConfig == StreamingPreferences::VCC_FORCE_PYROWAVE) ? 350000 : 200000;
            m_StreamConfig.bitrate = std::clamp(vrDefault, 10000, maxKbps);
            source = "VR default";
        }
        // §RES-BITRATE：PCVR 的碼率在這裡定案，refreshSessionBitrate 不重算（只套 WS 隧道上限）
        m_BitrateVrKbps = m_StreamConfig.bitrate;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-SESSION] bitrate %d kbps (%s; %dx%d@%d, flat setting %d kbps / flat default %d kbps ignored, ABR %s)",
                    m_StreamConfig.bitrate, source, m_StreamConfig.width, m_StreamConfig.height, m_StreamConfig.fps,
                    m_Preferences->bitrateKbps, flatDefault, m_StreamConfig.autoAdjustBitrate ? "on" : "off");
    }
    else {
        // §RES-BITRATE：寬高這時已經過自動模式與 host 顯示器 clamp，fps 已是這一場的畫面 fps
        refreshSessionBitrate("initial");
    }

#ifndef STEAM_LINK
    // Opt-in to all encryption features if we detect that the platform
    // has AES cryptography acceleration instructions and more than 2 cores.
    if (StreamUtils::hasFastAes() && SDL_GetCPUCount() > 2) {
        m_StreamConfig.encryptionFlags = ENCFLG_ALL;
    }
    else {
        // Enable audio encryption as long as we're not on Steam Link.
        // That hardware can hardly handle Opus decoding at all.
        m_StreamConfig.encryptionFlags = ENCFLG_AUDIO;
    }
#endif

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Video bitrate: %d kbps",
                m_StreamConfig.bitrate);

    // §REVIEW-1111（第二輪）：亂數產生失敗時金鑰會是全 0，不能照樣串流
    if (RAND_bytes(reinterpret_cast<unsigned char*>(m_StreamConfig.remoteInputAesKey),
                   sizeof(m_StreamConfig.remoteInputAesKey)) != 1 ||
            // Only the first 4 bytes are populated in the RI key IV
            RAND_bytes(reinterpret_cast<unsigned char*>(m_StreamConfig.remoteInputAesIv), 4) != 1) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "RAND_bytes() failed; cannot create the input encryption key");
        // §REVIEW-1119：這條失敗路徑原本直接 return，測試視窗與 SDL video 參考計數都沒還，GUI／CLI 也看不到原因。
        // 第一個 RAND_bytes 成功時金鑰已經寫進 remoteInputAesKey，不清會留到 QML 回收。
        // PCVR 的 XR 在 decideVrRequest 就建好了，不還的話下一場拿不到 owner。
#if defined(_WIN32)
        SecureZeroMemory(m_StreamConfig.remoteInputAesKey, sizeof(m_StreamConfig.remoteInputAesKey));
        SecureZeroMemory(m_StreamConfig.remoteInputAesIv, sizeof(m_StreamConfig.remoteInputAesIv));
#elif defined(__linux__)
        explicit_bzero(m_StreamConfig.remoteInputAesKey, sizeof(m_StreamConfig.remoteInputAesKey));
        explicit_bzero(m_StreamConfig.remoteInputAesIv, sizeof(m_StreamConfig.remoteInputAesIv));
#else
        // macOS 沒有 explicit_bzero，跟 wipeStdString 一樣用 volatile 寫，編譯器不能省略
        volatile unsigned char* wipe = reinterpret_cast<volatile unsigned char*>(m_StreamConfig.remoteInputAesKey);
        for (size_t wi = 0; wi < sizeof(m_StreamConfig.remoteInputAesKey); ++wi) {
            wipe[wi] = 0;
        }
        wipe = reinterpret_cast<volatile unsigned char*>(m_StreamConfig.remoteInputAesIv);
        for (size_t wi = 0; wi < sizeof(m_StreamConfig.remoteInputAesIv); ++wi) {
            wipe[wi] = 0;
        }
#endif
        destroyXrContext();
        m_XrPcvr = false;
        releaseXrOwner();
        emit displayLaunchError(tr("Failed to generate input encryption key."));
        SDL_DestroyWindow(testWindow);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }

    switch (m_Preferences->audioConfig)
    {
    case StreamingPreferences::AC_STEREO:
        m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
        break;
    case StreamingPreferences::AC_51_SURROUND:
        m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_51_SURROUND;
        break;
    case StreamingPreferences::AC_71_SURROUND:
        m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_71_SURROUND;
        break;
    }

    LiInitializeAudioCallbacks(&m_AudioCallbacks);
    m_AudioCallbacks.init = arInit;
    m_AudioCallbacks.cleanup = arCleanup;
    m_AudioCallbacks.decodeAndPlaySample = arDecodeAndPlaySample;
    m_AudioCallbacks.capabilities = getAudioRendererCapabilities(m_StreamConfig.audioConfiguration);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Audio channel count: %d",
                CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(m_StreamConfig.audioConfiguration));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Audio channel mask: %X",
                CHANNEL_MASK_FROM_AUDIO_CONFIGURATION(m_StreamConfig.audioConfiguration));

    // VipleStream DIAG: crash at second-connect with FRUC toggle was narrowed
    // down to somewhere between audio-mask-log and the decoder probe. These
    // milestone logs help pinpoint the exact failure when a user hits it.
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-DIAG] session::initialize M1 before codec list");

    // Start with all codecs and profiles in priority order
    m_SupportedVideoFormats.append(VIDEO_FORMAT_AV1_HIGH10_444);
    m_SupportedVideoFormats.append(VIDEO_FORMAT_AV1_MAIN10);
    m_SupportedVideoFormats.append(VIDEO_FORMAT_H265_REXT10_444);
    m_SupportedVideoFormats.append(VIDEO_FORMAT_H265_MAIN10);
    m_SupportedVideoFormats.append(VIDEO_FORMAT_AV1_HIGH8_444);
    m_SupportedVideoFormats.append(VIDEO_FORMAT_AV1_MAIN8);
    m_SupportedVideoFormats.append(VIDEO_FORMAT_H265_REXT8_444);
    m_SupportedVideoFormats.append(VIDEO_FORMAT_H265);
    m_SupportedVideoFormats.append(VIDEO_FORMAT_H264_HIGH8_444);
    m_SupportedVideoFormats.append(VIDEO_FORMAT_H264);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-DIAG] session::initialize M2 codec list populated vcc=%d",
                (int)m_Preferences->videoCodecConfig);

    switch (m_Preferences->videoCodecConfig)
    {
    case StreamingPreferences::VCC_AUTO:
    {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-DIAG] session::initialize M3 VCC_AUTO entered");
        // Codecs are checked in order of ascending decode complexity to ensure
        // the the deprioritized list prefers lighter codecs for software decoding

        // H.264 is already the lowest priority codec, so we don't need to do
        // any probing for deprioritization for it here.

        auto hevcDA = getDecoderAvailability(testWindow,
                                             m_Preferences->videoDecoderSelection,
                                             m_Preferences->enableYUV444 ?
                                                 (wants10Bit(m_Preferences) ? VIDEO_FORMAT_H265_REXT10_444 : VIDEO_FORMAT_H265_REXT8_444) :
                                                 (wants10Bit(m_Preferences) ? VIDEO_FORMAT_H265_MAIN10 : VIDEO_FORMAT_H265),
                                             m_StreamConfig.width,
                                             m_StreamConfig.height,
                                             m_StreamConfig.fps);
        if (hevcDA == DecoderAvailability::None && wants10Bit(m_Preferences)) {
            // Remove all 10-bit HEVC profiles
            m_SupportedVideoFormats.removeByMask(VIDEO_FORMAT_MASK_H265 & VIDEO_FORMAT_MASK_10BIT);

            // Check if we have 10-bit AV1 support
            auto av1DA = getDecoderAvailability(testWindow,
                                                m_Preferences->videoDecoderSelection,
                                                m_Preferences->enableYUV444 ? VIDEO_FORMAT_AV1_HIGH10_444 : VIDEO_FORMAT_AV1_MAIN10,
                                                m_StreamConfig.width,
                                                m_StreamConfig.height,
                                                m_StreamConfig.fps);
            if (av1DA == DecoderAvailability::None) {
                // Remove all 10-bit AV1 profiles
                m_SupportedVideoFormats.removeByMask(VIDEO_FORMAT_MASK_AV1 & VIDEO_FORMAT_MASK_10BIT);

                // There are no available 10-bit profiles, so reprobe for 8-bit HEVC
                // and we'll proceed as normal for an SDR streaming scenario.
                SDL_assert(!(m_SupportedVideoFormats & VIDEO_FORMAT_MASK_10BIT));
                hevcDA = getDecoderAvailability(testWindow,
                                                m_Preferences->videoDecoderSelection,
                                                m_Preferences->enableYUV444 ? VIDEO_FORMAT_H265_REXT8_444 : VIDEO_FORMAT_H265,
                                                m_StreamConfig.width,
                                                m_StreamConfig.height,
                                                m_StreamConfig.fps);
            }
        }

        if (hevcDA != DecoderAvailability::Hardware) {
            // Deprioritize HEVC unless the user forced software decoding and enabled HDR.
            // We need HEVC in that case because we cannot support 10-bit content with H.264,
            // which would ordinarily be prioritized for software decoding performance.
            if (m_Preferences->videoDecoderSelection != StreamingPreferences::VDS_FORCE_SOFTWARE || !wants10Bit(m_Preferences)) {
                m_SupportedVideoFormats.deprioritizeByMask(VIDEO_FORMAT_MASK_H265);
            }
        }

        // Deprioritize AV1 unless we can't hardware decode HEVC, and have HDR enabled
        // or we're on Windows or a non-x86 Linux/BSD.
        //
        // Normally, we'd assume hardware that can't decode HEVC definitely can't decode
        // AV1 either, and we wouldn't even bother probing for AV1 support. However, some
        // Windows business systems have HEVC support disabled in firmware from the factory,
        // yet they can still decode AV1 in hardware. To avoid falling back to H.264 on
        // these systems, we don't deprioritize AV1. This firmware-based HEVC licensing
        // behavior seems to be unique to Windows, and Linux on the same system is able
        // to decode HEVC in hardware normally using VAAPI.
        // https://www.reddit.com/r/GeForceNOW/comments/1omsckt/psa_be_wary_of_purchasing_dell_computers_with/
        //
        // Some embedded Linux platforms have incomplete V4L2 decoding support which can
        // lead to unusual cases where a system might support H.264 and AV1 but not HEVC,
        // even if the underlying hardware supports all three. RK3588 is an example of
        // such a SoC. To handle this situation, we will also probe for AV1 if we're on
        // a non-x86 non-macOS UNIX system.
        //
        // We want to keep AV1 at the top of the list for HDR with software decoding
        // because dav1d is higher performance than FFmpeg's HEVC software decoder.
        if (hevcDA == DecoderAvailability::Hardware
#if !defined(Q_OS_WIN32) && (!(defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)) || defined(Q_PROCESSOR_X86))
            || !wants10Bit(m_Preferences)
#endif
            ) {
            m_SupportedVideoFormats.deprioritizeByMask(VIDEO_FORMAT_MASK_AV1);
        }
        else if (!wants10Bit(m_Preferences) &&
                   getDecoderAvailability(testWindow,
                                          m_Preferences->videoDecoderSelection,
                                          m_Preferences->enableYUV444 ? VIDEO_FORMAT_AV1_HIGH8_444 : VIDEO_FORMAT_AV1_MAIN8,
                                          m_StreamConfig.width,
                                          m_StreamConfig.height,
                                          m_StreamConfig.fps) != DecoderAvailability::Hardware) {
            m_SupportedVideoFormats.deprioritizeByMask(VIDEO_FORMAT_MASK_AV1);
        }

#ifdef Q_OS_DARWIN
        {
            // Prior to GFE 3.11, GFE did not allow us to constrain
            // the number of reference frames, so we have to fixup the SPS
            // to allow decoding via VideoToolbox on macOS. Since we don't
            // have fixup code for HEVC, just avoid it if GFE is too old.
            QVector<int> gfeVersion = NvHTTP::parseQuad(hostGfeVersion);
            if (gfeVersion.isEmpty() || // Very old versions don't have GfeVersion at all
                    gfeVersion.size() < 2 ||  // §REVIEW-1111（第二輪）：只有一段的版號不能讀第二段
                    gfeVersion[0] < 3 ||
                    (gfeVersion[0] == 3 && gfeVersion[1] < 11)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Disabling HEVC on macOS due to old GFE version");
                m_SupportedVideoFormats.removeByMask(VIDEO_FORMAT_MASK_H265);
            }
        }
#endif
        break;
    }
    case StreamingPreferences::VCC_FORCE_H264:
        m_SupportedVideoFormats.removeByMask(~VIDEO_FORMAT_MASK_H264);
        break;
    case StreamingPreferences::VCC_FORCE_HEVC:
    case StreamingPreferences::VCC_FORCE_HEVC_HDR_DEPRECATED:
        m_SupportedVideoFormats.removeByMask(~VIDEO_FORMAT_MASK_H265);
        break;
    case StreamingPreferences::VCC_FORCE_AV1:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-DIAG] M3b VCC_FORCE_AV1 entered");
        // We'll try to fall back to HEVC first if AV1 fails. We'd rather not fall back
        // straight to H.264 if the user asked for AV1 and the host doesn't support it.
        m_SupportedVideoFormats.removeByMask(~(VIDEO_FORMAT_MASK_AV1 | VIDEO_FORMAT_MASK_H265));
        break;
    case StreamingPreferences::VCC_FORCE_PYROWAVE:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-DIAG] M3b VCC_FORCE_PYROWAVE entered");
        m_SupportedVideoFormats.prepend(VIDEO_FORMAT_PYROWAVE_MAIN8);
        m_SupportedVideoFormats.prepend(VIDEO_FORMAT_PYROWAVE_444_8);
        m_SupportedVideoFormats.prepend(VIDEO_FORMAT_PYROWAVE_MAIN10);
        m_SupportedVideoFormats.prepend(VIDEO_FORMAT_PYROWAVE_444_10);
        // Keep HEVC as fallback if the paired host does not support PyroWave
        m_SupportedVideoFormats.removeByMask(~(VIDEO_FORMAT_MASK_PYROWAVE | VIDEO_FORMAT_MASK_H265));
        break;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-DIAG] session::initialize M3c switch exited");

    // NB: Since deprioritization puts codecs in reverse order (at the bottom of the list),
    // we want to deprioritize for the most critical attributes last to ensure they are the
    // lowest priority codecs during server negotiation. Here we do that with YUV 4:4:4 and
    // HDR to ensure we never pick a codec profile that doesn't meet the user's requirement
    // if we can avoid it.

    // Mask off YUV 4:4:4 codecs if the option is not enabled
    if (!m_Preferences->enableYUV444) {
        m_SupportedVideoFormats.removeByMask(VIDEO_FORMAT_MASK_YUV444);
    }
    else {
        // Deprioritize YUV 4:2:0 codecs if the user wants YUV 4:4:4
        //
        // NB: Since this happens first before deprioritizing HDR, we will
        // pick a YUV 4:4:4 profile instead of a 10-bit profile if they
        // aren't both available together for any codec.
        m_SupportedVideoFormats.deprioritizeByMask(~VIDEO_FORMAT_MASK_YUV444);
    }

    // Mask off 10-bit codecs if HDR is not enabled
    if (!wants10Bit(m_Preferences)) {
        m_SupportedVideoFormats.removeByMask(VIDEO_FORMAT_MASK_10BIT);
    }
    else {
        // Deprioritize 8-bit codecs if HDR is enabled
        m_SupportedVideoFormats.deprioritizeByMask(~VIDEO_FORMAT_MASK_10BIT);
    }

    switch (m_Preferences->windowMode)
    {
    default:
        // Normally we'd default to fullscreen desktop when starting in windowed
        // mode, but in the case of a slow GPU, we want to use real fullscreen
        // to allow the display to assist with the video scaling work.
        if (WMUtils::isGpuSlow()) {
            m_FullScreenFlag = SDL_WINDOW_FULLSCREEN;
            break;
        }
        // Fall-through
    case StreamingPreferences::WM_FULLSCREEN_DESKTOP:
        // Only use full-screen desktop mode if we're running a desktop environment
        if (WMUtils::isRunningDesktopEnvironment()) {
            m_FullScreenFlag = SDL_WINDOW_FULLSCREEN_DESKTOP;
            break;
        }
        // Fall-through
    case StreamingPreferences::WM_FULLSCREEN:
#ifdef Q_OS_DARWIN
        if (qEnvironmentVariableIntValue("I_WANT_BUGGY_FULLSCREEN") == 0) {
            // Don't use "real" fullscreen on macOS by default. See comments above.
            m_FullScreenFlag = SDL_WINDOW_FULLSCREEN_DESKTOP;
        }
        else {
            m_FullScreenFlag = SDL_WINDOW_FULLSCREEN;
        }
#else
        m_FullScreenFlag = SDL_WINDOW_FULLSCREEN;
#endif
        break;
    }

#if !SDL_VERSION_ATLEAST(2, 0, 11)
    // HACK: Using a full-screen window breaks mouse capture on the Pi's LXDE
    // GUI environment. Force the session to use windowed mode (which won't
    // really matter anyway because the MMAL renderer always draws full-screen).
    if (qgetenv("DESKTOP_SESSION") == "LXDE-pi") {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Forcing windowed mode on LXDE-Pi");
        m_FullScreenFlag = 0;
    }
#endif

#ifdef Q_OS_LINUX
    // §F6 — G-α 驗收用：記錄最終的全螢幕旗標（isGpuSlow 為 true 時上面的
    // windowed/default 分支會強制 SDL_WINDOW_FULLSCREEN）。只記錄、不改行為。
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-LNXFE] windowMode=%d fullscreenFlag=%s isGpuSlow=%d",
                (int)m_Preferences->windowMode,
                m_FullScreenFlag == SDL_WINDOW_FULLSCREEN ? "FULLSCREEN" :
                (m_FullScreenFlag == SDL_WINDOW_FULLSCREEN_DESKTOP ? "FULLSCREEN_DESKTOP" :
                 (m_FullScreenFlag == 0 ? "NONE" : "OTHER")),
                WMUtils::isGpuSlow() ? 1 : 0);
#endif

    // §VR：VR profile 是 SDR 4:2:0，AV1 要等 L1（vr_architecture.md §3.4）。§VR-10BIT：開了 vr10bit 才留 10-bit 的格式
    if (m_VrRequested) {
        m_SupportedVideoFormats.removeByMask(VIDEO_FORMAT_MASK_AV1 | VIDEO_FORMAT_MASK_YUV444 |
                                             (wants10Bit(m_Preferences) ? 0 : VIDEO_FORMAT_MASK_10BIT));
        if (m_Preferences->vr10Bit && !wants10Bit(m_Preferences)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-SESSION] 10-bit was requested (vr10bit) but this device's hardware decoder path only outputs 8-bit - streaming 8-bit");
        }
        if (m_SupportedVideoFormats.isEmpty()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-SESSION] no 8-bit HEVC/H.264 decoder available — streaming flat");
            m_VrRequested = false;
            if (m_XrPcvr) {
                destroyXrContext();  // M4a R1：平面串流不留 PCVR 的 XR session
                m_XrPcvr = false;
            }
            vrFallBackToFlat(tr("This device has no 8-bit HEVC or H.264 video decoder for VR streaming."));
            if (!m_VrNoFlatError.isEmpty()) {
                emit displayLaunchError(m_VrNoFlatError);
                // §REVIEW-1119：上面拆了 context，owner 還沒還。
                releaseXrOwner();
                SDL_DestroyWindow(testWindow);
                SDL_QuitSubSystem(SDL_INIT_VIDEO);
                return false;
            }
        }
    }

    // Check for validation errors/warnings and emit
    // signals for them, if appropriate
    bool ret = validateLaunch(testWindow);

    if (ret) {
        // Video format is now locked in
        m_StreamConfig.supportedVideoFormats = m_SupportedVideoFormats.front();

        // §VR：vrCodecs 告訴 server 這次實際會用的 codec
        if (m_VrRequested) {
            m_VrLaunch.codecs = (m_StreamConfig.supportedVideoFormats & VIDEO_FORMAT_MASK_H265)
                                    ? VIPLE_VR_CODEC_HEVC : VIPLE_VR_CODEC_H264;
        }

        // Populate decoder-dependent properties.
        // Must be done after validateLaunch() since m_StreamConfig is finalized.
        ret = populateDecoderProperties(testWindow);
        // §REVIEW-1104：populateDecoderProperties（chooseDecoder）失敗時補發 displayLaunchError
        if (!ret) {
            emit displayLaunchError(tr("Failed to initialize video decoder."));
        }
    }

    SDL_DestroyWindow(testWindow);

    if (!ret) {
        // §REVIEW-1119：validateLaunch 或解碼器探測失敗時 XR 還在，不要等 QML 回收。
        destroyXrContext();
        m_XrPcvr = false;
        releaseXrOwner();
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }

    startupTimer.ok = true;
    return true;
}

void Session::emitLaunchWarning(QString text)
{
    if (m_Preferences->configurationWarnings) {
        // Queue this launch warning to be displayed after validation
        m_LaunchWarnings.append(text);
        emit launchWarningsChanged();
    }
}

bool Session::validateLaunch(SDL_Window* testWindow)
{
    // §REVIEW-1119：name／gfeVersion 是 QString。輪詢執行緒在寫鎖裡整份換掉，鎖外讀會用到已釋放的緩衝。
    // 下面會做解碼器探測，不能一路持鎖。
    QString hostName;
    QString hostGfeVersion;
    bool hostSupported = false;
    {
        QReadLocker computerLock(&m_Computer->lock);
        hostName = m_Computer->name;
        hostGfeVersion = m_Computer->gfeVersion;
        hostSupported = m_Computer->isSupportedServerVersion;
    }
    if (!hostSupported) {
        emit displayLaunchError(tr("The version of GeForce Experience on %1 is not supported by this build of Moonlight. You must update Moonlight to stream from %1.").arg(hostName));
        return false;
    }

    // §SF-ABSMOUSE：Steam Frame 上絕對滑鼠是預設值（SteamVR 雷射給的是絕對位置），不是使用者特意選的，不警告
    if (m_Preferences->absoluteMouseMode && !m_App.isAppCollectorGame && !SfEnv::isSteamFrame()) {
        emitLaunchWarning(tr("Your selection to enable remote desktop mouse mode may cause problems in games."));
    }

    if (m_Preferences->videoDecoderSelection == StreamingPreferences::VDS_FORCE_SOFTWARE) {
        emitLaunchWarning(tr("Your settings selection to force software decoding may cause poor streaming performance."));
    }

    if (m_SupportedVideoFormats & VIDEO_FORMAT_MASK_PYROWAVE) {
        if (m_SupportedVideoFormats.maskByServerCodecModes(m_Computer->serverCodecModeSupport & SCM_MASK_PYROWAVE) == 0) {
            if (m_Preferences->videoCodecConfig == StreamingPreferences::VCC_FORCE_PYROWAVE) {
                emitLaunchWarning(tr("Your host software or GPU doesn't support encoding PyroWave."));
            }
            m_SupportedVideoFormats.removeByMask(VIDEO_FORMAT_MASK_PYROWAVE);
        }
        else {
            if (getDecoderAvailability(testWindow,
                                       m_Preferences->videoDecoderSelection,
                                       VIDEO_FORMAT_PYROWAVE_MAIN8,
                                       m_StreamConfig.width,
                                       m_StreamConfig.height,
                                       m_StreamConfig.fps) == DecoderAvailability::None) {
                emitLaunchWarning(tr("Your client GPU doesn't support PyroWave Vulkan 1.3 decoding. Falling back to HEVC/H.264."));
                m_SupportedVideoFormats.removeByMask(VIDEO_FORMAT_MASK_PYROWAVE);
            }
            else if (m_Preferences->videoCodecConfig == StreamingPreferences::VCC_FORCE_PYROWAVE) {
                m_SupportedVideoFormats.removeByMask(~VIDEO_FORMAT_MASK_PYROWAVE);

                // §PYROWAVE-HINT（2026-11-18）：PyroWave 在一般碼率下整個畫面都是糊的（11-18 遠端 1080p60＠20 Mbps
                // 實際遇到），串流前先講。VR 場次的碼率另有 PyroWave 預設，不在這裡管
                const int recommended = StreamingPreferences::getPyroWaveRecommendedBitrate(m_StreamConfig.width, m_StreamConfig.height,
                                                                                            m_StreamConfig.fps);
                if (!m_VrRequested && m_StreamConfig.bitrate < recommended) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-PYROWAVE] bitrate %d kbps is below the %d kbps PyroWave needs for %dx%d@%d - the picture will be blurry",
                                m_StreamConfig.bitrate, recommended, m_StreamConfig.width, m_StreamConfig.height, m_StreamConfig.fps);
                    emitLaunchWarning(tr("PyroWave needs a much higher bitrate than other codecs and is meant for a wired LAN. At %1 Mbps the picture will be blurry: raise the bitrate to at least %2 Mbps, or set the video codec to Automatic.")
                                          .arg(m_StreamConfig.bitrate / 1000.0).arg(recommended / 1000.0));
                }
            }
        }
    }

    if (m_SupportedVideoFormats & VIDEO_FORMAT_MASK_AV1) {
        if (m_SupportedVideoFormats.maskByServerCodecModes(m_Computer->serverCodecModeSupport & SCM_MASK_AV1) == 0) {
            if (m_Preferences->videoCodecConfig == StreamingPreferences::VCC_FORCE_AV1) {
                emitLaunchWarning(tr("Your host software or GPU doesn't support encoding AV1."));
            }

            // Moonlight-common-c will handle this case already, but we want
            // to set this explicitly here so we can do our hardware acceleration
            // check below.
            m_SupportedVideoFormats.removeByMask(VIDEO_FORMAT_MASK_AV1);
        }
        else {
            if (!wants10Bit(m_Preferences) && // HDR is checked below
                 m_Preferences->videoDecoderSelection == StreamingPreferences::VDS_AUTO && // Force hardware decoding checked below
                 m_Preferences->videoCodecConfig != StreamingPreferences::VCC_AUTO && // Auto VCC is already checked in initialize()
                 getDecoderAvailability(testWindow,
                                        m_Preferences->videoDecoderSelection,
                                        VIDEO_FORMAT_AV1_MAIN8,
                                        m_StreamConfig.width,
                                        m_StreamConfig.height,
                                        m_StreamConfig.fps) != DecoderAvailability::Hardware) {
                emitLaunchWarning(tr("Using software decoding due to your selection to force AV1 without GPU support. This may cause poor streaming performance."));
            }

            if (m_Preferences->videoCodecConfig == StreamingPreferences::VCC_FORCE_AV1) {
                m_SupportedVideoFormats.removeByMask(~VIDEO_FORMAT_MASK_AV1);
            }
        }
    }

    if (m_SupportedVideoFormats & VIDEO_FORMAT_MASK_H265) {
        if (m_Computer->maxLumaPixelsHEVC == 0) {
            if (m_Preferences->videoCodecConfig == StreamingPreferences::VCC_FORCE_HEVC) {
                emitLaunchWarning(tr("Your host PC doesn't support encoding HEVC."));
            }

            // Moonlight-common-c will handle this case already, but we want
            // to set this explicitly here so we can do our hardware acceleration
            // check below.
            m_SupportedVideoFormats.removeByMask(VIDEO_FORMAT_MASK_H265);
        }
        else {
            if (!wants10Bit(m_Preferences) && // HDR is checked below
                 m_Preferences->videoDecoderSelection == StreamingPreferences::VDS_AUTO && // Force hardware decoding checked below
                 m_Preferences->videoCodecConfig != StreamingPreferences::VCC_AUTO && // Auto VCC is already checked in initialize()
                 getDecoderAvailability(testWindow,
                                        m_Preferences->videoDecoderSelection,
                                        VIDEO_FORMAT_H265,
                                        m_StreamConfig.width,
                                        m_StreamConfig.height,
                                        m_StreamConfig.fps) != DecoderAvailability::Hardware) {
                emitLaunchWarning(tr("Using software decoding due to your selection to force HEVC without GPU support. This may cause poor streaming performance."));
            }

            if (m_Preferences->videoCodecConfig == StreamingPreferences::VCC_FORCE_HEVC) {
                m_SupportedVideoFormats.removeByMask(~VIDEO_FORMAT_MASK_H265);
            }
        }
    }

    if (!(m_SupportedVideoFormats & ~VIDEO_FORMAT_MASK_H264) &&
            m_Preferences->videoDecoderSelection == StreamingPreferences::VDS_AUTO &&
            getDecoderAvailability(testWindow,
                                   m_Preferences->videoDecoderSelection,
                                   VIDEO_FORMAT_H264,
                                   m_StreamConfig.width,
                                   m_StreamConfig.height,
                                   m_StreamConfig.fps) != DecoderAvailability::Hardware) {

        if (m_Preferences->videoCodecConfig == StreamingPreferences::VCC_FORCE_H264) {
            emitLaunchWarning(tr("Using software decoding due to your selection to force H.264 without GPU support. This may cause poor streaming performance."));
        }
        else {
            if (m_Computer->maxLumaPixelsHEVC == 0 &&
                    getDecoderAvailability(testWindow,
                                           m_Preferences->videoDecoderSelection,
                                           VIDEO_FORMAT_H265,
                                           m_StreamConfig.width,
                                           m_StreamConfig.height,
                                           m_StreamConfig.fps) == DecoderAvailability::Hardware) {
                emitLaunchWarning(tr("Your host PC and client PC don't support the same video codecs. This may cause poor streaming performance."));
            }
            else {
                emitLaunchWarning(tr("Your client GPU doesn't support H.264 decoding. This may cause poor streaming performance."));
            }
        }
    }

    if (wants10Bit(m_Preferences)) {
        if (m_Preferences->videoCodecConfig == StreamingPreferences::VCC_FORCE_H264) {
            emitLaunchWarning(tr("HDR is not supported using the H.264 codec."));
            m_SupportedVideoFormats.removeByMask(VIDEO_FORMAT_MASK_10BIT);
        }
        else if (!(m_SupportedVideoFormats & VIDEO_FORMAT_MASK_10BIT)) {
            emitLaunchWarning(tr("This PC's GPU doesn't support 10-bit HEVC or AV1 decoding for HDR streaming."));
        }
        // Check that the server GPU supports HDR
        else if (m_SupportedVideoFormats.maskByServerCodecModes(m_Computer->serverCodecModeSupport & SCM_MASK_10BIT) == 0) {
            emitLaunchWarning(tr("Your host PC doesn't support HDR streaming."));
            m_SupportedVideoFormats.removeByMask(VIDEO_FORMAT_MASK_10BIT);
        }
        else if (m_Preferences->videoCodecConfig != StreamingPreferences::VCC_AUTO) { // Auto was already checked during init
            bool displayedHdrSoftwareDecodeWarning = false;

            // Check that the available HDR-capable codecs on the client and server are compatible
            if (m_SupportedVideoFormats.maskByServerCodecModes(m_Computer->serverCodecModeSupport & SCM_PYROWAVE_MAIN10)) {
                auto da = getDecoderAvailability(testWindow,
                                                 m_Preferences->videoDecoderSelection,
                                                 VIDEO_FORMAT_PYROWAVE_MAIN10,
                                                 m_StreamConfig.width,
                                                 m_StreamConfig.height,
                                                 m_StreamConfig.fps);
                if (da == DecoderAvailability::None) {
                    emitLaunchWarning(tr("This PC's GPU doesn't support PyroWave Main10 decoding for HDR streaming."));
                    m_SupportedVideoFormats.removeByMask(VIDEO_FORMAT_PYROWAVE_MAIN10);
                }
            }
            if (m_SupportedVideoFormats.maskByServerCodecModes(m_Computer->serverCodecModeSupport & SCM_AV1_MAIN10)) {
                auto da = getDecoderAvailability(testWindow,
                                                 m_Preferences->videoDecoderSelection,
                                                 VIDEO_FORMAT_AV1_MAIN10,
                                                 m_StreamConfig.width,
                                                 m_StreamConfig.height,
                                                 m_StreamConfig.fps);
                if (da == DecoderAvailability::None) {
                    emitLaunchWarning(tr("This PC's GPU doesn't support AV1 Main10 decoding for HDR streaming."));
                    m_SupportedVideoFormats.removeByMask(VIDEO_FORMAT_AV1_MAIN10);
                }
                else if (da == DecoderAvailability::Software &&
                           m_Preferences->videoDecoderSelection != StreamingPreferences::VDS_FORCE_SOFTWARE &&
                           !displayedHdrSoftwareDecodeWarning) {
                    emitLaunchWarning(tr("Using software decoding due to your selection to force HDR without GPU support. This may cause poor streaming performance."));
                    displayedHdrSoftwareDecodeWarning = true;
                }
            }
            if (m_SupportedVideoFormats.maskByServerCodecModes(m_Computer->serverCodecModeSupport & SCM_HEVC_MAIN10)) {
                auto da = getDecoderAvailability(testWindow,
                                                 m_Preferences->videoDecoderSelection,
                                                 VIDEO_FORMAT_H265_MAIN10,
                                                 m_StreamConfig.width,
                                                 m_StreamConfig.height,
                                                 m_StreamConfig.fps);
                if (da == DecoderAvailability::None) {
                    emitLaunchWarning(tr("This PC's GPU doesn't support HEVC Main10 decoding for HDR streaming."));
                    m_SupportedVideoFormats.removeByMask(VIDEO_FORMAT_H265_MAIN10);
                }
                else if (da == DecoderAvailability::Software &&
                         m_Preferences->videoDecoderSelection != StreamingPreferences::VDS_FORCE_SOFTWARE &&
                         !displayedHdrSoftwareDecodeWarning) {
                    emitLaunchWarning(tr("Using software decoding due to your selection to force HDR without GPU support. This may cause poor streaming performance."));
                    displayedHdrSoftwareDecodeWarning = true;
                }
            }
        }

        // Check for compatibility between server and client codecs
        if ((m_SupportedVideoFormats & VIDEO_FORMAT_MASK_10BIT) && // Ignore this check if we already failed one above
            !(m_SupportedVideoFormats.maskByServerCodecModes(m_Computer->serverCodecModeSupport) & VIDEO_FORMAT_MASK_10BIT)) {
            emitLaunchWarning(tr("Your host PC and client PC don't support the same HDR video codecs."));
            m_SupportedVideoFormats.removeByMask(VIDEO_FORMAT_MASK_10BIT);
        }
    }

    if (m_Preferences->enableYUV444) {
        if (!(m_Computer->serverCodecModeSupport & SCM_MASK_YUV444)) {
            emitLaunchWarning(tr("Your host PC doesn't support YUV 4:4:4 streaming."));
            m_SupportedVideoFormats.removeByMask(VIDEO_FORMAT_MASK_YUV444);
        }
        else {
            m_SupportedVideoFormats.removeByMask(~m_SupportedVideoFormats.maskByServerCodecModes(m_Computer->serverCodecModeSupport));

            if (!m_SupportedVideoFormats.isEmpty() &&
                !(m_SupportedVideoFormats.front() & VIDEO_FORMAT_MASK_YUV444)) {
                emitLaunchWarning(tr("Your host PC doesn't support YUV 4:4:4 streaming for selected video codec."));
            }
            else if (m_Preferences->videoDecoderSelection != StreamingPreferences::VDS_FORCE_SOFTWARE) {
                while (!m_SupportedVideoFormats.isEmpty() &&
                       (m_SupportedVideoFormats.front() & VIDEO_FORMAT_MASK_YUV444) &&
                       getDecoderAvailability(testWindow,
                                              m_Preferences->videoDecoderSelection,
                                              m_SupportedVideoFormats.front(),
                                              m_StreamConfig.width,
                                              m_StreamConfig.height,
                                              m_StreamConfig.fps) != DecoderAvailability::Hardware) {
                    if (m_Preferences->videoDecoderSelection == StreamingPreferences::VDS_FORCE_HARDWARE) {
                        m_SupportedVideoFormats.removeFirst();
                    }
                    else {
                        emitLaunchWarning(tr("Using software decoding due to your selection to force YUV 4:4:4 without GPU support. This may cause poor streaming performance."));
                        break;
                    }
                }
                if (!m_SupportedVideoFormats.isEmpty() &&
                    !(m_SupportedVideoFormats.front() & VIDEO_FORMAT_MASK_YUV444)) {
                    emitLaunchWarning(tr("This PC's GPU doesn't support YUV 4:4:4 decoding for selected video codec."));
                }
            }
        }
    }

    if (m_StreamConfig.width >= 3840) {
        // Only allow 4K on GFE 3.x+
        if (hostGfeVersion.isEmpty() || hostGfeVersion.startsWith("2.")) {
            emitLaunchWarning(tr("GeForce Experience 3.0 or higher is required for 4K streaming."));

            m_StreamConfig.width = 1920;
            m_StreamConfig.height = 1080;
            refreshSessionBitrate("GFE 4K limit");  // §RES-BITRATE
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-DIAG] session::initialize M4 before testAudio");
    // Test if audio works at the specified audio configuration
    bool audioTestPassed = testAudio(m_StreamConfig.audioConfiguration);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-DIAG] session::initialize M5 after testAudio passed=%d", (int)audioTestPassed);

    // Gracefully degrade to stereo if surround sound doesn't work
    if (!audioTestPassed && CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(m_StreamConfig.audioConfiguration) > 2) {
        audioTestPassed = testAudio(AUDIO_CONFIGURATION_STEREO);
        if (audioTestPassed) {
            m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
            emitLaunchWarning(tr("Your selected surround sound setting is not supported by the current audio device."));
        }
    }

    // If nothing worked, warn the user that audio will not work
    if (!audioTestPassed) {
        emitLaunchWarning(tr("Failed to open audio device. Audio will be unavailable during this session."));
    }

    // §HID-PROBE：每場開始前重測一次（正常機器 ~ms）：Puck 若在上一場串流中才卡死，
    // 啟動時的快照是「OK」，不重測就會在下面的 getUnmappedGamepads 卡死主執行緒；
    // 反過來使用者重插後也不必重啟程式。有無回應裝置 → 這場手把與 SC-HID 都停用，
    // 用 launch warning 提醒（GUI 與 CLI stream 都走這裡；受 configurationWarnings 偏好控制）。
    HidProbe::refresh();
    if (HidProbe::anyUnresponsive()) {
        emitLaunchWarning(tr("A HID device is not responding (%1). Gamepad input and Steam Controller passthrough are disabled for this session. Unplug and replug the device, then start the stream again.").arg(HidProbe::describeUnresponsive()));
    }
    // Check for unmapped gamepads
    else if (!SdlInputHandler::getUnmappedGamepads().isEmpty()) {
        emitLaunchWarning(tr("An attached gamepad has no mapping and won't be usable. Visit the Moonlight help to resolve this."));
    }

    // If we removed all codecs with the checks above, use H.264 as the codec of last resort.
    if (m_SupportedVideoFormats.empty()) {
        m_SupportedVideoFormats.append(VIDEO_FORMAT_H264);
    }

    // NVENC will fail to initialize when any dimension exceeds 4096 using:
    // - H.264 on all versions of NVENC
    // - HEVC prior to Pascal
    //
    // However, if we aren't using Nvidia hosting software, don't assume anything about
    // encoding capabilities by using HEVC Main 10 support. It will likely be wrong.
    if ((m_StreamConfig.width > 4096 || m_StreamConfig.height > 4096) && m_Computer->isNvidiaServerSoftware) {
        // Pascal added support for 8K HEVC encoding support. Maxwell 2 could encode HEVC but only up to 4K.
        // We can't directly identify Pascal, but we can look for HEVC Main10 which was added in the same generation.
        if (m_Computer->maxLumaPixelsHEVC == 0 || !(m_Computer->serverCodecModeSupport & SCM_HEVC_MAIN10)) {
            emit displayLaunchError(tr("Your host PC's GPU doesn't support streaming video resolutions over 4K."));
            return false;
        }
        else if ((m_SupportedVideoFormats & ~VIDEO_FORMAT_MASK_H264) == 0) {
            emit displayLaunchError(tr("Video resolutions over 4K are not supported by the H.264 codec."));
            return false;
        }
    }

    if (m_Preferences->videoDecoderSelection == StreamingPreferences::VDS_FORCE_HARDWARE &&
            !(m_SupportedVideoFormats & VIDEO_FORMAT_MASK_10BIT) && // HDR was already checked for hardware decode support above
            getDecoderAvailability(testWindow,
                                   m_Preferences->videoDecoderSelection,
                                   m_SupportedVideoFormats.front(),
                                   m_StreamConfig.width,
                                   m_StreamConfig.height,
                                   m_StreamConfig.fps) != DecoderAvailability::Hardware) {
        if (m_Preferences->videoCodecConfig == StreamingPreferences::VCC_AUTO) {
            emit displayLaunchError(tr("Your selection to force hardware decoding cannot be satisfied due to missing hardware decoding support on this PC's GPU."));
        }
        else {
            emit displayLaunchError(tr("Your codec selection and force hardware decoding setting are not compatible. This PC's GPU lacks support for decoding your chosen codec."));
        }

        // Fail the launch, because we won't manage to get a decoder for the actual stream
        return false;
    }

    return true;
}


// §REVIEW-1119：std::fill 寫完沒人再讀，最佳化可以整段刪掉。空字串的 data() 可能指到靜態結尾字元，不能寫。
// §REVIEW-1119：QByteArray 解構不會把內容清成 0。空的 data() 可能指到共用的結尾，不能寫。
static void wipeQByteArray(QByteArray& b)
{
    if (b.size() > 0) {
#if defined(_WIN32)
        SecureZeroMemory(b.data(), static_cast<size_t>(b.size()));
#elif defined(__linux__)
        explicit_bzero(b.data(), static_cast<size_t>(b.size()));
#else
        volatile char* p = b.data();
        for (int i = 0; i < b.size(); ++i) {
            p[i] = 0;
        }
#endif
    }
    b.clear();
}

static void sendVrHotspotStateIfAny(FrameHotspot& hotspot);  // §HOTSPOT-ADAPT 第 3 步，定義在 readVrHotspotCreds 前面

static void wipeStdString(std::string& s)
{
    if (!s.empty()) {
#if defined(_WIN32)
        SecureZeroMemory(s.data(), s.size());
#elif defined(__linux__)
        explicit_bzero(s.data(), s.size());
#else
        // macOS 沒有 explicit_bzero：透過 volatile 指標寫，編譯器不能省略
        volatile char* p = s.data();
        for (size_t i = 0; i < s.size(); i++) {
            p[i] = 0;
        }
#endif
    }
    s.clear();
}

// §REVIEW-1119：主迴圈結束後，clVrMessage、adaptive trigger、XR pad 的 SDL_malloc 不會再被釋放。
// 要先把 SDL_USEREVENT 整段取完再放回其餘事件。一批 64 筆裡沒有要釋放的就停的話，前面先排了 64 筆別的事件，
// 後面的 VR 訊息會漏掉；邊取邊放回則會一直重覆同一批。
// producersStillRunning：主迴圈那次排水時控制執行緒還沒停，還在 SDL_PushEvent。不設上限的話對方一直灌，
// 這個迴圈不返回，主執行緒走不到 LiStopConnection，沒有配置的事件也會從有上限的 SDL 佇列被搬進無上限的 keep。
// LiStopConnection 已 join 那些執行緒之後再排的那次傳 false，佇列是有限的，要排完才不會漏掉最後一批。
static void freeQueuedUserPayloads(bool producersStillRunning)
{
    QVector<SDL_Event> keep;
    SDL_Event pending[64];
    int n = 0;
    int batches = 0;
    while ((n = SDL_PeepEvents(pending, SDL_arraysize(pending), SDL_GETEVENT,
                               SDL_USEREVENT, SDL_USEREVENT)) > 0) {
        for (int i = 0; i < n; i++) {
            if (pending[i].user.code == SDL_CODE_VR_MESSAGE ||
                    pending[i].user.code == SDL_CODE_GAMECONTROLLER_SET_ADAPTIVE_TRIGGERS) {
                if (pending[i].user.code == SDL_CODE_VR_MESSAGE) {
                    s_VrMsgQueued.fetch_sub(1, std::memory_order_relaxed);
                }
                SDL_free(pending[i].user.data2);
            }
            else if (pending[i].user.code == SDL_CODE_XR_PAD) {
                SDL_free(pending[i].user.data1);
            }
            else {
                keep.append(pending[i]);
            }
        }
        if (producersStillRunning && ++batches >= 16) {
            break;
        }
    }
    if (!keep.isEmpty()) {
        SDL_PeepEvents(keep.data(), static_cast<int>(keep.size()), SDL_ADDEVENT,
                       SDL_USEREVENT, SDL_USEREVENT);
    }
}

class DeferredSessionCleanupTask : public QRunnable
{
public:
    DeferredSessionCleanupTask(Session* session) :
        m_Session(session) {}

private:
    virtual ~DeferredSessionCleanupTask() override
    {
        // Allow another session to start now that we're cleaned up
        Session::s_ActiveSession = nullptr;
        Session::s_ActiveSessionSemaphore.release();

        // Notify that the session is ready to be cleaned up
        emit m_Session->readyForDeletion();
    }

    void run() override
    {
        // §M.1.f fix (2026-05-14) — split the original `shouldQuit` into two
        // independent concerns:
        //   (a) shouldNotifyServerCancel — does the client need to tell the
        //       server "stream is over" via HTTP /cancel?  Yes whenever this
        //       was a graceful exit (quit key combo, window close, etc),
        //       regardless of whether the client app itself is going to
        //       quit afterwards.  Sending /cancel triggers server-side
        //       proc.terminate() / proc.detach() which clears _owner_uuid,
        //       allowing other paired devices to /launch.  Without it the
        //       §M.1 ownership guard keeps rejecting the next client with
        //       "Server in use by another paired device" 503.
        //   (b) shouldQuitClientApp — does *this* moonlight-qt process exit
        //       after the stream ends?  Controlled by the `quitAppAfter`
        //       preference (default false).  Unchanged behaviour.
        //
        // Pre-fix code coupled these two concerns under one boolean, so
        // when a user pressed the quit key combo with quitAppAfter=false
        // (the default), /cancel was never sent and ownership stayed stuck
        // on the host.  See TODO.md §M.1.f.
        bool shouldNotifyServerCancel = !m_Session->m_UnexpectedTermination;
        bool shouldQuitClientApp =
                !m_Session->m_UnexpectedTermination &&
                m_Session->m_Preferences->quitAppAfter;

        // Notify the UI (we'll emit sessionFinished later, after /cancel
        // round-trip, when we go through the quit-client-app path)
        if (shouldQuitClientApp) {
            emit m_Session->quitStarting();
        }
        else {
            emit m_Session->sessionFinished(m_Session->m_PortTestResults);
        }

        // The video decoder must already be destroyed, since it could
        // try to interact with APIs that can only be called between
        // LiStartConnection() and LiStopConnection().
        SDL_assert(m_Session->m_VideoDecoder == nullptr);

        // VipleStream §N — 停 file transfer 之後再 LiStopConnection。stop() 內部
        // 透過 BlockingQueuedConnection 等 worker thread cleanup 完才返回，所以
        // 這邊直接同步 call 即可，不需要再 queue 到別的 thread。
        if (m_Session->m_FileTransferClient) {
            m_Session->m_FileTransferClient->stop();
            m_Session->m_FileTransferClient->deleteLater();
            m_Session->m_FileTransferClient = nullptr;
        }

        // §SC-HID: Stop Steam Controller passthrough before LiStopConnection
        m_Session->m_ScHid.stop();
        m_Session->m_WifiScanBlock.stop();  // §WIFI-SCANBLOCK
        // §FRAME-HOTSPOT-PAUSE 的 resume() 移到 LiStopConnection() 之後（見下方）
        // §VR：tracking 執行緒會呼叫 LiSendVrTracking，必須在 LiStopConnection 之前停
        m_Session->m_VrTracking.stop();
        m_Session->m_Mic.reset();  // §VR-MIC：編碼執行緒會呼叫 LiSendVrMessage，同理
        m_Session->m_InputTestStop = true;  // §LAT-2D（dev）
        if (m_Session->m_InputTestThread.joinable()) {
            m_Session->m_InputTestThread.join();
        }
        TailscalePath::stop();  // §TS-PATH

        // Finish cleanup of the connection state
        LiStopConnection();
        // §REVIEW-1119：主迴圈那次排水時連線執行緒還能再推進帶 SDL_malloc 的事件。
        // LiStopConnection 會等那些執行緒停，這裡再排一次才收得到最後那一批（視窗建立失敗、沒走到主迴圈排水的也靠這次）。
        // 這次執行緒已停，不設上限，避免主迴圈那次為了不被灌住而提早停、漏掉剩下的。
        freeQueuedUserPayloads(false);

        // §FRAME-HOTSPOT-PAUSE：這一場暫停過熱點就還原。§REVIEW-1120 第四輪：放在 LiStopConnection 之後——
        // resume() 可能同步等背景的「再開熱點」（最久約 19 秒），排在前面時 host 會一直對這台送影像。
        // 它只碰 softapmanager 與標記檔；要送 host 的 HOTSPOT_STATE 只由主迴圈送（主迴圈已經停了），不依賴連線
        m_Session->m_FrameHotspot.resume();

        // VipleStream: tear down the relay UDP tunnel. LiStopConnection
        // has already closed all moonlight-common-c sockets so nothing
        // is still sending to 127.0.0.1:47998/48000.
        if (m_Session->m_UdpTunnel) {
            m_Session->m_UdpTunnel->stop();
            m_Session->m_UdpTunnel->wait(3000);
            delete m_Session->m_UdpTunnel;
            m_Session->m_UdpTunnel = nullptr;
        }

        // §M.1.f fix: always notify server of graceful stream end via
        // /cancel — this releases server-side ownership so other paired
        // devices can /launch.  Independent of whether the client app
        // itself is going to quit afterwards.
        if (shouldNotifyServerCancel) {
            NvComputer* computer = m_Session->m_Computer;
            StreamingPreferences* prefs = m_Session->m_Preferences;

            if (computer->onlineViaRelay && prefs &&
                !prefs->relayUrl.isEmpty() && !computer->uuid.isEmpty()) {
                // VipleStream: host is only reachable via relay — route /cancel
                // through the relay HTTP proxy. Direct /cancel would hang
                // for the full Qt network timeout.
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-NAT] Sending /cancel via relay proxy");
                QString resp = RelayLookup::httpProxy(
                    prefs->relayUrl, prefs->relayPsk,
                    computer->uuid,
                    QStringLiteral("/cancel?uniqueid=0123456789ABCDEF"),
                    8000);
                if (resp.isEmpty()) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-NAT] Relay /cancel returned empty response");
                }
            } else {
                NvHTTP http(computer);
                // Logging is already done inside NvHTTP
                try {
                    http.quitApp();
                } catch (const GfeHttpResponseException&) {
                } catch (const QtNetworkReplyException&) {
                }
            }
        }

        if (shouldQuitClientApp) {
            // Session is finished now (the earlier UI emit was quitStarting,
            // emit the actual sessionFinished after the /cancel round-trip)
            emit m_Session->sessionFinished(m_Session->m_PortTestResults);
        }

        // Exit the entire program if requested
        if (m_Session->m_ShouldExit) {
            QCoreApplication::instance()->quit();
        }
    }

    Session* m_Session;
};

// §H.4-AUTO：把「自動（本機螢幕最佳）」換成實際串流值。
//   本機值：一般＝串流視窗所在螢幕的桌面解析度／目前更新率；XR 桌面＝2560x1440，更新率先用
//           本機螢幕的值當暫定，XrContext bring-up 後由 applyXrDesktopAutoFps 換成 runtime 實際值。
//   與 host 取小：host 回報 DisplayModeSwitchable（串流前會把顯示器切到要求模式）時，解析度與
//           更新率都跟它回報的「可切換最高模式」取小；沒有時更新率不夾、解析度交給下面原本的
//           §H.4 clamp（夾到 host 目前模式）。
//   位元率：這裡不算（§RES-BITRATE）。後面還有 host 顯示器 clamp，由 refreshSessionBitrate 依最終值推導。
// 結果只放在 m_StreamConfig.width/height 與 m_OriginalFps，不寫回 m_Preferences（寫回會被 save() 存成使用者設定）。
void Session::resolveAutoDisplayMode()
{
    const bool autoRes = m_Preferences->effectiveAutoResolution();
    const bool autoFps = m_Preferences->effectiveAutoFps();
    if (!autoRes && !autoFps) {
        return;
    }
    if (m_VrRequested) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-RES] auto display mode skipped: PCVR uses the HMD eye size / refresh");
        return;
    }

    const bool xrDesktop = m_Preferences->displayTarget == StreamingPreferences::DT_XR_DESKTOP;
    const int displayIndex = getStreamDisplayIndex();
    int localW = 0, localH = 0, localHz = 0;
    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(displayIndex, &dm) == 0) {
        localW = dm.w;
        localH = dm.h;
        localHz = dm.refresh_rate;
    }
    else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-RES] auto: SDL_GetDesktopDisplayMode(%d) failed: %s",
                    displayIndex, SDL_GetError());
    }
    // 與設定頁同樣的正規化（有些螢幕回報 59／119 之類的值）
    if (localHz >= 58 && localHz <= 62) {
        localHz = 60;
    }
    else if (localHz >= 28 && localHz <= 32) {
        localHz = 30;
    }
    if (xrDesktop) {
        localW = 2560;
        localH = 1440;
        if (localHz <= 0) {
            localHz = 60;  // 暫定值；XR runtime 量到後會取代
        }
    }

    int hostW = 0, hostH = 0, hostHz = 0;
    const bool hostSwitchable = !m_Computer->isNvidiaServerSoftware && m_Computer->displayModeSwitchable;
    if (hostSwitchable) {
        QVector<NvDisplayMode> hostModesCopy;  // §REVIEW-1111（第二輪）：鎖內複製再走訪
        {
            QReadLocker computerLock(&m_Computer->lock);
            hostModesCopy = m_Computer->displayModes;
        }
        for (const NvDisplayMode &mode : std::as_const(hostModesCopy)) {
            // §REVIEW-1119：負值已在 nvcomputer 拿掉。細長模式與 1 Hz／上千 Hz 仍會把自動解析度或更新率壓壞。
            if (!usableHostDisplayMode(mode.width, mode.height) ||
                    mode.refreshRate < 20 || mode.refreshRate > 1000) {
                continue;
            }
            const long long area = (long long)mode.width * mode.height;
            if (area > (long long)hostW * hostH || (area == (long long)hostW * hostH && mode.refreshRate > hostHz)) {
                hostW = mode.width;
                hostH = mode.height;
                hostHz = mode.refreshRate;
            }
        }
    }

    int newW = m_Preferences->width;
    int newH = m_Preferences->height;
    int newFps = m_Preferences->fps;
    if (autoRes && localW > 0 && localH > 0) {
        newW = localW;
        newH = localH;
        if (hostW > 0 && hostH > 0 && (long long)newW * newH > (long long)hostW * hostH) {
            newW = hostW;
            newH = hostH;
        }
    }
    if (autoFps && localHz > 0) {
        newFps = localHz;
        if (hostHz > 0 && newFps > hostHz) {
            newFps = hostHz;
        }
    }

    // §RES-BITRATE：結果只放在這一場（m_StreamConfig 與 m_OriginalFps），不寫 m_Preferences——那會被 save() 存成
    // 使用者的設定。碼率這時還沒定案（後面還有 host 顯示器 clamp），由 refreshSessionBitrate 依最終值算。
    m_StreamConfig.width = newW;
    m_StreamConfig.height = newH;
    m_OriginalFps = newFps;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RES] auto display mode: resolution=%s fps=%s local=%dx%d@%d (%s, display %d) "
                "host=%s%dx%d@%d -> stream %dx%d@%d (bitrate decided after the host display clamp: %s)",
                autoRes ? "auto" : "manual", autoFps ? "auto" : "manual",
                localW, localH, localHz, xrDesktop ? "XR desktop" : "desktop mode", displayIndex,
                hostSwitchable ? "switchable max " : "not switchable ", hostW, hostH, hostHz,
                newW, newH, newFps,
                m_BitrateFollowsDefault ? "follows default" : (m_Preferences->bitrateFromCli ? "--bitrate" : "user setting"));
}

// §H.4-AUTO：XR 桌面自動更新率。XrContext 在 startConnectionAsync（/launch 之前）才 bring-up，
// 所以更新率要在這裡、/launch 之前才換成 runtime 實際值。這一場真的在補幀（m_BitrateFruc）時不動
// （server fps 已依 UI fps 折半）。偏好開著但 4:4:4 走全幀時不能早退。
void Session::applyXrDesktopAutoFps()
{
#ifdef HAVE_OPENXR
    if (m_Preferences->displayTarget != StreamingPreferences::DT_XR_DESKTOP || m_VrRequested ||
        !m_Preferences->effectiveAutoFps() || m_XrContext == nullptr) {
        return;
    }
    const float measured = m_XrContext->waitRefreshHz(3000);
    int fps = static_cast<int>(std::lround(measured));
    if (fps <= 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-RES] auto fps (XR desktop): runtime refresh unknown - keeping %d fps", m_StreamConfig.fps);
        return;
    }
    int hostHz = 0;
    if (!m_Computer->isNvidiaServerSoftware && m_Computer->displayModeSwitchable) {
        int hostW = 0, hostH = 0;
        QVector<NvDisplayMode> hostModesCopy;  // §REVIEW-1111（第二輪）：鎖內複製再走訪
        {
            QReadLocker computerLock(&m_Computer->lock);
            hostModesCopy = m_Computer->displayModes;
        }
        for (const NvDisplayMode &mode : std::as_const(hostModesCopy)) {
            // §REVIEW-1119：同 resolveAutoDisplayMode，不採用細長模式與不合理的更新率。
            if (!usableHostDisplayMode(mode.width, mode.height) ||
                    mode.refreshRate < 20 || mode.refreshRate > 1000) {
                continue;
            }
            const long long area = (long long)mode.width * mode.height;
            if (area > (long long)hostW * hostH || (area == (long long)hostW * hostH && mode.refreshRate > hostHz)) {
                hostW = mode.width;
                hostH = mode.height;
                hostHz = mode.refreshRate;
            }
        }
        if (hostHz > 0 && fps > hostHz) {
            fps = hostHz;
        }
    }
    // 審查 11-21：早退看這一場是不是真的在補幀，不看偏好。4:4:4＋補幀偏好開著時 host 送的是全幀
    // （m_BitrateFruc 為 false），暫定的 SDL 更新率仍要換成 runtime，否則頭盔 120 Hz 會停在 60。
    if (m_BitrateFruc) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-RES] auto fps (XR desktop): runtime %.2f Hz, FRUC on - keeping server fps %d",
                    (double)measured, m_StreamConfig.fps);
        return;
    }
    if (fps == m_StreamConfig.fps) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-RES] auto fps (XR desktop): runtime %.2f Hz -> %d fps (unchanged)", (double)measured, fps);
        return;
    }
    // §RES-BITRATE：只改這一場的值，不寫 m_Preferences；碼率由 refreshSessionBitrate 依新的 fps 推導
    const int oldFps = m_StreamConfig.fps;
    m_StreamConfig.fps = fps;
    m_OriginalFps = fps;
    char hostDesc[48];
    if (hostHz > 0) {
        SDL_snprintf(hostDesc, sizeof(hostDesc), "host max %d Hz", hostHz);
    }
    else {
        SDL_strlcpy(hostDesc, "host not switchable", sizeof(hostDesc));  // 不印「0 Hz」：那不是 host 回報的值
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RES] auto fps (XR desktop): runtime %.2f Hz, %s -> stream %dx%d@%d (was %d)",
                (double)measured, hostDesc, m_StreamConfig.width, m_StreamConfig.height, fps, oldFps);
    refreshSessionBitrate("XR desktop auto fps");
#endif
}

// §RES-BITRATE：開場判定一次「碼率跟著預設值」。以 autoAdjustBitrate 旗標為準（拖滑桿→false、按「使用預設值」→true），
// 不再用「目前值＝某組寬高／fps 的預設值」去猜——三個改寫點各比各的，中間有一步沒重算就永遠對不上。
// 保守處理：旗標是 true、但存的碼率不是「偏好組合」的預設值（StreamingPreferences::bitrateLooksDefault，
// 和設定頁「使用預設值」按鈕的顯示條件相同）時，當成使用者的值保留，不蓋掉。
// §RES-BITRATE（第二輪審查 C06）：原本拿常見解析度 × 更新率 × 4:4:4 × 補幀、加上所有螢幕的桌面模式去比（約 2000 組），
// 預設值都是 1000 kbps 的整數倍，5～85 Mbps 每個 5 Mbps 的整數幾乎全中，舊版 CLI 存進來的 30000／50000 會被蓋掉，
// 設定頁卻還顯示 50 Mbps 與「使用預設值」按鈕。改成只比一組：按鈕看得到的時候，串流一定用存的值。
// 舊版場次寫回造成的不一致（例如 2560x1440@144 配 40000）也照存的值送（2.0.257 本來就是這樣），按一下「使用預設值」就回來。
void Session::decideBitrateFollowsDefault()
{
    m_BitrateFollowsDefault = false;
    if (m_Preferences->bitrateFromCli) {
        return;  // --bitrate：原樣
    }
    if (m_Preferences->bitrateDefaultFromCli) {
        m_BitrateFollowsDefault = true;  // CLI 改了解析度／fps 而沒帶 --bitrate：照舊跟著預設值
        return;
    }
    if (!m_Preferences->autoAdjustBitrate) {
        return;  // 使用者拖過滑桿
    }

    const bool looksDefault = m_Preferences->bitrateLooksDefault();
    m_BitrateFollowsDefault = looksDefault;
    if (!looksDefault) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-RES] bitrate: autoAdjustBitrate is on but the stored %d kbps is not the default of the saved mode "
                    "(%dx%d@%d yuv444=%d fruc=%d -> %d kbps) - keeping it as a user setting "
                    "(press \"Use Default\" in settings to follow the default again)",
                    m_Preferences->bitrateKbps, m_Preferences->width, m_Preferences->height, m_Preferences->fps,
                    (int)m_Preferences->enableYUV444, (int)m_Preferences->enableFrameInterpolation,
                    StreamingPreferences::getDefaultBitrate(m_Preferences->width, m_Preferences->height, m_Preferences->fps,
                                                            m_Preferences->enableYUV444,
                                                            m_Preferences->enableFrameInterpolation));
    }
}

// §RES-BITRATE：碼率＝最終串流參數的函式，不是一路改上來的累積值。
//   PCVR（或 PCVR 晚退回平面）→ VR 段落定下的值，不重算
//   跟著預設值              → 預設(實際串流寬高, 畫面 fps, 這一場是否真的用 4:4:4, 補幀)
//   否則                    → 偏好值（使用者滑桿或 --bitrate），原樣
//   最後有傳輸上限（WS 隧道）就取小
// 畫面 fps＝補幀除 2／3 之前的 m_OriginalFps：getDefaultBitrate 自己會處理補幀。
// 補幀用 m_BitrateFruc（這一場 host 是不是真的送半幀），不讀偏好旗標：§FRUC-444-FULLFPS 時偏好開著補幀，
// host 卻送全幀（第二輪審查 C08）。
void Session::refreshSessionBitrate(const char* why)
{
    const int before = m_StreamConfig.bitrate;
    const char* source;
    int kbps;
    if (m_BitrateVrKbps > 0) {
        kbps = m_BitrateVrKbps;
        source = "VR";
    }
    else if (m_BitrateFollowsDefault) {
        kbps = StreamingPreferences::getDefaultBitrate(m_StreamConfig.width, m_StreamConfig.height, m_OriginalFps,
                                                       m_BitrateYuv444, m_BitrateFruc);
        source = "default";
    }
    else {
        kbps = m_Preferences->bitrateKbps;
        source = m_Preferences->bitrateFromCli ? "--bitrate" : "user setting";
    }
    const bool capped = m_BitrateCapKbps > 0 && kbps > m_BitrateCapKbps;
    if (capped) {
        kbps = m_BitrateCapKbps;
    }
    m_StreamConfig.bitrate = kbps;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RES] bitrate %d -> %d kbps (%s; %dx%d@%d, yuv444=%d fruc=%d, %s%s)", before, kbps, why,
                m_StreamConfig.width, m_StreamConfig.height, m_OriginalFps,
                (int)m_BitrateYuv444, (int)m_BitrateFruc, source, capped ? ", capped" : "");
}

int Session::getStreamDisplayIndex()
{
    int displayIndex = 0;

    if (m_Window != nullptr) {
        displayIndex = SDL_GetWindowDisplayIndex(m_Window);
        SDL_assert(displayIndex >= 0);
    }
    // Create our window on the same display that Qt's UI
    // was being displayed on.
    else {
        Q_ASSERT(m_QtWindow != nullptr);
        if (m_QtWindow != nullptr) {
            QScreen* screen = m_QtWindow->screen();
            if (screen != nullptr) {
                QRect displayRect = screen->geometry();

                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Qt UI screen is at (%d,%d)",
                            displayRect.x(), displayRect.y());
                for (int i = 0; i < SDL_GetNumVideoDisplays(); i++) {
                    SDL_Rect displayBounds;

                    if (SDL_GetDisplayBounds(i, &displayBounds) == 0) {
                        if (displayBounds.x == displayRect.x() &&
                            displayBounds.y == displayRect.y()) {
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                        "SDL found matching display %d",
                                        i);
                            displayIndex = i;
                            break;
                        }
                    }
                    else {
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                    "SDL_GetDisplayBounds(%d) failed: %s",
                                    i, SDL_GetError());
                    }
                }
            }
            else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Qt window is not associated with a QScreen!");
            }
        }
    }

    return displayIndex;
}

void Session::getWindowDimensions(int& x, int& y,
                                  int& width, int& height)
{
    const int displayIndex = getStreamDisplayIndex();

    SDL_Rect usableBounds;
    if (SDL_GetDisplayUsableBounds(displayIndex, &usableBounds) == 0) {
        // If the stream resolution fits within the usable display area, use it directly
        if (m_StreamConfig.width <= usableBounds.w &&
            m_StreamConfig.height <= usableBounds.h) {
            width = m_StreamConfig.width;
            height = m_StreamConfig.height;
        } else {
            // Otherwise, use 80% of usable bounds and preserve aspect ratio
            SDL_Rect src, dst;
            src.x = src.y = dst.x = dst.y = 0;
            src.w = m_StreamConfig.width;
            src.h = m_StreamConfig.height;

            dst.w = ((int)(usableBounds.w * 0.80f)) & ~0x1;  // even width
            dst.h = ((int)(usableBounds.h * 0.80f)) & ~0x1;  // even height

            StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

            width = dst.w;
            height = dst.h;
        }
    }
    else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_GetDisplayUsableBounds() failed: %s",
                     SDL_GetError());

        width = m_StreamConfig.width;
        height = m_StreamConfig.height;
    }

    x = y = SDL_WINDOWPOS_CENTERED_DISPLAY(displayIndex);
}

void Session::updateOptimalWindowDisplayMode()
{
    SDL_DisplayMode desktopMode, bestMode, mode;
    int displayIndex = SDL_GetWindowDisplayIndex(m_Window);

    // Try the current display mode first. On macOS, this will be the normal
    // scaled desktop resolution setting.
    if (SDL_GetDesktopDisplayMode(displayIndex, &desktopMode) == 0) {
        // If this doesn't fit the selected resolution, use the native
        // resolution of the panel (unscaled).
        if (desktopMode.w < m_ActiveVideoWidth || desktopMode.h < m_ActiveVideoHeight) {
            SDL_Rect safeArea;
            if (!StreamUtils::getNativeDesktopMode(displayIndex, &desktopMode, &safeArea)) {
                return;
            }
        }
    }
    else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL_GetDesktopDisplayMode() failed: %s",
                    SDL_GetError());
        return;
    }

    // On devices with slow GPUs, we will try to match the display mode
    // to the video stream to offload the scaling work to the display.
    //
    // We also try to match the video resolution if we're using KMSDRM,
    // because scaling on the display is generally higher quality than
    // scaling performed by drmModeSetPlane().
    bool matchVideo;
    if (!Utils::getEnvironmentVariableOverride("MATCH_DISPLAY_MODE_TO_VIDEO", &matchVideo)) {
        matchVideo = WMUtils::isGpuSlow() || QString(SDL_GetCurrentVideoDriver()) == "KMSDRM";
    }

#ifdef Q_OS_LINUX
    // §F6 — G-α 驗收用（msm / Adreno 不再被判為慢 GPU → 不強制 matchVideo）
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-LNXFE] matchVideo=%d (isGpuSlow=%d videoDriver=%s)",
                matchVideo ? 1 : 0,
                WMUtils::isGpuSlow() ? 1 : 0,
                SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
#endif

    bestMode = desktopMode;
    bestMode.refresh_rate = 0;
    if (!matchVideo) {
        // Start with the native desktop resolution and try to find
        // the highest refresh rate that our stream FPS evenly divides.
        int numDisplayModes = SDL_GetNumDisplayModes(displayIndex);
        for (int i = 0; i < numDisplayModes; i++) {
            if (SDL_GetDisplayMode(displayIndex, i, &mode) == 0) {
                // §REVIEW-1104：取餘數前先確認 m_StreamConfig.fps > 0，避免除以零
                if (mode.w == desktopMode.w && mode.h == desktopMode.h &&
                    m_StreamConfig.fps > 0 && mode.refresh_rate % m_StreamConfig.fps == 0) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Found display mode with desktop resolution: %dx%dx%d",
                                mode.w, mode.h, mode.refresh_rate);
                    if (mode.refresh_rate > bestMode.refresh_rate) {
                        bestMode = mode;
                    }
                }
            }
        }
    }

    // If we didn't find a mode that matched the current resolution and
    // had a high enough refresh rate, start looking for lower resolution
    // modes that can meet the required refresh rate and minimum video
    // resolution. We will also try to pick a display mode that matches
    // aspect ratio closest to the video stream.
    if (bestMode.refresh_rate == 0) {
        float bestModeAspectRatio = 0;
        float videoAspectRatio = m_ActiveVideoHeight > 0 ? (float)m_ActiveVideoWidth / (float)m_ActiveVideoHeight : 16.0f / 9.0f;
        int numDisplayModes = SDL_GetNumDisplayModes(displayIndex);
        for (int i = 0; i < numDisplayModes; i++) {
            if (SDL_GetDisplayMode(displayIndex, i, &mode) == 0) {
                float modeAspectRatio = (float)mode.w / (float)mode.h;
                // §REVIEW-1104：取餘數前先確認 m_StreamConfig.fps > 0，避免除以零
                if (mode.w >= m_ActiveVideoWidth && mode.h >= m_ActiveVideoHeight &&
                        m_StreamConfig.fps > 0 && mode.refresh_rate % m_StreamConfig.fps == 0) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Found display mode with video resolution: %dx%dx%d",
                                mode.w, mode.h, mode.refresh_rate);
                    if (mode.refresh_rate >= bestMode.refresh_rate &&
                            (bestModeAspectRatio == 0 || fabs(videoAspectRatio - modeAspectRatio) <= fabs(videoAspectRatio - bestModeAspectRatio))) {
                        bestMode = mode;
                        bestModeAspectRatio = modeAspectRatio;
                    }
                }
            }
        }
    }

    if (bestMode.refresh_rate == 0) {
        // We may find no match if the user has moved a 120 FPS
        // stream onto a 60 Hz monitor (since no refresh rate can
        // divide our FPS setting). We'll stick to the default in
        // this case.
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "No matching display mode found; using desktop mode");
        bestMode = desktopMode;
    }

    if ((SDL_GetWindowFlags(m_Window) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN) {
        // Only print when the window is actually in full-screen exclusive mode,
        // otherwise we're not actually using the mode we've set here
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Chosen best display mode: %dx%dx%d",
                    bestMode.w, bestMode.h, bestMode.refresh_rate);
    }

    SDL_SetWindowDisplayMode(m_Window, &bestMode);
}

void Session::cancelFileTransfer()
{
    // §N.6 (v1.4.103) — in-session keystroke (Ctrl+Alt+Shift+T) hook.
    // Web UI 重新整理 / 等 stream 結束本來就能中止 transfer; 本路徑加快 user
    // 工作流程, 不必離開 stream session 切 Web UI.  m_FileTransferClient 在
    // session 啟動時 alloc (line 2241), stream 結束 stopAndFreeRendererStuff()
    // 內 stop+deleteLater (line 1435-1438).  null check 因 transfer client
    // 可能尚未 init 或已 teardown.
    if (m_FileTransferClient) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-XFER] cancelFileTransfer hotkey invoked");
        m_FileTransferClient->cancelCurrent();
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-XFER] cancelFileTransfer hotkey: no active client");
    }
}

void Session::toggleFRUC()
{
    // v1.4.151 §R2-α-1: 純切 renderer FRUC pause flag, 不再動 server fps.
    // R1 加的 LiRequestFpsChange runtime toggle 移除 — 切 server fps 會 re-init
    // NVENC, 卡 50ms+ 黑屏不划算. 純 client-side pause/resume interp 即可.
    if (m_VideoDecoder) {
        auto* ffmpegDecoder = dynamic_cast<FFmpegVideoDecoder*>(m_VideoDecoder);
        if (ffmpegDecoder) {
            auto* renderer = ffmpegDecoder->getBackendRenderer();
            if (renderer) {
                renderer->toggleFRUC();
            }
        }
    }
}

void Session::toggleFullscreen()
{
    bool fullScreen = !(SDL_GetWindowFlags(m_Window) & m_FullScreenFlag);

#if defined(Q_OS_WIN32) || defined(Q_OS_DARWIN)
    // Destroy the video decoder before toggling full-screen because D3D9 can try
    // to put the window back into full-screen before we've managed to destroy
    // the renderer. This leads to excessive flickering and can cause the window
    // decorations to get messed up as SDL and D3D9 fight over the window style.
    //
    // On Apple Silicon Macs, the AVSampleBufferDisplayLayer may cause WindowServer
    // to deadlock when transitioning out of fullscreen. Destroy the decoder before
    // exiting fullscreen as a workaround. See issue #973.
    SDL_LockMutex(m_DecoderLock);
    delete m_VideoDecoder;
    m_VideoDecoder = nullptr;
    SDL_UnlockMutex(m_DecoderLock);
#endif

    // Actually enter/leave fullscreen
    SDL_SetWindowFullscreen(m_Window, fullScreen ? m_FullScreenFlag : 0);

#ifdef Q_OS_DARWIN
    // SDL on macOS has a bug that causes the window size to be reset to crazy
    // large dimensions when exiting out of true fullscreen mode. We can work
    // around the issue by manually resetting the position and size here.
    if (!fullScreen && m_FullScreenFlag == SDL_WINDOW_FULLSCREEN) {
        int x, y, width, height;
        getWindowDimensions(x, y, width, height);
        SDL_SetWindowSize(m_Window, width, height);
        SDL_SetWindowPosition(m_Window, x, y);
    }
#endif

    // Input handler might need to start/stop keyboard grab after changing modes
    m_InputHandler->updateKeyboardGrabState();

    // Input handler might need stop/stop mouse grab after changing modes
    m_InputHandler->updatePointerRegionLock();
}

void Session::notifyMouseEmulationMode(bool enabled)
{
    const int refCount = (m_MouseEmulationRefCount += enabled ? 1 : -1);
    SDL_assert(refCount >= 0);

    // We re-use the status update overlay for mouse mode notification
    if (refCount > 0) {
        m_OverlayManager.updateOverlayText(Overlay::OverlayStatusUpdate, "Gamepad mouse mode active\nLong press Start to deactivate");
        m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, true);
    }
    else {
        m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, false);
    }
}

class AsyncConnectionStartThread : public QThread
{
public:
    AsyncConnectionStartThread(Session* session) :
        QThread(nullptr),
        m_Session(session)
    {
        setObjectName("Async Conn Start");
    }

    void run() override
    {
        m_Session->m_AsyncConnectionSuccess = m_Session->startConnectionAsync();
    }

    Session* m_Session;
};

// Called in a non-main thread
// §VR M3a X1：XR 虛擬螢幕（β）的 XrContext 在 /launch 與 relay 之前 bring-up（設計 §2.4 ①；
// PCVR 的 /launch 要先量 FOV 等，β 共用同一個時機）。失敗依不變式 5：/launch 前 → 記原因、
// 退回平面，不中斷連線。X1 只有 loading quad，影像仍畫在平面視窗（XrRenderer 在 X2）。
void Session::setupXrDesktop()
{
    if (m_Preferences->displayTarget != StreamingPreferences::DT_XR_DESKTOP) {
        return;
    }
#ifdef HAVE_OPENXR
    if (!m_Preferences->xrRuntimeJsonPath.isEmpty()) {
        // dev 覆寫（CLI --xr-runtime-json）：只影響這個行程，不是使用者設定（不變式 7）
        // §REVIEW-1119：log 只留檔名。完整路徑含家目錄，會進磁碟上的紀錄檔。
        const QString xrJson = m_Preferences->xrRuntimeJsonPath;
        const int slash = qMax(xrJson.lastIndexOf(QLatin1Char('/')), xrJson.lastIndexOf(QLatin1Char('\\')));
        qputenv("XR_RUNTIME_JSON", QFile::encodeName(xrJson));
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] dev override: XR_RUNTIME_JSON=%s (in-process)",
                    qUtf8Printable(slash >= 0 ? xrJson.mid(slash + 1) : xrJson));
    }
    m_XrRebuildAttempt = 0;
    QString err;
    if (!createXrContext(false, &err)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-XR] xr-desktop unavailable (%s) - falling back to the flat window (invariant 5)",
                    qUtf8Printable(err));
        // §REVIEW-1103：createXrContext 一開頭就請 VR 介面讓出 XR；退回平面後原本要等整場結束才歸還，
        // 這段時間頭盔裡沒有任何 XR 畫面。退回平面就歸還。
        releaseXrOwner();
        return;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-XR] xr-desktop session running (video path %s)",
                m_XrContext->video() != nullptr ? "ready" : "unavailable - video stays in the flat window");
#else
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-XR] xr-desktop requested but this build has no OpenXR - falling back to the flat window");
#endif
}

// §VR M3a X5：建立並 bring-up XrContext（首次與 LOSS 後重建共用）。失敗時 m_XrContext 維持 nullptr。
namespace {
std::function<void(bool)> s_XrOwnerHook;  // §VR-SHELL
}

void Session::setXrOwnerHook(std::function<void(bool sessionWantsXr)> hook)
{
    s_XrOwnerHook = std::move(hook);
}

void Session::releaseXrOwner()
{
    if (m_XrOwnerHeld) {
        m_XrOwnerHeld = false;
        if (s_XrOwnerHook) {
            s_XrOwnerHook(false);
        }
    }
}

bool Session::createXrContext(bool isRebuild, QString* error)
{
#ifdef HAVE_OPENXR
    if (!m_XrOwnerHeld && s_XrOwnerHook) {
        // §VR-SHELL：VR 介面先讓出它的 XR session（重建時已經拿著，不必再要）
        m_XrOwnerHeld = true;
        s_XrOwnerHook(true);
    }
    Q_UNUSED(isRebuild)
    XrContext::Options xo;
    xo.pcvr = m_XrPcvr;  // M4a R1
    // M4a 收尾：PCVR 明確要求更新率（偏好 vrRefreshHz，預設 90；取 runtime 可用值中最接近的）
    xo.preferredRefreshHz = m_XrPcvr ? static_cast<float>(m_Preferences->vrRefreshHz) : 0.0f;
    xo.testVrInput = m_Preferences->vrTestInput;  // M4a R2（dev）：--vr-test-input
    xo.pcvrSynth = m_XrPcvr && m_Preferences->vrHalfRate && m_Preferences->vrSynth;  // §VR-SYNTH：只在半速串流時有意義
    // §VR-RATE-AUTO（2026-11-08）：全速串流時網路不穩就要求 host 中途改半速（開場就選半速時不作用）
    xo.pcvrRateAuto = m_XrPcvr && m_Preferences->vrRateAuto && !m_Preferences->vrHalfRate;
    // §VR-BUFFER：頭盔端多緩衝一格吸收到達抖動（設定 vrbufferframes，預設 1）。2.0.25 加了設定與 XR 端，這裡一直漏接，
    // 緩衝實際上從沒開過——遠端（行動網路，到達抖動約 10 ms＞一格 8.3 ms）每 10 s 約 200 張重複＋200 張跳過（2026-11-04）
    // §VR-BUFFER-AUTO（11-04）：自動時 swapchain 照最多 2 格配置，實際深度由 XrVideo 依重複幀在 1、2 之間切換
    xo.pcvrBufferAuto = m_XrPcvr && m_Preferences->vrBufferAuto && m_Preferences->vrBufferFrames > 0;
    xo.pcvrBufferFrames = m_XrPcvr ? (xo.pcvrBufferAuto ? 2 : m_Preferences->vrBufferFrames) : 0;
    if (m_XrPcvr && !isRebuild) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LDIAG] buffer=%d%s (effective %d: off while synth is on)", xo.pcvrBufferFrames,
                    xo.pcvrBufferAuto ? " auto(1-2)" : "", xo.pcvrSynth ? 0 : xo.pcvrBufferFrames);
    }
    xo.pcvrTrackLock = m_XrPcvr && m_Preferences->vrTrackLock;  // §VR-TRACK-LOCK
    xo.pcvrTrackRelock = xo.pcvrTrackLock && m_Preferences->effectiveTrackRelock();  // §VR-TRACK-RELOCK（--vr-track-relock 優先）
    xo.pcvrLatchDelayUs = m_XrPcvr ? static_cast<uint32_t>(m_Preferences->vrLatchDelayMs * 1000.0) : 0u;  // §VR-LATE-LATCH
    xo.pcvrLatchRelatch = m_XrPcvr && m_Preferences->effectiveLatchRelatch();  // §VR-LATCH-RELATCH（--vr-latch-relatch 優先）
    xo.pcvrLatchAbSec = m_XrPcvr ? m_Preferences->effectiveLatchAbSec() : 0;   // §VR-LATCH A/B（--vr-latch-ab 優先）
    // §VR-LATCH-WAIT（--vr-latch-wait 優先）。串流是不是半速要等量到更新率才知道，見 setupXrPcvr() 的 setPcvrStreamDiv
    xo.pcvrLatchWaitUs = m_XrPcvr ? static_cast<uint32_t>(std::lround(std::clamp(m_Preferences->effectiveLatchWaitMs(), 0.0, 1.0) * 1000.0)) : 0u;
    // §VR-LATCH-FLIGHT（--vr-latch-flight 優先）。設了它上面的等待就不作用（XrContext::pcvrLatchWaitEffUs）
    xo.pcvrLatchFlightUs = m_XrPcvr ? static_cast<uint32_t>(std::lround(std::clamp(m_Preferences->effectiveLatchFlightMs(), 0.0, 1.0) * 1000.0)) : 0u;
    xo.pcvrOverscanDeg = m_XrPcvr ? static_cast<float>(m_Preferences->vrOverscanDeg) : 0.0f;  // dev：--vr-overscan
    xo.dumpFramePath = isRebuild ? QString() : m_Preferences->xrDumpFramePath;  // dev：--xr-dump-frame
    xo.testStallMs = isRebuild ? 0 : m_Preferences->xrTestStallMs;             // dev：--xr-test-stall-ms
    xo.unwarpMax = static_cast<float>(m_Preferences->vrUnwarpMax);  // §VR-UNWARP-MAX（dev）
    xo.unwarpYuv = !m_Preferences->vrUnwarpRgb;                      // §VR-UNWARP-YUV
    xo.fovLayers = !m_Preferences->vrFovSingleLayer;                 // §VR-FOVEA-LAYERS
    xo.testJitter = (m_XrPcvr && !isRebuild) ? m_Preferences->vrTestJitter : QString();  // dev：--vr-test-jitter（§VR-LDIAG，只給 PCVR）
    xo.testRecenterSec = isRebuild ? 0 : m_Preferences->xrTestRecenterSec;     // dev：--xr-test-recenter-sec
    xo.testPointer = m_Preferences->xrTestPointer;                              // dev：--xr-test-pointer
    xo.testKeyboardText = isRebuild ? QString() : m_Preferences->xrTestKeyboard;   // dev：--xr-test-keyboard
    xo.dumpKeyboardPath = isRebuild ? QString() : m_Preferences->xrDumpKeyboardPath;  // dev：--xr-dump-keyboard
    if (m_XrPcvr && !isRebuild) {
        // §VR-LDIAG：這一場 client 端和 latch 有關的設定值。事後看 log 要對得上是哪一組（延後挑影像幾毫秒只看
        // predictAhead 是猜不準的）。halfRate 是設定值，實際有沒有減半看 [VIPLE-VR-SESSION] half-rate 那一行
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-LDIAG] client cfg: latchDelay=%.1f ms trackLock=%d halfRate=%d synth=%d fovea=%d overscan=%.1f jitter=%s"
                    " trackRelock=%d latchRelatch=%d latchWait=%.2f ms abSec=%d flight=%.2f ms",
                    xo.pcvrLatchDelayUs / 1000.0, xo.pcvrTrackLock ? 1 : 0, m_Preferences->vrHalfRate ? 1 : 0, xo.pcvrSynth ? 1 : 0,
                    m_Preferences->vrFoveation, static_cast<double>(xo.pcvrOverscanDeg),
                    xo.testJitter.isEmpty() ? "off" : qUtf8Printable(xo.testJitter), xo.pcvrTrackRelock ? 1 : 0,
                    // 補幀開著時 relatch 與等待都不作用：印實際會用的值（等待另外受「延後＋等待 ≤ 6 ms」限制，
                    // 而且 Windows 上不作用，見 XrContext::pcvrLatchWaitSupported；生效的上限在 10 s 行的 wait=）。
                    // 設了 §VR-LATCH-FLIGHT 時等待也不作用，同樣印 0
                    xo.pcvrLatchRelatch && !xo.pcvrSynth ? 1 : 0,
                    (xo.pcvrSynth || xo.pcvrLatchFlightUs > 0) ? 0.0 : xo.pcvrLatchWaitUs / 1000.0, xo.pcvrLatchAbSec,
                    // §VR-LATCH-FLIGHT：行尾新增，印的是設定值（補幀開著也作用）。平台、延後量、更新率造成的不作用由
                    // XrContext 另外印一行 `latch flight … ignored`；實際的上限在 10 s 行的 flight=
                    xo.pcvrLatchFlightUs / 1000.0);
    }
    // X5（dev）：--xr-test-fail。首次：bringup＝直接失敗；loss／loss3／exit＝15 s 後模擬。
    // 重建：loss3＝每次都失敗（驗 3 次後退回平面）；loss＝重建成功且不再注入。
    const QString& tf = m_Preferences->xrTestFail;
    if (!isRebuild) {
        xo.testFailBringUp = (tf == QLatin1String("bringup"));
        if (tf == QLatin1String("loss") || tf == QLatin1String("loss3")) {
            xo.testFailAfterSec = 15;
            xo.testFailKind = XrContext::kEndedLoss;
        }
        else if (tf == QLatin1String("exit")) {
            xo.testFailAfterSec = 15;
            xo.testFailKind = XrContext::kEndedExit;
        }
        else if (tf == QLatin1String("gpuwedge")) {
            // M4a 收尾：模擬 GPU 等待逾時（runtime 串流中死掉）→ wedged 拆除＋loss 流程
            xo.testFailAfterSec = 15;
            xo.testFailKind = XrContext::kTestGpuWedge;
        }
    }
    else {
        xo.testFailBringUp = (tf == QLatin1String("loss3"));
    }
    // X4：射線滑鼠事件從 XR frame thread 推到 main loop（SDL_PushEvent 可跨執行緒）
    xo.inputSink.pointer = [](float u, float v) {
        const uint32_t u16 = (uint32_t)(std::clamp(u, 0.0f, 1.0f) * 65535.0f + 0.5f);
        const uint32_t v16 = (uint32_t)(std::clamp(v, 0.0f, 1.0f) * 65535.0f + 0.5f);
        SDL_Event ev = {};
        ev.type = SDL_USEREVENT;
        ev.user.code = SDL_CODE_XR_POINTER;
        ev.user.data1 = (void*)(uintptr_t)((u16 << 16) | v16);
        SDL_PushEvent(&ev);
    };
    xo.inputSink.button = [](int button, bool pressed) {
        SDL_Event ev = {};
        ev.type = SDL_USEREVENT;
        ev.user.code = SDL_CODE_XR_BUTTON;
        ev.user.data1 = (void*)(uintptr_t)button;
        ev.user.data2 = (void*)(uintptr_t)(pressed ? 1 : 0);
        SDL_PushEvent(&ev);
    };
    xo.inputSink.scroll = [](int amount) {
        SDL_Event ev = {};
        ev.type = SDL_USEREVENT;
        ev.user.code = SDL_CODE_XR_SCROLL;
        ev.user.data1 = (void*)(intptr_t)amount;
        SDL_PushEvent(&ev);
    };
    // 虛擬鍵盤：按鍵依序推到 main loop（SDL 事件佇列保序，修飾鍵與一般鍵的先後不會亂）
    // §KBD-IME-STATE：data1 的 bit24–31 是 LiSendKeyboardEvent2 的 flags（「中/英」才非 0）
    xo.inputSink.key = [](int vk, bool down, uint8_t modifiers, uint8_t flags) {
        SDL_Event ev = {};
        ev.type = SDL_USEREVENT;
        ev.user.code = SDL_CODE_XR_KEY;
        ev.user.data1 = (void*)(uintptr_t)(((uint32_t)flags << 24) | ((uint32_t)modifiers << 16) | ((uint32_t)vk & 0xFFFFu));
        ev.user.data2 = (void*)(uintptr_t)(down ? 1 : 0);
        SDL_PushEvent(&ev);
    };
    // §VR-EXIT-COMBO：頭盔手把 A＋X＋扳機＋握把＝結束串流（同手把的 Start+Select+L1+R1：推 SDL_QUIT）
    xo.inputSink.exitCombo = []() {
        SDL_Event ev = {};
        ev.type = SDL_QUIT;
        ev.quit.timestamp = SDL_GetTicks();
        SDL_PushEvent(&ev);
    };
    // §VR-WHEEL：方向盤模式的初始值（設定頁），串流中兩支搖桿同時按住 1.5 秒切換
    {
        const int range = qBound(90, m_Preferences->vrWheelRange, 900);
        xo.vrWheel = m_Preferences->vrWheel ? static_cast<uint8_t>(VIPLE_VR_WHEEL_ON | (range / 10)) : 0;
    }
    if (!xo.pcvr) {
        // §XR-PADMODE：2D 串流的 Xbox 手把模式（兩支選單鍵同時按住 2 秒切換，和 VR 串流同一個操作）
        xo.inputSink.gamepad = [](const XrPadState& s) {
            XrPadState* copy = static_cast<XrPadState*>(SDL_malloc(sizeof(XrPadState)));
            if (copy == nullptr) {
                return;
            }
            *copy = s;
            SDL_Event ev = {};
            ev.type = SDL_USEREVENT;
            ev.user.code = SDL_CODE_XR_PAD;
            ev.user.data1 = copy;
            if (SDL_PushEvent(&ev) != 1) {
                SDL_free(copy);
            }
        };
        xo.inputSink.padMode = [](bool on) {
            SDL_Event ev = {};
            ev.type = SDL_USEREVENT;
            ev.user.code = SDL_CODE_XR_PADMODE;
            ev.user.data1 = (void*)(uintptr_t)(on ? 1 : 0);
            SDL_PushEvent(&ev);
        };
    }
    xo.inputSink.wheelToggled = [](bool on) {
        SDL_Event ev = {};
        ev.type = SDL_USEREVENT;
        ev.user.code = SDL_CODE_XR_WHEEL;
        ev.user.data1 = (void*)(uintptr_t)(on ? 1 : 0);
        SDL_PushEvent(&ev);
    };
    // X5：runtime 結束 session（frame thread 呼叫，只推事件）
    xo.onEnded = [](int reason) {
        SDL_Event ev = {};
        ev.type = SDL_USEREVENT;
        ev.user.code = SDL_CODE_XR_ENDED;
        ev.user.data1 = (void*)(intptr_t)reason;
        SDL_PushEvent(&ev);
    };
    m_XrContext = new XrContext(xo);
    // 首次在 /launch 前（設計 §2.4 ①）逾時 5 s；重建在 main thread，也用 5 s
    if (!m_XrContext->bringUp(5000, error)) {
        delete m_XrContext;
        m_XrContext = nullptr;
        return false;
    }
    return true;
#else
    Q_UNUSED(isRebuild);
    if (error) *error = QStringLiteral("no OpenXR in this build");
    return false;
#endif
}

// §VR M3a X5：XR session 被 runtime 結束。decoder 綁著舊 XrContext 的 XrVideo，先刪 decoder
// 再拆 XrContext；之後一律經 SDL_RENDER_DEVICE_RESET 走既有的 decoder 重建流程（有 m_XrContext
// 就接回 XR，沒有就是平面）——重建 decoder 本來就需要 IDR，這條路徑已經過大量實戰；改成把
// XrRenderer 掛到新 context 則要處理舊 XrVideo 內 in-flight 幀的所有權，風險較高。
// PCVR（DT_PCVR）：VR session 建立後的 XR 失敗應送 /cancel 並顯示錯誤（不變式 5），M4a 接 PCVR XR 時做。
// §VR-NETBENCH：網路測試的一階。量滿（第一個視窗不算，之後 (秒數−10)/10 個 10 秒視窗）就寫結果並正常結束串流
// （SDL_QUIT → /cancel，host 停掉測試片段）；XR 一直沒起來或沒有視窗時，逾時寫一份「沒有結果」再結束
void Session::netBenchTick()
{
    if (m_NetBenchDone) {
        return;
    }
    const Uint32 now = SDL_GetTicks();
    if (m_NetBenchStartMs == 0) {
        m_NetBenchStartMs = now;
    }
    const NetBench::Config& cfg = NetBench::config();
    const auto windows = NetBench::Collector::get().windows();
    const size_t need = NetBench::kSkipWindows + static_cast<size_t>((std::max)(1, (cfg.seconds - 10) / 10));
    const bool full = windows.size() >= need;
    const bool timeout = SDL_TICKS_PASSED(now, m_NetBenchStartMs + static_cast<Uint32>(cfg.seconds + 60) * 1000u);
    if (!full && !timeout) {
        return;
    }
    m_NetBenchDone = true;
    NetBench::StepResult r = NetBench::summarize(m_Preferences->vrEyeWidth, windows, NetBench::Collector::get().sawHalf());
    if (!full) {
        r.error = QStringLiteral("timed out after %1 window(s)").arg(windows.size());
    }
    const bool ok = NetBench::writeResult(cfg.resultPath, r);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VR-NETBENCH] step eye=%d windows=%d deficit=%.3f%% rep10=%.1f run=%d half=%d mtp50=%.1f mtp95=%.1f "
                "recv=%.1f Mbps lateNet=%llu lateHeadset=%llu held=%d headsetLimited=%d%s%s -> %s",
                r.eye, r.windows, r.deficitPct, r.rep10, r.maxRun, r.half ? 1 : 0, r.mtp50, r.mtp95, r.recvMbps,
                static_cast<unsigned long long>(r.lateNet), static_cast<unsigned long long>(r.lateHeadset), r.held ? 1 : 0,
                r.headsetLimited ? 1 : 0, r.error.isEmpty() ? "" : " error=", qUtf8Printable(r.error),
                ok ? "written" : "could not write the result file");
    SDL_Event quit = {};
    quit.type = SDL_QUIT;
    SDL_PushEvent(&quit);
    // 結果已經寫好：正常收尾 20 秒還沒結束就直接結束程序。11-09 頭盔實測：硬體解碼器（v4l2m2m）整階沒解出任何影格時，
    // 收尾卡在 ~FFmpegVideoDecoder 等解碼執行緒（它卡在 v4l2 的 poll 裡），子行程永遠不退出，測試流程跟著停住
    std::thread([] {
        // §REVIEW-1111：20 → 45 秒。收尾裡的 /cancel 最多等 30 秒（網路測試當下線路常常就是慢的），加上解碼器收尾約 3 秒；
        // 20 秒就結束會把還沒送到的 /cancel 砍掉，host 還佔著這一場，下一階的 /launch 就被拒絕。
        std::this_thread::sleep_for(std::chrono::seconds(45));
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-NETBENCH] the stream did not shut down within 45 s after the result was written - exiting");
        fflush(nullptr);
        std::_Exit(0);
    }).detach();
}

void Session::handleXrEnded(int reason)
{
#ifdef HAVE_OPENXR
    if (m_XrContext == nullptr) {
        return;
    }
    if (m_XrPcvr) {
        // M4a R1／收尾：VR session 已建立後 XR 失效——不變式 5：送 /cancel 並顯示錯誤，不退回平面（平面留著
        // VR 形狀沒有意義）。/cancel：這裡推的 SDL_QUIT 走正常退出路徑（m_UnexpectedTermination=false →
        // shouldNotifyServerCancel），server 收到後結束 VR session、編排器 disarm。錯誤經 displayLaunchError
        // 顯示（GUI 對話框；CLI 印 stderr 並以 rc 1 結束，見 StreamSegue.qml／main.cpp）。
        emit displayLaunchError(tr("The XR runtime %1 after the VR session started, so the stream was ended.")
                                    .arg(reason == XrContext::kEndedExit ? tr("exited") : tr("stopped responding or was lost")));
        m_VrTracking.stop();
        SDL_LockMutex(m_DecoderLock);
        delete m_VideoDecoder;
        m_VideoDecoder = nullptr;
        SDL_UnlockMutex(m_DecoderLock);
        destroyXrContext();
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VR-SESSION] XR session %s after the VR session started - ending the stream (invariant 5)",
                     reason == XrContext::kEndedExit ? "exited" : "lost");
        SDL_Event quit = {};
        quit.type = SDL_QUIT;
        SDL_PushEvent(&quit);
        return;
    }
    SDL_LockMutex(m_DecoderLock);
    delete m_VideoDecoder;
    m_VideoDecoder = nullptr;
    SDL_UnlockMutex(m_DecoderLock);
    destroyXrContext();

    if (reason == XrContext::kEndedExit) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-XR] runtime ended the XR session (EXITING) - continuing in the flat window");
        requestDecoderRecreate();
        return;
    }
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-XR] XR session lost - rebuilding (up to 3 attempts); video paused");
    m_XrRebuildAttempt = 0;
    scheduleXrRebuild();
#else
    Q_UNUSED(reason);
#endif
}

void Session::scheduleXrRebuild()
{
    // 退避 0.5／1／2 s；generation 讓 session 結束或被取代後的舊計時事件失效
    const uint32_t delayMs = 500u << (std::min)(m_XrRebuildAttempt, 2);
    const uint32_t gen = ++m_XrRebuildGeneration;
    m_XrRebuildTimer = SDL_AddTimer(delayMs, [](Uint32, void* param) -> Uint32 {
        SDL_Event ev = {};
        ev.type = SDL_USEREVENT;
        ev.user.code = SDL_CODE_XR_REBUILD;
        ev.user.data1 = param;
        SDL_PushEvent(&ev);
        return 0;  // 一次性
    }, (void*)(uintptr_t)gen);
}

void Session::handleXrRebuild(uint32_t generation)
{
    if (generation != m_XrRebuildGeneration || m_XrContext != nullptr) {
        return;  // 過期或已有 context
    }
    m_XrRebuildTimer = 0;
    m_XrRebuildAttempt++;
    QString err;
    if (createXrContext(true, &err)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] XR session rebuilt (attempt %d/3)", m_XrRebuildAttempt);
        requestDecoderRecreate();
        return;
    }
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] XR rebuild attempt %d/3 failed: %s",
                m_XrRebuildAttempt, qUtf8Printable(err));
    if (m_XrRebuildAttempt < 3) {
        scheduleXrRebuild();
        return;
    }
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-XR] giving up on XR after 3 attempts - falling back to the flat window");
    releaseXrOwner();  // §REVIEW-1103：同 setupXrDesktop，放棄 XR 就把它還給 VR 介面
    requestDecoderRecreate();
}

void Session::requestDecoderRecreate()
{
    // 走既有的 SDL_RENDER_DEVICE_RESET：刪舊 decoder、依 m_XrContext 重選 frontend、要 IDR
    SDL_Event ev = {};
    ev.type = SDL_RENDER_DEVICE_RESET;
    SDL_PushEvent(&ev);
}

bool Session::xrOwnsMouse() const
{
#ifdef HAVE_OPENXR
    return m_XrContext != nullptr && m_XrContext->inputFocused();
#else
    return false;
#endif
}

void Session::xrToggleKeyboard()
{
#ifdef HAVE_OPENXR
    if (m_XrContext != nullptr) {
        m_XrContext->requestKeyboardToggle();
        return;
    }
#endif
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] keyboard toggle ignored: no XR session");
}

void Session::xrRecenter()
{
#ifdef HAVE_OPENXR
    if (m_XrContext != nullptr) {
        m_XrContext->requestRecenter();
        return;
    }
#endif
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] recenter ignored: no XR session");
}

void Session::destroyXrContext()
{
    // M4a R2（dev）：--vr-test-haptic 的注入計時器要在 XrContext 拆除前移除
    if (m_VrTestHapticTimer != 0) {
        SDL_RemoveTimer(m_VrTestHapticTimer);
        m_VrTestHapticTimer = 0;
    }
    // §VR M3a X5：取消尚未觸發的重建計時，並讓已排隊的 SDL_CODE_XR_REBUILD 失效
    if (m_XrRebuildTimer != 0) {
        SDL_RemoveTimer(m_XrRebuildTimer);
        m_XrRebuildTimer = 0;
    }
    m_XrRebuildGeneration++;
#ifdef HAVE_OPENXR
    if (m_XrContext != nullptr) {
        // M4a R1：tracking 送出執行緒的樣本來源會呼叫 m_XrContext->sampleHmd，拆之前先停
        m_VrTracking.stop();
        m_XrContext->shutdown();
        delete m_XrContext;
    }
#endif
    m_XrContext = nullptr;
}

// M4a R1：PCVR 走真 XR。在 initialize()（/launch 之前、串流尺寸決定之前）bring-up，量每眼 FOV／eyeToHead／
// period 填 /launch 參數。失敗回 false（呼叫端退回平面，不變式 5）。
bool Session::setupXrPcvr()
{
#ifdef HAVE_OPENXR
    if (!m_Preferences->xrRuntimeJsonPath.isEmpty()) {
        // §REVIEW-1119：同 setupXrDesktop，log 不印完整路徑。
        const QString xrJson = m_Preferences->xrRuntimeJsonPath;
        const int slash = qMax(xrJson.lastIndexOf(QLatin1Char('/')), xrJson.lastIndexOf(QLatin1Char('\\')));
        qputenv("XR_RUNTIME_JSON", QFile::encodeName(xrJson));
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] dev override: XR_RUNTIME_JSON=%s (in-process)",
                    qUtf8Printable(slash >= 0 ? xrJson.mid(slash + 1) : xrJson));
    }
    m_XrPcvr = true;
    m_XrRebuildAttempt = 0;
    QString err;
    if (!createXrContext(false, &err)) {
        m_XrPcvr = false;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-SESSION] PCVR: XR runtime unavailable (%s) - streaming flat (invariant 5)", qUtf8Printable(err));
        vrFallBackToFlat(tr("Could not start VR on this headset (%1). Make sure SteamVR is running, then try again.").arg(err));
        return false;
    }
    XrContext::ViewInfo vi;
    if (!m_XrContext->waitViews(&vi, 3000)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-SESSION] PCVR: XR views not available within 3 s - streaming flat (invariant 5)");
        destroyXrContext();
        m_XrPcvr = false;
        vrFallBackToFlat(tr("The headset's VR runtime did not respond in time. Make sure SteamVR is running, then try again."));
        return false;
    }
    // FOV：tan 的絕對值×10000（左、右、上、下；與 M1a 預設一致，server 端一律取絕對值）
    for (int e = 0; e < 2; e++) {
        const XrFovf& f = vi.fov[e];
        const float t[4] = {std::tan(-f.angleLeft), std::tan(f.angleRight), std::tan(f.angleUp), std::tan(-f.angleDown)};
        for (int i = 0; i < 4; i++) {
            m_VrLaunch.fovTan[e * 4 + i] = (int)std::lround(std::clamp(std::fabs(t[i]), 0.01f, 10.0f) * 10000.0f);
        }
        const XrPosef& p = vi.eyeToHead[e];
        m_VrLaunch.eyeToHead[e * 7 + 0] = (int)std::lround(p.position.x * 100000.0f);  // m → mm×100
        m_VrLaunch.eyeToHead[e * 7 + 1] = (int)std::lround(p.position.y * 100000.0f);
        m_VrLaunch.eyeToHead[e * 7 + 2] = (int)std::lround(p.position.z * 100000.0f);
        m_VrLaunch.eyeToHead[e * 7 + 3] = (int)std::lround(p.orientation.x * 10000.0f);
        m_VrLaunch.eyeToHead[e * 7 + 4] = (int)std::lround(p.orientation.y * 10000.0f);
        m_VrLaunch.eyeToHead[e * 7 + 5] = (int)std::lround(p.orientation.z * 10000.0f);
        m_VrLaunch.eyeToHead[e * 7 + 6] = (int)std::lround(p.orientation.w * 10000.0f);
    }
    const float dx = vi.eyeToHead[1].position.x - vi.eyeToHead[0].position.x;
    const float dy = vi.eyeToHead[1].position.y - vi.eyeToHead[0].position.y;
    const float dz = vi.eyeToHead[1].position.z - vi.eyeToHead[0].position.z;
    m_VrLaunch.ipd = (int)std::lround(std::sqrt(dx * dx + dy * dy + dz * dz) * 100000.0f);
    // Hz：夾在協定範圍（Monado null compositor 等測試 runtime 可能低於 60）
    int hz = (int)std::lround(vi.refreshHz);
    const int measuredHz = hz;
    hz = std::clamp(hz, (int)VIPLE_VR_HZ_MIN, (int)VIPLE_VR_HZ_MAX);
    m_VrLaunch.refreshHz = hz;
    m_VrLaunch.periodNs = (measuredHz == hz && vi.periodNs > 0) ? (qint64)vi.periodNs : 1000000000LL / hz;
    // §VR-HALF-RATE（2026-10-08）：遊戲與串流跑顯示更新率的一半（頭盔照樣全速顯示，沒有新畫面的那一格重送上一張，
    // 轉動由頭盔的重投影補）。算圖、編碼、傳輸、解碼的負擔都減半；補幀（合成中間那一格）建立在這個模式上。
    s_VrTrackingHz = hz;  // 追蹤樣本照顯示更新率送（半速串流時不跟著減半）
    if (m_Preferences->vrHalfRate && hz / 2 >= (int)VIPLE_VR_HZ_MIN) {
        m_VrLaunch.refreshHz = hz / 2;
        m_VrLaunch.periodNs *= 2;
        // vrDispHz：host 的姿態預測要知道頭盔實際的顯示更新率（vrHz 這時是串流的）。沒減半時維持 0＝不帶：
        // m_VrLaunch 每次 decideVrRequest() 都重新建構，不會留著上一次的值
        m_VrLaunch.displayHz = hz;
        m_XrContext->setPcvrStreamDiv(2);  // §VR-LATCH-WAIT：串流一幀佔兩個顯示格（沒有新影像的那一格不等）
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-SESSION] half-rate: display %d Hz, stream %d Hz", hz, hz / 2);
    }
    else {
        // §VR-RATE-AUTO：每一場從全速開始（上一場可能停在自動半速）
        m_XrContext->setPcvrStreamDiv(1);
        m_XrContext->resetPcvrRate();
        if (m_Preferences->vrRateAuto && !m_Preferences->vrHalfRate) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-RATE] automatic frame rate on: %d Hz, drops to %d Hz while the network is unstable", hz, hz / 2);
        }
    }
    if (!vi.timeConversion) {
        m_VrLaunch.caps &= ~VIPLE_VR_CLIENT_CAP_TRACK_THREAD;  // frameloop 模式
    }
    if (m_XrContext != nullptr && m_XrContext->eyeGazeSupported()) {
        m_VrLaunch.caps |= VIPLE_VR_CLIENT_CAP_GAZE | VIPLE_VR_CLIENT_CAP_GAZE_FOVEA;
    }
    m_VrLaunch.caps |= VIPLE_VR_CLIENT_CAP_FOVEA_INSET;  // §VR-FOVEA-INSET：懂分段注視點曲線
    // §VR-FOVEA-TILES：懂「每眼兩塊影像」。要兩個圖層＋直接讀 YUV 平面那一趟才畫得出來：dev 選項把那兩條路關掉時
    // 不宣告，host 就照舊用注視點曲線。2.0.128 起開著補幀也可以（swapchain 兩倍高，下半是合成的那一張、同樣的排法）
    if (m_XrPcvr && !m_Preferences->vrFovSingleLayer && !m_Preferences->vrUnwarpRgb) {
        m_VrLaunch.caps |= VIPLE_VR_CLIENT_CAP_FOVEA_TILES;
    }
    // §VR-PLAYAREA：真實遊玩區與坐姿零點交給 server 的 chaperone（拿不到就不帶，server 用預設值）
    float areaW = 0.0f, areaD = 0.0f;
    if (m_XrContext->playAreaBounds(&areaW, &areaD)) {
        m_VrLaunch.playAreaWidthMm = (int)std::lround(areaW * 1000.0f);
        m_VrLaunch.playAreaDepthMm = (int)std::lround(areaD * 1000.0f);
    }
    float seatPos[3] = {};
    float seatYaw = 0.0f;
    if (m_XrContext->seatedPose(seatPos, &seatYaw)) {
        m_VrLaunch.hasSeated = true;
        for (int i = 0; i < 3; i++) {
            m_VrLaunch.seatedPosMm[i] = (int)std::lround(seatPos[i] * 1000.0f);
        }
        m_VrLaunch.seatedYawMdeg = (int)std::lround(seatYaw * 57295.7795f);
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VR-SESSION] PCVR XR measured: space=%s tracking=%s hz=%.2f%s period=%lld ns recommended=%ux%u "
                "ipd=%.2f mm fov L[%.1f %.1f %.1f %.1f] R[%.1f %.1f %.1f %.1f] deg%s -> launch eye=%dx%d hz=%d",
                qUtf8Printable(vi.trackingSpace), vi.timeConversion ? "thread" : "frameloop",
                (double)vi.refreshHz, measuredHz != hz ? " (clamped)" : "", (long long)m_VrLaunch.periodNs,
                vi.recommendedWidth, vi.recommendedHeight, m_VrLaunch.ipd / 100.0,
                vi.fov[0].angleLeft * 57.29578, vi.fov[0].angleRight * 57.29578, vi.fov[0].angleUp * 57.29578,
                vi.fov[0].angleDown * 57.29578, vi.fov[1].angleLeft * 57.29578, vi.fov[1].angleRight * 57.29578,
                vi.fov[1].angleUp * 57.29578, vi.fov[1].angleDown * 57.29578,
                m_Preferences->vrOverscanDeg > 0.0 ? qUtf8Printable(QString(" (incl. dev overscan %1 deg per side)").arg(m_Preferences->vrOverscanDeg)) : "",
                m_VrLaunch.eyeWidth, m_VrLaunch.eyeHeight, m_VrLaunch.refreshHz);
    return true;
#else
    return false;
#endif
}

bool Session::startConnectionAsync()
{
    s_VrHapticSeenCb[0].store(0);  // §REVIEW-1111：新的一場，host 的 eventId 重新起算
    s_VrHapticSeenCb[1].store(0);
    // The UI should have ensured the old game was already quit
    // if we decide to stream a different game.
    Q_ASSERT(m_Computer->currentGameId == 0 ||
             m_Computer->currentGameId == m_App.id);

    setupXrDesktop();  // §VR M3a X1：/launch、relay 之前
    applyXrDesktopAutoFps();  // §H.4-AUTO：XR 桌面自動更新率＝runtime 實際值（/launch 之前）

    bool enableGameOptimizations;
    if (m_Computer->isNvidiaServerSoftware) {
        // GFE will set all settings to 720p60 if it doesn't recognize
        // the chosen resolution. Avoid that by disabling SOPS when it
        // is not streaming a supported resolution.
        enableGameOptimizations = false;
        QVector<NvDisplayMode> hostModesCopy;  // §REVIEW-1111（第二輪）：鎖內複製再走訪
        {
            QReadLocker computerLock(&m_Computer->lock);
            hostModesCopy = m_Computer->displayModes;
        }
        for (const NvDisplayMode &mode : std::as_const(hostModesCopy)) {
            if (mode.width == m_StreamConfig.width &&
                    mode.height == m_StreamConfig.height) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Found host supported resolution: %dx%d",
                            mode.width, mode.height);
                enableGameOptimizations = m_Preferences->gameOptimizations;
                break;
            }
        }
    }
    else {
        // Always send SOPS to Sunshine because we may repurpose the
        // option to control whether the display mode is adjusted
        enableGameOptimizations = m_Preferences->gameOptimizations;
    }

    QString rtspSessionUrl;
    bool useRelayForStream = false;
    RelayTcpTunnel *rtspTunnel = nullptr;
    // m_UdpTunnel (session member) — survives past startConnectionAsync
    // so the proxy sockets stay up for the whole streaming session.

    // Helper: launch via relay HTTP proxy. Sets rtspSessionUrl + useRelayForStream on success.
    // Returns true on success, false on failure (caller decides how to handle error).
    auto tryRelayLaunch = [&]() -> bool {
        if (m_Preferences->relayUrl.isEmpty() || m_Computer->uuid.isEmpty()) {
            return false;
        }

        // §VR：rc 以前 VR 視訊強制 RTP/UDP，server 對 relay 路徑回 VR_TRANSPORT_UNSUPPORTED；
        // 這裡直接不要求 VR，以平面串流（GA 的 QUIC VR 政策完成後再開放）
        if (m_VrRequested) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-SESSION] relay path — VR not requested (transport unsupported before GA), streaming flat");
            m_StreamConfig.vrFlags = 0;
        }

        int riKeyId;
        memcpy(&riKeyId, m_StreamConfig.remoteInputAesIv, sizeof(riKeyId));
        riKeyId = qFromBigEndian(riKeyId);

        QString verb = m_Computer->currentGameId != 0 ? "resume" : "launch";
        QString launchPath = QString("/%1?appid=%2&mode=%3x%4x%5&additionalStates=1&sops=%6"
            "&rikey=%7&rikeyid=%8&localAudioPlayMode=%9&surroundAudioInfo=%10"
            "&remoteControllersBitmap=%11&gcmap=%11&gcpersist=0&uniqueid=0123456789ABCDEF")
            .arg(verb)
            .arg(m_App.id)
            .arg(m_StreamConfig.width).arg(m_StreamConfig.height).arg(m_StreamConfig.fps)
            .arg(enableGameOptimizations ? 1 : 0)
            .arg(QByteArray(m_StreamConfig.remoteInputAesKey, sizeof(m_StreamConfig.remoteInputAesKey)).toHex())
            .arg(riKeyId)
            .arg(m_Preferences->playAudioOnHost ? 1 : 0)
            .arg(SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(m_StreamConfig.audioConfiguration))
            .arg(m_InputHandler->getAttachedGamepadMask())
            + "&corever=1"  // Required for encrypted RTSP (rtspenc://)
            + (m_Takeover ? "&takeover=1" : "");

        // Pre-cancel any app the previous session left running. The
        // normal quitApp() path goes through direct HTTPS which the
        // relay-mode client can't reach, so after a forced exit
        // (user pressed disconnect while the tunnel was congested)
        // Sunshine still has proc::proc.running() > 0 and /launch
        // responds 400 "An app is already running on this host" —
        // which Moonlight surfaces as "Failed to launch via relay
        // proxy". Calling /cancel first is a no-op on a clean host
        // (200 OK) and a cleanup on a dirty one. Short 5 s timeout:
        // /cancel only terminates local state, no encoder probe.
        QString cancelResp = RelayLookup::httpProxy(
            m_Preferences->relayUrl, m_Preferences->relayPsk,
            m_Computer->uuid, "/cancel", 5000);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-NAT] Pre-launch /cancel sent (resp_len=%d)",
                    cancelResp.length());

        QString launchResp = RelayLookup::httpProxy(
            m_Preferences->relayUrl, m_Preferences->relayPsk,
            m_Computer->uuid, launchPath, 45000); // /launch needs 30s+ (encoder probe)

        // /launch responds with <gamesession>... ; /resume responds with
        // <sessionUrl0>... <resume>1</resume>. Accept either.
        const bool okResponse = !launchResp.isEmpty() &&
            (launchResp.contains("gamesession") ||
             launchResp.contains("sessionUrl0"));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-NAT] Relay launch response: empty=%d len=%d accept=%d",
                    launchResp.isEmpty(), launchResp.length(),
                    okResponse ? 1 : 0);

        if (okResponse) {
            rtspSessionUrl = NvHTTP::getXmlString(launchResp, "sessionUrl0");
            useRelayForStream = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-NAT] Relay launch succeeded! RTSP URL: %s",
                        qPrintable(rtspSessionUrl));
            return true;
        }

        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-NAT] Relay launch failed. Response: %s",
                    qPrintable(launchResp.left(200)));
        return false;
    };

    // VipleStream: If the computer is only reachable via relay proxy (direct HTTPS
    // to its STUN/public IP is unreachable), skip the direct /launch attempt and
    // go straight to the relay HTTP proxy. The direct attempt would otherwise
    // hang for ~90s (Qt's default network timeout) before the catch-fallback ran.
    //
    // `forceRelayStream` is a testing knob that always takes the relay path even
    // when direct is reachable — used to exercise the UDP tunnel when a VPN like
    // Cloudflare WARP would otherwise make the direct /launch succeed.
    const bool wantRelayPath = m_Computer->onlineViaRelay ||
                               m_Preferences->forceRelayStream;
    if (wantRelayPath) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-NAT] Taking relay path (onlineViaRelay=%d forceRelayStream=%d)",
                    m_Computer->onlineViaRelay ? 1 : 0,
                    m_Preferences->forceRelayStream ? 1 : 0);
        if (!tryRelayLaunch()) {
            emit displayLaunchError(tr("Failed to launch via relay proxy"));
            return false;
        }
    } else {
    retryVrLaunch:
        try {
            NvHTTP http(m_Computer);
            QString launchResponse;
            // §FLAT-ML（2026-11-18）：平面場次、host 支援、這台裝置有網卡和 host 同網段時要求多連線。
            // 2026-11-20 審查：原本另外要求「host 位址是私有 IPv4（10/8、172.16/12、192.168/16）」，那是拿位址類別當遠近的代理，
            // 已拿掉。現在的條件都是結構上的事實或使用者的選擇，補包實際用不用由 common-c 量到的往返時間決定：
            //   - 位址是 IPv4：多連線的線上格式（LINK_HELLO 的 VIPLE_VR_LINK_DESC）只放得下 IPv4；
            //   - 這一場連的那個 host 位址和這台裝置的某張網卡同網段（activeOnLink）：hub 的連線要 host 看到的來源位址等於
            //     client 宣告的位址才會確認，中間有 NAT（另一層路由器、公網位址）時永遠不會確認，只會白白關掉 QUIC 與
            //     推測式丟幀回報。只看「同網段」這個從網卡表讀得到的事實，不看位址屬於哪一類；
            //   - 單一路徑而且這一場會走 MP-QUIC 時不要求（使用者選的傳輸）；
            //   - 經 VPN 的場次不要求（保留，理由見下面）。
            bool flatMlAsked = false;
            m_StreamConfig.flatMultilink = 0;
            if (!m_VrRequested && (m_Computer->vipleStreamVr & VIPLE_VR_SERVER_CAP_FLAT_ML)) {
                // §REVIEW-1119：activeAddress 的字串同樣會被輪詢執行緒整份換掉。先複製再解析。
                QString activeAddrCopy;
                {
                    QReadLocker computerLock(&m_Computer->lock);
                    activeAddrCopy = m_Computer->activeAddress.address();
                }
                bool isV4 = false;
                QHostAddress(activeAddrCopy).toIPv4Address(&isV4);
                flatMlAsked = isV4;
                // 第二條路：兩張以上的網卡和 host 的某個位址同網段（桌機的有線＋Wi-Fi），或者是有串流轉接器熱點的頭盔
                // （熱點在場次開始後才出現）。只有一條路時，會走 MP-QUIC 的場次維持原本的傳輸
                if (flatMlAsked) {
                    QVector<QHostAddress> hostAddrs;
                    QVector<NvNetworkInterface> advertisedIfaces;
                    {
                        // §REVIEW-1119：QVector 與 NvAddress 的字串會被輪詢執行緒整份換掉，鎖外走訪會用到已釋放的緩衝。
                        QReadLocker computerLock(&m_Computer->lock);
                        hostAddrs.append(QHostAddress(activeAddrCopy));
                        advertisedIfaces = m_Computer->serverAdvertisedInterfaces;
                    }
                    for (const auto& iface : std::as_const(advertisedIfaces)) {
                        const QHostAddress ha(iface.address);
                        if (!ha.isNull() && ha.protocol() == QAbstractSocket::IPv4Protocol) {
                            hostAddrs.append(ha);
                        }
                    }
                    int paired = 0;
                    bool activeOnLink = false;  // hostAddrs[0]（這一場連的位址）和某張網卡同網段
                    const auto netIfs = QNetworkInterface::allInterfaces();
                    for (const QNetworkInterface& nif : netIfs) {
                        if (!(nif.flags() & QNetworkInterface::IsUp) || !(nif.flags() & QNetworkInterface::IsRunning) ||
                                (nif.flags() & QNetworkInterface::IsLoopBack)) {
                            continue;
                        }
                        bool hit = false;
                        const auto entries = nif.addressEntries();
                        for (const QNetworkAddressEntry& e : entries) {
                            if (e.ip().protocol() != QAbstractSocket::IPv4Protocol || e.prefixLength() <= 0 || e.prefixLength() >= 32) {
                                continue;
                            }
                            activeOnLink = activeOnLink || hostAddrs.first().isInSubnet(e.ip(), e.prefixLength());
                            for (const QHostAddress& ha : std::as_const(hostAddrs)) {
                                if (ha.isInSubnet(e.ip(), e.prefixLength())) {
                                    hit = true;
                                    break;
                                }
                            }
                        }
                        paired += hit ? 1 : 0;
                    }
                    std::string hsSsid, hsKey;
                    const bool frameHotspot = readVrHotspotCreds(&hsSsid, &hsKey);
                    // §REVIEW-1119：std::fill 寫完沒人再讀，最佳化可以整段刪掉，熱點金鑰留在 SSO 或堆積。
                    wipeStdString(hsSsid);
                    wipeStdString(hsKey);
                    // §FLAT-REPAIR（2026-11-20）：只有一條路、而且這一場本來就不會走 MP-QUIC 時也要求——多連線的 hub 在只有
                    // 一條連線時照樣運作，丟包補傳（VIPLE_VR_LINK_F_REPAIR_FLAT）掛在它上面，單一 Wi-Fi 正是最需要補傳的情況。
                    // 會走 MP-QUIC 的場次維持原樣（多連線會把 QUIC 關掉，那是使用者選的傳輸）。host 不支援補傳時（2.0.249 以前）
                    // 這一場只是改由 hub 的單一連線送，影像內容與封包格式相同。
                    // 2026-11-20 審查：和沒有 hub 的場次有一處不同——common-c 在多連線的場次不做推測式丟幀回報，補包沒在用時
                    // （舊 host、往返時間超過門檻）RFI／IDR 最多晚一個幀週期。那要在 common-c 的 VideoStream.c 修（只在真的有
                    // 東西可等時才關掉推測式回報），這裡能做的只有不在連線建不起來的情況要求 hub（下面的 !activeOnLink）。
                    const bool singlePath = paired < 2 && !frameHotspot;
                    const bool quicWanted = m_Preferences->enableMpQuic && m_Computer->isMpQuicCapable;
                    if (!activeOnLink) {
                        // 這一場連的位址不在任何一張網卡的網段裡：中間隔著路由器，有 NAT 時 hub 的連線永遠不會確認（host 退回
                        // 原本的單一路徑送），要求了只會少掉 QUIC 與推測式丟幀回報。分不出有沒有 NAT，一律不要求（原本靠
                        // 「私有位址」擋掉公網位址，雙層 NAT 的私有位址擋不住）。不用 paired 判斷：host 廣告的區網位址可能
                        // 剛好和人在外面時的網段撞號（192.168.1.x 之類）
                        flatMlAsked = false;
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "[VIPLE-VR-LINK] flat (2D) session: the host address in use is not on a subnet of any of this "
                                    "device's network interfaces - multi-link not requested");
                    }
                    else if (singlePath && quicWanted) {
                        flatMlAsked = false;
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "[VIPLE-VR-LINK] flat (2D) session: only %d network interface(s) can reach the host and MP-QUIC is in use - multi-link not requested",
                                    paired);
                    }
                    // 經 VPN 連回來（VPN 的介面剛好和 host 同網段，例如自架 WireGuard 的 10.x）不要求：那種場次只有一條連線，
                    // 多連線沒有好處、還會讓它用不了 QUIC。頭盔有熱點憑證時上面的網卡檢查擋不住這種情況。
                    // 2026-11-20 審查：這一條仍然是用「是不是 VPN」分流，刻意保留——拿掉會讓經 VPN 的場次改走 hub（開著 MP-QUIC
                    // 的還會失去 QUIC），而 hub 經 VPN 的行為沒有驗過（連線配對只認同網段的網卡、host 的補包規則 repair_rtt／
                    // repair_far 在平面場次是關的）。要讓 VPN 場次也用量到的往返時間決定補包，得連同 host 與 common-c 另做一輪。
                    // m_ViaVpn 在使用者自訂封包大小時不會被設定，所以這裡自己問一次（會做一次 TCP 連線，host 在線上時很快）
                    else if (m_ViaVpn || m_Computer->getActiveAddressReachability() == NvComputer::RI_VPN) {
                        flatMlAsked = false;
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "[VIPLE-VR-LINK] flat (2D) session: the host is reached through a VPN - multi-link not requested");
                    }
                    else if (singlePath) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "[VIPLE-FLAT-REPAIR] flat (2D) session: %d network interface(s) on a subnet of the host - multi-link "
                                    "requested with a single link so lost packets can be resent (used only if the host offers it "
                                    "and the measured round trip is short enough)",
                                    paired);
                    }
                }
            }
            // [VIPLE-STARTUP]：/launch（或 /resume）送出與回來的時刻；VR 的回應要等 host 的編排器，這一段就是 host 那邊花的
            const char* const launchVerb = m_Computer->currentGameId != 0 ? "resume" : "launch";
            const long long launchT0Ms = startupNowMs();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-STARTUP] t=%lld %s request", launchT0Ms, launchVerb);
            http.startApp(launchVerb,
                          m_Computer->isNvidiaServerSoftware,
                          m_App.id, &m_StreamConfig,
                          enableGameOptimizations,
                          m_Preferences->playAudioOnHost,
                          m_InputHandler->getAttachedGamepadMask(),
                          !m_Preferences->multiController,
                          rtspSessionUrl,
                          m_Takeover,
                          m_VrRequested ? m_VrLaunch.toQuery() : (flatMlAsked ? QStringLiteral("&flatMl=1") : QString()),
                          &launchResponse);
            // §FLAT-ML：送了 flatMl=1 而且 server 回了 <VipleStreamFlatMl>1 才啟用（舊 server 不會回，維持現狀）
            if (flatMlAsked && NvHTTP::getXmlString(launchResponse, "VipleStreamFlatMl") == QLatin1String("1")) {
                m_StreamConfig.flatMultilink = 1;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VR-LINK] flat (2D) session: the server accepted multi-link - video and audio go over every "
                            "usable network interface (MP-QUIC stays off for this session)");
            }
            {
                const long long nowMs = startupNowMs();
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-STARTUP] t=%lld %s reply took=%lld ms",
                            nowMs, launchVerb, nowMs - launchT0Ms);
            }

            // §VR：不變式 1——client 送了 vr=1 **而且** server 回了
            // <VipleStreamVRSession> 才啟用；不看 IS_SUNSHINE／isVipleStreamPeer
            if (m_VrRequested) {
                m_VrSession = VrSessionInfo::parse(NvHTTP::getXmlString(launchResponse, "VipleStreamVRSession"));
                if (m_VrSession.valid) {
                    m_StreamConfig.vrFlags = VIPLE_VR_SF_ENABLED |
                                             (m_VrSession.recoveryIntra ? VIPLE_VR_SF_RECOVERY_INTRA : 0) |
                                             // §VR-MULTILINK：server 回了 multilink=1 才啟用（selftest 是 dev 選項）
                                             (m_VrSession.multilink ? VIPLE_VR_SF_MULTILINK : 0) |
                                             (m_VrSession.multilink && m_Preferences->vrLinkSelftest ? VIPLE_VR_SF_MULTILINK_SELFTEST : 0) |
                                             // §VR-REPAIR-RTT：client 這一邊只是 offer，用不用由 host 的 vr_multilink_repair_rtt 決定
                                             (m_VrSession.multilink && m_Preferences->effectiveRepairRtt() ? VIPLE_VR_SF_REPAIR_RTT : 0);
                    m_StreamConfig.vrRepairHoldMs = m_Preferences->effectiveRepairHoldMs();
                    m_StreamConfig.vrRepairAbSec = m_Preferences->cliRepairAbSec;
                    m_StreamConfig.vrRepairIdle = m_Preferences->cliRepairIdle;
                    m_StreamConfig.vrRepairNoAhead = m_Preferences->cliRepairNoAhead ? 1 : 0;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-VR-SESSION] negotiated: %s",
                                qPrintable(m_VrSession.describe()));
                    if (m_VrSession.multilink) {
                        static const char* const k_IdleNames[] = {"auto", "fixed", "frame", "learned", "size"};  // VRRR_IDLE_* 的順序
                        const int idle = m_StreamConfig.vrRepairIdle;
                        const QString hold = m_StreamConfig.vrRepairHoldMs > 0 ? QString("%1 ms").arg(m_StreamConfig.vrRepairHoldMs)
                                                                               : QStringLiteral("auto");
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "[VIPLE-VR-SESSION] repair options: rtt=%s hold=%s ab=%d s idle=%s ahead=%s",
                                    (m_StreamConfig.vrFlags & VIPLE_VR_SF_REPAIR_RTT) ? "offer" : "off", qPrintable(hold),
                                    m_StreamConfig.vrRepairAbSec,
                                    (idle >= 0 && idle < (int)SDL_arraysize(k_IdleNames)) ? k_IdleNames[idle] : "?",
                                    m_StreamConfig.vrRepairNoAhead ? "off" : "on");
                    }
#ifdef HAVE_OPENXR
                    if (m_XrPcvr && m_XrContext != nullptr) {
                        m_XrContext->setPcvrFoveation(m_VrSession.fovea);  // §VR-FOVEA：server 確認了才還原
                        m_XrContext->setPcvrFoveaInset(m_VrSession.foveaInsetU0);  // §VR-FOVEA-INSET
                        m_XrContext->setPcvrGazeFovea(m_VrSession.gazeFovea);
                        // §VR-FOVEA-TILES：每眼兩塊影像（tileW＝0＝沒有分塊）
                        m_XrContext->setPcvrFoveaTiles(m_VrSession.tileW, m_VrSession.tileH, m_VrSession.tileRenderW, m_VrSession.tileRenderH);
                    }
#endif
                    if (m_VrSession.tileW > 0) {
                        // §VR-FOVEA-TILES：編碼畫面是 (2·每塊寬)×(2·每塊高)，不是「兩眼並排」的尺寸。之後建解碼器、RTSP 都照它
                        // （host 也會把 RTSP 裡的尺寸改成這個）
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "[VIPLE-VR-FOVEA] tiles: %dx%d per tile, stream %dx%d (was %dx%d), gaze tile = native crop of the %dx%d render",
                                    m_VrSession.tileW, m_VrSession.tileH, m_VrSession.packedWidth, m_VrSession.packedHeight,
                                    m_StreamConfig.width, m_StreamConfig.height, m_VrSession.tileRenderW, m_VrSession.tileRenderH);
                        m_StreamConfig.width = m_VrSession.packedWidth;
                        m_StreamConfig.height = m_VrSession.packedHeight;
                    }
                    if (m_VrSession.packedWidth != m_StreamConfig.width ||
                            m_VrSession.packedHeight != m_StreamConfig.height ||
                            m_VrSession.refreshHz != m_StreamConfig.fps) {
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                    "[VIPLE-VR-SESSION] server packed %dx%d@%d differs from stream %dx%d@%d",
                                    m_VrSession.packedWidth, m_VrSession.packedHeight, m_VrSession.refreshHz,
                                    m_StreamConfig.width, m_StreamConfig.height, m_StreamConfig.fps);
                    }
                }
                else {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-VR-SESSION] server did not confirm a VR session — streaming flat");
                    m_StreamConfig.vrFlags = 0;
                    if (m_XrPcvr) {
                        destroyXrContext();  // M4a R1：沒有 VR session 就沒有 render pose，XR 無從投影
                        m_XrPcvr = false;
                    }
                    // §VR-LAUNCHER：無頭時不能以平面串流（看不到）
                    if (pcvrCannotFallBackToFlat()) {
                        emit displayLaunchError(tr("%1 did not start a VR session. Make sure PCVR is enabled on the host, then try again.")
                                                    .arg(m_Computer->name));
                        return false;
                    }
                }
            }
        } catch (const QtNetworkReplyException&) {
            // §VR-LAUNCHER：PCVR 不改走 relay（relay 只能平面串流）。
            // §REVIEW-1111：原本只有無頭時才擋。有桌面的機器會繼續走 relay，但這時串流尺寸已經是 VR 的左右並排、
            // XR session 也建好了——relay 那邊不帶 VR 參數，結果是 host 送平面畫面、解析度卻是 VR 打包尺寸，
            // client 又把它交給等不到姿態資料的 XR 後端。要求的是 VR 就直接回報連線失敗。
            if (m_VrRequested) {
                emit displayLaunchError(tr("Connection to host failed"));
                return false;
            }
            // Direct launch failed — try via relay proxy as fallback.
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-NAT] Direct launch failed, trying relay proxy...");
            if (!tryRelayLaunch()) {
                emit displayLaunchError(tr("Connection to host failed"));
                return false;
            }
        } catch (const GfeHttpResponseException& e) {
            // 2026-10-30：host 上 Steam 自己的串流（vrlink）的 SteamVR 還開著時 /launch 回 503 VRLINK_ACTIVE。
            // 實務上那就是同一個使用者剛從 Steam 內建串流切回來（使用者回報切回來開遊戲 503）→ 自動帶 vrForce=1 重試一次，
            // 讓 host 重啟 SteamVR 改用我們的 driver
            if (m_VrRequested && !m_VrLaunch.force && e.toQString().contains(QStringLiteral(VIPLE_VR_ERR_VRLINK_ACTIVE))) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-SESSION] host busy with Steam's own VR streaming; retrying /launch with vrForce=1");
                m_VrLaunch.force = true;
                goto retryVrLaunch;
            }
            emit displayLaunchError(tr("Host returned error: %1").arg(e.toQString()));
            return false;
        }
    }

    // VipleStream: If using relay, set up RTSP TCP tunnel + UDP tunnel
    QByteArray hostnameStr;
    if (useRelayForStream) {
        // Bring up the UDP tunnel FIRST so its OS-assigned ephemeral
        // loopback ports are known before the RTSP tunnel starts —
        // the RTSP TCP tunnel needs that mapping to rewrite the
        // `server_port=` entries in Sunshine's SETUP responses so
        // moonlight-common-c targets our proxies (127.0.0.1:<eph>)
        // instead of the literal Sunshine ports (which Windows
        // Hyper-V / winnat often has reserved out from under us).
        QVector<uint16_t> tunneledPorts{47998, 47999, 48000};
        m_UdpTunnel = new RelayUdpTunnel(m_Preferences->relayUrl,
                                         m_Preferences->relayPsk,
                                         m_Computer->uuid,
                                         tunneledPorts);
        QVector<QPair<uint16_t, uint16_t>> portMap;
        if (m_UdpTunnel->startAndWaitReady(7000)) {
            portMap = m_UdpTunnel->portMap();
            hostnameStr = QByteArrayLiteral("127.0.0.1");
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-NAT] UDP tunnel ready — video/audio routed via relay (host=127.0.0.1)");
            // Bandwidth cap for the tunnel path. The WS carrier goes
            // TLS-over-TCP (often through Cloudflare Tunnel) which
            // can't sustain high-bitrate video. At 2 Mbps server-side
            // drops were ~0 but mouse-move bursts still exceeded the
            // sustained pipe rate briefly, growing the TCP queue and
            // visibly freezing the picture. 1.5 Mbps leaves enough
            // headroom for motion-induced bitrate bursts to stay
            // within the pipe's sustainable rate.
            constexpr int TUNNEL_BITRATE_CAP_KBPS = 1500;
            if (m_StreamConfig.bitrate > TUNNEL_BITRATE_CAP_KBPS) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-NAT] capping bitrate %d -> %d kbps for WS tunnel",
                            m_StreamConfig.bitrate, TUNNEL_BITRATE_CAP_KBPS);
            }
            // §RES-BITRATE：上限記成成員，後面 4:4:4 回退重算時才不會把它蓋掉
            m_BitrateCapKbps = TUNNEL_BITRATE_CAP_KBPS;
            refreshSessionBitrate("WS tunnel");
            // Tell moonlight-common-c to target our ephemeral loopback
            // ports instead of the parsed Sunshine ports. The RTSP
            // traffic is end-to-end encrypted (corever=1), so rewriting
            // `server_port=` inside the TCP tunnel doesn't work — this
            // override runs AFTER common-c decrypts and parses.
            uint16_t vPort = 0, aPort = 0, cPort = 0;
            for (const auto &p : portMap) {
                if (p.first == 47998) vPort = p.second;
                else if (p.first == 47999) cPort = p.second;
                else if (p.first == 48000) aPort = p.second;
            }
            LiOverrideUdpPorts(vPort, aPort, cPort);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-NAT] UDP port overrides: video=%u audio=%u control=%u",
                        vPort, aPort, cPort);
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-NAT] UDP tunnel unavailable, falling back to direct UDP");
            delete m_UdpTunnel;
            m_UdpTunnel = nullptr;
            // §REVIEW-1119：stunAddress 是 QString，輪詢執行緒會在寫鎖裡換掉。先複製再拿去當位址。
            QString stunFallback;
            {
                QReadLocker computerLock(&m_Computer->lock);
                stunFallback = m_Computer->stunAddress.address();
            }
            if (!stunFallback.isEmpty()) {
                hostnameStr = stunFallback.toUtf8();
            } else {
                hostnameStr = QByteArrayLiteral("127.0.0.1");
            }
        }

        // Now start the RTSP TCP tunnel, with a rewriter wired up so
        // every chunk coming back from Sunshine has the server UDP
        // port strings substituted to our ephemeral local ones.
        uint16_t rtspPort = 48010; // default RTSP port
        rtspTunnel = new RelayTcpTunnel(m_Preferences->relayUrl, m_Preferences->relayPsk,
                                         m_Computer->uuid, rtspPort);
        if (!portMap.isEmpty()) {
            auto mapCopy = portMap;
            rtspTunnel->setRelayToLocalRewriter([mapCopy](const QByteArray &in) -> QByteArray {
                QByteArray out = in;
                for (const auto &p : mapCopy) {
                    if (p.first == p.second) continue;
                    QByteArray needle = "server_port=" + QByteArray::number(p.first);
                    QByteArray repl   = "server_port=" + QByteArray::number(p.second);
                    int idx = 0;
                    while ((idx = out.indexOf(needle, idx)) >= 0) {
                        // Only replace if the char right after the
                        // digits isn't another digit (avoid matching
                        // 47998 inside 479980).
                        int after = idx + needle.size();
                        char c = (after < out.size()) ? out[after] : 0;
                        if (c >= '0' && c <= '9') {
                            idx = after;
                            continue;
                        }
                        out.replace(idx, needle.size(), repl);
                        idx += repl.size();
                    }
                }
                return out;
            });
        }
        uint16_t localRtspPort = rtspTunnel->startAndGetPort();
        rtspSessionUrl = QString("rtspenc://127.0.0.1:%1").arg(localRtspPort);

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-NAT] RTSP tunneled via localhost:%d, UDP to %s",
                    localRtspPort, hostnameStr.data());
    } else {
        hostnameStr = m_Computer->activeAddress.address().toUtf8();
    }
    QByteArray siAppVersion = m_Computer->appVersion.toUtf8();

    SERVER_INFORMATION hostInfo;
    // §REVIEW-1119：沒有 GfeVersion 或 sessionUrl0 時那兩個指標原本是堆疊上的垃圾。
    // LiStartConnection 會對 rtspSessionUrl 做 strrchr；不是 NULL 就會解到野指標。
    LiInitializeServerInformation(&hostInfo);
    hostInfo.address = hostnameStr.data();
    hostInfo.serverInfoAppVersion = siAppVersion.data();
    hostInfo.serverCodecModeSupport = m_Computer->serverCodecModeSupport;

    // Older GFE versions didn't have this field
    QByteArray siGfeVersion;
    if (!m_Computer->gfeVersion.isEmpty()) {
        siGfeVersion = m_Computer->gfeVersion.toUtf8();
    }
    if (!siGfeVersion.isEmpty()) {
        hostInfo.serverInfoGfeVersion = siGfeVersion.data();
    }

    // Older GFE and Sunshine versions didn't have this field
    QByteArray rtspSessionUrlStr;
    if (!rtspSessionUrl.isEmpty()) {
        rtspSessionUrlStr = rtspSessionUrl.toUtf8();
        hostInfo.rtspSessionUrl = rtspSessionUrlStr.data();
    }

    if (m_Preferences->packetSize != 0) {
        // Override default packet size and remote streaming detection
        // NB: Using STREAM_CFG_AUTO will cap our packet size at 1024 for remote hosts.
        m_StreamConfig.streamingRemotely = STREAM_CFG_LOCAL;
        m_StreamConfig.packetSize = m_Preferences->packetSize;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Using custom packet size: %d bytes",
                    m_Preferences->packetSize);
    }
    else {
        // Use 1392 byte video packets by default
        m_StreamConfig.packetSize = 1392;

        // getActiveAddressReachability() does network I/O, so we only attempt to check
        // reachability if we've already contacted the PC successfully.
        int reachMtu = 0;          // §PERF-1113 A22：出口介面的 MTU（0＝不知道）
        QHostAddress reachPeer;    // 實際連上的對端位址
        switch (m_Computer->getActiveAddressReachability(&reachMtu, &reachPeer)) {
        case NvComputer::RI_LAN:
            // This address is on-link, so treat it as a local address
            // even if it's not in RFC 1918 space or it's an IPv6 address.
            m_StreamConfig.streamingRemotely = STREAM_CFG_LOCAL;
            break;
        case NvComputer::RI_VPN:
            // It looks like our route to this PC is over a VPN, so cap at 1024 bytes.
            // Treat it as remote even if the target address is in RFC 1918 address space.
            m_StreamConfig.streamingRemotely = STREAM_CFG_REMOTE;
            m_ViaVpn = true;  // §FRAME-HOTSPOT-PAUSE
            m_StreamConfig.packetSize = 1024;
            // §PERF-1113 A22：1024 是「不知道這條 VPN 的 MTU」時的保守值。Tailscale 兩端的 MTU 固定 1280，影像封包可以用
            // 1184（common-c 對遠端 IPv6 用的就是這個值：IPv4 封包 1228 B、IPv6 1248 B；開影像加密時 common-c 會從
            // packetSize 扣掉加密前綴，封包大小不變），同碼率下每秒少約一成的封包。下面的條件缺一個就維持 1024：
            //   - 對端是 Tailscale 位址（100.64.0.0/10 或 fd7a:115c:a1e0::/48）
            //   - 出口介面回報的 MTU ≥ 1280（0＝不知道；使用者把 Tailscale 的 MTU 調小時也會擋在這裡）
            //   - 影像不經 relay 的通道、也不走 QUIC（MP-QUIC 的 datagram 上限另外算，這裡不動）
            {
                bool peerIsV4 = false;
                const quint32 peerV4 = reachPeer.toIPv4Address(&peerIsV4);
                const bool tailscalePeer = peerIsV4 ? ((peerV4 & 0xFFC00000u) == 0x64400000u)
                                                    : reachPeer.isInSubnet(QHostAddress(QStringLiteral("fd7a:115c:a1e0::")), 48);
                bool videoOverQuic = false;
#ifdef VIPLE_MPQUIC
                // 和下面「Wire MP-QUIC preferences」的條件相同
                videoOverQuic = m_Preferences->enableMpQuic && m_Computer->isMpQuicCapable && !m_VrRequested;
#endif
                const char* reason;
                if (!tailscalePeer) {
                    reason = "VPN with unknown MTU";
                }
                else if (reachMtu == 0) {
                    reason = "Tailscale peer, interface MTU unknown";
                }
                else if (reachMtu < 1280) {
                    reason = "Tailscale peer, interface MTU below 1280";
                }
                else if (useRelayForStream) {
                    reason = "Tailscale peer, video through the relay tunnel";
                }
                else if (videoOverQuic) {
                    reason = "Tailscale peer, video over QUIC";
                }
                else {
                    m_StreamConfig.packetSize = 1184;
                    reason = "Tailscale peer, interface MTU at least 1280";
                }
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-PKTSIZE] video packet size %d over VPN (%s; peer %s, interface MTU %d)",
                            m_StreamConfig.packetSize, reason, qPrintable(reachPeer.toString()), reachMtu);
            }
            // §P4-TUNNEL 2026-08-27 — 目標是同網段私網位址、卻被判定要走
            // VPN/通道，代表「看起來的 LAN 串流」其實整條繞出去再繞回來。
            // 這個坑非常安靜：使用者以為在區網直連，實測卻是延遲 25-36ms
            // （真 LAN 應 1-4ms）、MTU 1280 → packetSize 砍半 → 每幀切更多
            // shard、叢發丟包更容易吃掉整幀，嚴重時上行佇列積壓造成
            // bufferbloat（實測控制通道 RTT 衝到 4194ms 而丟包僅 0.12%）。
            // 更糟的是下面 hole punch 那行會印「direct private address」，
            // 反而讓人以為是直連。這裡明講，省下重複的除錯輪迴。
            {
                QHostAddress tgt(m_Computer->activeAddress.address());
                if (tgt.isInSubnet(QHostAddress("10.0.0.0"), 8) ||
                    tgt.isInSubnet(QHostAddress("172.16.0.0"), 12) ||
                    tgt.isInSubnet(QHostAddress("192.168.0.0"), 16)) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-NAT] §P4-TUNNEL 目標 %s 是私網位址，"
                                "但路由判定要走 VPN/通道（packetSize 降為 1024）。"
                                "區網串流繞經通道會同時放大延遲、抖動與丟包 —— "
                                "若兩端在同一區網，請在 VPN 用戶端（如 Cloudflare "
                                "WARP / Tailscale）把該網段排除（split tunnel），"
                                "或串流時暫停 VPN。",
                                qPrintable(m_Computer->activeAddress.address()));
                }
            }
            break;
        default:
            // If we don't have reachability info, let moonlight-common-c decide.
            m_StreamConfig.streamingRemotely = STREAM_CFG_AUTO;
            break;
        }
    }

    // If the user has chosen YUV444 without adjusting the bitrate but the host doesn't
    // support YUV444 streaming, use the default non-444 bitrate for the stream instead.
    // This should provide equivalent image quality for YUV420 as the stream would have
    // had if the host supported YUV444 (though obviously with 4:2:0 subsampling).
    // If the user has adjusted the bitrate from default, we'll assume they really wanted
    // that value and not second guess them.
    // §RES-BITRATE：「使用者有沒有調過」改由開場判定的 m_BitrateFollowsDefault 決定（原本用值相等，寬高被 host 壓過
    // 就永遠對不上）。這裡只把 4:4:4 拿掉再推導一次；fps 用畫面 fps（m_OriginalFps），補幀看 m_BitrateFruc（4:4:4 全幀那一支維持 false）。
    // 一定要經過 refreshSessionBitrate：WS 隧道的上限在這之前就設了，直接重算會把它蓋掉。
    if (m_BitrateYuv444 && !(m_StreamConfig.supportedVideoFormats & VIDEO_FORMAT_MASK_YUV444)) {
        m_BitrateYuv444 = false;
        refreshSessionBitrate("no 4:4:4 this session");
    }

    // VipleStream: If no STUN endpoint cached, try relay lookup.
    // §REVIEW-1119：stun* 與 uuid 是 QString，輪詢執行緒在寫鎖裡換掉。lookup 最多 5 秒，不能持鎖。
    // 已存的位址不在這裡清掉（輪詢與設定還在用）。不能當打洞目標的只拿掉這次的複本。
    QString stunIp;
    QString stunNat;
    uint16_t stunPunch = 0;
    uint16_t stunHttpPort = 0;
    QString computerUuid;
    {
        QReadLocker computerLock(&m_Computer->lock);
        stunIp = m_Computer->stunAddress.address();
        stunHttpPort = m_Computer->stunAddress.port();
        stunNat = m_Computer->stunNatType;
        stunPunch = m_Computer->stunPunchPort;
        computerUuid = m_Computer->uuid;
    }
    if (stunIp.isEmpty()) {
        QString relayUrl = m_Preferences->relayUrl;
        QString relayPsk = m_Preferences->relayPsk;
        if (!relayUrl.isEmpty() && !computerUuid.isEmpty()) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-NAT] No cached STUN endpoint, querying relay: %s",
                        qPrintable(relayUrl));

            auto result = RelayLookup::lookup(relayUrl, relayPsk, computerUuid, 5000);
            // §REVIEW-1119：不是公網單播 IPv4 就不寫進去，也不拿去打洞。原文不印，避免換行灌進 log。
            if (result.success && publicUnicastStunIpv4(result.stunIp)) {
                uint16_t httpPort;
                {
                    QReadLocker computerLock(&m_Computer->lock);
                    httpPort = m_Computer->activeAddress.port();
                }
                if (httpPort == 0) {
                    httpPort = 47989;
                }
                {
                    QWriteLocker computerLock(&m_Computer->lock);
                    if (m_Computer->stunAddress.address().isEmpty()) {
                        m_Computer->stunAddress = NvAddress(result.stunIp, httpPort);
                        m_Computer->stunNatType = result.natType;
                        m_Computer->stunPunchPort = result.stunPort;  // §REVIEW-1104：打洞用 relay 回報的實際埠
                    }
                    stunIp = m_Computer->stunAddress.address();
                    stunHttpPort = m_Computer->stunAddress.port();
                    stunNat = m_Computer->stunNatType;
                    stunPunch = m_Computer->stunPunchPort;
                }
                // §REVIEW-1119：lookup 的幾秒內輪詢可能已經寫進別的位址。複本再驗一次才拿去打洞，也不把原文印進 log。
                if (!publicUnicastStunIpv4(stunIp)) {
                    stunIp.clear();
                    LocalControlPort = 0;
                }
                else {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-NAT] Relay provided endpoint: %s:%d",
                                qPrintable(stunIp), (int)stunHttpPort);
                }
            }
            else if (result.success) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-NAT] Ignoring relay STUN endpoint that is not a public IPv4 address");
            }
        }
    }
    else if (!publicUnicastStunIpv4(stunIp)) {
        stunIp.clear();
        LocalControlPort = 0;
    }

    // VipleStream: Attempt hole punch if we have a STUN endpoint for the server.
    // This is best-effort — failure is not fatal (ENet will try connecting anyway).
    if (!stunIp.isEmpty()) {
        // 連線目標若是私網（RFC1918）、loopback（LAN 直連、relay tunnel）
        // 或 CGNAT 100.64/10（Tailscale），NAT 打洞沒有意義——對 server 的
        // WAN IP 打洞必定 3 秒逾時（-4 no ACK），每場串流啟動固定多等 3 秒，
        // 直接跳過。目標不是 IP（hostname）時 QHostAddress 解析失敗、判斷
        // 都為 false，維持原行為。
        QHostAddress connectTarget(QString::fromUtf8(hostnameStr));
        if (connectTarget.isLoopback() || connectTarget.isPrivateUse() ||
            connectTarget.isInSubnet(QHostAddress("100.64.0.0"), 10)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-NAT] Skipping hole punch: direct private address %s",
                        hostnameStr.constData());
            // 上一場若打過洞，HolePunch.c 的 LocalControlPort 會殘留舊 port；
            // 不歸零的話 ENet client socket 會綁死上一場的 port 而非 wildcard，
            // 該 port 被占用時 LAN 連線直接失敗。
            LocalControlPort = 0;
        }
        else {
            QByteArray serverUuid = computerUuid.toUtf8().left(16);
            QByteArray clientUuid = QByteArray(16, 0);  // placeholder
            serverUuid.resize(16, 0);

            // stunAddress stores the HTTP port (for API fallback). The hole-punch
            // intercept on Sunshine lives on the ENet control port = HTTP port + 10
            // (see Sunshine/src/stream.h CONTROL_PORT = 10). Previously this sent
            // SYNs to the HTTP port where they were silently dropped.
            // §REVIEW-1104（使用者決策 B）：有 STUN 回報的實際埠就打它（NAT 換埠時 +10 會打錯）；舊版 server 沒給才退回 +10
            // §REVIEW-1119：HTTP 埠大於 65525 時 +10 會繞回小埠。那種就不打。
            const int portSum = stunPunch != 0 ? (int)stunPunch : (int)stunHttpPort + 10;
            if (portSum <= 0 || portSum > 65535) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-NAT] Skipping hole punch: control port %d is outside 1..65535",
                            portSum);
                LocalControlPort = 0;
            }
            else {
            const uint16_t controlPort = static_cast<uint16_t>(portSum);

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-NAT] Attempting hole punch to %s:%d (NAT: %s)",
                        qPrintable(stunIp),
                        controlPort,
                        qPrintable(stunNat));

            int punchResult = LiHolePunch(
                qPrintable(stunIp),
                controlPort,
                (const uint8_t *)serverUuid.constData(),
                (const uint8_t *)clientUuid.constData(),
                3000);

            if (punchResult == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-NAT] Hole punch succeeded!");
            } else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-NAT] Hole punch failed (%d), proceeding with direct connection", punchResult);
            }

            // VipleStream diag: dump the local port LiHolePunch used — ENet control
            // socket should bind to the same value so the NAT pinhole carries over.
            // (LocalControlPort is declared in HolePunch.h, defined in HolePunch.c)
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-NAT] LocalControlPort = %u (ENet client will bind to this)",
                        LocalControlPort);
            }
        }
    }

#ifdef VIPLE_MPQUIC
    // VipleStream 2.0 §VR-LAUNCHER：PCVR 的影像強制走 RTP，VR 的實測也都在沒有 MP-QUIC 下做的——這個 session
    // 不開 QUIC，偏好設定不動（GUI 另開的 VR 子行程不帶 --no-quic，免得 session 開始時 save() 把桌面模式的
    // MP-QUIC 設定也關掉）
    if (m_VrRequested && m_Preferences->enableMpQuic && m_Computer->isMpQuicCapable) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-SESSION] PCVR: MP-QUIC off for this session (preference unchanged)");
    }
    // §VR-MULTILINK：多連線要知道 server 的每個位址，才能替 client 的每張網卡找到同子網路的那一個。
    // 沿用 MP-QUIC 的 alt peer 清單欄位（QUIC 本身在 VR session 維持關閉）。
    if ((m_VrRequested && (m_StreamConfig.vrFlags & VIPLE_VR_SF_MULTILINK)) || m_StreamConfig.flatMultilink) {
        QString activeAddr;
        QVector<NvNetworkInterface> advertisedIfaces;
        NvAddress localAddress;
        NvAddress manualAddress;
        {
            // §REVIEW-1119：同上面平面多連線，鎖外走訪 serverAdvertisedInterfaces 會用到已釋放的緩衝。
            QReadLocker computerLock(&m_Computer->lock);
            activeAddr = m_Computer->activeAddress.address();
            advertisedIfaces = m_Computer->serverAdvertisedInterfaces;
            localAddress = m_Computer->localAddress;
            manualAddress = m_Computer->manualAddress;
        }
        m_StreamConfig.quicAltPeerCount = 0;
        auto addPeer = [&](const QString& ip) {
            if (ip.isEmpty() || !usableQuicAltPeer(ip) || ip == activeAddr ||
                    m_StreamConfig.quicAltPeerCount >= QUIC_MAX_ALT_PEERS) {
                return;
            }
            for (int i = 0; i < m_StreamConfig.quicAltPeerCount; i++) {
                if (ip == QString::fromUtf8(m_StreamConfig.quicAltPeers[i])) {
                    return;
                }
            }
            const QByteArray utf8 = ip.toUtf8();
            if (utf8.size() >= QUIC_ALT_PEER_LEN) {
                return;
            }
            memcpy(m_StreamConfig.quicAltPeers[m_StreamConfig.quicAltPeerCount], utf8.constData(), utf8.size() + 1);
            m_StreamConfig.quicAltPeerCount++;
        };
        for (const auto& iface : std::as_const(advertisedIfaces)) {
            addPeer(iface.address);
        }
        if (!localAddress.isNull()) {
            addPeer(localAddress.address());
        }
        if (!manualAddress.isNull()) {
            addPeer(manualAddress.address());
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-LINK] multi-link negotiated: %d other server address(es) known (advertised=%d)%s",
                    m_StreamConfig.quicAltPeerCount, (int)advertisedIfaces.size(),
                    (m_StreamConfig.vrFlags & VIPLE_VR_SF_MULTILINK_SELFTEST) ? " [selftest]" : "");
    }
    // Wire MP-QUIC preferences into stream config for moonlight-common-c
    if (m_Preferences->enableMpQuic && m_Computer->isMpQuicCapable && !m_VrRequested && !m_StreamConfig.flatMultilink) {
        m_StreamConfig.useQuicTransport = 1;
        m_StreamConfig.quicPort = 48010;
        m_StreamConfig.quicScheduler = m_Preferences->mpQuicScheduler;
        m_StreamConfig.quicCongestion = 1; // BBR default, optimal for streaming

        // §MP-ADV: 收集 server 的所有已知 IP 作為 alt peer。
        // 優先使用伺服器透過 /serverinfo 廣告的介面（最權威來源），
        // 再 fallback 到客戶端自己探測到的位址（向後相容舊版伺服器）。
        {
            QString activeAddr;
            QVector<NvNetworkInterface> advertisedIfaces;
            NvAddress localAddress;
            NvAddress remoteAddress;
            NvAddress manualAddress;
            int vrCaps = 0;
            {
                // §REVIEW-1119：MP-QUIC 的 alt peer 來源同樣在輪詢執行緒的寫鎖裡被換掉。
                QReadLocker computerLock(&m_Computer->lock);
                activeAddr = m_Computer->activeAddress.address();
                advertisedIfaces = m_Computer->serverAdvertisedInterfaces;
                localAddress = m_Computer->localAddress;
                remoteAddress = m_Computer->remoteAddress;
                manualAddress = m_Computer->manualAddress;
                vrCaps = m_Computer->vipleStreamVr;
            }
            m_StreamConfig.quicAltPeerCount = 0;

            auto tryAdd = [&](const QString& ip) {
                if (ip.isEmpty() || !usableQuicAltPeer(ip)) return;
                if (ip == activeAddr) return;  // 跳過當前使用的位址
                // 去重：檢查是否已加入
                for (int i = 0; i < m_StreamConfig.quicAltPeerCount; i++) {
                    if (ip == QString::fromUtf8(m_StreamConfig.quicAltPeers[i])) return;
                }
                if (m_StreamConfig.quicAltPeerCount >= QUIC_MAX_ALT_PEERS) return;
                QByteArray utf8 = ip.toUtf8();
                if (utf8.size() >= QUIC_ALT_PEER_LEN) return;
                memcpy(m_StreamConfig.quicAltPeers[m_StreamConfig.quicAltPeerCount],
                       utf8.constData(), utf8.size() + 1);
                m_StreamConfig.quicAltPeerCount++;
            };

            auto tryAddAddr = [&](const NvAddress& addr) {
                if (addr.isNull()) return;
                tryAdd(addr.address());
            };

            // 第一優先：伺服器廣告的介面（/serverinfo 的 NetworkInterface）
            for (const auto& iface : std::as_const(advertisedIfaces)) {
                tryAdd(iface.address);
            }
            // §HOTSPOT-FLAT：頭盔熱點上已經連著的裝置（App 啟動時先開的熱點，host 的無線網卡多半已經連上）。
            // /serverinfo 的快取可能還是它連上之前的，這裡直接補。不是 host 的裝置只會多一次沒有回應的探測
            if (vrCaps & VIPLE_VR_SERVER_CAP_HOTSPOT_FLAT) {
                for (const std::string& ip : FrameHotspot::peerAddresses()) {
                    tryAdd(QString::fromStdString(ip));
                }
            }

            // 第二優先：客戶端探測到的位址（向後相容無 §MP-ADV 的舊版伺服器）
            tryAddAddr(localAddress);
            tryAddAddr(remoteAddress);
            tryAddAddr(manualAddress);

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-MPQUIC] Enabled (scheduler=%d, port=%d, cc=BBR, altPeers=%d, serverAdvertised=%d)",
                        m_StreamConfig.quicScheduler, m_StreamConfig.quicPort,
                        m_StreamConfig.quicAltPeerCount,
                        advertisedIfaces.size());
            for (int i = 0; i < m_StreamConfig.quicAltPeerCount; i++) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-MPQUIC]   altPeer[%d]: %s",
                            i, m_StreamConfig.quicAltPeers[i]);
            }
        }

        // §5f: 0-RTT ticket store path. One file holds tickets for all
        // SNIs (picoquic keys them internally); auto-loaded at
        // quicConnect and auto-saved at disconnect.
        {
            QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
            if (base.isEmpty()) {
                base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
            }
            if (!base.isEmpty() && QDir().mkpath(base)) {
                QString ticketFile = base + "/quic-tickets.bin";
                quicSetTicketStorePath(ticketFile.toUtf8().constData());
            } else {
                quicSetTicketStorePath(nullptr);
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-MPQUIC] No writable data dir; 0-RTT disabled");
            }
        }
    }
#endif

    // §VIPLE-VR-LAT10：收包報告的時間換成 tracking 用的 steady_clock（host 拿同一組時鐘偏移算單向延遲）。
    // 一定要在連線開始前設：中途改值會讓 host 的 ABR 看到單向延遲跳一大段
    LiSetVrFeedbackClockOffsetUs((int64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()).count() -
                                 (int64_t)LiGetMicroseconds());
    // §LAT-2D：host 宣告 <VipleStreamInput> bit 0 才送累計位移滑鼠（舊 host、上游 Sunshine 照舊可靠傳送）；--input-test-unreliable 的 A/B 量測時不用
    LiSetRelativeMouseCumulative((m_Computer->vipleStreamInput & 1) != 0 && !m_Preferences->inputTestUnreliable);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-INPUT] relative mouse: %s",
                ((m_Computer->vipleStreamInput & 1) != 0 && !m_Preferences->inputTestUnreliable) ? "cumulative (unreliable, no ACK wait)" : "reliable (legacy)");
    // §RES-BITRATE：最後送出的值（前面每個改寫點各印一行 [VIPLE-RES] bitrate a -> b）
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RES] final: stream %dx%d, host fps %d (display fps %d), bitrate %d kbps (%s%s)",
                m_StreamConfig.width, m_StreamConfig.height, m_StreamConfig.fps, m_OriginalFps, m_StreamConfig.bitrate,
                m_BitrateVrKbps > 0 ? "VR" :
                (m_BitrateFollowsDefault ? "default" : (m_Preferences->bitrateFromCli ? "--bitrate" : "user setting")),
                (m_BitrateCapKbps > 0 && m_StreamConfig.bitrate == m_BitrateCapKbps) ? ", capped" : "");
    int err = LiStartConnection(&hostInfo, &m_StreamConfig, &k_ConnCallbacks,
                                &m_VideoCallbacks, &m_AudioCallbacks,
                                NULL, 0, NULL, 0);
    // [VIPLE-STARTUP]：最後一個連線階段結束的時刻（各階段的起點在 clStageStarting 印）
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-STARTUP] t=%lld LiStartConnection returned %d", startupNowMs(), err);

    // VipleStream: Clean up RTSP tunnel after handshake completes (or fails).
    // The tunnel is only needed for the RTSP handshake — streaming uses UDP.
    if (rtspTunnel) {
        rtspTunnel->stop();
        rtspTunnel->wait(5000);
        delete rtspTunnel;
        rtspTunnel = nullptr;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-NAT] RTSP tunnel cleaned up");
    }

    if (err != 0) {
        // We already displayed an error dialog in the stage failure
        // listener.
        return false;
    }

    // §TS-PATH：host 走 Tailscale（100.64.0.0/10）時在背景監看路徑；掉到中繼就由輕到重嘗試恢復直連（直連時什麼都不做）
    TailscalePath::start(m_Computer->activeAddress.address());

    // VipleStream §N — 啟動 in-stream file transfer client。對應 server tray
    // Send/Receive 行為，每 2s 對 host /transfer/poll 拿命令、透過 OSD overlay
    // 顯示進度。FileTransferClient 內部會 spawn 自己的 QThread + Qt event loop
    // —— 因為 Session::exec() 跑 SDL main loop，主執行緒的 Qt event loop 在
    // streaming 期間整段被 starve，QTimer / QNAM 不會運作。
    if (m_Computer && m_Computer->activeHttpsPort != 0) {
        m_FileTransferClient = new FileTransferClient(
            m_Computer->activeAddress.address(),
            m_Computer->activeHttpsPort,
            IdentityManager::get()->getSslConfig(),
            m_Computer->serverCert,
            nullptr);  // parent nullptr — 自己跑 thread，不掛 Session 的 child tree
        connect(m_FileTransferClient, &FileTransferClient::statusChanged,
                this, [this](const QString &status) {
                    m_OverlayManager.updateOverlayText(Overlay::OverlayStatusUpdate,
                                                       status.toUtf8().constData());
                    m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, true);
                });
        // §PERF-1113：頭盔 VR 串流時每 10 秒才問一次（原本 2 秒）。功能保留，只是 host 送檔最多晚 10 秒才開始；
        // 每次輪詢都是一次 HTTPS 往返，戴著的時候不值得每 2 秒把 Wi-Fi 叫醒一次
        if (m_XrPcvr) {
            m_FileTransferClient->setPollIntervalMs(10000);
        }
        m_FileTransferClient->start();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-XFER] FileTransferClient started for %s:%u (worker thread)",
                    m_Computer->activeAddress.address().toUtf8().constData(),
                    m_Computer->activeHttpsPort);
    }

    // §SC-HID: 連線建立後啟動 passthrough 讀取執行緒（vendor 介面已在 Session::start() prepare 好；
    // 暖機查詢由讀取執行緒代跑，這裡不會碰任何 hidapi 呼叫）
    m_ScHid.start();
    // §FRAME-HOTSPOT-PAUSE：經 VPN 連到 host 的場次，Steam Frame 自己的串流轉接器熱點沒有人用，卻要和外面的 Wi-Fi 分同一顆
    // 無線電（每約 30 秒搶走幾秒）。熱點開著、上面又沒有任何裝置時先暫停它，串流結束還原；使用者自己關掉的不碰。
    // 放在擋掃描之前：熱點停下來之後 wlanap 就不會被列進去。其他平台與不是經 VPN 的場次什麼都不做
    {
        bool viaVpn = m_ViaVpn;
        const QHostAddress hostAddr(m_Computer->activeAddress.address());
        if (!viaVpn && !hostAddr.isNull()) {
            // 自訂封包大小時上面不查路徑：對端是 Tailscale 位址也算（100.64.0.0/10、fd7a:115c:a1e0::/48）
            viaVpn = hostAddr.isInSubnet(QHostAddress(QStringLiteral("100.64.0.0")), 10) ||
                     hostAddr.isInSubnet(QHostAddress(QStringLiteral("fd7a:115c:a1e0::")), 48);
        }
        m_FrameHotspot.pauseIfUnused(viaVpn);
        // §FRAME-HOTSPOT-AUTO：host 在區網裡、熱點關著：自動開起來。等不到這台 host 就關掉；連著玩下一個遊戲時留著，
        // App 結束（或當掉）、換到另一台 host 等不到時才關
        // §HOTSPOT-FLAT（2026-11-18）：平面場次也開——host 收得了熱點資訊、而且這一場有 MP-QUIC（第二條路徑由它用）
        m_HotspotFlat = !viaVpn && !(m_StreamConfig.vrFlags & VIPLE_VR_SF_ENABLED) &&
                        (m_StreamConfig.useQuicTransport || m_StreamConfig.flatMultilink) &&
                        (m_Computer->vipleStreamVr & VIPLE_VR_SERVER_CAP_HOTSPOT_FLAT);
        if (!viaVpn && ((m_StreamConfig.vrFlags & VIPLE_VR_SF_ENABLED) || m_HotspotFlat)) {
            std::string ssid, key;
            // §HOTSPOT-ADAPT 第 3 步：server 會回報熱點加入狀態時，FrameHotspot 才會依回報提早關、送 HOTSPOT_STATE
            m_FrameHotspot.setServerReports((m_Computer->vipleStreamVr & VIPLE_VR_SERVER_CAP_HOTSPOT_STATUS) != 0);
            if (readVrHotspotCreds(&ssid, &key)) {
                m_FrameHotspot.openForLan(ssid, key, m_Computer->uuid.toStdString());
            }
            // §REVIEW-1119：非 Linux 的 openForLan 不會清這兩串。Linux 上它會先 scrub 再 clear，這裡 size 已是 0，不會再寫。
            wipeStdString(ssid);
            wipeStdString(key);
        }
    }
    // §WIFI-SCANBLOCK：Steam Frame 上串流期間擋住 Wi-Fi 背景掃描（和 Steam 內建串流同一個做法）；其他平台什麼都不做
    m_WifiScanBlock.start();

    // §VR：tracking 上行（M1a 為合成 pose）。LiStartConnection 已依 vrFlags 生效
    // §LAT-2D（dev，--input-test）：每 4 ms 送一次 +1／-1 像素的相對滑鼠移動（左右交替，游標不會跑掉），
    // host 的 [VIPLE-INPUT10] 依到達間隔看輸入有沒有被卡住（例：可靠傳輸等 ACK、影像方向塞車時 ACK 晚到）
    LiSetRelativeMouseUnreliable(m_Preferences->inputTestUnreliable);  // §LAT-2D（dev）
    if (m_Preferences->inputTest) {
        m_InputTestStop = false;
        m_InputTestThread = std::thread([this]() {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-INPUT-TEST] sending a +/-1 px mouse move every 4 ms");
            short dir = 1;
            auto next = std::chrono::steady_clock::now();
            while (!m_InputTestStop.load()) {
                LiSendMouseMoveEvent(dir, 0);
                dir = -dir;
                next += std::chrono::milliseconds(4);
                std::this_thread::sleep_until(next);
            }
        });
    }

    if (m_HotspotFlat) {
        checkAndSendVrHotspotInfo();  // §HOTSPOT-FLAT
        sendVrHotspotStateIfAny(m_FrameHotspot);  // §HOTSPOT-ADAPT：這一場不開熱點（CLOSED）或只問（QUERY）
    }
    if (m_StreamConfig.vrFlags & VIPLE_VR_SF_ENABLED) {
        checkAndSendVrHotspotInfo();
        sendVrHotspotStateIfAny(m_FrameHotspot);  // §HOTSPOT-ADAPT
        // §VR-MIC：頭盔麥克風 → host 的 Steam Streaming Microphone
        if ((m_Computer->vipleStreamVr & VIPLE_VR_SERVER_CAP_MIC) && m_Preferences->vrMic) {
            m_Mic = MicCapture::start([](const uint8_t* payload, int len) {
                uint8_t tlv[2 + VIPLE_VR_MAX_CTRL_PAYLOAD];
                if (len <= 0 || len > VIPLE_VR_MAX_CTRL_PAYLOAD) {
                    return;
                }
                tlv[0] = VIPLE_VR_C2S_MIC;
                tlv[1] = static_cast<uint8_t>(len);
                memcpy(tlv + 2, payload, static_cast<size_t>(len));
                LiSendVrMessage(tlv, len + 2, false);
            });
        }
        else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-MIC] not sending the microphone (%s)",
                        !(m_Computer->vipleStreamVr & VIPLE_VR_SERVER_CAP_MIC) ? "host has no Steam Streaming Microphone or is too old"
                                                                              : "turned off in settings");
        }
        VrSyntheticMotion motion = (VrSyntheticMotion)m_Preferences->vrSyntheticMotion;
#ifdef HAVE_OPENXR
        if (m_XrPcvr && m_XrContext != nullptr) {
            // M4a R1：HMD 樣本來自 XR runtime（destroyXrContext 會先 stop 這條執行緒）
            XrContext* xr = m_XrContext;
            // dev（--vr-synthetic-hmd）：頭的姿態換成合成運動，其餘（取樣時機、predictNs、控制器）照舊來自 XR runtime。
            // 無人配戴的場次頭盔是靜止的，host 端「對不到算圖姿態」這類只有頭在動才出現的現象驗不到
            const bool syntheticHmd = m_Preferences->vrSyntheticHmd;
            const bool syntheticHands = m_Preferences->vrSyntheticHands;  // dev：控制器換成在眼前揮動的合成姿態
            const bool syntheticToss = m_Preferences->vrSyntheticToss;    // dev：左手換成每 2 秒一次的拋球動作
            const auto synthStart = std::chrono::steady_clock::now();
            // dev（--vr-synthetic-hmd）：頭的合成軌跡，起點就是 synthStart。下面的來源照它算送給 host 的樣本，XrContext 量
            // 姿態誤差／落後時也拿同一條當基準（頭盔真的姿態放著不動，拿來比只會量到一兩公尺的誤差、估不出落後）
            VrSyntheticHeadTrack synthHead;
            synthHead.motion = motion;
            synthHead.startNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(synthStart.time_since_epoch()).count();
            if (syntheticHmd) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VR-POSE] dev: --vr-synthetic-hmd - the head pose sent to the host is synthetic (%s), not the tracked one. "
                            "The picture will swing in the headset; do not wear it.",
                            vrSyntheticMotionName(motion));
                if (xr->setPcvrSyntheticPoseRef(synthHead)) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-VR-POSEERR] dev: pose error and lag are measured against the synthetic head pose (--vr-synthetic-hmd)");
                }
                else {
                    // frameloop 模式：這一格的顯示時刻只有估計值（約兩個週期），對不準合成軌跡上的時刻，量出來的落後會差好幾毫秒
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-VR-POSEERR] dev: --vr-synthetic-hmd without an XrTime conversion (frameloop tracking) - the display time "
                                "cannot be placed on the synthetic motion exactly, so pose error and lag are still measured against the tracked head pose");
                }
            }
            const double syntheticMenuAt = m_Preferences->vrSyntheticMenuAtSec;  // dev：第幾秒按一下左手 MENU
            const bool syntheticGaze = m_Preferences->vrSyntheticGaze;  // dev（§VR-FOVEA-TILES）
            if (syntheticGaze) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VR-GAZE] dev: --vr-synthetic-gaze - the gaze sent to the host is synthetic (saccades every 0.7 s, pursuit, blinks), not the tracked one");
            }
            m_VrTracking.setSource([xr, syntheticHmd, syntheticHands, syntheticToss, syntheticGaze, syntheticMenuAt, synthHead, synthStart](VIPLE_VR_TRACKING* s) {
                uint32_t predict = 0;
                VIPLE_VR_POSE& hmd = s->pose[VIPLE_VR_POSE_HMD];
                if (!xr->sampleHmd(hmd.pos, hmd.rot, hmd.linVel, hmd.angVel, &predict)) {
                    return false;
                }
                s->flags = VIPLE_VR_TRK_HMD | VIPLE_VR_TRK_PRESENCE;
                s->predictNs = predict;
                // M4a R2：左右控制器（pose、按鍵、pressCtr、flags active／focused）
                xr->sampleControllers(s);
                if (xr->pcvrGazeFovea()) {  // §VR-GAZE-FOVEA：協商成功才取樣（沒開時省掉每秒 ~576 次 xrLocateSpace）
                    xr->sampleEyeGaze(s);
                    if (syntheticGaze) {
                        // dev（--vr-synthetic-gaze）：視線換成合成的掃視／追視（沒人戴時真的視線無效）
                        vrSyntheticGaze(std::chrono::duration<double>(std::chrono::steady_clock::now() - synthStart).count(), s);
                    }
                }
                if (syntheticHmd) {
                    // 樣本的語意是「sampleTimeNs＋predictNs 那一刻的姿態」：真的樣本是 runtime 預測到那一刻的，host 也把它當成
                    // 那一刻的姿態再往後外插。合成的照同一刻算，位置、方向、速度都是。2026-10-10 之前送的是「現在」的姿態，
                    // host 以為它已經是 predictNs 之後的，少外插了這麼多：算繪姿態在合成軌跡上固定落後一個 predictNs
                    // （頭盔上約 26 ms），拿合成軌跡當基準量落後、讓 host 的 §VR-PREDICT 去補，收斂的值就和實際配戴不同
                    synthHead.poseAt(s->sampleTimeNs + predict, &hmd);
                }
                // 合成的手和合成的頭一樣，照「sampleTimeNs＋predictNs 那一刻」算（2026-11-14：原本用「現在」——取樣之後才讀時鐘，
                // 這條執行緒被擠到的那幾拍，合成的姿態就和它標的時刻差了那麼多；量拍子軌跡的平滑度時這會算成是我們的抖動）
                const double synthT = (double)(int64_t)(s->sampleTimeNs + predict - synthHead.startNs) * 1e-9;
                if (syntheticHands) {
                    vrSyntheticHands(synthT, s);
                }
                if (syntheticToss) {
                    vrSyntheticToss(synthT, s);
                }
                vrSyntheticMenu(std::chrono::duration<double>(std::chrono::steady_clock::now() - synthStart).count(), syntheticMenuAt, s);
                return true;
            }, syntheticHmd ? "xr+synthetic-hmd" : (m_XrContext->trackingThreadMode() ? "xr-thread" : "xr-frameloop"));
        }
#endif
        m_VrTracking.setSyntheticHands(m_Preferences->vrSyntheticHands);
        m_VrTracking.setSyntheticToss(m_Preferences->vrSyntheticToss);
        m_VrTracking.setSyntheticMenuAt(m_Preferences->vrSyntheticMenuAtSec);
        m_VrTracking.setSyntheticPadTest(m_Preferences->vrSyntheticPadTestSec);
        m_VrTracking.setPuppetPath(m_Preferences->vrPuppetFile.toStdString());
        if (!m_Preferences->vrPuppetFile.isEmpty()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-INPUT] dev: --vr-puppet - poses and buttons follow %s (only with --vr-emulate)",
                        qUtf8Printable(m_Preferences->vrPuppetFile));
        }
        if (m_Preferences->vrSyntheticPadTestSec > 0.0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-PADMODE] dev: --vr-synthetic-pad-test - the Xbox controller mode button sequence starts %.1f s after tracking "
                        "starts (only with --vr-emulate)",
                        m_Preferences->vrSyntheticPadTestSec);
        }
        if (m_Preferences->vrSyntheticMenuAtSec > 0.0) {
            // host 只在左手控制器 active 時才看按鍵：--vr-emulate 一定是；真的 XR 連線上沒被追蹤、又沒有合成的手，這一下會被丟掉
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-INPUT] dev: --vr-synthetic-menu-at - the left MENU button is pressed %.1f s after tracking starts "
                        "(toggles the host's SteamVR dashboard)%s",
                        m_Preferences->vrSyntheticMenuAtSec,
                        m_Preferences->vrSyntheticHands ? "" :
                        "; on a real XR runtime the host ignores it unless the left controller is tracked - add --vr-synthetic-hands");
        }
        m_VrTracking.start(s_VrTrackingHz > 0 ? s_VrTrackingHz : m_StreamConfig.fps, motion);
#ifdef HAVE_OPENXR
        if (m_XrPcvr && m_Preferences->vrTestHaptic && m_VrTestHapticTimer == 0) {
            // M4a R2（dev）：每 3 s 本地注入一則 HAPTIC（左右交替），走與 server 訊息相同的
            // clVrMessage → SDL_USEREVENT → handleVrMessage 路徑。destroyXrContext 前移除
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-HAPTIC] dev --vr-test-haptic: injecting a local HAPTIC every 3 s");
            m_VrTestHapticTimer = SDL_AddTimer(3000, [](Uint32 interval, void* param) -> Uint32 {
                auto* self = static_cast<Session*>(param);
                const uint32_t n = ++self->m_VrTestHapticSeq;
                uint8_t tlv[2 + sizeof(VIPLE_VR_TLV_HAPTIC)] = {};
                tlv[0] = VIPLE_VR_S2C_HAPTIC;
                tlv[1] = (uint8_t)sizeof(VIPLE_VR_TLV_HAPTIC);
                VIPLE_VR_TLV_HAPTIC h;
                memset(&h, 0, sizeof(h));
                h.device = (n & 1) ? VIPLE_VR_POSE_LEFT : VIPLE_VR_POSE_RIGHT;
                h.durationUs = 120000;
                h.frequencyHz = 160.0f;
                h.amplitude = 0.6f;
                h.eventId = n;
                h.timestampNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                memcpy(tlv + 2, &h, sizeof(h));
                self->clVrMessage(tlv, (int)sizeof(tlv));
                return interval;
            }, this);
        }
#endif
    }

    // [VIPLE-SESSION] 一行結構化 session 標記：每秒遙測行不帶 host/codec，
    // 事後跨 session log 分析靠這行把遙測歸戶到正確的 host/session。
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-SESSION] host=%s app=%d fps=%d bitrate=%d codecs=0x%x",
                m_Computer->activeAddress.address().toUtf8().constData(),
                m_App.id,
                m_StreamConfig.fps,
                m_StreamConfig.bitrate,
                (unsigned int)m_StreamConfig.supportedVideoFormats);
#ifdef Q_OS_LINUX
    // §SF-ENV（M2a）：session 開場補一行執行環境摘要。summaryLine() 尾端已現取 SDL 實際採用的
    // video driver 與 SDL_APP_ID（sdl-driver=／sdl-app-id=；R3 驗收：Flatpak 的 SDL3＋sdl2-compat
    // 下 env 可能沒生效，要看 SDL 自己的值），這裡不再另外印。非 Linux 不編譯（summaryLine 也是空字串）。
    {
        const QString sfEnv = SfEnv::summaryLine();
        if (!sfEnv.isEmpty()) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-SF-ENV] session: %s",
                        sfEnv.toUtf8().constData());
        }
    }
#endif

    // [VIPLE-STARTUP]：連線執行緒這一側做完（LiStartConnection 之後的雜項也算在內）；之後回主執行緒建視窗與解碼器
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-STARTUP] t=%lld connectionStarted", startupNowMs());
    emit connectionStarted();
    return true;
}

void Session::flushWindowEvents()
{
    // Pump events to ensure all pending OS events are posted
    SDL_PumpEvents();

    // Insert a barrier to discard any additional window events.
    // We don't use SDL_FlushEvent() here because it could cause
    // important events to be lost.
    m_FlushingWindowEventsRef++;

    // This event will cause us to set m_FlushingWindowEvents back to false.
    SDL_Event flushEvent = {};
    flushEvent.type = SDL_USEREVENT;
    flushEvent.user.code = SDL_CODE_FLUSH_WINDOW_EVENT_BARRIER;
    // §REVIEW-1103：推不進去（佇列滿或被 filter 擋）時 ref 永遠不會歸零，之後所有視窗事件都被丟掉
    if (SDL_PushEvent(&flushEvent) <= 0) {
        m_FlushingWindowEventsRef--;
    }
}

void Session::setShouldExit(bool quitHostApp)
{
    // If the caller has explicitly asked us to quit the host app,
    // override whatever the preferences say and do it. If the
    // caller doesn't override to force quit, let the preferences
    // dictate what we do.
    if (quitHostApp) {
        m_Preferences->quitAppAfter = true;
    }

    // 使用者主動要求退出——之後才到的 connectionTerminated 不再彈錯誤框
    m_UserInitiatedQuit.store(true);

    m_ShouldExit = true;
}

void Session::start()
{
    // [VIPLE-STARTUP]：initialize 結束到這裡之間是介面那一側（啟動警告的提示等）
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-STARTUP] t=%lld start", startupNowMs());

    // Wait for any old session to finish cleanup
    s_ActiveSessionSemaphore.acquire();

    // We're now active
    s_ActiveSession = this;

    // §SC-HID / §SC-THREAD-OWNER：先在主執行緒開好 Steam Controller 的 vendor 介面（不碰連線、
    // 不送 feature），下面 SdlInputHandler 建構時才知道要不要把 SC 家族加進 SDL 忽略清單——
    // SDL 3.4.x 內建的 Steam hidapi 驅動會自己開同一組介面送 feature report，與 passthrough
    // 搶一次性暫存器。開不到（沒插、Linux 沒 hidraw 權限）就不加，SC 照舊當一般 SDL 手把。
    // validateLaunch() 已 HidProbe::refresh() 過：卡死的 SC 在 prepare() 內就被拒絕。
    m_ScHid.prepare();
    m_ScHid.hidDevicesChanged();   // §SC-HOTPLUG：記下 HID 變動計數的基準

    // Initialize the gamepad code with our preferences
    // NB: m_InputHandler must be initialize before starting the connection.
    m_InputHandler = new SdlInputHandler(*m_Preferences, m_StreamConfig.width, m_StreamConfig.height);

    // Kick off the async connection thread then return to the caller to pump the event loop
    auto thread = new AsyncConnectionStartThread(this);
    QObject::connect(thread, &QThread::finished, this, &Session::exec);
    QObject::connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void Session::interrupt()
{
    // interrupt() 只由使用者／UI 主動中止時呼叫——視同使用者退出
    m_UserInitiatedQuit.store(true);

    // Stop any connection in progress
    LiInterruptConnection();

    // Inject a quit event to our SDL event loop
    SDL_Event event;
    event.type = SDL_QUIT;
    event.quit.timestamp = SDL_GetTicks();
    SDL_PushEvent(&event);
}

// §SC-HOTPLUG（2026-10-24）：passthrough 原本只在 Session::start() 列舉一次，串流開始後才
// 連上的 Steam Controller（藍牙 SC 從休眠醒來最常見）整場都不會被接手；中途斷線的 SC
// 也接不回來。主迴圈每秒看一次：
//   - passthrough 在跑、讀取執行緒回報裝置不在了 → stop()，SDL 忽略清單拿掉 SC 家族
//   - passthrough 沒在跑、SDL 偵測到 HID 裝置變動 → 重新 prepare()；開到就把已走 SDL
//     路徑的 Valve 手把收掉、加回忽略清單，再 start()
// 全部在主執行緒，與 Session::start() 的 prepare() 同一條執行緒；hidapi 的同步呼叫仍只在
// SC-HID 讀取執行緒上（prepare 只做列舉與開 handle）。
void Session::pollScHidHotplug()
{
    const Uint32 now = SDL_GetTicks();
    if (m_ScHidHotplugTick != 0 && now - m_ScHidHotplugTick < 1000) {
        return;
    }
    m_ScHidHotplugTick = now ? now : 1;
    if (m_InputHandler == nullptr) {
        return;
    }

    if (m_ScHid.isPrepared()) {
        if (m_ScHid.devicesGone()) {
            // §INPUT-STALL（A.3-2）：這裡是主執行緒，不可用會等讀取執行緒最多 1 秒的 stop()。
            // stopNonBlocking() 不等：讀取執行緒設了 devicesGone 之後通常幾毫秒內就退出，這一次就收掉；
            // 還沒退出就回 false，下一次輪詢（約 1 秒後）再來，等滿上限照 stop() 的規則遺棄。
            // 收掉之前 isPrepared() 仍為 true，下面的重新 prepare() 不會跑，SDL 忽略清單也維持不變。
            if (!m_ScHid.stopNonBlocking()) {
                return;
            }
            m_InputHandler->setScHidPassthroughPrepared(false);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[SC-HID] hotplug: Steam Controller gone - passthrough stopped, waiting for it to come back");
        }
        else {
            // 在跑時也更新基準，免得之後一次把累積的變動當成新裝置
            m_ScHid.hidDevicesChanged();
        }
        return;
    }

#ifdef Q_OS_WIN32
    // §REVIEW-1104：不在 SDL 主迴圈同步等待 HidProbe::refresh()，改由工作執行緒探測完再於後續輪詢 prepare
    static std::atomic<bool> s_HotplugProbeRunning {false};
    static std::atomic<bool> s_HotplugProbeReady {false};
    if (s_HotplugProbeRunning.load()) {
        return;
    }
    if (!s_HotplugProbeReady.exchange(false)) {
        if (!m_ScHid.hidDevicesChanged()) {
            return;
        }
        s_HotplugProbeRunning.store(true);
        struct ProbeTask {
            static int SDLCALL run(void*) {
                HidProbe::refresh();
                s_HotplugProbeReady.store(true);
                s_HotplugProbeRunning.store(false);
                return 0;
            }
        };
        SDL_Thread* probeThread = SDL_CreateThread(&ProbeTask::run, "ScHidHotplugProbe", nullptr);
        if (probeThread != nullptr) {
            SDL_DetachThread(probeThread);
        }
        else {
            s_HotplugProbeRunning.store(false);
        }
        return;
    }
#else
    if (!m_ScHid.hidDevicesChanged()) {
        return;
    }
#endif
    if (!m_ScHid.prepare(true /* quiet */)) {
        return;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[SC-HID] hotplug: Steam Controller appeared mid-session - taking it over");
    m_InputHandler->setScHidPassthroughPrepared(true);
    m_ScHid.start();
}

void Session::exec()
{
    // If the connection failed, clean up and abort the connection.
    if (!m_AsyncConnectionSuccess) {
        delete m_InputHandler;
        m_InputHandler = nullptr;
        destroyXrContext();  // §VR M3a X1
        releaseXrOwner();
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        QThreadPool::globalInstance()->start(new DeferredSessionCleanupTask(this));
        return;
    }

    // [VIPLE-STARTUP]：連線執行緒結束、回到主執行緒的時刻（之後建視窗，SHOWN 才建解碼器）
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-STARTUP] t=%lld exec begin", startupNowMs());

    // Pump the Qt event loop one last time before we create our SDL window
    // This is sometimes necessary for the QML code to process any signals
    // we've emitted from the async connection thread.
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    QCoreApplication::sendPostedEvents();

    int x, y, width, height;
    getWindowDimensions(x, y, width, height);

#ifdef STEAM_LINK
    // We need a little delay before creating the window or we will trigger some kind
    // of graphics driver bug on Steam Link that causes a jagged overlay to appear in
    // the top right corner randomly.
    SDL_Delay(500);
#endif

    // Request at least 8 bits per color for GL
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);

    // Disable depth and stencil buffers
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);

    // We always want a resizable window with High DPI enabled
    Uint32 defaultWindowFlags = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;

    // If we're starting in windowed mode and the Moonlight GUI is maximized or
    // minimized, match that with the streaming window.
    if (!m_IsFullScreen && m_QtWindow != nullptr) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        // Qt 5.10+ can propagate multiple states together
        if (m_QtWindow->windowStates() & Qt::WindowMaximized) {
            defaultWindowFlags |= SDL_WINDOW_MAXIMIZED;
        }
        if (m_QtWindow->windowStates() & Qt::WindowMinimized) {
            defaultWindowFlags |= SDL_WINDOW_MINIMIZED;
        }
#else
        // Qt 5.9 only supports a single state at a time
        if (m_QtWindow->windowState() == Qt::WindowMaximized) {
            defaultWindowFlags |= SDL_WINDOW_MAXIMIZED;
        }
        else if (m_QtWindow->windowState() == Qt::WindowMinimized) {
            defaultWindowFlags |= SDL_WINDOW_MINIMIZED;
        }
#endif
    }

    // We use only the computer name on macOS to match Apple conventions where the
    // app name is featured in the menu bar and the document name is in the title bar.
#ifdef Q_OS_DARWIN
    std::string windowName = QString(m_Computer->name).toStdString();
#else
    std::string windowName = QString(m_Computer->name + " - Moonlight").toStdString();
#endif

    m_Window = SDL_CreateWindow(windowName.c_str(),
                                x,
                                y,
                                width,
                                height,
                                defaultWindowFlags | StreamUtils::getPlatformWindowFlags());
    if (!m_Window) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL_CreateWindow() failed with platform flags: %s",
                    SDL_GetError());

        m_Window = SDL_CreateWindow(windowName.c_str(),
                                    x,
                                    y,
                                    width,
                                    height,
                                    defaultWindowFlags);
        if (!m_Window) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL_CreateWindow() failed: %s",
                         SDL_GetError());

            delete m_InputHandler;
            m_InputHandler = nullptr;
            destroyXrContext();  // §VR M3a X1
            releaseXrOwner();
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            QThreadPool::globalInstance()->start(new DeferredSessionCleanupTask(this));
            return;
        }
    }

    m_InputHandler->setWindow(m_Window);

    QSvgRenderer svgIconRenderer(QString(":/res/moonlight.svg"));
    QImage svgImage(ICON_SIZE, ICON_SIZE, QImage::Format_RGBA8888);
    svgImage.fill(0);

    QPainter svgPainter(&svgImage);
    svgIconRenderer.render(&svgPainter);
    SDL_Surface* iconSurface = SDL_CreateRGBSurfaceWithFormatFrom((void*)svgImage.constBits(),
                                                                  svgImage.width(),
                                                                  svgImage.height(),
                                                                  32,
                                                                  4 * svgImage.width(),
                                                                  SDL_PIXELFORMAT_RGBA32);
#ifndef Q_OS_DARWIN
    // Other platforms seem to preserve our Qt icon when creating a new window.
    if (iconSurface != nullptr) {
        // This must be called before entering full-screen mode on Windows
        // or our icon will not persist when toggling to windowed mode
        SDL_SetWindowIcon(m_Window, iconSurface);
    }
#endif

    // Update the window display mode based on our current monitor
    // for if/when we enter full-screen mode.
    updateOptimalWindowDisplayMode();

    // Enter full screen if requested
    if (m_IsFullScreen) {
        SDL_SetWindowFullscreen(m_Window, m_FullScreenFlag);
    }

    bool needsFirstEnterCapture = false;
    bool needsPostDecoderCreationCapture = false;

    // HACK: For Wayland, we wait until we get the first SDL_WINDOWEVENT_ENTER
    // event where it seems to work consistently on GNOME. For other platforms,
    // especially where SDL may call SDL_RecreateWindow(), we must only capture
    // after the decoder is created.
    if (strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0) {
        // Native Wayland: Capture on SDL_WINDOWEVENT_ENTER
        needsFirstEnterCapture = true;
    }
    else {
        // X11/XWayland: Capture after decoder creation
        needsPostDecoderCreationCapture = true;
    }

    // Stop text input. SDL enables it by default
    // when we initialize the video subsystem, but this
    // causes an IME popup when certain keys are held down
    // on macOS.
    SDL_StopTextInput();

    // Disable the screen saver if requested
    if (m_Preferences->keepAwake) {
        SDL_DisableScreenSaver();
    }

    // Hide Qt's fake mouse cursor on EGLFS systems
    if (QGuiApplication::platformName() == "eglfs") {
        QGuiApplication::setOverrideCursor(QCursor(Qt::BlankCursor));
    }

    // Set timer resolution to 1 ms on Windows for greater
    // sleep precision and more accurate callback timing.
    SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "1");

    int currentDisplayIndex = SDL_GetWindowDisplayIndex(m_Window);

    // Now that we're about to stream, any SDL_QUIT event is expected
    // unless it comes from the connection termination callback where
    // (m_UnexpectedTermination is set back to true).
    m_UnexpectedTermination = false;
    // §REVIEW-1111：連線在上面建視窗的那段時間就已經在跑，斷線回呼可能比這一行早到——它設的 true 會被這一行清掉，
    // 收尾就改送 /cancel 把 host 上的遊戲關掉，而不是留著給使用者接回去。回呼先設 m_UnexpectedSeen 再設旗標，
    // 這裡清完再看一次：不論兩邊怎麼交錯，回呼來過最後就是 true。
    if (m_UnexpectedSeen.load()) {
        m_UnexpectedTermination = true;
    }

    // Start rich presence to indicate we're in game
    RichPresenceManager presence(*m_Preferences, m_App.name);

    // Toggle the stats overlay if requested by the user
    m_OverlayManager.setOverlayState(Overlay::OverlayDebug, m_Preferences->showPerformanceOverlay);

    // Switch to async logging mode when we enter the SDL loop
    StreamUtils::enterAsyncLoggingMode();

    // 零音訊紀錄：開串 60 秒後 m_AudioSampleCount 仍為 0 只寫一行 log。
    // server 在主機沒出聲時本來就不送音訊封包，所以「沒收到」多半只是
    // 靜音，不再拿它跳警告（§AUD-MON 之前的作法，誤報太多）。
    // 主執行緒 Qt event loop 在串流期間被 starve，QTimer 不會動，所以掛在
    // SDL loop 上（WaitEventTimeout 保證至少每秒迭代一次）。
    const Uint32 audioSilenceCheckTime = SDL_GetTicks() + 60000;
    bool audioSilenceChecked = false;
    // §AUD-MON：server 比對播放裝置的音量計與擷取結果，真的擷取不到才回報
    // （LiGetServerAudioStatus）。這裡只在狀態變化時顯示提示；舊 server 不回報
    // 就永遠不顯示。overlay 到時自動收掉（OverlayStatusUpdate 是共用 slot，
    // 收掉前比對文字仍是自己寫的那份，避免誤關別人的通知）。
    static const char* k_AudioStalledOverlayMsg =
        "⚠ 主機正在播放聲音，但伺服器擷取不到（已自動重試）";
    static const char* k_FrucUnavailableOverlayMsg =
        "⚠ 補幀在 YUV 4:4:4 串流下無法使用（這次改由 Vulkan 解碼顯示）。要補幀請關閉 4:4:4";
    static const char* k_AudioInitFailedOverlayMsg =
        "⚠ 主機的音訊擷取無法啟動，這次串流沒有聲音";
    static const char* k_AudioRecoveredOverlayMsg =
        "音訊擷取已恢復";
    const char* audioOverlayMsg = nullptr;
    Uint32 audioOverlayHideTime = 0;
    uint32_t audioStatusSeq = 0;

    // Hijack this thread to be the SDL main thread. We have to do this
    // because we want to suspend all Qt processing until the stream is over.
    SDL_Event event;
    // §INPUT-STALL：主迴圈 watchdog（見檔頭註解）
    s_LoopBeat.store(0);
    s_LoopPhase.store(-1);
#ifdef Q_OS_WIN32
    {
        SDL_SysWMinfo wmInfo;
        SDL_VERSION(&wmInfo.version);
        s_StallWatchdogHwnd = (m_Window != nullptr && SDL_GetWindowWMInfo(m_Window, &wmInfo)) ? wmInfo.info.win.window : nullptr;
    }
#endif
#ifdef Q_OS_WIN32
    s_StallMainThreadId = GetCurrentThreadId();
#endif
    // 只在 Windows 開 watchdog 執行緒（理由見檔頭 §INPUT-STALL 註解）；其他平台 stallWatchdog 維持 nullptr，
    // 結尾的收尾本來就會檢查
    SDL_Thread* stallWatchdog = nullptr;
#ifdef Q_OS_WIN32
    s_StallWatchdogRun.store(true);
    stallWatchdog = SDL_CreateThread(inputStallWatchdogProc, "InputStallWD", nullptr);
#endif

    // §VR-LOWBATT：XR（頭盔）時才看電量
    BatteryMonitor battery(m_Preferences->vrFakeBatteryPct, m_Preferences->vrLowBatteryAtPct);
    Uint32 batteryPollTime = 0, batteryLogTime = 0;
    int batteryIconPct = -1;
    const void* batteryIconCtx = nullptr;  // 上一次送圖示的 XrContext
    // §VR-NETSEL：Steam Frame 的網路狀態自動選擇（解構時交還家用 Wi-Fi）
    std::unique_ptr<VrNetMode> netMode;

    for (;;) {
        s_LoopBeat.fetch_add(1);
        s_LoopPhase.store(-3);
        pollScHidHotplug();
#ifdef HAVE_OPENXR
#ifdef Q_OS_LINUX
        // §VR-NETBENCH：網路測試的子行程不碰網路狀態（由 GUI 的測試流程切換、送心跳）
        if (m_XrContext != nullptr && (m_StreamConfig.vrFlags & VIPLE_VR_SF_ENABLED) && !NetBench::config().active()) {
            if (!netMode) {
                netMode = std::make_unique<VrNetMode>(m_Preferences->vrNetMode, m_Preferences->vrNetRetest, m_StreamConfig.fps);
                netMode->toast = [this](const QString& text) {
                    if (m_XrContext != nullptr) {
                        m_XrContext->showToast(text, 6000);
                    }
                };
                // §VR-NETSEL-HOLD：切網路前請 host 別因那幾秒的掉包降碼（ENet 與每條連線各送一份）
                netMode->netSwitch = [](int phase, int holdMs) {
                    VIPLE_VR_TLV_NET_SWITCH ns{};
                    ns.phase = static_cast<uint8_t>(phase);
                    ns.holdMs = static_cast<uint16_t>(std::clamp(holdMs, 0, 65535));
                    uint8_t tlv[2 + sizeof(ns)];
                    tlv[0] = VIPLE_VR_C2S_NET_SWITCH;
                    tlv[1] = sizeof(ns);
                    memcpy(tlv + 2, &ns, sizeof(ns));
                    LiSendVrMessage(tlv, sizeof(tlv), true);
                };
            }
            netMode->tick(SDL_GetTicks(), m_XrContext->pcvrNewFrames());
            if (m_Preferences->vrNetRetest && netMode->retestConsumed()) {
                m_Preferences->vrNetRetest = false;  // 重測只做一次
                m_Preferences->save();
            }
        }
#endif
        if (NetBench::config().active()) {
            netBenchTick();
        }
        if (m_XrContext != nullptr && SDL_TICKS_PASSED(SDL_GetTicks(), batteryPollTime)) {
            batteryPollTime = SDL_GetTicks() + 10000;
            if (const auto br = battery.read()) {
                const bool entered = battery.update(*br);
                if (batteryLogTime == 0 || SDL_TICKS_PASSED(SDL_GetTicks(), batteryLogTime)) {
                    batteryLogTime = SDL_GetTicks() + 60000;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-BATT] %d%% %s current=%d mA voltage=%d mV%s",
                                br->capacity, br->charging ? "charging" : "discharging", br->currentMa, br->voltageMv,
                                battery.low() ? " LOW" : "");
                }
                const int want = battery.low() ? br->capacity : -1;
                // §REVIEW-1111：XR session 重建後是新的 XrContext（圖示預設不顯示），百分比沒變也要再送一次
                if (want != batteryIconPct || batteryIconCtx != m_XrContext) {
                    batteryIconPct = want;
                    batteryIconCtx = m_XrContext;
                    m_XrContext->setBatteryIcon(want);
                }
                if (entered) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-BATT] low battery: %d%% - showing the icon and beeping",
                                br->capacity);
                    m_BeepRequest.store(true);
                }
            }
        }
#endif
        if ((m_StreamConfig.vrFlags & VIPLE_VR_SF_ENABLED) || m_HotspotFlat) {
            static Uint32 lastHotspotPollTime = 0;
            // §HOTSPOT-ADAPT 第 3 步：還在等 host 時每秒一輪（host 的否定回報一秒內生效），其餘照舊每 5 秒
            if (SDL_TICKS_PASSED(SDL_GetTicks(), lastHotspotPollTime + (m_FrameHotspot.waitingForHost() ? 1000 : 5000))) {
                lastHotspotPollTime = SDL_GetTicks();
                // §HOTSPOT-REOPEN-ASYNC（第三輪審查 C35／C41／D13）：背景的「再開一次」做完了就在這裡（主迴圈）套用，
                // 接著的 checkAndSendVrHotspotInfo 把熱點資訊送出去。還在做的期間不送熱點資訊（advertiseToHost 為否）、
                // 也不跑 tick 與 host 回報的判斷（HOTSPOT_STATUS 留在 common-c 裡，seq 不變，做完的下一輪照樣讀得到）
                m_FrameHotspot.finishReopen();
                checkAndSendVrHotspotInfo();
                if (!m_FrameHotspot.reopenRunning()) {
                    // §HOTSPOT-VERIFY：有多連線時以「這一場有沒有連線走熱點」為準（熱點上有別台 host 的網卡不算）
                    // §HOTSPOT-ADAPT：沒有多連線的場次多給一個線索——這台 host 自己回報的位址出現在熱點上就是它
                    const bool hotspotHaveLinks = LiGetVrLinkCount() > 0;
                    std::vector<std::string> hotspotHostAddrs;
                    if (!hotspotHaveLinks && m_FrameHotspot.wantsHostAddrs()) {
                        QReadLocker computerLock(&m_Computer->lock);
                        for (const auto& iface : std::as_const(m_Computer->serverAdvertisedInterfaces)) {
                            hotspotHostAddrs.push_back(iface.address.toStdString());
                        }
                        hotspotHostAddrs.push_back(m_Computer->activeAddress.address().toStdString());
                    }
                    m_FrameHotspot.tick(hotspotHaveLinks ? LiVrLinkOnDedicated() : -1, hotspotHostAddrs);  // §FRAME-HOTSPOT-AUTO
                    // §HOTSPOT-ADAPT 第 3 步：host 的回報（排在 tick 之後：這一輪剛確認的連線優先於 host 的說法）
                    VIPLE_VR_TLV_HOTSPOT_STATUS hsStatus;
                    uint32_t hsSeq = 0;
                    if (LiGetVrHotspotStatus(&hsStatus, &hsSeq) >= 0) {
                        m_FrameHotspot.onHostStatus(hsStatus.state, hsStatus.flags, hsStatus.attempts, hsStatus.restarts,
                                                    hsStatus.elapsedDs, hsStatus.wlanReason, hsSeq);
                    }
                    // 第三輪審查 D12：QUERY 的逾時排在這一輪的回報之後判斷（剛到的 READY 優先）
                    m_FrameHotspot.expireQuery();
                    if (m_FrameHotspot.takeReopen()) {
                        // 問過 host、它現在有閒著的無線介面：這時才開熱點（背景）；開好了下一輪才送熱點資訊
                        std::string ssid, key;
                        const bool haveCreds = readVrHotspotCreds(&ssid, &key);
                        const bool started = haveCreds && m_FrameHotspot.startReopen(ssid, key, m_Computer->uuid.toStdString());
                        wipeStdString(ssid);
                        wipeStdString(key);
                        if (!started) {
                            checkAndSendVrHotspotInfo();  // 沒有開始（不是 Frame、讀不到設定等）：照舊送熱點資訊
                        }
                    }
                }
                sendVrHotspotStateIfAny(m_FrameHotspot);
                if (m_HotspotFlat) {
                    // §HOTSPOT-FLAT：host 的無線網卡這時才連上熱點：把它的位址交給 MP-QUIC（已經有就不動）
                    for (const std::string& ip : FrameHotspot::peerAddresses()) {
                        LiAddQuicAltPeer(ip.c_str());
                    }
                }
            }
        }
        if (!audioSilenceChecked && SDL_TICKS_PASSED(SDL_GetTicks(), audioSilenceCheckTime)) {
            audioSilenceChecked = true;
            if (m_AudioSampleCount == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-AUD] No audio traffic in the first 60 seconds "
                            "(host silent, or capture broken — server status %d)",
                            LiGetServerAudioStatus(nullptr));
            }
        }
        {
            uint32_t seq;
            int audioStatus = LiGetServerAudioStatus(&seq);
            if (seq != audioStatusSeq) {
                audioStatusSeq = seq;
                const char* msg = nullptr;
                Uint32 showMs = 0;
                switch (audioStatus) {
                case LI_AUDIO_STATUS_STALLED:
                    msg = k_AudioStalledOverlayMsg;
                    showMs = 15000;
                    break;
                case LI_AUDIO_STATUS_INIT_FAILED:
                    msg = k_AudioInitFailedOverlayMsg;
                    showMs = 15000;
                    break;
                case LI_AUDIO_STATUS_RECOVERED:
                    msg = k_AudioRecoveredOverlayMsg;
                    showMs = 5000;
                    break;
                default:
                    break;
                }
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-AUD-MON] server audio status %d", audioStatus);
                // gate 同 clConnectionStatusUpdate：尊重 connectionWarnings
                // 偏好、不蓋掉 mouse emulation 的常駐提示。
                if (msg != nullptr && m_Preferences->connectionWarnings && m_MouseEmulationRefCount == 0) {
                    m_OverlayManager.updateOverlayText(Overlay::OverlayStatusUpdate, msg);
                    m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, true);
                    audioOverlayMsg = msg;
                    audioOverlayHideTime = SDL_GetTicks() + showMs;
                }
            }
        }
        // §FRUC-444-NOTICE：補幀因 4:4:4 無法使用時提示 10 秒。不受「連線警告」開關影響（這是回應使用者自己開的補幀，
        // 不是連線狀態）；只避開 mouse emulation 的常駐提示
        if (m_FrucUnavailablePending.exchange(false) && m_MouseEmulationRefCount == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-FRUC] showing 4:4:4 frame interpolation notice");
            m_OverlayManager.updateOverlayText(Overlay::OverlayStatusUpdate, k_FrucUnavailableOverlayMsg);
            m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, true);
            audioOverlayMsg = k_FrucUnavailableOverlayMsg;  // 共用計時收掉的邏輯（比對文字才關）
            audioOverlayHideTime = SDL_GetTicks() + 10000;
        }
        if (audioOverlayHideTime != 0 &&
            SDL_TICKS_PASSED(SDL_GetTicks(), audioOverlayHideTime)) {
            audioOverlayHideTime = 0;
            if (audioOverlayMsg != nullptr &&
                strcmp(m_OverlayManager.getOverlayText(Overlay::OverlayStatusUpdate),
                       audioOverlayMsg) == 0) {
                m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, false);
            }
            audioOverlayMsg = nullptr;
        }
        // §INPUT-STALL：從這裡起才是「在 SDL 事件泵裡」；迴圈開頭到這一行是 -3（loop-top）
        s_LoopPhase.store(-1);
#if SDL_VERSION_ATLEAST(2, 0, 18) && !defined(STEAM_LINK)
        // SDL 2.0.18 has a proper wait event implementation that uses platform
        // support to block on events rather than polling on Windows, macOS, X11,
        // and Wayland. It will fall back to 1 ms polling if a joystick is
        // connected, so we don't use it for STEAM_LINK to ensure we only poll
        // every 10 ms.
        //
        // NB: This behavior was introduced in SDL 2.0.16, but had a few critical
        // issues that could cause indefinite timeouts, delayed joystick detection,
        // and other problems.
        if (!SDL_WaitEventTimeout(&event, 1000)) {
            presence.runCallbacks();
            continue;
        }
#else
        // We explicitly use SDL_PollEvent() and SDL_Delay() because
        // SDL_WaitEvent() has an internal SDL_Delay(10) inside which
        // blocks this thread too long for high polling rate mice and high
        // refresh rate displays.
        if (!SDL_PollEvent(&event)) {
#ifndef STEAM_LINK
            SDL_Delay(1);
#else
            // Waking every 1 ms to process input is too much for the low performance
            // ARM core in the Steam Link, so we will wait 10 ms instead.
            SDL_Delay(10);
#endif
            presence.runCallbacks();
            continue;
        }
#endif
        s_LoopBeat.fetch_add(1);
        s_LoopPhase.store(event.type == SDL_USEREVENT ? (0x10000 | (event.user.code & 0xFFFF)) : (int32_t)event.type);
        switch (event.type) {
        case SDL_QUIT:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Quit event received");
            // SDL_QUIT 絕大多數來自使用者（quit combo／關窗）；連線終止
            // callback 自己推的 SDL_QUIT 也會到這裡，但那時對話框早已
            // 送出、callback 不會再觸發，設旗標無副作用。
            m_UserInitiatedQuit.store(true);
            goto DispatchDeferredCleanup;

        case SDL_USEREVENT:
            switch (event.user.code) {
            case SDL_CODE_FRAME_READY:
                if (m_VideoDecoder != nullptr) {
                    m_VideoDecoder->renderFrameOnMainThread();
                }
                break;
            case SDL_CODE_FLUSH_WINDOW_EVENT_BARRIER:
                m_FlushingWindowEventsRef--;
                break;
            case SDL_CODE_GAMECONTROLLER_RUMBLE:
                m_InputHandler->rumble((uint16_t)(uintptr_t)event.user.data1,
                                       (uint16_t)((uintptr_t)event.user.data2 >> 16),
                                       (uint16_t)((uintptr_t)event.user.data2 & 0xFFFF));
                break;
            case SDL_CODE_GAMECONTROLLER_RUMBLE_TRIGGERS:
                m_InputHandler->rumbleTriggers((uint16_t)(uintptr_t)event.user.data1,
                                               (uint16_t)((uintptr_t)event.user.data2 >> 16),
                                               (uint16_t)((uintptr_t)event.user.data2 & 0xFFFF));
                break;
            case SDL_CODE_GAMECONTROLLER_SET_MOTION_EVENT_STATE:
                m_InputHandler->setMotionEventState((uint16_t)(uintptr_t)event.user.data1,
                                                    (uint8_t)((uintptr_t)event.user.data2 >> 16),
                                                    (uint16_t)((uintptr_t)event.user.data2 & 0xFFFF));
                break;
            case SDL_CODE_GAMECONTROLLER_SET_CONTROLLER_LED:
                m_InputHandler->setControllerLED((uint16_t)(uintptr_t)event.user.data1,
                                                 (uint8_t)((uintptr_t)event.user.data2 >> 16),
                                                 (uint8_t)((uintptr_t)event.user.data2 >> 8),
                                                 (uint8_t)((uintptr_t)event.user.data2));
                break;
            case SDL_CODE_GAMECONTROLLER_SET_ADAPTIVE_TRIGGERS:
                m_InputHandler->setAdaptiveTriggers((uint16_t)(uintptr_t)event.user.data1,
                                                    (DualSenseOutputReport *)event.user.data2);
                break;
            case SDL_CODE_XR_RECENTER:
                xrRecenter();
                break;
            case SDL_CODE_XR_POINTER: {
                const uint32_t packed = (uint32_t)(uintptr_t)event.user.data1;
                m_InputHandler->handleXrPointer((uint16_t)(packed >> 16), (uint16_t)(packed & 0xFFFF));
                break;
            }
            case SDL_CODE_XR_BUTTON:
                m_InputHandler->handleXrButton((int)(uintptr_t)event.user.data1, (uintptr_t)event.user.data2 != 0);
                break;
            case SDL_CODE_XR_KEY: {
                const uint32_t packed = (uint32_t)(uintptr_t)event.user.data1;
                m_InputHandler->handleXrKey((int)(packed & 0xFFFFu), (uintptr_t)event.user.data2 != 0,
                                            (uint8_t)((packed >> 16) & 0xFFu),
                                            (uint8_t)((packed >> 24) & 0xFFu));
                break;
            }
            case SDL_CODE_XR_SCROLL:
                m_InputHandler->handleXrScroll((int)(intptr_t)event.user.data1);
                break;
            case SDL_CODE_XR_ENDED:
                handleXrEnded((int)(intptr_t)event.user.data1);
                break;
            case SDL_CODE_XR_WHEEL: {
                // §VR-WHEEL：快捷鍵切換 → 記進設定（下一場沿用）並在頭盔裡提示
                const bool on = event.user.data1 != nullptr;
                m_Preferences->vrWheel = on;
                m_Preferences->save();
#ifdef HAVE_OPENXR
                if (m_XrContext != nullptr) {
                    // 提示框一次放得下約兩行：手把模式沒開時改提醒「要在 Xbox 手把模式才有作用」
                    const QString text = !on ? tr("Steering wheel: off (left stick steers)")
                                             : (m_VrPadModeOn ? tr("Steering wheel: on - turn both controllers like a wheel")
                                                              : tr("Steering wheel: on (works in Xbox controller mode)"));
                    m_XrContext->showToast(text, 3000);
                }
#endif
                break;
            }
#ifdef HAVE_OPENXR
            case SDL_CODE_XR_PAD: {
                // §XR-PADMODE：兩支控制器合成的 Xbox 手把狀態
                XrPadState* s = static_cast<XrPadState*>(event.user.data1);
                if (s != nullptr) {
                    m_InputHandler->handleXrGamepad(s->buttons, s->lt, s->rt, s->lx, s->ly, s->rx, s->ry);
                    SDL_free(s);
                }
                break;
            }
            case SDL_CODE_XR_PADMODE: {
                const bool on = event.user.data1 != nullptr;
                m_VrPadModeOn = on;
                if (m_XrContext != nullptr) {
                    m_XrContext->showToast(on ? tr("Xbox controller mode: on (hold both menu buttons to switch back)")
                                              : tr("Xbox controller mode: off (laser pointer)"),
                                           3000);
                }
                break;
            }
#endif
            case SDL_CODE_XR_REBUILD:
                handleXrRebuild((uint32_t)(uintptr_t)event.user.data1);
                break;
            case SDL_CODE_VR_MESSAGE:
                s_VrMsgQueued.fetch_sub(1, std::memory_order_relaxed);
                handleVrMessage((const uint8_t*)event.user.data2, (int)(uintptr_t)event.user.data1);
                SDL_free(event.user.data2);
                break;
            default:
                SDL_assert(false);
            }
            break;

        case SDL_WINDOWEVENT:
            // Early handling of some events
            switch (event.window.event) {
            case SDL_WINDOWEVENT_FOCUS_LOST:
                if (m_Preferences->muteOnFocusLoss) {
                    m_AudioMuted = true;
                }
                m_InputHandler->notifyFocusLost();
                break;
            case SDL_WINDOWEVENT_FOCUS_GAINED:
                if (m_Preferences->muteOnFocusLoss) {
                    m_AudioMuted = false;
                }
                m_InputHandler->notifyFocusGained();
                break;
            case SDL_WINDOWEVENT_LEAVE:
                m_InputHandler->notifyMouseLeave();
                break;
            }

            presence.runCallbacks();

            // Capture the mouse on SDL_WINDOWEVENT_ENTER if needed
            if (needsFirstEnterCapture && event.window.event == SDL_WINDOWEVENT_ENTER) {
                m_InputHandler->setCaptureActive(true);
                needsFirstEnterCapture = false;
            }

            // We want to recreate the decoder for resizes (full-screen toggles) and the initial shown event.
            // We use SDL_WINDOWEVENT_SIZE_CHANGED rather than SDL_WINDOWEVENT_RESIZED because the latter doesn't
            // seem to fire when switching from windowed to full-screen on X11.
            if (event.window.event != SDL_WINDOWEVENT_SIZE_CHANGED &&
                (event.window.event != SDL_WINDOWEVENT_SHOWN || m_VideoDecoder != nullptr)) {
                // Check that the window display hasn't changed. If it has, we want
                // to recreate the decoder to allow it to adapt to the new display.
                // This will allow Pacer to pull the new display refresh rate.
#if SDL_VERSION_ATLEAST(2, 0, 18)
                // On SDL 2.0.18+, there's an event for this specific situation
                if (event.window.event != SDL_WINDOWEVENT_DISPLAY_CHANGED) {
                    break;
                }
#else
                // Prior to SDL 2.0.18, we must check the display index for each window event
                if (SDL_GetWindowDisplayIndex(m_Window) == currentDisplayIndex) {
                    break;
                }
#endif
            }
#ifdef Q_OS_WIN32
            // We can get a resize event after being minimized. Recreating the renderer at that time can cause
            // us to start drawing on the screen even while our window is minimized. Minimizing on Windows also
            // moves the window to -32000, -32000 which can cause a false window display index change. Avoid
            // that whole mess by never recreating the decoder if we're minimized.
            else if (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_MINIMIZED) {
                break;
            }
#endif

            if (m_FlushingWindowEventsRef > 0) {
                // Ignore window events for renderer reset if flushing
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Dropping window event during flush: %d (%d %d)",
                            event.window.event,
                            event.window.data1,
                            event.window.data2);
                break;
            }

            // Allow the renderer to handle the state change without being recreated
            if (m_VideoDecoder) {
                bool forceRecreation = false;

                WINDOW_STATE_CHANGE_INFO windowChangeInfo = {};
                windowChangeInfo.window = m_Window;

                if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    windowChangeInfo.stateChangeFlags |= WINDOW_STATE_CHANGE_SIZE;

                    windowChangeInfo.width = event.window.data1;
                    windowChangeInfo.height = event.window.data2;
                }

                int newDisplayIndex = SDL_GetWindowDisplayIndex(m_Window);
                if (newDisplayIndex != currentDisplayIndex) {
                    windowChangeInfo.stateChangeFlags |= WINDOW_STATE_CHANGE_DISPLAY;

                    windowChangeInfo.displayIndex = newDisplayIndex;

                    // If the refresh rates have changed, we will need to go through the full
                    // decoder recreation path to ensure Pacer is switched to the new display
                    // and that we apply any V-Sync disablement rules that may be needed for
                    // this display.
                    SDL_DisplayMode oldMode, newMode;
                    if (SDL_GetCurrentDisplayMode(currentDisplayIndex, &oldMode) < 0 ||
                            SDL_GetCurrentDisplayMode(newDisplayIndex, &newMode) < 0 ||
                            oldMode.refresh_rate != newMode.refresh_rate) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "Forcing renderer recreation due to refresh rate change between displays");
                        forceRecreation = true;
                    }
                }

                if (!forceRecreation && m_VideoDecoder->notifyWindowChanged(&windowChangeInfo)) {
                    // Update the window display mode based on our current monitor
                    // NB: Avoid a useless modeset by only doing this if it changed.
                    if (newDisplayIndex != currentDisplayIndex) {
                        currentDisplayIndex = newDisplayIndex;
                        updateOptimalWindowDisplayMode();
                    }

                    break;
                }
            }

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Recreating renderer for window event: %d (%d %d)",
                        event.window.event,
                        event.window.data1,
                        event.window.data2);

            // Fall through
        case SDL_RENDER_DEVICE_RESET:

            if (event.type != SDL_WINDOWEVENT) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Recreating renderer by internal request: %d",
                            event.type);
            }

            // [VIPLE-STARTUP]：開始建（或重建）解碼器的時刻；開場的第一次就是視窗 SHOWN 之後這一次
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-STARTUP] t=%lld decoder (re)create begin", startupNowMs());

            SDL_LockMutex(m_DecoderLock);

            // Destroy the old decoder
            delete m_VideoDecoder;

            // Insert a barrier to discard any additional window events
            // that could cause the renderer to be and recreated again.
            // We don't use SDL_FlushEvent() here because it could cause
            // important events to be lost.
            flushWindowEvents();

            // Update the window display mode based on our current monitor
            // NB: Avoid a useless modeset by only doing this if it changed.
            if (currentDisplayIndex != SDL_GetWindowDisplayIndex(m_Window)) {
                currentDisplayIndex = SDL_GetWindowDisplayIndex(m_Window);
                updateOptimalWindowDisplayMode();
            }

            // Now that the old decoder is dead, flush any events it may
            // have queued to reset itself (if this reset was the result
            // of device loss or an internal error).
            SDL_PumpEvents();
            SDL_FlushEvent(SDL_RENDER_DEVICE_RESET);

            {
                // If the stream exceeds the display refresh rate (plus some slack),
                // forcefully disable V-sync to allow the stream to render faster
                // than the display.
                int displayHz = StreamUtils::getDisplayRefreshRate(m_Window);
                bool enableVsync = m_Preferences->enableVsync;
                // §P1-PACE 2026-08-27 — 幀節拍與 V-Sync 是正交的兩件事：
                // 節拍決定「幀何時送出」（Pacer 用 D3DKMTWaitForVerticalBlank
                // 等真實 vblank，見 dxvsyncsource.cpp，跟 swapchain present
                // 模式無關），V-Sync 決定「Present 要不要擋住等交換」。
                // 上游把兩者綁死（enableVsync && framePacing），結果使用者
                // 為了低延遲關掉 V-Sync，就連帶關掉唯一能吸收到達抖動的
                // 機制 —— log 實證：「Frame pacing disabled: target 60 Hz
                // with 60 FPS stream」，明明是 1:1 最適合節拍的情境。
                // 代價是 LAN 1080p60 有 25-75% 的 5 秒視窗 p99 幀間隔
                // > 33ms（60Hz 掉兩幀）；把樣本限縮到「server 全速交付」
                // 的 1238 個視窗仍有 42.8% 會頓，所以不是 server 少送。
                // 解耦後：節拍照 vblank 發幀（順），Present 仍走 tearing
                // 立即模式（不多等一次交換），兩邊的好處都拿到。
                bool enableFramePacing = m_Preferences->framePacing;
                if (displayHz + 5 < m_StreamConfig.fps) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Disabling V-sync because refresh rate limit exceeded");
                    enableVsync = false;
                    // 串流本來就快過螢幕時，節拍只會把它壓回螢幕速率，
                    // 與「讓它跑得比螢幕快」的初衷相反 —— 維持原行為關掉。
                    enableFramePacing = false;
                }

                // §P1-PACE 診斷：Pacer 自己只會印「Frame pacing (disabled):
                // target N Hz」，看不出「為什麼」關掉（是使用者沒開？還是
                // 被 V-Sync 閘擋掉？還是撞到刷新率上限？）。事後從 log 判斷
                // 節拍狀態時這三個輸入缺一不可，補一行把決策攤開。
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-PACE] pref=%d vsync=%d displayHz=%d streamFps=%d -> %s",
                            m_Preferences->framePacing ? 1 : 0,
                            enableVsync ? 1 : 0,
                            displayHz, m_StreamConfig.fps,
                            enableFramePacing ? "ENABLED" : "disabled");

                // Choose a new decoder (hopefully the same one, but possibly
                // not if a GPU was removed or something).
                if (!chooseDecoder(m_Preferences->videoDecoderSelection,
                                   m_Window, m_ActiveVideoFormat, m_ActiveVideoWidth,
                                   m_ActiveVideoHeight, m_ActiveVideoFrameRate,
                                   enableVsync,
                                   enableFramePacing,
                                   false,
                                   s_ActiveSession->m_VideoDecoder,
                                   s_ActiveSession->m_XrContext)) {
                    SDL_UnlockMutex(m_DecoderLock);
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "Failed to recreate decoder after reset");
                    emit displayLaunchError(tr("Unable to initialize video decoder. Please check your streaming settings and try again."));
                    goto DispatchDeferredCleanup;
                }

                // As of SDL 2.0.12, SDL_RecreateWindow() doesn't carry over mouse capture
                // or mouse hiding state to the new window. By capturing after the decoder
                // is set up, this ensures the window re-creation is already done.
                if (needsPostDecoderCreationCapture) {
                    m_InputHandler->setCaptureActive(true);
                    needsPostDecoderCreationCapture = false;
                }
            }

            // Request an IDR frame to complete the reset
            LiRequestIdrFrame();

            // Set HDR mode. We may miss the callback if we're in the middle
            // of recreating our decoder at the time the HDR transition happens.
            m_VideoDecoder->setHdrMode(LiGetCurrentHostDisplayHdrMode());

            // After a window resize, we need to reset the pointer lock region
            m_InputHandler->updatePointerRegionLock();

            SDL_UnlockMutex(m_DecoderLock);
            // [VIPLE-STARTUP]：解碼器建好、要了 IDR（真正送出是 common-c 的「IDR frame request sent」，那一行沒有毫秒）。
            // 放掉 m_DecoderLock 之後才印
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-STARTUP] t=%lld IDR requested (decoder ready)", startupNowMs());
            break;

        case SDL_KEYUP:
        case SDL_KEYDOWN:
            presence.runCallbacks();
            m_InputHandler->handleKeyEvent(&event.key);
#ifdef Q_OS_WIN32
            // Workaround: Windows IME can swallow Right Shift key events before they
            // reach SDL, because IME uses Right Shift as an input method toggle.
            // We use GetAsyncKeyState() to detect Right Shift state changes that SDL missed.
            {
                static bool s_RShiftWasDown = false;
                // §RSHIFT-DUP-FIX（2026-09-21）：SDL 正常送到的 RShift 事件也會走到這裡，
                // 上面 handleKeyEvent 已送一次，接著 GetAsyncKeyState 又看到按下 → 再合成
                // 一次 → host 收到 DOWN,DOWN,UP,UP（10:19 場 log 每次 RShift 都成雙）。
                // host 端中文輸入法把 RShift 當中英切換，按兩次等於沒切。本事件就是
                // RShift 時只同步狀態、不合成。
                if (event.key.keysym.scancode == SDL_SCANCODE_RSHIFT) {
                    s_RShiftWasDown = (event.type == SDL_KEYDOWN);
                    break;
                }
                bool rShiftIsDown = (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
                if (rShiftIsDown && !s_RShiftWasDown) {
                    // Right Shift pressed but SDL didn't report it — synthesize the event
                    SDL_KeyboardEvent synth = {};
                    synth.type = SDL_KEYDOWN;
                    synth.state = SDL_PRESSED;
                    synth.keysym.scancode = SDL_SCANCODE_RSHIFT;
                    synth.keysym.sym = SDLK_RSHIFT;
                    synth.keysym.mod = SDL_GetModState();
                    m_InputHandler->handleKeyEvent(&synth);
                } else if (!rShiftIsDown && s_RShiftWasDown) {
                    // Right Shift released but SDL didn't report it — synthesize the event
                    SDL_KeyboardEvent synth = {};
                    synth.type = SDL_KEYUP;
                    synth.state = SDL_RELEASED;
                    synth.keysym.scancode = SDL_SCANCODE_RSHIFT;
                    synth.keysym.sym = SDLK_RSHIFT;
                    synth.keysym.mod = SDL_GetModState();
                    m_InputHandler->handleKeyEvent(&synth);
                }
                s_RShiftWasDown = rShiftIsDown;
            }
#endif
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            presence.runCallbacks();
            if (xrOwnsMouse()) {
                break;  // §VR M3a X4：XR FOCUSED 期間滑鼠由射線負責，平面／gamescope 的滑鼠事件忽略
            }
            m_InputHandler->handleMouseButtonEvent(&event.button);
            break;
        case SDL_MOUSEMOTION:
            if (xrOwnsMouse()) {
                break;
            }
            m_InputHandler->handleMouseMotionEvent(&event.motion);
            break;
        case SDL_MOUSEWHEEL:
            if (xrOwnsMouse()) {
                break;
            }
            m_InputHandler->handleMouseWheelEvent(&event.wheel);
            break;
        case SDL_CONTROLLERAXISMOTION:
            m_InputHandler->handleControllerAxisEvent(&event.caxis);
            break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
            presence.runCallbacks();
            m_InputHandler->handleControllerButtonEvent(&event.cbutton);
            break;
#if SDL_VERSION_ATLEAST(2, 0, 14)
        case SDL_CONTROLLERSENSORUPDATE:
            m_InputHandler->handleControllerSensorEvent(&event.csensor);
            break;
        case SDL_CONTROLLERTOUCHPADDOWN:
        case SDL_CONTROLLERTOUCHPADUP:
        case SDL_CONTROLLERTOUCHPADMOTION:
            m_InputHandler->handleControllerTouchpadEvent(&event.ctouchpad);
            break;
#endif
#if SDL_VERSION_ATLEAST(2, 24, 0)
        case SDL_JOYBATTERYUPDATED:
            m_InputHandler->handleJoystickBatteryEvent(&event.jbattery);
            break;
#endif
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED:
            m_InputHandler->handleControllerDeviceEvent(&event.cdevice);
            break;
        case SDL_JOYDEVICEADDED:
            m_InputHandler->handleJoystickArrivalEvent(&event.jdevice);
            break;
        case SDL_FINGERDOWN:
        case SDL_FINGERMOTION:
        case SDL_FINGERUP:
            m_InputHandler->handleTouchFingerEvent(&event.tfinger);
            break;
        case SDL_DISPLAYEVENT:
            switch (event.display.event) {
            case SDL_DISPLAYEVENT_CONNECTED:
            case SDL_DISPLAYEVENT_DISCONNECTED:
                m_InputHandler->updatePointerRegionLock();
                break;
            }
            break;
        }
    }

DispatchDeferredCleanup:
    // §VR：主迴圈已結束，佇列裡還沒處理的 VR_S2C 副本（clVrMessage 以 SDL_malloc 複製）
    // 不會再被 handleVrMessage 釋放。連線執行緒還沒停，最多排 16 批就停，避免對方一直灌就走不到 LiStopConnection。
    // LiStopConnection 之後還會再排一次（那次不設上限）。
    freeQueuedUserPayloads(true);

    // §INPUT-STALL：收 watchdog
    s_StallWatchdogRun.store(false);
    s_LoopPhase.store(-2);
    if (stallWatchdog != nullptr) {
        SDL_WaitThread(stallWatchdog, nullptr);
        stallWatchdog = nullptr;
    }
    // Switch back to synchronous logging mode
    StreamUtils::exitAsyncLoggingMode();

    // Uncapture the mouse and hide the window immediately,
    // so we can return to the Qt GUI ASAP.
    m_InputHandler->setCaptureActive(false);
    SDL_EnableScreenSaver();
    SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "0");
    if (QGuiApplication::platformName() == "eglfs") {
        QGuiApplication::restoreOverrideCursor();
    }

    // Raise any keys that are still down
    m_InputHandler->raiseAllKeys();

    // Destroy the input handler now. This must be destroyed
    // before allowwing the UI to continue execution or it could
    // interfere with SDLGamepadKeyNavigation.
    delete m_InputHandler;
    m_InputHandler = nullptr;

    // Destroy the decoder, since this must be done on the main thread
    // NB: This must happen before LiStopConnection() for pull-based
    // decoders.
    SDL_LockMutex(m_DecoderLock);
    delete m_VideoDecoder;
    m_VideoDecoder = nullptr;
    SDL_UnlockMutex(m_DecoderLock);

    // Propagate state changes from the SDL window back to the Qt window
    //
    // NB: We're making a conscious decision not to propagate the maximized
    // or normal state of the window here. The thinking is that users may
    // routinely maximize the streaming window simply to view the stream
    // in a larger window, but they don't necessarily want the UI in such
    // a large window.
    if (!m_IsFullScreen && m_QtWindow != nullptr && m_Window != nullptr) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        if (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_MINIMIZED) {
            m_QtWindow->setWindowStates(m_QtWindow->windowStates() | Qt::WindowMinimized);
        }
        else if (m_QtWindow->windowStates() & Qt::WindowMinimized) {
            m_QtWindow->setWindowStates(m_QtWindow->windowStates() & ~Qt::WindowMinimized);
        }
#else
        if (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_MINIMIZED) {
            m_QtWindow->setWindowState(Qt::WindowMinimized);
        }
        else if (m_QtWindow->windowState() & Qt::WindowMinimized) {
            m_QtWindow->setWindowState(Qt::WindowNoState);
        }
#endif
    }

    // §VR M3a X1：XrContext 在 decoder 之後、視窗之前拆（設計 §2.4 ④；X2 起 XrRenderer 屬於
    // decoder，先刪 decoder 才能停 XR frame thread）
    destroyXrContext();
    releaseXrOwner();

    // This must be called after the decoder is deleted, because
    // the renderer may want to interact with the window
    SDL_DestroyWindow(m_Window);

    if (iconSurface != nullptr) {
        SDL_FreeSurface(iconSurface);
    }

    SDL_QuitSubSystem(SDL_INIT_VIDEO);

    // Cleanup can take a while, so dispatch it to a worker thread.
    // When it is complete, it will release our s_ActiveSessionSemaphore
    // reference.
    QThreadPool::globalInstance()->start(new DeferredSessionCleanupTask(this));
}


// §HOTSPOT-ADAPT 第 3 步（2026-11-20）：頭盔的熱點關了、這一場不開（CLOSED），或熱點沒開只問 host 能不能連（QUERY）。
// FrameHotspot 只在 server 有 VIPLE_VR_SERVER_CAP_HOTSPOT_STATUS 時才會交出要送的東西（舊 server 不認得這一則）
static void sendVrHotspotStateIfAny(FrameHotspot& hotspot)
{
    const int state = hotspot.takeStateToSend();
    if (state < 0) {
        return;
    }
    uint8_t tlv[2 + sizeof(VIPLE_VR_TLV_HOTSPOT_STATE)];
    tlv[0] = VIPLE_VR_C2S_HOTSPOT_STATE;
    tlv[1] = static_cast<uint8_t>(sizeof(VIPLE_VR_TLV_HOTSPOT_STATE));
    tlv[2] = 1;
    tlv[3] = static_cast<uint8_t>(state);
    if (LiSendVrMessage(tlv, sizeof(tlv), true) == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-HOTSPOT] sent hotspot state=%s to the host",
                    state == VIPLE_VR_HOTSPOT_STATE_QUERY ? "query" : "closed");
    }
    else {
        hotspot.stateSendFailed(state);  // 下一輪再送
    }
}

// §FRAME-HOTSPOT-AUTO：hotspot.json 裡的 SSID 與金鑰（呼叫端用完要清掉；這裡不印任何內容）
bool Session::readVrHotspotCreds(std::string* ssid, std::string* key)
{
    QString dataHome = qEnvironmentVariable("XDG_DATA_HOME");
    if (dataHome.isEmpty()) {
        dataHome = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    }
    if (dataHome.isEmpty()) {
        return false;
    }
    QFile file(QDir(dataHome).filePath(QStringLiteral("viplestream/hotspot.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    // §REVIEW-1119：這個檔正常只有幾百 bytes。readAll 遇到被換成大檔會一次配出整份。
    if (file.size() > 4096) {
        return false;
    }
    QByteArray raw = file.read(4096);
    const bool truncated = raw.size() == 4096 && !file.atEnd();
    file.close();
    if (truncated) {
        wipeQByteArray(raw);
        return false;
    }
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    wipeQByteArray(raw);
    // 自動開只認 6 GHz 的那一種（指令格式固定是 hbs6）
    if (!obj.value(QStringLiteral("band")).toString().contains(QStringLiteral("6G"), Qt::CaseInsensitive)) {
        return false;
    }
    *ssid = obj.value(QStringLiteral("ssid")).toString().toStdString();
    *key = obj.value(QStringLiteral("key")).toString().toStdString();
    if (ssid->empty() || key->empty()) {
        // 只寫進其中一欄就失敗時，另一欄的明文不能留在呼叫端的 string 裡
        wipeStdString(*ssid);
        wipeStdString(*key);
        return false;
    }
    return true;
}

void Session::checkAndSendVrHotspotInfo()
{
    // §REVIEW-1118：QByteArray 解構不會把熱點金鑰清成 0，含提前 return 都要抹掉
    struct ZQa {
        QByteArray b;
        ~ZQa()
        {
            if (b.size() > 0) {
                // constData() 不會再配置；data() 在解構裡 detach 可能丟例外
#if defined(_WIN32)
                SecureZeroMemory(const_cast<char*>(b.constData()), static_cast<size_t>(b.size()));
#else
                explicit_bzero(const_cast<char*>(b.constData()), static_cast<size_t>(b.size()));
#endif
            }
        }
    };

    // §VR-HOTSPOT-AUTO：VR session，或 §HOTSPOT-FLAT 成立的平面場次
    if (!(m_StreamConfig.vrFlags & VIPLE_VR_SF_ENABLED) && !m_HotspotFlat) {
        return;
    }
    // §HOTSPOT-ADAPT（2026-11-20）：熱點確定沒開（這台 host 最近連不上所以這一場不開、開不起來、等不到已經關掉）就不送，
    // 否則 host 整場對不存在的熱點重試、還會重啟網卡。已經送出去的那一則由 HOTSPOT_STATE CLOSED 收回（sendVrHotspotStateIfAny；舊 server 收不回來）
    if (!m_FrameHotspot.advertiseToHost()) {
        return;
    }

    // 取得 Flatpak 內路徑 $XDG_DATA_HOME/viplestream/hotspot.json
    QString dataHome = qEnvironmentVariable("XDG_DATA_HOME");
    if (dataHome.isEmpty()) {
        dataHome = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    }
    if (dataHome.isEmpty()) {
        return;
    }

    QString hotspotPath = QDir(dataHome).filePath(QStringLiteral("viplestream/hotspot.json"));
    QFile file(hotspotPath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return;
    }
    // §REVIEW-1119：同 readVrHotspotCreds，不要把整份檔讀進記憶體。
    if (file.size() > 4096) {
        return;
    }

    ZQa rawJson;
    rawJson.b = file.read(4096);
    const bool truncated = rawJson.b.size() == 4096 && !file.atEnd();
    file.close();
    if (truncated || rawJson.b.isEmpty()) {
        return;
    }

    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(rawJson.b, &parseErr);
    if (doc.isNull() || !doc.isObject()) {
        return;
    }

    QJsonObject obj = doc.object();
    QString ssid = obj.value(QStringLiteral("ssid")).toString();
    QString key = obj.value(QStringLiteral("key")).toString();
    QString bandStr = obj.value(QStringLiteral("band")).toString();
    int channel = obj.value(QStringLiteral("channel")).toInt(0);
    int authType = obj.value(QStringLiteral("auth_type")).toInt(VIPLE_VR_HOTSPOT_AUTH_WPA2_PSK);

    ZQa ssidBytes;
    ssidBytes.b = ssid.toUtf8();
    ZQa keyBytes;
    keyBytes.b = key.toUtf8();

    if (ssidBytes.b.isEmpty() || ssidBytes.b.size() > 32 || keyBytes.b.isEmpty() || keyBytes.b.size() > 64) {
        return;
    }

    VIPLE_VR_TLV_HOTSPOT_INFO info {};
    // §HOTSPOT-ADAPT 第 3 步：version 2＝這個 client 認得 S2C HOTSPOT_STATUS（格式不變）。只對宣告了能力的 server 送 2
    info.version = (m_Computer->vipleStreamVr & VIPLE_VR_SERVER_CAP_HOTSPOT_STATUS) ? VIPLE_VR_HOTSPOT_VERSION_2 : VIPLE_VR_HOTSPOT_VERSION_1;
    info.authType = static_cast<uint8_t>(authType);
    if (bandStr.contains(QStringLiteral("6G"), Qt::CaseInsensitive)) {
        info.band = VIPLE_VR_HOTSPOT_BAND_6GHZ;
    } else if (bandStr.contains(QStringLiteral("5G"), Qt::CaseInsensitive)) {
        info.band = VIPLE_VR_HOTSPOT_BAND_5GHZ;
    } else if (bandStr.contains(QStringLiteral("2"), Qt::CaseInsensitive)) {
        info.band = VIPLE_VR_HOTSPOT_BAND_2GHZ;
    } else {
        info.band = VIPLE_VR_HOTSPOT_BAND_6GHZ;
    }
    info.channel = static_cast<uint8_t>(channel);
    info.ssidLen = static_cast<uint8_t>(ssidBytes.b.size());
    info.keyLen = static_cast<uint8_t>(keyBytes.b.size());
    std::memcpy(info.ssid, ssidBytes.b.constData(), info.ssidLen);
    std::memcpy(info.key, keyBytes.b.constData(), info.keyLen);

    uint8_t tlv[2 + sizeof(VIPLE_VR_TLV_HOTSPOT_INFO)];
    tlv[0] = VIPLE_VR_C2S_HOTSPOT_INFO;
    tlv[1] = static_cast<uint8_t>(sizeof(info));
    std::memcpy(tlv + 2, &info, sizeof(info));

    // §REVIEW-1118：比對用的快取只留摘要，熱點金鑰不要在 Session 裡留到解構
    ZQa currentPayload;
    currentPayload.b = QByteArray(reinterpret_cast<const char*>(tlv), sizeof(tlv));
    const QByteArray payloadDigest = QCryptographicHash::hash(currentPayload.b, QCryptographicHash::Sha256);
    if (payloadDigest == m_LastVrHotspotPayload) {
        // 內容未變更，不重複發送；立即抹除暫存記憶體
#if defined(_WIN32)
        SecureZeroMemory(&info, sizeof(info));
        SecureZeroMemory(tlv, sizeof(tlv));
#else
        explicit_bzero(&info, sizeof(info));
        explicit_bzero(tlv, sizeof(tlv));
#endif
        return;
    }

    // 發送 reliable TLV
    if (LiSendVrMessage(tlv, sizeof(tlv), true) == 0) {
        m_LastVrHotspotPayload = payloadDigest;
        // 安全鐵律：log 只能印 SSID 的前 3 個字元＋***、金鑰長度
        QString prefix = ssid.left(3);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-HOTSPOT] sent hotspot info ssid=%s*** keylen=%d band=6GHz",
                    prefix.toUtf8().constData(),
                    static_cast<int>(keyBytes.b.size()));
    }

    // 抹除記憶體中的機密金鑰
#if defined(_WIN32)
    SecureZeroMemory(&info, sizeof(info));
    SecureZeroMemory(tlv, sizeof(tlv));
#else
    explicit_bzero(&info, sizeof(info));
    explicit_bzero(tlv, sizeof(tlv));
#endif
}
