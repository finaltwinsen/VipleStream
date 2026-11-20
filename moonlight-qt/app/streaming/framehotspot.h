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

#include <Limelight.h>  // VipleVr.h：§HOTSPOT-ADAPT 的 VIPLE_VR_HS_*、VIPLE_VR_HOTSPOT_STATE_*

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
        if (m_HostId.empty()) {
            m_HostId = hostId;  // 下面提早返回時 tick() 也要知道是哪一台（使用者自己開的熱點連上了要清紀錄）
        }
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
        m_HostId = hostId;
        m_Verified = false;
        // §HOTSPOT-VERIFY（2026-11-19）：這台 host 不久前才等過一輪、沒有連上熱點（例如它的無線網卡掃不到頭盔的 6 GHz 熱點）：
        // 這一場不再開著等。熱點是我們開的（App 啟動時先開、或上一台 host 留下的）就關掉——上面掛著別台的網卡也一樣，
        // 它對這一場沒有用，卻要和家用 Wi-Fi 分同一顆無線電。
        // §HOTSPOT-ADAPT（2026-11-20）：多久之後再試看這台連續失敗幾次（noJoinBackoffS）
        int fails = 0;
        char cls = 'R';
        long recState = 0;
        long remainS = noJoinRemainingS(hostId, &fails, &cls, &recState);
        // §HOTSPOT-ADAPT 第 3 步：靜態原因（S 類：host 沒有閒著的無線介面、功能關閉、平台不支援）不用時間退避。
        // server 會回報時：熱點關著就不開、只送一則 QUERY，回 READY 才開（takeReopen）；熱點已經開著就照常送熱點資訊，
        // server 半秒內就有答案。舊 server 沒辦法問，S 類照 R 類的時間退避
        // §HOTSPOT-DISABLED-EXPIRE（2026-11-20 第二輪審查 C15）：DISABLED（host 關掉 vr_hotspot_autoconnect）例外——
        // host 的 Steam 仍可能自己連上（所以 deferStatic 對它一律延後），只靠 QUERY 的話熱點永遠不會再開、紀錄每場刷新、
        // 永遠不過期。所以 DISABLED 照 R 類的時間退避（10／30／90 分鐘、上限 4 小時）：退避期間照舊只送 QUERY
        // （READY＝設定被打開了，這一場就開；回 DISABLED 不刷新紀錄的時刻，見 onHostStatus），到期後照一般流程開熱點，
        // 讓 deferStatic 的延後判斷有機會成立；Steam 還是沒連上，closeForStatic 再記一筆、次數加一、退避拉長
        const bool disabledRec = cls == 'S' && recState == VIPLE_VR_HS_DISABLED;
        m_QueryRecState = recState;
        if (m_ServerReports && cls == 'S' && fails > 0 && (!disabledRec || remainS > 0)) {
            if (state == "0") {
                scrub(cmd);
                m_Advertise = false;
                m_QueryPending = true;
                m_QueryAt = std::chrono::steady_clock::now();
                m_StateToSend = VIPLE_VR_HOTSPOT_STATE_QUERY;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VR-HOTSPOT] auto: this host reported it has no way to join the hotspot (%d time(s)) - "
                            "not switching it on, asking the host first",
                            fails);
                return;
            }
            remainS = 0;
        }
        if (remainS > 0) {
            scrub(cmd);
            m_GaveUp = true;
            long pid = 0;
            std::string prevHost;
            const bool ours = state == "2" && readOpenedMarker(&pid, &prevHost);
            // 熱點資訊只在熱點確實開著時才送給 host：這裡剩下「使用者自己開的」那一種還開著（我們不碰它，host 連得上就清紀錄）
            m_Advertise = state == "2" && !ours;
            if (!m_Advertise) {
                m_StateToSend = VIPLE_VR_HOTSPOT_STATE_CLOSED;  // 上一場的看門狗可能還在跑：請 host 停手
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-HOTSPOT] auto: this host could not join the hotspot a short while ago (%d time(s) in a row) - "
                        "not switching it on for this session (it is tried again in about %ld min)%s",
                        fails, (remainS + 59) / 60,
                        m_Advertise ? "; the hotspot was switched on by the user and is left alone" : "");
            if (ours) {
                closeOpened("this host could not join the hotspot a short while ago");
            }
            return;
        }
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
            resetRound();
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
                            "[VIPLE-VR-HOTSPOT] auto: the hotspot is on from a session with another host - waiting about %d s (up to "
                            "%d s while the host reports it is still connecting) for this host to join (%d device(s) of the "
                            "previous one ignored)",
                            kJoinWaitS, kJoinWaitMaxS, static_cast<int>(m_Baseline.size()));
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
            m_Advertise = false;  // 熱點沒有開起來：不叫 host 來連
            m_StateToSend = VIPLE_VR_HOTSPOT_STATE_CLOSED;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-HOTSPOT] auto: softapmanager did not accept create - hotspot left off");
            return;
        }
        m_Opened = true;
        m_PeerSeen = false;
        m_OpenedAt = std::chrono::steady_clock::now();
        resetRound();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-HOTSPOT] auto: the host is on the local network and the streaming adapter hotspot was off - "
                    "switched it on; it is switched off again if the host does not join within about %d s (up to %d s while the "
                    "host reports it is still connecting), %s",
                    kJoinWaitS, kJoinWaitMaxS, m_OwnerIsSelf ? "and when this session ends" : "and when the app exits (it stays on between games)");
#else
        (void) ssid;
        (void) key;
        (void) hostId;
#endif
    }

    // §FRAME-HOTSPOT-AUTO：串流中定期呼叫（約每 5 秒）。更新標記檔的時間；等對端，等不到就關掉
    // §HOTSPOT-VERIFY（2026-11-19）：hostLinkOnHotspot＝這一場有沒有一條已確認的連線走熱點（1 有、0 沒有、-1 這一場沒有
    // 多連線、無從判斷）。能判斷時只認它：「熱點上有裝置」可能是另一台 host 的網卡（它存著連線設定檔，熱點一出現就自己
    // 連上來），11-19 實測連開發機的場次因此把熱點整場開著，家用 Wi-Fi 那條每隔幾秒掉一批影格。
    // §HOTSPOT-ADAPT（2026-11-20）：hostAddrs＝這台 host 自己回報的位址（/serverinfo 的網卡清單＋場次位址），只在沒有多連線
    // 的場次（-1）用：熱點上出現其中一個位址＝這台 host 的網卡確實掛在上面，不管它是不是早就在（換 host 時的 baseline 裡）。
    // 時間：這裡的間隔一律用單調時鐘（m_OpenedAt、m_LinkReadSince）；牆上時鐘只用在 hotspot-nojoin 的存檔
    void tick(int hostLinkOnHotspot = -1, const std::vector<std::string>& hostAddrs = std::vector<std::string>())
    {
#if defined(__linux__)
        if (m_QueryPending &&
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - m_QueryAt).count() >= kStatusWaitS) {
            m_QueryPending = false;
            m_GaveUp = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-HOTSPOT] auto: the host did not answer the query within %d s - hotspot left off for this session",
                        kStatusWaitS);
        }
        if (!m_Opened) {
            // 熱點不是這個行程開的（使用者自己開的）或已經放棄：不動它。但這一場確認有連線走熱點＝這台 host 現在連得上，
            // 把它的失敗紀錄清掉，之後恢復自動開（熱點是誰開的都一樣）
            if (hostLinkOnHotspot == 1 && !m_Verified && !m_HostId.empty()) {
                m_Verified = true;
                if (clearNoJoin(m_HostId)) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-VR-HOTSPOT] auto: this session has a link to the host over a hotspot this app did not switch "
                                "on - cleared the host's no-join record");
                }
            }
            return;
        }
        touchOpenedMarker();
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        const int waitedS = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(now - m_OpenedAt).count());
        if (m_ServerReports && m_StatusCount == 0 && !m_NoStatusLogged && waitedS >= kStatusWaitS && m_Advertise) {
            m_NoStatusLogged = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-HOTSPOT] auto: no status from the host after %d s - falling back to the %d s wait", waitedS, kJoinWaitS);
        }
        // §HOTSPOT-ADAPT（2026-11-20 審查）：期限依 host 最新的回報決定（joinDeadlineS），不再固定 kJoinWaitS
        const int deadlineS = joinDeadlineS();
        if (!m_ExtendLogged && !m_Verified && !m_PeerSeen && waitedS >= kJoinWaitS && deadlineS > waitedS) {
            m_ExtendLogged = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-HOTSPOT] auto: no link over the hotspot after %d s, but the host reports it is still getting there "
                        "(state=%d flags=0x%x) - waiting until %d s",
                        waitedS, m_HostState, m_HostFlags, deadlineS);
        }
        if (hostLinkOnHotspot >= 0) {
            if (m_Verified) {
                return;
            }
            if (!m_LinkReadValid) {
                m_LinkReadValid = true;
                m_LinkReadSince = now;
            }
            if (hostLinkOnHotspot == 1) {
                m_Verified = true;
                m_PeerSeen = true;
                m_StaticPending = 0;  // host 說它不會自己連，但連線是實測：以連線為準
                clearNoJoin(m_HostId);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VR-HOTSPOT] auto: this session has a link to the host over the hotspot (%d s) - keeping it on", waitedS);
                return;
            }
            // 多連線很晚才協商好（控制連線重連之類）時，第一次讀到 0 不算數：能判斷的讀數要連續 kLinkReadMinS 秒
            const int readS = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(now - m_LinkReadSince).count());
            if (waitedS >= deadlineS && readS >= kLinkReadMinS) {
                if (m_StaticPending != 0) {
                    closeForStatic(m_StaticPending, waitedS);  // 延後處理的靜態回報：短期限內沒有連線，這時才關、記 S 類
                    return;
                }
                const int others = static_cast<int>(hotspotPeers().size());
                int fails = 0;
                const long retryS = recordNoJoin(m_HostId, &fails);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VR-HOTSPOT] auto: no link to this host over the hotspot after %d s (%d other device(s) on it do not "
                            "count); %d time(s) in a row - not tried again for this host for %ld min",
                            waitedS, others, fails, retryS / 60);
                closeOpened("this host is not on the hotspot");
                m_GaveUp = true;
                m_Advertise = false;  // 之後不再把熱點資訊送給 host
                m_StateToSend = VIPLE_VR_HOTSPOT_STATE_CLOSED;  // 已經送出去的那一則：請 host 停手（舊 server 收不到，照舊重試）
                m_ClosedAtCount = m_StatusCount;
            }
            return;
        }
        m_LinkReadValid = false;
        if (m_PeerSeen) {
            return;
        }
        // §HOTSPOT-ADAPT 第 3 步：沒有多連線可以確認的場次，host 自己說連上了（這一場的 JOINED）也算數
        // 靜態回報延後處理中（m_StaticPending）：只認這台 host 自己的位址，「熱點上有別的裝置」不算（可能是別台 host 的網卡）
        const bool ownOnAp = hotspotHasAnyOf(hostAddrs);
        const bool ownAddr = ownOnAp || m_HostState == VIPLE_VR_HS_JOINED;
        if (ownAddr || (m_StaticPending == 0 && hotspotHasNewPeer(m_Baseline))) {
            m_PeerSeen = true;
            m_OwnAddrSeen = ownOnAp;
            m_StaticPending = 0;
            if (ownAddr) {
                clearNoJoin(m_HostId);
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-HOTSPOT] auto: the host joined the hotspot after %d s - keeping it on (%s)",
                        waitedS,
                        m_HostState == VIPLE_VR_HS_JOINED ? "the host reports it joined" :
                        ownAddr ? "one of this host's own addresses is on it" : "a device is on it");
            return;
        }
        if (waitedS >= deadlineS) {
            if (m_StaticPending != 0) {
                closeForStatic(m_StaticPending, waitedS);
                return;
            }
            closeOpened("the host did not join the hotspot");
            m_GaveUp = true;
            m_Advertise = false;
            m_StateToSend = VIPLE_VR_HOTSPOT_STATE_CLOSED;
            m_ClosedAtCount = m_StatusCount;
        }
#else
        (void) hostLinkOnHotspot;
        (void) hostAddrs;
#endif
    }

    // §HOTSPOT-ADAPT 第 3 步（2026-11-20）：這台 server 會不會回報熱點加入狀態（<VipleStreamVR> 有
    // VIPLE_VR_SERVER_CAP_HOTSPOT_STATUS）。openForLan() 之前呼叫。不會回報時下面幾個函式都不做事，行為和以前一樣
    void setServerReports(bool yes)
    {
#if defined(__linux__)
        m_ServerReports = yes;
#else
        (void) yes;
#endif
    }

    // §HOTSPOT-ADAPT 第 3 步：server 最新一則 HOTSPOT_STATUS（LiGetVrHotspotStatus）。每輪 tick() 之後呼叫；seq 沒變就不做事。
    // 回報只用來提早關與分類失敗，留不留熱點仍以 tick() 的連線確認為準：
    //   - 靜態原因（平台不支援、功能關閉、沒有可用的無線介面）→ 關掉這一場為它開的熱點，記 S 類。這幾種回報半秒內就到，
    //     一定比連線確認早，所以「server 不會主動連」不等於「這台 host 不會出現在熱點上」的情況要延後（deferStatic）：
    //     DISABLED（host 的 Steam 可能自己連）一律延後；其餘兩種只在熱點上已經有裝置時延後。延後＝等 kStaticWaitS 秒，
    //     這段時間內連線確認了就留著、不記；沒有才關、記 S 類（tick() 裡的 closeForStatic）；
    //   - 經 VPN → 立刻關，不記；
    //   - 判定連不上（不在範圍內／看得到但連不上）→ 立刻關，記 R 類（照退避表）；
    //   - 已連上、正在連 → 不關，只改期限（joinDeadlineS）：正在連而且有 SEEN 或 RESTARTED 時延到 kJoinWaitMaxS；已連上時
    //     給「連上後 kJoinConfirmS 秒」確認連線（不短於 kJoinWaitS、不長於 kJoinWaitMaxS）。沒有多連線的場次 tick(-1) 會把
    //     「已連上」當成這台連上了。
    // 冪等：每一則依當下的狀態判斷，重送的同一則、遲到的訊息都不會重複動作——
    //   - 這一場已經確認過連線（m_Verified）之後，任何否定的回報都不理（連線是實測，回報只是 host 的說法）；
    //   - 熱點不是這個行程開的（使用者自己開的、已經關掉）就不會再關一次、不會重複記失敗；
    //   - 熱點已經因為等不到或否定的回報關掉之後才收到「已連上」：這一場不重開（中途重開會把分時放進遊玩當中），
    //     只把這台的連續失敗次數降回 1，下一場（最短的退避到期後）再試。
    // 不印也不保存任何位址、SSID
    void onHostStatus(int state, int flags, int attempts, int restarts, int elapsedDs, unsigned reason, unsigned seq)
    {
#if defined(__linux__)
        if (!m_ServerReports || (m_StatusCount != 0 && seq == m_StatusSeq)) {
            return;
        }
        m_StatusSeq = seq;
        ++m_StatusCount;
        const bool changed = state != m_HostState || flags != m_HostFlags;
        const int prevState = m_HostState;
        m_HostState = state;
        m_HostFlags = flags;
        if (!changed) {
            return;  // 每 5 秒重送的同一則
        }
        // 第一次看到「已連上」的時刻（從開熱點起算的秒數）：joinDeadlineS() 從這裡起算確認連線的時間。離開 JOINED 就清掉
        if (state != VIPLE_VR_HS_JOINED) {
            m_JoinedAtS = -1;
        }
        else if (prevState != VIPLE_VR_HS_JOINED && m_Opened) {
            m_JoinedAtS = static_cast<int>(
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - m_OpenedAt).count());
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-HOTSPOT] status state=%d flags=0x%x attempts=%d restarts=%d elapsed=%d.%ds reason=0x%x", state, flags,
                    attempts, restarts, elapsedDs / 10, elapsedDs % 10, reason);
        const bool isStatic = state == VIPLE_VR_HS_UNSUPPORTED || state == VIPLE_VR_HS_DISABLED || state == VIPLE_VR_HS_NO_ADAPTER;
        const bool isVerdict = state == VIPLE_VR_HS_UNREACHABLE || state == VIPLE_VR_HS_FAILED;
        if (m_QueryPending) {
            // 熱點沒開、只問了一句的那種場次
            if (state == VIPLE_VR_HS_READY) {
                m_QueryPending = false;
                clearNoJoin(m_HostId);
                m_Advertise = true;
                m_Reopen = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VR-HOTSPOT] auto: the host now has a free wireless interface - record cleared, switching the hotspot on");
            }
            else if (isStatic || state == VIPLE_VR_HS_REMOTE) {
                m_QueryPending = false;
                m_GaveUp = true;
                // §HOTSPOT-DISABLED-EXPIRE：紀錄本來就是 DISABLED、這次也回 DISABLED＝沒有新的資訊，不刷新時刻（讓退避會到期）
                if (isStatic && !(state == VIPLE_VR_HS_DISABLED && m_QueryRecState == VIPLE_VR_HS_DISABLED)) {
                    int fails = 0;
                    recordNoJoin(m_HostId, &fails, 'S', state);
                }
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VR-HOTSPOT] auto: the host still cannot join (state %d) - hotspot left off for this session", state);
            }
            return;
        }
        if (!m_Opened) {
            if (state == VIPLE_VR_HS_JOINED && m_GaveUp && m_ClosedAtCount >= 0 && !m_LateJoinHandled && !m_HostId.empty()) {
                m_LateJoinHandled = true;
                const bool eased = easeNoJoin(m_HostId);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VR-HOTSPOT] auto: the host reports it joined after the hotspot was already switched off - not "
                            "switching it on again this session%s",
                            eased ? "; its failure count is back to 1, so the next try comes sooner" : "");
            }
            return;
        }
        // m_OwnAddrSeen：沒有多連線的場次已經在熱點上看到這台 host 自己的位址（那種場次的確認方式），同樣以實測為準
        if (m_Verified || m_OwnAddrSeen || !(isStatic || isVerdict || state == VIPLE_VR_HS_REMOTE)) {
            if (!isStatic) {
                m_StaticPending = 0;  // host 不再回報靜態原因（例如設定被打開、開始連了）：回到一般的期限
            }
            return;
        }
        const int waitedS =
            static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - m_OpenedAt).count());
        if (isStatic) {
            if (deferStatic(state)) {
                m_StaticPending = state;
                m_PeerSeen = false;  // 之前因為「熱點上有裝置」算數的不算了：要連線確認或這台自己的位址
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VR-HOTSPOT] auto: the host reports it will not join the hotspot by itself (state %d, %d s) - "
                            "something else on the host may still join: waiting up to %d s for a link before switching it off",
                            state, waitedS, kStaticWaitS);
                return;
            }
            closeForStatic(state, waitedS);
            return;
        }
        m_StaticPending = 0;
        if (state == VIPLE_VR_HS_REMOTE) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-HOTSPOT] auto: the host reports this session is remote for it (%d s) - no record", waitedS);
        }
        else {
            int fails = 0;
            const long retryS = recordNoJoin(m_HostId, &fails, 'R', state);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-HOTSPOT] auto: the host reports it cannot reach the hotspot (state %d: %s, %d s; %d time(s) in a "
                        "row) - not tried again for this host for %ld min",
                        state, state == VIPLE_VR_HS_FAILED ? "seen but cannot connect" : "not in range", waitedS, fails, retryS / 60);
        }
        closeOpened("the host reports it cannot join the hotspot");
        m_GaveUp = true;
        m_Advertise = false;
        m_StateToSend = VIPLE_VR_HOTSPOT_STATE_CLOSED;
        m_ClosedAtCount = m_StatusCount;
#else
        (void) state;
        (void) flags;
        (void) attempts;
        (void) restarts;
        (void) elapsedDs;
        (void) reason;
        (void) seq;
#endif
    }

    // §HOTSPOT-ADAPT 第 3 步：有沒有一則 C2S HOTSPOT_STATE 要送（VIPLE_VR_HOTSPOT_STATE_*；-1＝沒有）。拿走就算送了；
    // 送不出去時呼叫端用 stateSendFailed() 放回來，下一輪再送。server 不會回報（舊 server）時永遠是 -1
    int takeStateToSend()
    {
#if defined(__linux__)
        if (!m_ServerReports) {
            return -1;
        }
        const int st = m_StateToSend;
        m_StateToSend = -1;
        return st;
#else
        return -1;
#endif
    }

    void stateSendFailed(int state)
    {
#if defined(__linux__)
        if (m_StateToSend < 0) {
            m_StateToSend = state;  // 這期間有新的就以新的為準
        }
#else
        (void) state;
#endif
    }

    // §HOTSPOT-ADAPT 第 3 步：QUERY 得到 READY：呼叫端重新讀熱點的 SSID 與金鑰、再呼叫一次 openForLan()
    bool takeReopen()
    {
#if defined(__linux__)
        const bool yes = m_Reopen;
        m_Reopen = false;
        return yes;
#else
        return false;
#endif
    }

    // §HOTSPOT-ADAPT 第 3 步：還在等這台 host（熱點開了還沒確認、或問了還沒回）：呼叫端這段時間每秒輪詢一次，
    // 否定的回報才能在一秒內生效；其餘時間照舊每 5 秒
    bool waitingForHost() const
    {
#if defined(__linux__)
        return m_QueryPending || (m_Opened && !m_Verified && !m_PeerSeen);
#else
        return false;
#endif
    }

    // §HOTSPOT-ADAPT：這一場要不要把熱點資訊送給 host。熱點確定沒開（因為失敗紀錄不開、開不起來、等不到已經關掉、
    // 經 VPN 的場次暫停了）就不送，否則 host 整場對不存在的熱點重試、還會重啟網卡。狀態不明（不是 Frame、softapmanager
    // 沒回應、熱點正在變動）維持原本的行為：送
    bool advertiseToHost()
    {
#if defined(__linux__)
        const bool yes = m_Advertise && m_RestoreCmd.empty();
        if (!yes && !m_AdvertiseLogged) {
            m_AdvertiseLogged = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-HOTSPOT] auto: the hotspot is not on for this session - no (more) hotspot info is sent to the host");
        }
        return yes;
#else
        return true;
#endif
    }

    // §HOTSPOT-ADAPT：tick() 這一輪用不用得到 hostAddrs（用不到時呼叫端不必去拿）
    bool wantsHostAddrs() const
    {
#if defined(__linux__)
        return m_Opened && !m_PeerSeen;
#else
        return false;
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

    // §HOTSPOT-ADAPT 3.5（2026-11-20 審查）：App 啟動要不要先開熱點之前問一次。hostIds＝這台裝置已知的每一台 host（uuid）。
    // 回傳 true＝**每一台**都有還沒到期的「連不上熱點」紀錄（R 類還在退避中；S 類的平台不支援／沒有可用的無線介面）——
    // 這時先開只會白佔無線電最多 kAppJoinWaitS 秒，場次開始又被關掉。只要有一台沒有紀錄（包含清單是空的）就回 false：
    // App 啟動時還不知道要連哪一台，沒有把握就照原本的開。S 類的 DISABLED 不算：那種 host 的 Steam 可能自己連上來，
    // App 啟動先開正是它連得上的時機。只讀檔（很小），不要在介面執行緒呼叫
    static bool allHostsHeldOff(const std::vector<std::string>& hostIds)
    {
#if defined(__linux__)
        if (hostIds.empty()) {
            return false;
        }
        const std::vector<NoJoinEntry> entries = loadNoJoin();
        const long long now = static_cast<long long>(time(nullptr));
        for (const std::string& id : hostIds) {
            bool held = false;
            for (const NoJoinEntry& e : entries) {
                if (e.host != id) {
                    continue;
                }
                const long long ageS = now - e.at;
                if (ageS < 0 || ageS >= kNoJoinStaleS) {
                    break;  // 同 noJoinRemainingS：時鐘往回跳或紀錄太舊都當作沒有
                }
                held = e.cls == 'S' ? e.state != VIPLE_VR_HS_DISABLED : ageS < noJoinBackoffS(e.fails);
                break;
            }
            if (!held) {
                return false;
            }
        }
        return true;
#else
        (void) hostIds;
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
    // §HOTSPOT-ADAPT 2.3（2026-11-20 審查補上）：host 回報還在路上時的期限
    static constexpr int kJoinWaitMaxS = 75;   // 正在連而且有 SEEN／RESTARTED、或已連上還沒確認連線：最多等到這裡
    static constexpr int kJoinConfirmS = 20;   // host 說連上之後給多久確認連線（1 Hz 的 LINK_ADDRS＋PING/PONG）
    static constexpr int kStaticWaitS = 20;    // 延後處理的靜態回報等多久（host 的 Steam 自己連要 4～14 秒，加上確認連線）
    static constexpr int kOwnerFreshS = 60;  // 標記檔這麼久沒被更新＝擁有者不在了
    static constexpr int kLinkReadMinS = 10;   // 多連線的讀數至少連續這麼久是「沒有」才關
    static constexpr int kStatusWaitS = 5;     // §HOTSPOT-ADAPT 第 3 步：會回報的 server 這麼久沒有任何一則＝退回 kJoinWaitS／不問了
    static constexpr int kNoJoinMaxHosts = 8;  // §HOTSPOT-ADAPT：hotspot-nojoin 最多記幾台（超過丟最舊的）
    static constexpr int kNoJoinMaxFails = 99;
    static constexpr long kNoJoinStaleS = 7L * 24 * 3600;  // 紀錄這麼久沒更新就當作沒有

    // §HOTSPOT-ADAPT：連續失敗 n 次之後隔多久才再為這台 host 開一次熱點：10 → 30 → 90 分鐘 → 上限 4 小時
    static long noJoinBackoffS(int fails)
    {
        return fails <= 1 ? 600L : fails == 2 ? 1800L : fails == 3 ? 5400L : 14400L;
    }

    // §HOTSPOT-ADAPT 2.3：這一輪等這台 host 到第幾秒（從開熱點起算）。舊 server、還沒有任何回報＝kJoinWaitS（原本的行為）。
    // 上限固定 kJoinWaitMaxS：回報中斷、狀態停在舊的 TRYING 也不會一直等。否定的回報不經過這裡（onHostStatus 直接關）
    int joinDeadlineS() const
    {
        if (m_StaticPending != 0) {
            return kStaticWaitS;
        }
        if (!m_ServerReports || m_StatusCount == 0) {
            return kJoinWaitS;
        }
        if (m_HostState == VIPLE_VR_HS_JOINED) {
            // 例：重啟網卡後第 33 秒才連上 → 53 秒；第 8 秒連上 → 仍是 45 秒
            const int d = (m_JoinedAtS > 0 ? m_JoinedAtS : 0) + kJoinConfirmS;
            return d < kJoinWaitS ? kJoinWaitS : d > kJoinWaitMaxS ? kJoinWaitMaxS : d;
        }
        if (m_HostState == VIPLE_VR_HS_TRYING && (m_HostFlags & (VIPLE_VR_HS_F_SEEN | VIPLE_VR_HS_F_RESTARTED)) != 0) {
            return kJoinWaitMaxS;
        }
        return kJoinWaitS;
    }

    // 新的一輪（openForLan 把熱點開起來或接手）：上一輪留下的判斷歸零。host 的最新回報（m_HostState／m_HostFlags）不動，
    // 下一則進來自然會換掉
    void resetRound()
    {
        m_JoinedAtS = -1;
        m_StaticPending = 0;
        m_OwnAddrSeen = false;
        m_ExtendLogged = false;
    }

    // 靜態回報要不要延後（見 onHostStatus 的說明）。DISABLED＝server 不主動連，host 的 Steam 仍可能自己連上來；
    // 其餘兩種沒有人會替 host 連，只有熱點上這時已經有裝置（可能就是它，例如 App 啟動先開、選遊戲時連上的）才等
    bool deferStatic(int state) const
    {
        return state == VIPLE_VR_HS_DISABLED || hotspotHasNewPeer(m_Baseline);
    }

    // 靜態原因確定成立：關掉這一場為它開的熱點、記 S 類、請 host 停手
    void closeForStatic(int state, int waitedS)
    {
        int fails = 0;
        recordNoJoin(m_HostId, &fails, 'S', state);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-HOTSPOT] auto: the host reports it has no way to join the hotspot (state %d, %d s; %d time(s) in a "
                    "row) - recorded as static: the next session asks the host before switching the hotspot on",
                    state, waitedS, fails);
        closeOpened("the host reports it cannot join the hotspot");
        m_StaticPending = 0;
        m_GaveUp = true;
        m_Advertise = false;
        m_StateToSend = VIPLE_VR_HOTSPOT_STATE_CLOSED;
        m_ClosedAtCount = m_StatusCount;
    }

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

    // §HOTSPOT-VERIFY／§HOTSPOT-ADAPT：「這台 host 等過、沒有連上熱點」的紀錄（0600，沒有機密）。串流行程每場都是新的行程，
    // 所以放檔案。每行一台、tab 分隔，最多 kNoJoinMaxHosts 台：
    //   <host uuid>\t<類別 S|R>\t<連續失敗次數>\t<最後一次失敗的 epoch 秒>\t<最後的 state>
    // 類別：S＝host 回報的靜態原因（VIPLE_VR_HS_UNSUPPORTED／DISABLED／NO_ADAPTER），R＝收不到（host 判定連不上、或等不到）；
    // state＝host 最後回報的 VIPLE_VR_HS_*（等不到的那一種是 0）。S 類在會回報的 server 上不看時間（每場先問），其餘照退避。
    // 時間是牆上時鐘（要跨行程、跨重開機）；下次可重試的時刻＝最後一次失敗＋noJoinBackoffS(次數)。時鐘往回跳（年齡為負）
    // 或紀錄超過 kNoJoinStaleS 都當作到期。舊格式（只有一行 uuid、時間看檔案的 mtime）讀成一筆、失敗 1 次。
    struct NoJoinEntry {
        std::string host;
        char cls = 'R';
        int fails = 1;
        long long at = 0;  // 最後一次失敗（epoch 秒）
        long state = 0;
    };

    static std::string noJoinPath()
    {
        const std::string p = markerPath();
        return p.empty() ? p : p.substr(0, p.size() - strlen("hotspot-paused")) + "hotspot-nojoin";
    }

    static bool validHostId(const std::string& h)
    {
        return !h.empty() && h.size() <= 64 && h.find_first_of("\t\r\n") == std::string::npos;
    }

    // 整串都是十進位數字才算數（不用會丟例外的 std::stoi／stoll）
    static bool parseNum(const std::string& s, long long* out)
    {
        if (s.empty() || s.size() > 18) {
            return false;
        }
        long long v = 0;
        for (const char c : s) {
            if (c < '0' || c > '9') {
                return false;
            }
            v = v * 10 + (c - '0');
        }
        *out = v;
        return true;
    }

    // 讀不到、內容不對的行一律跳過（當作沒有那一筆）
    static std::vector<NoJoinEntry> loadNoJoin()
    {
        std::vector<NoJoinEntry> out;
        const std::string path = noJoinPath();
        struct stat st;
        std::string s;
        if (path.empty() || stat(path.c_str(), &st) != 0 || !readSmallFile(path.c_str(), &s)) {
            return out;
        }
        size_t pos = 0;
        while (pos < s.size() && static_cast<int>(out.size()) < kNoJoinMaxHosts) {
            size_t nl = s.find('\n', pos);
            if (nl == std::string::npos) {
                nl = s.size();
            }
            std::string line = s.substr(pos, nl - pos);
            pos = nl + 1;
            while (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            std::vector<std::string> f;
            size_t a = 0;
            for (;;) {
                const size_t tab = line.find('\t', a);
                if (tab == std::string::npos) {
                    f.push_back(line.substr(a));
                    break;
                }
                f.push_back(line.substr(a, tab - a));
                a = tab + 1;
            }
            NoJoinEntry e;
            e.host = f[0];
            if (!validHostId(e.host)) {
                continue;
            }
            if (f.size() == 1) {
                // 舊格式：整個檔只有一行 uuid，時間是檔案的 mtime。新格式的檔裡出現沒有 tab 的行＝壞掉的行，跳過
                if (!out.empty() || s.find_first_not_of("\r\n", pos < s.size() ? pos : s.size()) != std::string::npos) {
                    continue;
                }
                e.at = static_cast<long long>(st.st_mtime);
            }
            else {
                long long fails = 0, at = 0, state = 0;
                if (f.size() < 4 || f[1].size() != 1 || (f[1][0] != 'S' && f[1][0] != 'R') || !parseNum(f[2], &fails) ||
                    !parseNum(f[3], &at) || fails < 1 || at <= 0) {
                    continue;
                }
                if (f.size() >= 5 && parseNum(f[4], &state) && state <= 255) {
                    e.state = static_cast<long>(state);
                }
                e.cls = f[1][0];
                e.fails = static_cast<int>(fails > kNoJoinMaxFails ? kNoJoinMaxFails : fails);
                e.at = at;
            }
            bool dup = false;
            for (const NoJoinEntry& o : out) {
                dup = dup || o.host == e.host;
            }
            if (!dup) {
                out.push_back(e);
            }
        }
        return out;
    }

    // 先寫暫存檔再 rename（讀的人不會看到寫到一半的內容）；一筆都沒有就把檔案刪掉
    static void saveNoJoin(std::vector<NoJoinEntry>& entries)
    {
        const std::string path = noJoinPath();
        if (path.empty()) {
            return;
        }
        if (entries.empty()) {
            unlink(path.c_str());
            return;
        }
        while (static_cast<int>(entries.size()) > kNoJoinMaxHosts) {
            size_t oldest = 0;
            for (size_t i = 1; i < entries.size(); ++i) {
                if (entries[i].at < entries[oldest].at) {
                    oldest = i;
                }
            }
            entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(oldest));
        }
        std::string body;
        for (const NoJoinEntry& e : entries) {
            body += e.host + "\t" + std::string(1, e.cls) + "\t" + std::to_string(e.fails) + "\t" + std::to_string(e.at) + "\t" +
                    std::to_string(e.state) + "\n";
        }
        const std::string tmp = path + ".tmp";
        unlink(tmp.c_str());
        const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            return;  // 寫不進去只是下一場會再等一輪
        }
        const bool ok = write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size());
        close(fd);
        if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
            unlink(tmp.c_str());
        }
    }

    // 這台 host 還要等多久才再為它開熱點（秒；0＝沒有紀錄或已經到期）。fails 回傳目前的連續失敗次數
    static long noJoinRemainingS(const std::string& hostId, int* fails, char* cls = nullptr, long* state = nullptr)
    {
        *fails = 0;
        if (state != nullptr) {
            *state = 0;
        }
        if (!validHostId(hostId)) {
            return 0;
        }
        const long long now = static_cast<long long>(time(nullptr));
        for (const NoJoinEntry& e : loadNoJoin()) {
            if (e.host != hostId) {
                continue;
            }
            const long long ageS = now - e.at;
            if (ageS < 0 || ageS >= kNoJoinStaleS) {
                return 0;
            }
            *fails = e.fails;
            if (cls != nullptr) {
                *cls = e.cls;
            }
            if (state != nullptr) {
                *state = e.state;
            }
            const long waitS = noJoinBackoffS(e.fails);
            return ageS < waitS ? static_cast<long>(waitS - ageS) : 0;
        }
        return 0;
    }

    // 這台 host 又一次沒有連上：連續失敗次數加一（紀錄太舊就從 1 算起）。回傳接下來要等多久（秒）
    static long recordNoJoin(const std::string& hostId, int* fails, char cls = 'R', long state = 0)
    {
        *fails = 1;
        if (!validHostId(hostId)) {
            return noJoinBackoffS(1);
        }
        const long long now = static_cast<long long>(time(nullptr));
        std::vector<NoJoinEntry> entries = loadNoJoin();
        NoJoinEntry mine;
        mine.host = hostId;
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].host == hostId) {
                const long long ageS = now - entries[i].at;
                if (ageS >= 0 && ageS < kNoJoinStaleS) {
                    mine.fails = entries[i].fails >= kNoJoinMaxFails ? kNoJoinMaxFails : entries[i].fails + 1;
                }
                entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
        mine.at = now;
        mine.cls = cls;
        mine.state = state;
        *fails = mine.fails;
        entries.push_back(mine);
        saveNoJoin(entries);
        return noJoinBackoffS(mine.fails);
    }

    // §HOTSPOT-ADAPT 第 3 步：熱點關掉之後 host 才說連上了：連續失敗次數降回 1（時間不動）。回傳有沒有改到
    static bool easeNoJoin(const std::string& hostId)
    {
        std::vector<NoJoinEntry> entries = loadNoJoin();
        for (NoJoinEntry& e : entries) {
            if (e.host == hostId) {
                if (e.fails <= 1) {
                    return false;
                }
                e.fails = 1;
                saveNoJoin(entries);
                return true;
            }
        }
        return false;
    }

    // 這台 host 連上了：整筆刪掉。回傳原本有沒有紀錄
    static bool clearNoJoin(const std::string& hostId)
    {
        std::vector<NoJoinEntry> entries = loadNoJoin();
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].host == hostId) {
                entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i));
                saveNoJoin(entries);
                return true;
            }
        }
        return false;
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

    // 熱點上有沒有 addrs 裡的任何一個位址（addrs＝這台 host 自己回報的位址）
    static bool hotspotHasAnyOf(const std::vector<std::string>& addrs)
    {
        if (addrs.empty()) {
            return false;
        }
        for (const std::string& ip : hotspotPeers()) {
            if (std::find(addrs.begin(), addrs.end(), ip) != addrs.end()) {
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
    bool m_Verified = false;   // §HOTSPOT-VERIFY：這一場確認過有一條連線走熱點
    std::string m_HostId;      // 這一場的 host（uuid）
    bool m_OwnerIsSelf = true; // 熱點記在這個行程名下（不是 App 的子行程）：這一場結束就關
    std::vector<std::string> m_Baseline;  // 換 host 時上一台留在熱點上的裝置，不算這一台連上
    std::chrono::steady_clock::time_point m_OpenedAt;
    bool m_Advertise = true;         // §HOTSPOT-ADAPT：false＝熱點確定沒開，不把熱點資訊送給 host
    bool m_AdvertiseLogged = false;
    bool m_LinkReadValid = false;    // 多連線的讀數（≥0）目前連續中
    // §HOTSPOT-ADAPT 第 3 步：server 的回報
    bool m_ServerReports = false;    // server 有 VIPLE_VR_SERVER_CAP_HOTSPOT_STATUS
    int m_HostState = 0;             // 最新一則的 VIPLE_VR_HS_*；0＝還沒收到
    int m_HostFlags = 0;
    int m_JoinedAtS = -1;            // 第一次看到 JOINED 是開熱點後第幾秒；-1＝目前不是 JOINED
    int m_StaticPending = 0;         // 延後處理中的靜態回報（VIPLE_VR_HS_*）；0＝沒有
    bool m_OwnAddrSeen = false;      // 沒有多連線的場次：在熱點上看到這台 host 自己的位址了
    bool m_ExtendLogged = false;
    unsigned m_StatusSeq = 0;
    int m_StatusCount = 0;           // 這一場收到幾則（含重送）
    bool m_NoStatusLogged = false;
    int m_StateToSend = -1;          // 要送給 host 的 VIPLE_VR_HOTSPOT_STATE_*；-1＝沒有
    bool m_QueryPending = false;     // 送了 QUERY、還沒有答案（熱點沒開）
    std::chrono::steady_clock::time_point m_QueryAt;
    long m_QueryRecState = 0;        // 這一場開始時 hotspot-nojoin 裡這台的 state（§HOTSPOT-DISABLED-EXPIRE）
    bool m_Reopen = false;           // QUERY 得到 READY：請呼叫端再開一次
    int m_ClosedAtCount = -1;        // 這一場為這台開的熱點被關掉時已經收到幾則回報；-1＝沒有關過
    bool m_LateJoinHandled = false;
    std::chrono::steady_clock::time_point m_LinkReadSince;  // 連續讀數從什麼時候開始
#endif
};
