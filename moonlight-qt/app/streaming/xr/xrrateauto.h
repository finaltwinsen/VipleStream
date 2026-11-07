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
//   降：全速時，最近 3 個視窗合計 ≥ kDownSum，而且至少 2 個視窗各 ≥ kDownEach。
//   升：半速滿 upDwell（起始 60 秒），而且最近 kUpWindows 個視窗每個都 ≤ kUpMax、每 15 秒合計 ≤ kUpSum15、沒有斷流 → 試著升回全速。
//   試升：升回後 kProbeUs 內最新視窗 ≥ kProbeFail、且和前一個合計 ≥ kProbeFailSum → 退回半速，upDwell 加倍（上限 10 分鐘）；撐過就把 upDwell 還原。
//   開場 kStartSettleUs、每次切換後 kSwitchSettleUs 內不判斷（切換那一刻的重複是切換本身造成的）。
//   要求送出後 kReplyTimeoutUs 沒有回覆就重送，連續 kMaxRetries 次沒回覆＝host 不支援，這一場不再自動切。
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace xrrate {

    struct Window {
        uint32_t shortRep = 0;  // 預期外、而且屬於短段的重複格數
        uint32_t stalls = 0;    // 斷流（預期外的重複連續超過 kShortRunSlots 格）的次數
        uint32_t slotCount = 0; // 這個視窗顯示了幾格（太少＝沒在串流，不算數）；不叫 slots：Qt 把它定義成巨集
    };

    class AutoRate {
    public:
        static constexpr uint64_t kWindowUs = 5000000ull;
        static constexpr uint64_t kStartSettleUs = 15000000ull;
        static constexpr uint64_t kSwitchSettleUs = 4000000ull;
        static constexpr uint32_t kShortRunSlots = 6;
        static constexpr uint32_t kDownSum = 45;
        static constexpr uint32_t kDownEach = 10;
        static constexpr int kUpWindows = 6;  // 30 秒
        // 11-08 第二版：原本每個視窗 ≤ 4 太嚴（全速時網路順的那段每 5 秒也有 5～8 格），半速卡了 7 分鐘沒試過升。
        // 改成和降速對稱：最近 30 秒每 15 秒都低於降速門檻的一半、每個視窗 ≤ kUpMax、沒有斷流就試；失敗由試升的加倍退避擋住
        static constexpr uint32_t kUpMax = 10;
        static constexpr uint32_t kUpSum15 = kDownSum / 2;
        static constexpr uint64_t kUpDwellStartUs = 60000000ull;
        static constexpr uint64_t kUpDwellMaxUs = 600000000ull;
        static constexpr uint64_t kProbeUs = 30000000ull;
        // 試升失敗：最新一個視窗 ≥ kProbeFail 而且和前一個合計 ≥ kProbeFailSum。只看單一視窗時，網路正常（每秒 0.8 次）
        // 偶爾一個 5 秒冒出十幾格就會判失敗（離線模擬重現），白白退回半速再等兩倍時間
        static constexpr uint32_t kProbeFail = 12;
        static constexpr uint32_t kProbeFailSum = 22;
        static constexpr uint64_t kReplyTimeoutUs = 3000000ull;
        static constexpr int kMaxRetries = 3;
        static constexpr int kHist = 8;

        enum class Reason : uint8_t { None = 0, Down = 1, Up = 2, ProbeFail = 3 };

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

        // 每個顯示格呼叫一次。newFrame＝這一格顯示的是新影像
        void onSlot(bool newFrame, uint64_t nowUs)
        {
            if (m_StartUs == 0) {
                reset(nowUs);
            }
            if (newFrame) {
                closeRun();
                m_SinceNew = 0;
            }
            else {
                ++m_SinceNew;
                if (m_SinceNew >= static_cast<uint32_t>(m_Div)) {
                    ++m_RunUnexpected;  // 已經顯示滿該佔的格數，這一格是網路晚到造成的
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
                return;
            }
            setDiv(div, nowUs);
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

        void closeRun()
        {
            if (m_RunUnexpected > 0) {
                if (m_RunUnexpected <= kShortRunSlots) {
                    m_Cur.shortRep += m_RunUnexpected;
                }
                else {
                    ++m_Cur.stalls;
                }
            }
            m_RunUnexpected = 0;
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
            m_SwitchUs = nowUs;
            m_SettleUntilUs = (std::max)(m_SettleUntilUs, nowUs + kSwitchSettleUs);
            // 舊分頻量到的視窗不能拿來判斷新分頻
            m_HistN = 0;
            m_Cur = Window();
            m_WinStartUs = nowUs;
            if (div == 2 && before == 1) {
                ++m_Downs;
                if (m_ProbeUntilUs != 0 && nowUs < m_ProbeUntilUs) {
                    ++m_ProbeFails;
                    m_UpDwellUs = (std::min)(m_UpDwellUs * 2, kUpDwellMaxUs);
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
            const bool settling = nowUs < m_SettleUntilUs;
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
                if (m_ProbeUntilUs != 0) {
                    if (nowUs < m_ProbeUntilUs) {
                        const uint32_t prev = m_HistN >= 2 ? hist(1).shortRep : 0;
                        if (hist(0).shortRep >= kProbeFail && hist(0).shortRep + prev >= kProbeFailSum) {
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
                uint32_t sum = 0;
                int each = 0;
                for (int i = 0; i < 3; ++i) {
                    sum += hist(i).shortRep;
                    each += hist(i).shortRep >= kDownEach ? 1 : 0;
                }
                if (sum >= kDownSum && each >= 2) {
                    m_Decision = Request {2, Reason::Down, false};
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
    };

}  // namespace xrrate
