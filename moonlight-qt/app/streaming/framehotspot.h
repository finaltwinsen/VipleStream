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
// 安全：還原指令含熱點的金鑰。只放在記憶體與那個 0600 的檔案裡，任何 log 都不印它和 status 的回覆。
// Flatpak 要 --filesystem=/run/softapmanager.socket 與 --filesystem=/run/softapmanager.laststate:ro；沒有就什麼都不做。
#pragma once

#include <SDL_log.h>

#include <string>

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
#include <unistd.h>
#endif

class FrameHotspot
{
public:
    ~FrameHotspot() { resume(); }

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

    // 熱點上有沒有對端：ARP 表裡 wlanap 有完成的項目（Flags 不是 0x0）。沙箱和系統共用網路命名空間，/proc/net/arp 是同一份
    static bool hotspotHasPeer()
    {
        FILE* f = fopen("/proc/net/arp", "r");
        if (f == nullptr) {
            return false;
        }
        char line[256];
        bool found = false;
        while (fgets(line, sizeof(line), f) != nullptr) {
            char ip[64], hw[32], flags[16], mac[32], mask[16], dev[32];
            if (sscanf(line, "%63s %31s %15s %31s %15s %31s", ip, hw, flags, mac, mask, dev) == 6 &&
                strcmp(dev, kApInterface) == 0 && strcmp(flags, "0x0") != 0) {
                found = true;
                break;
            }
        }
        fclose(f);
        return found;
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
#endif
};
