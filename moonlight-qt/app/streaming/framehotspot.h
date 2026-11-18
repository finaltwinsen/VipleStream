// VipleStream §FRAME-HOTSPOT-PAUSE（2026-11-14）：遠端串流時暫停 Steam Frame 自己的串流轉接器熱點，結束時還原。
//
// Frame 只有一顆無線電。串流轉接器熱點（wlanap，6 GHz）開著時，頭盔連外面的 Wi-Fi（wlan0，2.4／5 GHz）要和它分時，
// 即使熱點上一個連線都沒有：2026-11-14 外地實測，每約 30 秒有 3～16 秒無線電只剩七、八成時間在 wlan0 上，兩端的延遲
// 同時變差、每次都被 host 當成壅塞砍一次碼率（整場卡在 2～4 Mbps、降到半速升不回來）。使用者手動關掉熱點之後週期消失、
// 整場全速、碼率目標爬到 9 Mbps。經 VPN 連回家時 host 不在頭盔旁邊，熱點本來就沒有人用。
//
// 做法和 Steam 設定頁的「啟用串流轉接器」開關一樣：對 softapmanager 的控制通道（/run/softapmanager.socket，0777）送
// "teardown"；還原時把關之前的那一則 "create\t<SSID>\t<金鑰>\t<頻段>\t…" 原樣送回去。那一則指令 softapmanager 自己存在
// /run/softapmanager.laststate（就地改寫、同一個 inode，沙箱裡看得到更新），所以這裡不必知道各欄位的意思。
// 協定（2026-11-14 在頭盔上確認）：SOCK_STREAM、一次連線一則指令、回一則；回覆以 tab 分隔，第一欄是數字：
//   "status"   → "0\tDisabled." 或 "2\tSoftAP Running\t<SSID>\t<金鑰>"
//   "teardown" → "1\tTearing down"（約 70 ms 後 wlanap 停下來）
//   "create…"  → "1\tCreating"（3 秒內熱點回到原來的頻道）
// Steam 不會輪詢熱點的狀態：我們關掉之後它不會自己重建，開回來之後它也不會關（實驗觀察 45 秒）；頭盔重開機時 Steam 會
// 照自己的設定重新送 create，所以最壞的情況（沒有還原）重開頭盔就恢復。
//
// 什麼時候暫停（三個條件都要成立，其餘一律不碰）：
//   1. 這一場經 VPN 連到 host（host 不可能連上頭盔的熱點）；
//   2. 熱點正在運作（使用者自己關掉的不碰，結束時也不會把它打開）；
//   3. 熱點上沒有任何對端（ARP 表裡 wlanap 沒有完成的項目）——在家裡經 VPN 位址連線、而 host 的網卡其實掛在熱點上時不關。
//
// 沒有還原就結束（被強制結束、當掉）時的保險：暫停前把還原用的指令和自己的 pid 寫到資料目錄的 hotspot-paused（0600）。
// 下一次有人呼叫 recoverStale()（VR 介面啟動時、串流子行程結束時、下一場開始前）看到那個 pid 已經不在就補做還原。
//
// §FRAME-HOTSPOT-AUTO（2026-11-15，11-16 改生命週期）：反過來的那一半。熱點關著（使用者在外面為了不讓它搶無線電而關掉）、
// 這一場不是經 VPN（host 在區網裡，可能就在旁邊）時，串流開始自動把熱點開起來，讓 host 的無線網卡照 §VR-HOTSPOT-AUTO 連上、
// 多連線熱加入；kJoinWaitS 秒內這台 host 沒有連上就關掉（空開著只會搶無線電）。
// 什麼時候關（11-16 使用者指定）：**不是**每一場串流結束時——連著玩幾個遊戲時每場都要重等 host 連上。關的時機是
//   1. App（VR 介面那個行程）結束：正常結束時它自己關（closeIfOwner）；
//   2. 換到另一台 host 開遊戲：那一台可能有自己的無線網卡，重新等它 kJoinWaitS 秒，等不到就關；
//   3. App 不正常結束：標記檔 hotspot-opened 沒有人再更新（擁有者每幾秒 touch 一次），下一次啟動的 recoverStale()、
//      或頭盔上常駐的 update-hotspot.sh（沙箱外，看得到同一個檔）發現它過期就補關。
// 擁有者：串流行程是 App 開的子行程時，擁有者是 App（父行程）；直接從命令列跑的串流行程自己就是 App，結束時關。
// 標記檔（0600，沒有機密）：第一行擁有者的 pid、第二行開給哪一台 host（uuid）。pid 只在同一個沙箱裡有意義，
// 所以「還活著」＝pid 在而且檔案最近 kOwnerFreshS 秒內被更新過。
// SSID 與金鑰來自 hotspot.json（頭盔上的 update-hotspot.sh 維護），指令格式是 2026-11-14 在頭盔上確認的
// "create\t<SSID>\t<金鑰>\thbs6\tnoroute\t--hidden"；softapmanager 不接受就什麼都不做。
//
// 安全：還原指令含熱點的金鑰。只放在記憶體與那個 0600 的檔案裡，任何 log 都不印它和 status 的回覆。
// Flatpak 要 --filesystem=/run/softapmanager.socket 與 --filesystem=/run/softapmanager.laststate:ro；沒有就什麼都不做。
#pragma once

#include <SDL_log.h>

#include <string>
#include <vector>

#if defined(__linux__)
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>
#endif

class FrameHotspot
{
public:
    ~FrameHotspot() { resume(); }

    // §FRAME-HOTSPOT-AUTO：不是經 VPN 的 VR 場次開始時呼叫。hostId＝這一場的 host（uuid）；ssid／key 會被清掉
    void openForLan(std::string& ssid, std::string& key, const std::string& hostId)
    {
#if defined(__linux__)
        std::string cmd;
        const bool fieldsOk = !ssid.empty() && ssid.size() <= 32 && key.size() >= 8 && key.size() <= 64 &&
                              ssid.find_first_of("\t\r\n") == std::string::npos && key.find_first_of("\t\r\n") == std::string::npos;
        if (fieldsOk) {
            cmd = "create\t" + ssid + "\t" + key + "\thbs6\tnoroute\t--hidden";
        }
        scrub(ssid);
        scrub(key);
        struct stat st;
        if (!fieldsOk || m_Opened || m_GaveUp || !m_RestoreCmd.empty() || stat(kSocket, &st) != 0) {
            scrub(cmd);
            return;
        }
        std::string reply;
        const bool asked = ask("status", &reply, 2000);
        const std::string state = firstField(reply);
        scrub(reply);
        if (!asked) {
            scrub(cmd);
            return;
        }
        const long owner = appPid();
        m_OwnerIsSelf = owner == static_cast<long>(getpid());
        m_Baseline.clear();
        if (state == "2") {
            scrub(cmd);
            // 已經開著。是我們之前開的（App 還在、連著玩下一個遊戲）就接手；不是就是使用者自己開的，不碰
            long pid = 0;
            std::string prevHost;
            if (!readOpenedMarker(&pid, &prevHost)) {
                return;
            }
            writeOpenedMarker(owner, hostId);
            m_Opened = true;
            m_OpenedAt = std::chrono::steady_clock::now();
            if (prevHost.empty()) {
                // §FRAME-HOTSPOT-EARLY：App 啟動時先開的（還沒有對應到哪一台 host）：上面已經有裝置就算這台連上了
                m_PeerSeen = hotspotHasNewPeer(m_Baseline);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VR-HOTSPOT] auto: the hotspot was switched on when the app started (%s)",
                            m_PeerSeen ? "a device is already connected to it" : "nothing connected to it yet - waiting for the host");
            }
            else if (prevHost == hostId) {
                m_PeerSeen = hotspotHasNewPeer(m_Baseline);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VR-HOTSPOT] auto: the hotspot is still on from the previous session with this host (%s)",
                            m_PeerSeen ? "host connected" : "waiting for the host again");
            }
            else {
                // 換了一台 host：上一台的網卡可能還掛在熱點上，不算數——等一個新的對端
                m_Baseline = hotspotPeers();
                m_PeerSeen = false;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VR-HOTSPOT] auto: the hotspot is on from a session with another host - waiting up to %d s for "
                            "this host to join (%d device(s) of the previous one ignored)",
                            kJoinWaitS, static_cast<int>(m_Baseline.size()));
            }
            return;
        }
        if (state != "0") {
            scrub(cmd);
            return;
        }
        writeOpenedMarker(owner, hostId);
        const bool ok = sendCreate(cmd);
        scrub(cmd);
        if (!ok) {
            removeOpenedMarker();
            m_GaveUp = true;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-HOTSPOT] auto: softapmanager did not accept create - hotspot left off");
            return;
        }
        m_Opened = true;
        m_PeerSeen = false;
        m_OpenedAt = std::chrono::steady_clock::now();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-HOTSPOT] auto: the host is on the local network and the streaming adapter hotspot was off - "
                    "switched it on; it is switched off again if the host does not join within %d s, %s",
                    kJoinWaitS, m_OwnerIsSelf ? "and when this session ends" : "and when the app exits (it stays on between games)");
#else
        (void) ssid;
        (void) key;
        (void) hostId;
#endif
    }

    // §FRAME-HOTSPOT-AUTO：串流中定期呼叫（約每 5 秒）。更新標記檔的時間；等對端，等不到就關掉
    void tick()
    {
#if defined(__linux__)
        if (!m_Opened) {
            return;
        }
        touchOpenedMarker();
        if (m_PeerSeen) {
            return;
        }
        const int waitedS = static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - m_OpenedAt).count());
        if (hotspotHasNewPeer(m_Baseline)) {
            m_PeerSeen = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-HOTSPOT] auto: the host joined the hotspot after %d s - keeping it on",
                        waitedS);
            return;
        }
        if (waitedS >= kJoinWaitS) {
            closeOpened("the host did not join the hotspot");
            m_GaveUp = true;
        }
#endif
    }

    // §FRAME-HOTSPOT-EARLY（2026-11-17）：App 啟動、看到已配對的 host 在區網上線時就先把熱點開起來（不等串流開始）。
    // host 的 Steam 開著時它自己會連這張網卡（熱點出現後 5～15 秒），使用者選遊戲的這段時間剛好讓它連好，開場就有兩條路徑。
    // 標記檔的 host 欄留空＝還沒有對應到哪一台；場次開始時 openForLan() 接手。kAppJoinWaitS 秒內沒有裝置連上、
    // 也沒有場次接手就關掉（host 的 Steam 沒開時沒有人會連，空開著只會分走無線電；場次開始時照原本的路徑再開）。
    // 會等 softapmanager 回覆（最久十幾秒），不要在介面執行緒呼叫。ssid／key 會被清掉
    static bool openAtAppStart(std::string& ssid, std::string& key)
    {
#if defined(__linux__)
        std::string cmd;
        const bool fieldsOk = !ssid.empty() && ssid.size() <= 32 && key.size() >= 8 && key.size() <= 64 &&
                              ssid.find_first_of("\t\r\n") == std::string::npos && key.find_first_of("\t\r\n") == std::string::npos;
        if (fieldsOk) {
            cmd = "create\t" + ssid + "\t" + key + "\thbs6\tnoroute\t--hidden";
        }
        scrub(ssid);
        scrub(key);
        struct stat st;
        const std::string paused = markerPath();
        // 有暫停標記＝經 VPN 的場次把熱點停掉了（或它的還原還沒補做）：不插手
        if (!fieldsOk || stat(kSocket, &st) != 0 || paused.empty() || stat(paused.c_str(), &st) == 0) {
            scrub(cmd);
            return false;
        }
        std::string reply;
        const bool asked = ask("status", &reply, 2000);
        const std::string state = firstField(reply);
        scrub(reply);
        if (!asked || state != "0") {
            scrub(cmd);  // 已經開著（使用者自己開的、或上一個 App 留下的）或正在變動：不碰
            return false;
        }
        writeOpenedMarker(static_cast<long>(getpid()), std::string());
        const bool ok = sendCreate(cmd);
        scrub(cmd);
        if (!ok) {
            removeOpenedMarker();
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-HOTSPOT] app: softapmanager did not accept create - hotspot left off");
            return false;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-HOTSPOT] app: a paired host is online on the local network and the streaming adapter hotspot was off - "
                    "switched it on ahead of the first game; it is switched off again if nothing joins within %d s, and when the app exits",
                    kAppJoinWaitS);
        return true;
#else
        (void) ssid;
        (void) key;
        return false;
#endif
    }

    // §FRAME-HOTSPOT-EARLY：openAtAppStart() 成功之後，App 每幾秒問一次
    enum class AppWait {
        Waiting,  // 還沒有裝置連上
        Joined,   // 有裝置連上了：留著
        Handed,   // 場次接手了（之後由它等、等不到由它關）
        Gone,     // 不在這個行程名下了（被關掉、被別人接走）
    };
    static AppWait appStartState()
    {
#if defined(__linux__)
        long pid = 0;
        std::string host;
        if (!readOpenedMarker(&pid, &host) || pid != static_cast<long>(getpid())) {
            return AppWait::Gone;
        }
        if (!host.empty()) {
            return AppWait::Handed;
        }
        return hotspotHasPeer() ? AppWait::Joined : AppWait::Waiting;
#else
        return AppWait::Gone;
#endif
    }

    // §FRAME-HOTSPOT-EARLY：等不到裝置：關掉（關之前再確認一次還是「沒人連、沒有場次接手」）。會等 softapmanager 回覆
    static void closeAppStart()
    {
#if defined(__linux__)
        if (appStartState() != AppWait::Waiting) {
            return;
        }
        std::string reply;
        const bool ok = ask("teardown", &reply, 5000) && firstField(reply) == "1";
        scrub(reply);
        if (ok) {
            removeOpenedMarker();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-HOTSPOT] app: nothing joined the hotspot within %d s - switched off again (a game on the local "
                        "network switches it on again)",
                        kAppJoinWaitS);
        }
        else {
            // 標記留著：App 結束時 closeIfOwner() 再關
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-HOTSPOT] app: nothing joined the hotspot, but softapmanager did not accept teardown - it is retried "
                        "when the app exits");
        }
#endif
    }

    static constexpr int kAppJoinWaitS = 60;  // §FRAME-HOTSPOT-EARLY：App 啟動時開了之後等裝置連上多久

    // §HOTSPOT-FLAT：熱點上現在有哪些裝置（IPv4 字串）。平面場次用它把 host 無線網卡的位址交給 MP-QUIC 當第二條路徑
    static std::vector<std::string> peerAddresses()
    {
#if defined(__linux__)
        return hotspotPeers();
#else
        return {};
#endif
    }

    // §FRAME-HOTSPOT-AUTO：App 的行程定期呼叫（約每 15 秒）：熱點是這個行程名下的就更新標記檔的時間
    static void heartbeat()
    {
#if defined(__linux__)
        long pid = 0;
        std::string host;
        if (readOpenedMarker(&pid, &host) && pid == static_cast<long>(getpid())) {
            touchOpenedMarker();
        }
#endif
    }

    // §FRAME-HOTSPOT-AUTO：App 正常結束時呼叫：熱點是這個行程名下的就關掉
    static void closeIfOwner()
    {
#if defined(__linux__)
        long pid = 0;
        std::string host;
        if (!readOpenedMarker(&pid, &host) || pid != static_cast<long>(getpid())) {
            return;
        }
        std::string reply;
        const bool asked = ask("status", &reply, 2000);
        const std::string state = firstField(reply);
        scrub(reply);
        if (!asked) {
            return;  // 標記留著：之後由 recoverStale() 或頭盔上的常駐腳本補關
        }
        if (state == "2" && !(ask("teardown", &reply, 5000) && firstField(reply) == "1")) {
            return;
        }
        removeOpenedMarker();
        if (state == "2") {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-HOTSPOT] auto: the app is exiting - hotspot switched off again");
        }
#endif
    }

    // 串流開始、擋 Wi-Fi 掃描之前呼叫。viaVpn＝這一場經 VPN 連到 host。
    // 不是經 VPN 的場次只做一件事：把上一個沒有還原就結束的行程留下的暫停補回來
    void pauseIfUnused(bool viaVpn)
    {
#if defined(__linux__)
        if (!viaVpn) {
            recoverStale();
            return;
        }
        if (!m_RestoreCmd.empty()) {
            return;
        }
        struct stat st;
        if (stat(kSocket, &st) != 0) {
            return;  // 不是 Frame，或沙箱沒有開這條路徑
        }
        std::string reply;
        if (!ask("status", &reply, 2000)) {
            return;
        }
        const std::string state = firstField(reply);
        scrub(reply);  // 運作中的回覆含 SSID 與金鑰
        if (state != "2") {
            // 熱點沒有在運作。可能是使用者自己關的（不碰），也可能是上一個行程暫停之後沒有還原：那一種接手，結束時還原
            adoptStale();
            return;
        }
        {
            // §FRAME-HOTSPOT-AUTO：開著的熱點是我們之前在區網場次自動開的（App 沒關、人帶著頭盔出門了）：直接關掉，不必還原
            long pid = 0;
            std::string host;
            if (readOpenedMarker(&pid, &host) && !hotspotHasPeer()) {
                if (ask("teardown", &reply, 5000) && firstField(reply) == "1") {
                    removeOpenedMarker();
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-VR-HOTSPOT] auto: remote session and the hotspot we switched on earlier has nothing on it - switched off");
                }
                scrub(reply);
                return;
            }
        }
        if (hotspotHasPeer()) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-HOTSPOT] remote session, but a device is connected to the headset hotspot - leaving it on");
            return;
        }
        std::string cmd = readRestoreCommand();
        if (cmd.empty()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-HOTSPOT] the hotspot is running but its create command cannot be read - leaving it on");
            return;
        }
        if (!writeMarker(cmd)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-HOTSPOT] cannot record the restore command (errno %d) - leaving the hotspot on", errno);
            scrub(cmd);
            return;
        }
        if (!ask("teardown", &reply, 5000) || firstField(reply) != "1") {
            removeMarker();
            scrub(cmd);
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-HOTSPOT] softapmanager did not accept teardown - hotspot left on");
            return;
        }
        m_RestoreCmd.swap(cmd);
        // 介面約 70 ms 後才停：等它，後面擋掃描的那一步才不會把 wlanap 也列進去
        int waitedMs = 0;
        while (waitedMs < 600 && interfaceUp(kApInterface)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            waitedMs += 25;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-HOTSPOT] streaming adapter hotspot paused for this remote session (it shares the headset's only radio "
                    "with the Wi-Fi link in use; nothing was connected to it); interface down after %d ms",
                    waitedMs);
#else
        (void) viaVpn;
#endif
    }

    // 串流結束時呼叫（解構時也會）。沒有暫停過就什麼都不做
    void resume()
    {
#if defined(__linux__)
        if (m_Opened) {
            if (m_OwnerIsSelf) {
                closeOpened("the session ended");
            }
            else {
                // App 還在：留著給下一個遊戲（App 結束、換 host 等不到、或 App 當掉時才關）
                touchOpenedMarker();
                m_Opened = false;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-HOTSPOT] auto: the session ended - hotspot left on until the app exits");
            }
        }
        if (m_RestoreCmd.empty()) {
            return;
        }
        const bool ok = sendCreate(m_RestoreCmd);
        scrub(m_RestoreCmd);
        if (ok) {
            removeMarker();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-HOTSPOT] streaming adapter hotspot restored");
        }
        else {
            // 標記留著：下一次 recoverStale() 再試
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-HOTSPOT] could not restore the streaming adapter hotspot now - it will be retried; it also comes "
                        "back when the headset restarts or when 'Enable streaming adapter' is switched off and on in the Steam settings");
        }
#endif
    }

    // 上一個行程暫停了熱點、沒有還原就結束：補做。那個行程還在跑（正在串流）就不動
    static void recoverStale()
    {
#if defined(__linux__)
        recoverStaleOpened();
        long pid = 0;
        std::string cmd;
        if (!readMarker(&pid, &cmd)) {
            return;
        }
        if (pid > 0 && pid != static_cast<long>(getpid()) && processAlive(pid)) {
            scrub(cmd);
            return;
        }
        std::string reply;
        const bool asked = ask("status", &reply, 2000);
        const std::string state = firstField(reply);
        scrub(reply);
        if (!asked) {
            scrub(cmd);
            return;  // softapmanager 連不上：標記留著，之後再試
        }
        if (state == "2") {
            removeMarker();  // 已經有人把它開回來了
            scrub(cmd);
            return;
        }
        const bool ok = sendCreate(cmd);
        scrub(cmd);
        if (ok) {
            removeMarker();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-HOTSPOT] streaming adapter hotspot restored (a previous run paused it and ended without restoring)");
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-HOTSPOT] a previous run paused the streaming adapter hotspot and it could not be restored yet");
        }
#endif
    }

private:
#if defined(__linux__)
    static constexpr const char* kSocket = "/run/softapmanager.socket";
    static constexpr const char* kLastState = "/run/softapmanager.laststate";
    static constexpr const char* kApInterface = "wlanap";
    static constexpr int kJoinWaitS = 45;    // §FRAME-HOTSPOT-AUTO：自動開了之後等這台 host 多久
    static constexpr int kOwnerFreshS = 60;  // 標記檔這麼久沒被更新＝擁有者不在了

    // §FRAME-HOTSPOT-AUTO：關掉這個行程開的熱點
    void closeOpened(const char* why)
    {
        std::string reply;
        const bool ok = ask("teardown", &reply, 5000) && firstField(reply) == "1";
        scrub(reply);
        m_Opened = false;
        if (ok) {
            removeOpenedMarker();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-HOTSPOT] auto: %s - hotspot switched off again", why);
        }
        else {
            // 標記留著：下一次 recoverStale() 再關
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-HOTSPOT] auto: %s, but softapmanager did not accept teardown - it will be retried", why);
        }
    }

    // 擁有者不在了（App 當掉、被強制結束）而熱點還開著：補關。擁有者是自己、或還活著就不動
    static void recoverStaleOpened()
    {
        long pid = 0;
        std::string host;
        if (!readOpenedMarker(&pid, &host)) {
            return;
        }
        if (pid == static_cast<long>(getpid())) {
            touchOpenedMarker();
            return;
        }
        if (pid > 0 && processAlive(pid) && openedMarkerAgeS() < kOwnerFreshS) {
            return;
        }
        std::string reply;
        if (!ask("status", &reply, 2000)) {
            scrub(reply);
            return;
        }
        const std::string state = firstField(reply);
        scrub(reply);
        if (state == "2" && !(ask("teardown", &reply, 5000) && firstField(reply) == "1")) {
            return;
        }
        removeOpenedMarker();
        if (state == "2") {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-HOTSPOT] auto: hotspot switched off (the app that switched it on ended without switching it off)");
        }
    }

    // 這個串流行程屬於哪一個 App 行程：父行程是同一支程式（VR 介面開的子行程）就是父行程，否則是自己
    static long appPid()
    {
        char self[64] = {}, parent[64] = {};
        char path[64];
        const long ppid = static_cast<long>(getppid());
        snprintf(path, sizeof(path), "/proc/%ld/comm", ppid);
        FILE* f = fopen("/proc/self/comm", "r");
        if (f != nullptr) {
            if (fgets(self, sizeof(self), f) == nullptr) {
                self[0] = '\0';
            }
            fclose(f);
        }
        f = fopen(path, "r");
        if (f != nullptr) {
            if (fgets(parent, sizeof(parent), f) == nullptr) {
                parent[0] = '\0';
            }
            fclose(f);
        }
        return (ppid > 1 && self[0] != '\0' && strcmp(self, parent) == 0) ? ppid : static_cast<long>(getpid());
    }

    static std::string openedMarkerPath()
    {
        const std::string p = markerPath();
        return p.empty() ? p : p.substr(0, p.size() - strlen("hotspot-paused")) + "hotspot-opened";
    }

    static void writeOpenedMarker(long owner, const std::string& hostId)
    {
        const std::string path = openedMarkerPath();
        if (path.empty()) {
            return;
        }
        const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd >= 0) {
            const std::string body = std::to_string(owner) + "\n" + hostId + "\n";
            if (write(fd, body.data(), body.size()) < 0) {
                // 寫不進去只是少了當掉時的保險
            }
            close(fd);
        }
    }

    static bool readOpenedMarker(long* pid, std::string* hostId)
    {
        const std::string path = openedMarkerPath();
        std::string s;
        if (path.empty() || !readSmallFile(path.c_str(), &s)) {
            return false;
        }
        *pid = strtol(s.c_str(), nullptr, 10);
        hostId->clear();
        const size_t nl = s.find('\n');
        if (nl != std::string::npos) {
            *hostId = s.substr(nl + 1);
            while (!hostId->empty() && (hostId->back() == '\n' || hostId->back() == '\r')) {
                hostId->pop_back();
            }
        }
        return *pid > 0;
    }

    static void touchOpenedMarker()
    {
        const std::string path = openedMarkerPath();
        if (!path.empty()) {
            utimensat(AT_FDCWD, path.c_str(), nullptr, 0);
        }
    }

    static long openedMarkerAgeS()
    {
        struct stat st;
        const std::string path = openedMarkerPath();
        if (path.empty() || stat(path.c_str(), &st) != 0) {
            return 1L << 30;
        }
        return static_cast<long>(time(nullptr) - st.st_mtime);
    }

    static void removeOpenedMarker()
    {
        const std::string path = openedMarkerPath();
        if (!path.empty()) {
            unlink(path.c_str());
        }
    }

    // 熱點沒在運作、而且有上一個行程留下的暫停：接手，這一場結束時還原
    void adoptStale()
    {
        long pid = 0;
        std::string cmd;
        if (!readMarker(&pid, &cmd)) {
            return;
        }
        if (pid > 0 && pid != static_cast<long>(getpid()) && processAlive(pid)) {
            scrub(cmd);
            return;
        }
        if (!writeMarker(cmd)) {
            scrub(cmd);
            return;
        }
        m_RestoreCmd.swap(cmd);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-HOTSPOT] streaming adapter hotspot was left paused by a previous run - keeping it paused for this "
                    "remote session and restoring it at the end");
    }

    static void scrub(std::string& s)
    {
        std::fill(s.begin(), s.end(), '\0');
        s.clear();
    }

    static std::string firstField(const std::string& reply)
    {
        const size_t tab = reply.find_first_of("\t\r\n");
        return tab == std::string::npos ? reply : reply.substr(0, tab);
    }

    static bool processAlive(long pid)
    {
        return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
    }

    static bool interfaceUp(const char* name)
    {
        char path[96];
        snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", name);
        char state[16] = {};
        FILE* f = fopen(path, "r");
        if (f == nullptr) {
            return false;
        }
        if (fgets(state, sizeof(state), f) == nullptr) {
            state[0] = '\0';
        }
        fclose(f);
        return strncmp(state, "up", 2) == 0;
    }

    // 一次連線一則指令、等一則回覆
    static bool ask(const std::string& cmd, std::string* reply, int timeoutMs)
    {
        reply->clear();
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return false;
        }
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        strncpy(sa.sun_path, kSocket, sizeof(sa.sun_path) - 1);
        bool ok = false;
        if (connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) == 0 &&
            send(fd, cmd.data(), cmd.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(cmd.size())) {
            struct pollfd p;
            p.fd = fd;
            p.events = POLLIN;
            p.revents = 0;
            if (poll(&p, 1, timeoutMs) > 0) {
                char buf[512];
                const ssize_t n = recv(fd, buf, sizeof(buf), 0);
                if (n > 0) {
                    reply->assign(buf, static_cast<size_t>(n));
                    ok = true;
                }
                memset(buf, 0, sizeof(buf));
            }
        }
        close(fd);
        return ok;
    }

    static bool sendCreate(const std::string& cmd)
    {
        std::string reply;
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (ask(cmd, &reply, 8000) && firstField(reply) == "1") {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }
        return false;
    }

    static bool validCreate(const std::string& cmd)
    {
        // "create\t<SSID>\t<金鑰>…"：至少三欄、單行、長度合理
        return cmd.size() > 10 && cmd.size() <= 600 && cmd.compare(0, 7, "create\t") == 0 &&
               cmd.find('\n') == std::string::npos && std::count(cmd.begin(), cmd.end(), '\t') >= 2;
    }

    static bool readSmallFile(const char* path, std::string* out)
    {
        out->clear();
        const int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return false;
        }
        char buf[1024];
        const ssize_t n = read(fd, buf, sizeof(buf));
        close(fd);
        if (n <= 0) {
            return false;
        }
        out->assign(buf, static_cast<size_t>(n));
        memset(buf, 0, sizeof(buf));
        return true;
    }

    static std::string readRestoreCommand()
    {
        std::string s;
        if (!readSmallFile(kLastState, &s)) {
            return std::string();
        }
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
            s.pop_back();
        }
        if (!validCreate(s)) {
            scrub(s);
        }
        return s;
    }

    // 熱點上的對端：ARP 表裡 wlanap 有完成的項目（Flags 不是 0x0）的位址。沙箱和系統共用網路命名空間，/proc/net/arp 是同一份
    static std::vector<std::string> hotspotPeers()
    {
        std::vector<std::string> peers;
        FILE* f = fopen("/proc/net/arp", "r");
        if (f == nullptr) {
            return peers;
        }
        char line[256];
        while (fgets(line, sizeof(line), f) != nullptr) {
            char ip[64], hw[32], flags[16], mac[32], mask[16], dev[32];
            if (sscanf(line, "%63s %31s %15s %31s %15s %31s", ip, hw, flags, mac, mask, dev) == 6 &&
                strcmp(dev, kApInterface) == 0 && strcmp(flags, "0x0") != 0) {
                peers.emplace_back(ip);
            }
        }
        fclose(f);
        return peers;
    }

    static bool hotspotHasPeer()
    {
        return !hotspotPeers().empty();
    }

    // 有沒有不在 baseline 裡的對端（baseline＝換 host 時上一台留在熱點上的裝置）
    static bool hotspotHasNewPeer(const std::vector<std::string>& baseline)
    {
        for (const std::string& ip : hotspotPeers()) {
            if (std::find(baseline.begin(), baseline.end(), ip) == baseline.end()) {
                return true;
            }
        }
        return false;
    }

    static std::string markerPath()
    {
        const char* base = getenv("XDG_DATA_HOME");
        std::string dir;
        if (base != nullptr && base[0] == '/') {
            dir = base;
        }
        else {
            const char* home = getenv("HOME");
            if (home == nullptr || home[0] != '/') {
                return std::string();
            }
            dir = std::string(home) + "/.local/share";
        }
        mkdir(dir.c_str(), 0700);
        dir += "/viplestream";
        mkdir(dir.c_str(), 0700);
        return dir + "/hotspot-paused";
    }

    // 內容：第一行 pid、第二行還原用的指令。0600；先寫暫存檔再 rename
    static bool writeMarker(const std::string& cmd)
    {
        const std::string path = markerPath();
        if (path.empty()) {
            errno = ENOENT;
            return false;
        }
        const std::string tmp = path + ".tmp";
        unlink(tmp.c_str());
        const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            return false;
        }
        std::string body = std::to_string(static_cast<long>(getpid())) + "\n" + cmd + "\n";
        const bool ok = write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size());
        scrub(body);
        close(fd);
        if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
            unlink(tmp.c_str());
            return false;
        }
        return true;
    }

    static bool readMarker(long* pid, std::string* cmd)
    {
        const std::string path = markerPath();
        std::string s;
        if (path.empty() || !readSmallFile(path.c_str(), &s)) {
            return false;
        }
        const size_t nl = s.find('\n');
        bool ok = false;
        if (nl != std::string::npos) {
            *pid = strtol(s.substr(0, nl).c_str(), nullptr, 10);
            *cmd = s.substr(nl + 1);
            while (!cmd->empty() && (cmd->back() == '\n' || cmd->back() == '\r')) {
                cmd->pop_back();
            }
            ok = validCreate(*cmd);
        }
        scrub(s);
        if (!ok) {
            scrub(*cmd);
            unlink(path.c_str());  // 內容不對的標記沒有用，留著只會每次都讀一遍
        }
        return ok;
    }

    static void removeMarker()
    {
        const std::string path = markerPath();
        if (!path.empty()) {
            unlink(path.c_str());
        }
    }

    std::string m_RestoreCmd;  // 暫停之前的 create 指令（含金鑰）；空＝這個行程沒有暫停熱點
    bool m_Opened = false;     // §FRAME-HOTSPOT-AUTO：這個行程把熱點開起來了
    bool m_PeerSeen = false;
    bool m_GaveUp = false;     // 這一場試過、沒有成功：不再試
    bool m_OwnerIsSelf = true; // 熱點記在這個行程名下（不是 App 的子行程）：這一場結束就關
    std::vector<std::string> m_Baseline;  // 換 host 時上一台留在熱點上的裝置，不算這一台連上
    std::chrono::steady_clock::time_point m_OpenedAt;
#endif
};
