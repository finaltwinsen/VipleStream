// VipleStream 2.0 §VR-RATE-AUTO（2026-11-08）：網路不穩時自動把 VR 串流降成半速（host 每兩張送一張、頭盔每張顯示兩格），
// 穩定後試著升回全速。header-only、不依賴 OpenXR／Vulkan：XrContext（frame thread）與離線測試共用同一份邏輯。
//
// 量的是「預期外的重複格」：顯示格輪到時沒有新影像，而且這張已經顯示滿該佔的格數（全速 1 格、半速 2 格）。
// 一段連續的預期外重複超過 kShortRunSlots 格（約 50 ms）算「斷流」——那是整段網路停住（11-07 每 5 分鐘一次的那種），
// 半速救不了，不拿來判斷要不要降；只記下次數，升回全速前要求一段時間沒有斷流。
//
// 依據（2026-11-08 三場配戴，每 10 秒的重複格數）：00:33 全速 20～60、使用者「很糟」；00:48 全速 4～13、使用者「滑順很多」；
// 11-07 早上全速 0～6。遠端時半速能把補包的等待上限從約 8 ms 放寬到 14 ms，10-10 同路徑交替量：全速每 3 秒一張救不回、
// 半速 200 秒 1 張。
//
// 規則（5 秒一個視窗）：
//   降：全速時，最近 3 個視窗合計 ≥ kDownSum，而且至少 2 個視窗各 ≥ kDownEach；這 3 個視窗都不能是中斷
//       （斷流 ≥ kOutageStalls 次，11-08 使用者：中斷不觸發降速）。
//   升：半速滿 upDwell（起始 60 秒），而且最近 kUpWindows 個視窗每個都 ≤ kUpMax、每 15 秒合計 ≤ kUpSum15、沒有斷流 → 試著升回全速。
//   試升：升回後 kProbeUs 內最新視窗 ≥ kProbeFail、且和前一個合計 ≥ kProbeFailSum → 退回半速，upDwell 加倍（上限 10 分鐘）；撐過就把 upDwell 還原。
//   開場 kStartSettleUs、每次切換後 kSwitchSettleUs 內不判斷（切換那一刻的重複是切換本身造成的）。
//   要求送出後 kReplyTimeoutUs 沒有回覆就重送，連續 kMaxRetries 次沒回覆＝host 不支援，這一場不再自動切。
//
// §VR-RATE-CAUSE（2026-11-09）：預期外的重複格分兩種原因。呼叫端在每一格告訴這裡「有沒有一張已經收齊超過兩格、
// 卻還沒解出的影格」（headsetLate）：有＝頭盔端跟不上（解碼／顯示），沒有＝影格還沒到（網路）。11-09 每眼 2304 兩輪：
// 網路 0 丟失、每 10 秒 1444～1446 張全到，顯示端照樣每 10 秒跳格 29～63 次，舊版把它標成「network unstable」。
//   降速照樣做（流暢第一，半速讓解碼多一倍時間），但原因記成 Headset；頭盔端的上限不會自己變好，所以試升的等待
//   從 kUpDwellHeadsetUs 起算（每 60 秒試一次只會每次都頓一下）。
//   最近 3 個視窗的短段重複裡頭盔端佔 ≥ kHeadsetShare 才算頭盔端。
//
// §VR-RATE-CAUSE-HOST（2026-11-20）：第三種原因——host 那一格沒出畫格（遊戲／合成器掉幀、擷取放棄）。11-20 的實例：
// host 被別的程式佔住，10 秒只出 1054／1440 張、網路零丟包，舊版把每 15 秒 404 格重複全算成網路，20 分鐘內四度降到半速，
// 試升又必然失敗。半速換不回 host 沒出的畫格，所以這一種**不降、不判試升失敗，只記錄**（使用者 11-20 的決定）。
//   判斷（HostCadence，下面）：每張影像的時戳是 host 的 Present 時刻（90 kHz）、幀號連續（半速跳過的不佔號）。
//   「這一張與前一張幀號連號、Present 卻隔了 (div＋0.75) 格以上」＝中間 host 少出了 round(間隔)−div 格；網路造不出這個訊號
//   （掉一張幀號就不連號）。幀號不連號、拿不到幀號的一律不算 host（寧可少算）。
//   一段預期外的重複在結束它的那張新影像到的時候結算：先扣掉 host 少出的格數（記進 hostRep），剩下的才照原本的規則
//   分短重複／補包／斷流、分頭盔端／網路。decide() 的式子沒有改——shortRep 本來就只剩網路＋頭盔端。
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace xrrate {

    struct Window {
        uint32_t shortRep = 0;  // 預期外、而且屬於短段的重複格數
        uint32_t stalls = 0;    // 斷流（預期外的重複連續超過 kShortRunSlots 格）的次數
        uint32_t slotCount = 0; // 這個視窗顯示了幾格（太少＝沒在串流，不算數）；不叫 slots：Qt 把它定義成巨集
        uint32_t headsetRep = 0; // §VR-RATE-CAUSE：shortRep 裡屬於頭盔端（影格收到了還沒解出）的格數
        uint32_t hostRep = 0;    // §VR-RATE-CAUSE-HOST：host 沒出畫格造成的重複格數（不在 shortRep 裡，不進任何判斷）
    };

    // §VR-RATE-CAUSE-HOST：從每一張解出來的影像（依解碼順序，被跳過沒顯示的也要給）算「host 累計少出了幾個顯示格」。
    // 不依賴 OpenXR／FFmpeg；影像 render thread 餵、把累計值跟著影像發布，frame thread 拿「這一張的累計 − 上一張顯示的
    // 累計」當這一段重複裡 host 的份量——中間有影像被跳過也對得上。
    class HostCadence {
    public:
        // Present 間隔要比該有的多出這麼多格才算少出（host 的 Present 時刻有抖動；沿用 xrlatchstats.h 配戴驗過的容忍值）
        static constexpr double kGapTolerance = 0.75;
        // 帶 REPEATED、Present 間隔也有缺口、而且不是關鍵幀的那一張（host 等到逾時沒有新畫面才重編上一張）：整段重複都算 host
        static constexpr uint32_t kWholeRun = 0xffffu;
        static constexpr int64_t kNoFrameNo = INT64_MIN;

        // 這一張與前一張之間 host 少出了幾個顯示格。frameNoDelta＝幀號差（1＝連號）；ptsDelta90＝Present 時刻的差
        // （90 kHz，已經處理過 32 bit 繞回）；periodUs＝顯示週期；div＝串流每張佔幾格；repeated＝這一張帶 REPEATED；
        // keyframe＝這一張是關鍵幀（解碼端拿不到時給 false）。
        // §RATE-CAUSE-HOST-IDR（2026-11-20 審查 C05）：host 收到 IDR 要求、當下沒有新影像時會直接重編上一張當 IDR，
        // 也帶 REPEATED、幀號照樣連號，但 Present 間隔只有幾 ms。那張大 IDR 在壅塞線路上晚到，前面那段重複是網路造成的。
        // 所以 REPEATED 也要先通過同一條 Present 間隔門檻（不寫死 100 ms，跟著實際週期走）；關鍵幀就算有缺口也只算
        // 缺口本身的格數，不整段算 host。寧可少算 host：多算會讓網路真的不穩時該降半速而不降。
        static uint32_t missedSlots(int64_t frameNoDelta, int64_t ptsDelta90, double periodUs, int div, bool repeated,
                                    bool keyframe = false)
        {
            if (frameNoDelta != 1) {
                return 0;  // 中間掉了一張（或幀號不可用）：歸網路
            }
            const double ptsPerPeriod = 90000.0 * periodUs / 1e6;
            if (!(ptsPerPeriod >= 1.0) || ptsDelta90 <= 0) {
                return 0;
            }
            const double d = div >= 2 ? 2.0 : 1.0;
            const double periods = static_cast<double>(ptsDelta90) / ptsPerPeriod;
            if (periods < d + kGapTolerance) {
                return 0;
            }
            if (repeated && !keyframe) {
                return kWholeRun;
            }
            const double missed = std::floor(periods + 0.5) - d;
            return missed <= 0.0 ? 0u : static_cast<uint32_t>((std::min)(missed, static_cast<double>(kWholeRun)));
        }

        // 32 bit 的 RTP 時戳（約 13 小時繞一圈）相減
        static int64_t ptsDelta(int64_t cur, int64_t prev)
        {
            int64_t d = cur - prev;
            if (d < -(static_cast<int64_t>(1) << 31)) {
                d += static_cast<int64_t>(1) << 32;
            }
            return d;
        }

        // 每一張解出來的影像一次。frameNo＝kNoFrameNo 表示拿不到幀號。回傳到這一張為止的累計（32 bit，會繞回，只拿來相減）
        uint32_t onFrame(int64_t frameNo, int64_t pts90, double periodUs, int div, bool repeated, bool keyframe = false)
        {
            if (m_Have && frameNo != kNoFrameNo && m_PrevFrameNo != kNoFrameNo) {
                const int64_t dn = frameNo - m_PrevFrameNo;
                if (dn == 1) {
                    ++m_Pairs;
                }
                else {
                    ++m_Breaks;
                }
                m_Cum += missedSlots(dn, ptsDelta(pts90, m_PrevPts), periodUs, div, repeated, keyframe);
            }
            m_Have = true;
            m_PrevFrameNo = frameNo;
            m_PrevPts = pts90;
            return m_Cum;
        }

        uint32_t cumulative() const { return m_Cum; }
        uint32_t pairs() const { return m_Pairs; }    // 幀號連號的組數（累計）
        uint32_t breaks() const { return m_Breaks; }  // 幀號不連號的組數（累計；掉幀、被 mailbox 蓋掉、解碼丟幀）

    private:
        bool m_Have = false;
        int64_t m_PrevFrameNo = kNoFrameNo;
        int64_t m_PrevPts = 0;
        uint32_t m_Cum = 0;
        uint32_t m_Pairs = 0, m_Breaks = 0;
    };

    class AutoRate {
    public:
        static constexpr uint64_t kWindowUs = 5000000ull;
        static constexpr uint64_t kStartSettleUs = 15000000ull;
        static constexpr uint64_t kSwitchSettleUs = 4000000ull;
        static constexpr uint32_t kShortRunSlots = 6;
        // §VR-REPAIR-WAN（2026-11-15）：補包（遠端路徑）把缺封包的那一張拖晚一輪到兩輪往返才放行，放行前那一段重複最長約
        // 等待上限 80 ms＋一個幀週期（144 Hz 是 14 格）。那不是線路停住——補回來的畫面是乾淨的、而且正是「缺封包多」的訊號：
        // 算成一段短重複（封頂在 kShortRunSlots 格），不算斷流。否則缺封包越多（越該降半速）的線路越容易被當成「中斷」
        // 而跳過降速判斷，半速時只要有一次就不試升
        static constexpr uint32_t kRepairRunSlots = 14;
        static constexpr uint32_t kDownSum = 45;
        static constexpr uint32_t kDownEach = 10;
        static constexpr int kUpWindows = 6;  // 30 秒
        // 11-08 第二版：原本每個視窗 ≤ 4 太嚴（全速時網路順的那段每 5 秒也有 5～8 格），半速卡了 7 分鐘沒試過升。
        // 改成和降速對稱：最近 30 秒每 15 秒都低於降速門檻的一半、每個視窗 ≤ kUpMax、沒有斷流就試；失敗由試升的加倍退避擋住
        static constexpr uint32_t kUpMax = 10;
        static constexpr uint32_t kUpSum15 = kDownSum / 2;
        static constexpr uint64_t kUpDwellStartUs = 60000000ull;
        static constexpr uint64_t kUpDwellMaxUs = 600000000ull;
        static constexpr uint64_t kUpDwellHeadsetUs = 300000000ull;  // §VR-RATE-CAUSE：頭盔端降速後 5 分鐘才試升
        static constexpr double kHeadsetShare = 0.7;
        static constexpr uint64_t kProbeUs = 30000000ull;
        // 試升失敗：最新一個視窗 ≥ kProbeFail 而且和前一個合計 ≥ kProbeFailSum。只看單一視窗時，網路正常（每秒 0.8 次）
        // 偶爾一個 5 秒冒出十幾格就會判失敗（離線模擬重現），白白退回半速再等兩倍時間
        static constexpr uint32_t kProbeFail = 12;
        static constexpr uint32_t kProbeFailSum = 22;
        // 11-08 使用者：網路中斷不要觸發降速（半速救不了）。一個視窗裡斷流 ≥ kOutageStalls 次＝這段是中斷，
        // 降速與試升失敗都不拿它判斷；最近 3 個視窗有中斷就先等（中斷過後 15 秒再看）。只有零星 1～2 次斷流的
        // 視窗照算——一直很差的網路（11-08 00:33）也常有零星長空檔，那正是半速有用的情況
        static constexpr uint32_t kOutageStalls = 3;
        static constexpr uint64_t kReplyTimeoutUs = 3000000ull;
        static constexpr int kMaxRetries = 3;
        static constexpr int kHist = 8;

        enum class Reason : uint8_t { None = 0, Down = 1, Up = 2, ProbeFail = 3, HeadsetDown = 4 };

        struct Request {
            int div = 0;  // 0＝不用送
            Reason reason = Reason::None;
            bool retry = false;
        };

        void reset(uint64_t nowUs)
        {
            *this = AutoRate();
            m_StartUs = nowUs;
            m_SettleUntilUs = nowUs + kStartSettleUs;
        }

        // 每個顯示格呼叫一次。newFrame＝這一格顯示的是新影像；headsetLate＝這一格有一張影格已經收齊超過兩格還沒解出
        //（§VR-RATE-CAUSE，只在預期外的重複格有意義）；repairReleased＝這一張新影像是被補包拖晚、剛放行的
        //（§VR-REPAIR-WAN，只在 newFrame 時有意義）；hostSlots＝這一張新影像與上一張顯示的影像之間 host 少出了幾格
        //（§VR-RATE-CAUSE-HOST，HostCadence 的累計值相減；只在 newFrame 時有意義）
        void onSlot(bool newFrame, uint64_t nowUs, bool headsetLate = false, bool repairReleased = false, uint32_t hostSlots = 0)
        {
            if (m_StartUs == 0) {
                reset(nowUs);
            }
            if (newFrame) {
                closeRun(repairReleased, hostSlots);
                m_SinceNew = 0;
            }
            else {
                ++m_SinceNew;
                if (m_SinceNew >= static_cast<uint32_t>(m_Div)) {
                    // 已經顯示滿該佔的格數，這一格是預期外的重複。原因（host 沒出／頭盔端／網路）等這一段結束才分得出來，
                    // 整場累計也在結算時才加（closeRun／rollWindow）
                    ++m_RunUnexpected;
                    if (headsetLate) {
                        ++m_RunHeadset;
                    }
                }
            }
            ++m_Cur.slotCount;
            if (m_WinStartUs == 0) {
                m_WinStartUs = nowUs;
            }
            if (nowUs - m_WinStartUs >= kWindowUs) {
                rollWindow(nowUs);
            }
        }

        // 每格（或每秒）呼叫：要不要送要求
        Request poll(uint64_t nowUs)
        {
            Request r;
            if (m_Disabled) {
                return r;
            }
            if (m_PendingDiv != 0) {
                if (nowUs - m_SentUs >= kReplyTimeoutUs) {
                    if (++m_Retries > kMaxRetries) {
                        m_Disabled = true;  // host 沒回：舊版，不支援
                        m_PendingDiv = 0;
                        return r;
                    }
                    m_SentUs = nowUs;
                    r.div = m_PendingDiv;
                    r.reason = m_PendingReason;
                    r.retry = true;
                }
                return r;
            }
            if (m_Decision.div != 0) {
                r = m_Decision;
                m_Decision = Request();
                m_PendingDiv = r.div;
                m_PendingReason = r.reason;
                if (r.div == 2) {
                    m_PendingCause = r.reason;
                }
                m_SentUs = nowUs;
                m_Retries = 0;
                ++m_Seq;
            }
            return r;
        }

        // host 回覆（S2C_RATE）。accepted＝false 時維持原本的分頻
        void onApplied(int div, bool accepted, uint64_t nowUs)
        {
            m_PendingDiv = 0;
            if (!accepted) {
                // 被拒（例如串流率太低）：這一場不再要求降速
                if (div == 1) {
                    m_DownRefused = true;
                }
                m_PendingCause = Reason::None;
                return;
            }
            setDiv(div, nowUs);
            // §REVIEW-1119：原因只給這一次 setDiv 用。留著的話，下一次不是試升失敗的降速也會把退避再加倍。
            m_PendingCause = Reason::None;
        }

        int div() const { return m_Div; }
        uint16_t seq() const { return m_Seq; }
        bool disabled() const { return m_Disabled; }
        bool pending() const { return m_PendingDiv != 0; }
        bool probing(uint64_t nowUs) const { return m_Div == 1 && m_ProbeUntilUs != 0 && nowUs < m_ProbeUntilUs; }
        uint64_t upDwellUs() const { return m_UpDwellUs; }
        uint32_t downs() const { return m_Downs; }
        uint32_t ups() const { return m_Ups; }
        uint32_t probeFails() const { return m_ProbeFails; }
        uint32_t outageSkips() const { return m_OutageSkips; }  // 因為最近有中斷而沒做降速判斷的次數
        uint32_t repairRuns() const { return m_RepairRuns; }  // §VR-REPAIR-WAN：因為是等補包而沒算成斷流的段數（累計）
        // §VR-RATE-CAUSE：整場累計的預期外重複格，分網路（影格還沒到）與頭盔端（收到了還沒解出）
        uint64_t totalNetSlots() const { return m_TotNetSlots; }
        uint64_t totalHeadsetSlots() const { return m_TotHeadsetSlots; }
        // §VR-RATE-CAUSE-HOST：整場累計、host 沒出畫格造成的重複格（不算進上面兩個，也不進降速與試升的判斷）
        uint64_t totalHostSlots() const { return m_TotHostSlots; }
        uint32_t recentHostSum(int n) const
        {
            uint32_t sum = 0;
            for (int i = 0; i < (std::min)(n, m_HistN); ++i) {
                sum += hist(i).hostRep;
            }
            return sum;
        }
        uint32_t headsetDowns() const { return m_HeadsetDowns; }
        uint32_t recentHeadsetSum(int n) const
        {
            uint32_t sum = 0;
            for (int i = 0; i < (std::min)(n, m_HistN); ++i) {
                sum += hist(i).headsetRep;
            }
            return sum;
        }
        // 最近一個完成的視窗（log 用）
        const Window& lastWindow() const { return m_Hist[(m_HistHead + kHist - 1) % kHist]; }
        uint32_t recentSum(int n) const
        {
            uint32_t sum = 0;
            for (int i = 0; i < (std::min)(n, m_HistN); ++i) {
                sum += hist(i).shortRep;
            }
            return sum;
        }

    private:
        const Window& hist(int back) const { return m_Hist[(m_HistHead + kHist - 1 - back) % kHist]; }
        static bool outage(const Window& w) { return w.stalls >= kOutageStalls; }

        // 整場累計：一段裡不是 host 的那幾格，分頭盔端與網路
        void addTotals(uint32_t slotsNotHost, uint32_t headset)
        {
            headset = (std::min)(headset, slotsNotHost);
            m_TotHeadsetSlots += headset;
            m_TotNetSlots += slotsNotHost - headset;
        }

        void closeRun(bool repairReleased = false, uint32_t hostSlots = 0)
        {
            if (m_RunUnexpected > 0) {
                // §VR-RATE-CAUSE-HOST：先扣掉 host 少出的格數（以這一段的長度為上限：多出來的仍算網路或頭盔端），
                // 剩下的才照原本的規則分類。host 卡 100 ms（整段都由 Present 的缺口解釋）不會記成斷流、不會擋試升
                const uint32_t host = (std::min)(m_RunUnexpected, hostSlots);
                const uint32_t rest = m_RunUnexpected - host;
                const uint32_t headset = (std::min)(m_RunHeadset, rest);
                m_Cur.hostRep += host;
                m_TotHostSlots += host;
                addTotals(rest, headset);
                if (rest == 0) {
                    // 整段都是 host 沒出畫格
                }
                else if (rest <= kShortRunSlots) {
                    m_Cur.shortRep += rest;
                    m_Cur.headsetRep += headset;
                }
                else if (repairReleased && rest <= kRepairRunSlots) {
                    m_Cur.shortRep += kShortRunSlots;  // 等補包的那一段：一段短重複，不是斷流
                    ++m_RepairRuns;
                }
                else {
                    ++m_Cur.stalls;
                }
            }
            m_RunUnexpected = 0;
            m_RunHeadset = 0;
        }

        void setDiv(int div, uint64_t nowUs)
        {
            div = div >= 2 ? 2 : 1;
            if (div == m_Div) {
                return;
            }
            const int before = m_Div;
            m_Div = div;
            m_SinceNew = 0;
            m_RunUnexpected = 0;
            m_RunHeadset = 0;
            m_SwitchUs = nowUs;
            m_SettleUntilUs = (std::max)(m_SettleUntilUs, nowUs + kSwitchSettleUs);
            // 舊分頻量到的視窗不能拿來判斷新分頻
            m_HistN = 0;
            m_Cur = Window();
            m_WinStartUs = nowUs;
            if (div == 2 && before == 1) {
                ++m_Downs;
                // §REVIEW-1119：回覆的往返若跨過試升截止，單看 nowUs 已經不在試升期，退避不會加倍，60 秒後又試一次。
                // 送出當下的原因還在 m_PendingCause。
                if (m_PendingCause == Reason::ProbeFail || (m_ProbeUntilUs != 0 && nowUs < m_ProbeUntilUs)) {
                    ++m_ProbeFails;
                    m_UpDwellUs = (std::min)(m_UpDwellUs * 2, kUpDwellMaxUs);
                }
                if (m_PendingCause == Reason::HeadsetDown) {
                    // §VR-RATE-CAUSE：頭盔端跟不上不會自己變好，試升至少等 5 分鐘
                    ++m_HeadsetDowns;
                    m_UpDwellUs = (std::max)(m_UpDwellUs, kUpDwellHeadsetUs);
                }
                m_ProbeUntilUs = 0;
            }
            else if (div == 1 && before == 2) {
                ++m_Ups;
                m_ProbeUntilUs = nowUs + kProbeUs;
            }
        }

        void rollWindow(uint64_t nowUs)
        {
            // §REVIEW-1119：新影像還沒到就切視窗的話，進行中的重複不會進 closeRun。整段斷流的視窗
            // shortRep 與 stalls 都是 0，半速會把它當成乾淨而升回全速。長過補包上限才在這裡算斷流並歸零；
            // 還可能是補包（7～14 格）的先留著，等 newFrame 的 repairReleased，避免把補包記成斷流。m_SinceNew 不重設。
            // §VR-RATE-CAUSE-HOST：這裡結算的段還沒等到結束它的那張新影像，分不出是不是 host，照舊算網路／頭盔端。
            // 每個視窗最多一段（短的 ≤ 6 格，低於 kDownEach），不會因此誤降；host 長時間停住而跨過視窗邊界的那一段
            // 會記成一次斷流（只影響試升，不觸發降速）。
            if (m_RunUnexpected > kRepairRunSlots) {
                ++m_Cur.stalls;
                addTotals(m_RunUnexpected, m_RunHeadset);
                m_RunUnexpected = 0;
                m_RunHeadset = 0;
            }
            else if (m_RunUnexpected > 0 && m_RunUnexpected <= kShortRunSlots) {
                m_Cur.shortRep += m_RunUnexpected;
                m_Cur.headsetRep += m_RunHeadset;
                addTotals(m_RunUnexpected, m_RunHeadset);
                m_RunUnexpected = 0;
                m_RunHeadset = 0;
            }
            // §REVIEW-1119：穩定期 4 秒、視窗 5 秒。用結算當下的 now 比，第一個視窗結算時穩定期已經過了，
            // 切換造成的重複會整段進 decide。改看這個視窗是不是在穩定期內開始的。
            const bool settling = m_WinStartUs < m_SettleUntilUs;
            if (!settling && m_Cur.slotCount > 0) {
                m_Hist[m_HistHead] = m_Cur;
                m_HistHead = (m_HistHead + 1) % kHist;
                m_HistN = (std::min)(m_HistN + 1, kHist);
                decide(nowUs);
            }
            m_Cur = Window();
            m_WinStartUs = nowUs;
        }

        void decide(uint64_t nowUs)
        {
            if (m_PendingDiv != 0 || m_Decision.div != 0) {
                return;
            }
            if (m_Div == 1) {
                // 試升期間：一個視窗就判
                if (m_ProbeUntilUs != 0 && m_DownRefused) {
                    // §REVIEW-1111（第二輪）：host 已經拒絕過降速，這一場不會再要求——試升也到此為止。
                    // 原本這裡不看 m_DownRefused：試升期間每個視窗都重送一次降速要求（30 秒內最多 6 次）。
                    m_ProbeUntilUs = 0;
                }
                if (m_ProbeUntilUs != 0) {
                    if (nowUs < m_ProbeUntilUs) {
                        const uint32_t prev = m_HistN >= 2 && !outage(hist(1)) ? hist(1).shortRep : 0;
                        if (!outage(hist(0)) && hist(0).shortRep >= kProbeFail && hist(0).shortRep + prev >= kProbeFailSum) {
                            m_Decision = Request {2, Reason::ProbeFail, false};
                        }
                        return;
                    }
                    m_ProbeUntilUs = 0;
                    m_UpDwellUs = kUpDwellStartUs;  // 撐過試升：下次從頭等
                }
                if (m_DownRefused || m_HistN < 3) {
                    return;
                }
                for (int i = 0; i < 3; ++i) {
                    if (outage(hist(i))) {
                        ++m_OutageSkips;
                        return;
                    }
                }
                uint32_t sum = 0;
                int each = 0;
                for (int i = 0; i < 3; ++i) {
                    sum += hist(i).shortRep;
                    each += hist(i).shortRep >= kDownEach ? 1 : 0;
                }
                if (sum >= kDownSum && each >= 2) {
                    // §VR-RATE-CAUSE：頭盔端佔大多數就記成 HeadsetDown（一樣降速，只是原因與試升等待不同）
                    const bool headset = static_cast<double>(recentHeadsetSum(3)) >= kHeadsetShare * static_cast<double>(sum);
                    m_Decision = Request {2, headset ? Reason::HeadsetDown : Reason::Down, false};
                }
                return;
            }
            // 半速：等滿 upDwell、最近 30 秒都乾淨才試著升
            if (nowUs - m_SwitchUs < m_UpDwellUs || m_HistN < kUpWindows) {
                return;
            }
            uint32_t sumA = 0, sumB = 0;
            for (int i = 0; i < kUpWindows; ++i) {
                if (hist(i).shortRep > kUpMax || hist(i).stalls > 0) {
                    return;
                }
                (i < kUpWindows / 2 ? sumA : sumB) += hist(i).shortRep;
            }
            if (sumA > kUpSum15 || sumB > kUpSum15) {
                return;
            }
            m_Decision = Request {1, Reason::Up, false};
        }

        int m_Div = 1;
        uint64_t m_StartUs = 0;
        uint64_t m_SettleUntilUs = 0;
        uint64_t m_SwitchUs = 0;
        uint64_t m_WinStartUs = 0;
        uint32_t m_SinceNew = 0;
        uint32_t m_RunUnexpected = 0;
        uint32_t m_RunHeadset = 0;  // §VR-RATE-CAUSE：這一段連續重複裡屬於頭盔端的格數
        Window m_Cur;
        std::array<Window, kHist> m_Hist {};
        int m_HistHead = 0;
        int m_HistN = 0;

        Request m_Decision;
        int m_PendingDiv = 0;
        Reason m_PendingReason = Reason::None;
        uint64_t m_SentUs = 0;
        int m_Retries = 0;
        uint16_t m_Seq = 0;
        bool m_Disabled = false;
        bool m_DownRefused = false;

        uint64_t m_UpDwellUs = kUpDwellStartUs;
        uint64_t m_ProbeUntilUs = 0;
        uint32_t m_Downs = 0, m_Ups = 0, m_ProbeFails = 0;
        uint32_t m_OutageSkips = 0;
        uint32_t m_RepairRuns = 0;
        Reason m_PendingCause = Reason::None;  // 最近一次送出的降速要求的原因
        uint64_t m_TotNetSlots = 0, m_TotHeadsetSlots = 0;
        uint64_t m_TotHostSlots = 0;  // §VR-RATE-CAUSE-HOST
        uint32_t m_HeadsetDowns = 0;
    };

}  // namespace xrrate
