// VipleStream §WIFI-SCANBLOCK（2026-11-09）：串流期間請 Steam Frame 的 softapmanager 擋住 Wi-Fi 背景掃描。
//
// Frame 的家用 Wi-Fi（wlan0）與串流轉接器熱點（wlanap）共用同一顆無線晶片。NetworkManager 讓 wpa_supplicant 定期
// 背景掃描（bgscan simple:30:-65:300），一次掃描約 8～16 秒、期間晶片一直跳去別的頻道：11-09 實測不串流時 200 Hz ping
// 路由器，掃描那幾秒每 0.2 秒就卡 50～171 ms——就是無人場次裡 L1 那 100～190 ms 的空檔。
// Steam 內建串流（vrlink）的做法：連上 /run/softapmanager.streaming.socket，每張串流用的網卡送一則
// "streamon\t<網卡>"；softapmanager 依網卡頻段寫 cfg80211_block_scans（全 6 GHz＝6、混合＝1），並把 wlan0 的漫遊
// 門檻從 −75 改成 −110 dBm。socket 一關（含程式當掉）就自動全部解除。11-09 實測：手動觸發掃描時 ping 最大 170→11.8 ms。
//
// 協定（反組譯 softapmanager 得到）：一次 recv 一則、以 tab 分隔欄位；"streamon\t<if>" 加入、"streamoff\t<if>" 移除，
// 最多 8 張。socket 權限 0777，不用 root；Flatpak 要 --filesystem=/run/softapmanager.streaming.socket。
#pragma once

#include <SDL_log.h>

#include <string>
#include <vector>

#if defined(__linux__)
#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <chrono>
#endif

class WifiScanBlock
{
public:
    ~WifiScanBlock() { stop(); }

    void start()
    {
#if defined(__linux__)
        if (m_Fd >= 0) {
            return;
        }
        const char* path = "/run/softapmanager.streaming.socket";
        struct stat st;
        if (stat(path, &st) != 0) {
            return;  // 不是 Frame（或沙盒沒開這條權限）：什麼都不做
        }
        const std::vector<std::string> ifs = wirelessInterfaces();
        if (ifs.empty()) {
            return;
        }
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return;
        }
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
        if (connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) != 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-WIFI-SCANBLOCK] connect failed (errno %d)", errno);
            close(fd);
            return;
        }
        std::string list;
        for (size_t i = 0; i < ifs.size(); ++i) {
            const std::string msg = "streamon\t" + ifs[i];
            // softapmanager 一次 recv 處理一則（不看換行）：兩則之間留一點時間，免得黏成一次 recv
            if (i > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (send(fd, msg.data(), msg.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(msg.size())) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-WIFI-SCANBLOCK] send failed (errno %d)", errno);
                close(fd);
                return;
            }
            list += (list.empty() ? "" : ",") + ifs[i];
        }
        m_Fd = fd;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-WIFI-SCANBLOCK] background Wi-Fi scans blocked while streaming (%s)", list.c_str());
#endif
    }

    void stop()
    {
#if defined(__linux__)
        if (m_Fd >= 0) {
            close(m_Fd);  // softapmanager 看到連線關閉就全部解除
            m_Fd = -1;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-WIFI-SCANBLOCK] released");
        }
#endif
    }

private:
#if defined(__linux__)
    // 有 IP、是無線的網卡（/sys/class/net/<if>/wireless 存在且 operstate 是 up）
    static std::vector<std::string> wirelessInterfaces()
    {
        std::vector<std::string> out;
        DIR* d = opendir("/sys/class/net");
        if (d == nullptr) {
            return out;
        }
        while (struct dirent* e = readdir(d)) {
            if (e->d_name[0] == '.' || out.size() >= 8) {
                continue;
            }
            const std::string base = std::string("/sys/class/net/") + e->d_name;
            struct stat st;
            if (stat((base + "/wireless").c_str(), &st) != 0) {
                continue;
            }
            char state[16] = {};
            FILE* f = fopen((base + "/operstate").c_str(), "r");
            if (f != nullptr) {
                if (fgets(state, sizeof(state), f) == nullptr) {
                    state[0] = '\0';
                }
                fclose(f);
            }
            if (strncmp(state, "up", 2) == 0) {
                out.emplace_back(e->d_name);
            }
        }
        closedir(d);
        return out;
    }
#endif
    int m_Fd = -1;
};
