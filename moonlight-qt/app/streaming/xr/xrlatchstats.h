// VipleStream 2.0 §VR-LDIAG — latch 量測用的小工具（`[VIPLE-VR-LDIAG]`）。
//
// header-only，只用標準函式庫（不依賴 Qt／OpenXR／libplacebo）：沒有 OpenXR 的建置也編得到，
// `xr-probe --selftest-latch`、命令列的 --vr-test-jitter 檢查、獨立的 g++ 測試都只 include 這一個檔。
//
//   Classifier：把 XR frame thread 每一格「挑到哪一張串流影像」分類成 late／lateShift／gap／early／catchup。
//               只看發布序號、影像的 pts（host 的 Present 時刻，90 kHz）與顯示格序號，不用固定的毫秒門檻。
//               另有一組不看基準的晚到計數（waitNew／resc／flip，見 Counters）：用等待格與影像的送出時刻，
//               回答「差一點沒趕上的有幾張、其中在那個 latch 當下已經送出的有幾張」。
//   Jitter    ：dev 的到達抖動注入（--vr-test-jitter）。命令列的檢查與影像 render thread 用同一份解析。
//   selfTest()：腳本序列（正常、晚到被跳過、早到滑格、host 少一幀、連續晚到後追回、半速，以及 frame thread 漏格、
//               pts 繞回、基準更新等）＋注入器的檢查＋不看基準的那幾個計數的腳本。
//
// 執行緒：Classifier、Jitter 都沒有鎖，各自只給一條執行緒用（Classifier＝XR frame thread、Jitter＝影像 render thread）。
//
// 看 Classifier 的計數要知道的三件事：
//   1. 單位。late、gap 是串流幀數；lateShift、early、catchup 是顯示格數。全速（一張一格）時「多出來的重複格數
//      ≈ late＋lateShift＋gap＋early」；半速（一張兩格）時 late 與 gap 要乘 2 才對得上重複的格數，而且一張只晚一格、
//      沒被跳過的影像會記成 lateShift 1＋catchup 1（回位時沒有任何一張被跳掉）。
//   2. 沒有回報的格（frame thread 沒跑到，或那一格沒送影像層）。跨過這種格的那一次對位變化分不出是影像晚到還是
//      frame thread 造成的：不算進 lateShift／early／catchup，之後往回走的那一次也不算；被跳過的影像要有「回報過、
//      而且已經輪到下一張的重複格」對應才算 late。所以 frame thread 漏格本身不會產生任何計數。
//   3. pts 的限制。「這一張屬於哪一格」是 pts 差逐對取整來的，所以分的是 host 的節拍：host 自己提早 Present 超過
//      半格的那一張算在前一格，後面空出來的那一格記成 gap，不是 early。顯示端完全正常的那幾次有放寬（pts 的間隔
//      差不到 0.75 格當成正常，見 onTick）；除此之外，Present 的間隔比正常多或少半格以上就可能多算 gap 1＋early 1
//      （client 其實沒有重複也沒有跳過），而且要等連 120 張正常才回到正確的基準。判讀時先對同一行
//      [VIPLE-VR-MTP10] 的 repeat：gap、early 明顯多於 repeat 就是這個假象。

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace xrlatch {

// XR frame thread 的一格（送出影像層的那幾格才有；沒有影像層的格不回報）
struct Tick {
    int64_t dispTick = 0;   // 顯示格序號：每個顯示週期 +1，frame thread 沒跑到的格也要算進去
    uint64_t latchUs = 0;   // 挑影像的時刻（client steady 時鐘）
    uint64_t seq = 0;       // 這一格送出的影像（XrVideo 的發布序號）
    double periodUs = 0.0;  // 顯示週期
};

struct Counters {
    // 有影像，但沒趕上自己那一格（那一格重複了上一張），之後又被下一張蓋掉、沒顯示過（串流幀數）
    uint32_t late = 0;
    // 整條對位晚了幾格顯示（之後每一張都晚這麼多格，直到 catchup）（顯示格數）
    uint32_t lateShift = 0;
    // 那一格根本沒有影像（串流幀數）。分類只看得到 pts 跳了一格，所以有三種來源：host 沒出幀、掉包，或影像到了
    // client 卻沒有發布（mailbox 裡被下一張蓋掉、沒有 meta 被丟、acquire／render 失敗——對同一段 [VIPLE-XR] 10s video
    // 的 overwritten／noMetaDropped／errors；開著 --vr-test-jitter 時 render thread 會睡，蓋掉的會變多）
    uint32_t gap = 0;
    // 影像提早搶了前面的格（顯示格數；全速時原本該在那一格的那張被蓋掉，之後那一格會重複）
    uint32_t early = 0;
    // 從晚幾格回到原位（顯示格數；全速時就是回位那一刻被跳掉的張數，半速時晚一格再回位不會跳掉任何一張）
    uint32_t catchup = 0;
    // 晚到的影像比它該趕上的那個 latch 晚了多久（視窗內最大值，上限 65535）
    uint32_t lateMaxUs = 0;

    // ── 不看基準的晚到計數（2026-10-11）──
    // 上面的 lateShift 與 early 是同一種事件，差別只在 Classifier 的基準貼哪一個標籤；基準要連 120 張正常才更新，
    // 每場開頭與事件多的場次可能整場停在另一邊，那時晚到的影像全部記成 early、lateMaxUs 是 0。下面幾個只看
    // 「等待格」（已經輪到下一張、它卻還沒發布的重複格）與影像的送出時刻（GPU 工作都送出去了的那一刻），不看基準。
    // 發布紀錄沒有帶送出時刻的影像不計；上一張新影像之後有顯示格沒回報的那一次也不計。
    //
    // take() 時抄的：串流一幀佔幾個顯示格（全速 1、半速 2）。重複的顯示格數＝lateShift＋early＋stride×(late＋gap)，
    // 這個總和不受基準影響
    uint32_t stride = 1;
    // 有等待格之後才顯示的新影像張數（每一段等待算一張：上一張之後第一張發布的那一張，它沒被顯示也算）
    uint32_t waitNew = 0;
    // 那一張比最後一個等待格的 latch 晚多久才發布：<250、<500、<750、<1000、<1500、<2500、<4000、其餘（µs）。
    // 落在後面幾格的不一定是晚到——前一張提早搶格之後，照原本節拍來的下一張也會落在這裡。要看的是前面幾格的形狀
    uint32_t waitBy[8] = {};
    // 那一段等了幾個等待格：1、2、3 以上
    uint32_t waitSlots[3] = {};
    // waitNew 裡，最後一個等待格的 latch 當下已經送出的（送出比那個 latch 早 kRescMarginUs 以上）：GPU 還沒做完、
    // 但在那一格就發布的話顯示得到。rescBy＝它們比那個 latch 晚多久才發布：<250、<500、<750、<1000、<1500、其餘
    uint32_t resc = 0;
    uint32_t rescBy[6] = {};
    // 沒有等待格、但上一格的 latch 當下已經送出的新影像張數：如果一送出就發布，它會早一格顯示、搶掉前一張的格
    // （半速時搶掉的是前一張第二次顯示的那一格）
    uint32_t flip = 0;
};

// 「在那個 latch 當下已經送出」的餘裕：要趕得上那一格，release 與發布（約 25 µs）得在那一格的 xrBeginFrame 之前完成，
// 而 xrBeginFrame 比 latch 早約 0.1 ms
constexpr uint64_t kRescMarginUs = 150;

class Classifier
{
public:
    // 影像 render thread 發布了一張。frame thread 抄錄時依序號呼叫；被跳過、沒顯示的也要給。
    // submitUs＝這張影像的 GPU 工作都送出去的時刻（和 pubUs 同一個時鐘）；0＝沒有，這一張不進「不看基準」的計數
    void onPublished(uint64_t seq, int64_t pts90, uint64_t pubUs, uint64_t submitUs = 0)
    {
        Rec& r = m_Ring[m_RingHead++ % kRing];
        r.seq = seq;
        r.pts90 = pts90;
        r.pubUs = pubUs;
        r.submitUs = submitUs;
    }

    // 這一格送出了 t.seq 這張影像（同一張重複顯示也要呼叫）。t.seq 的 onPublished 要先呼叫過
    void onTick(const Tick& t)
    {
        if (t.seq == 0) {
            return;
        }
        if (t.seq == m_LastSeq) {
            // 這一格重複上一張。一張串流影像本來就佔 m_Stride 格（全速 1、半速 2）；再多出來的重複格＝已經輪到
            // 下一張、它卻還沒到（下面叫「等待格」）。記下有幾格，以及第一格的 latch 時刻（晚到多久從它起算）。
            // 用顯示格序號算，中間有格子沒回報也不會錯位
            ++m_RepRun;
            if (t.dispTick - m_LastNewTick >= m_Stride) {
                if (m_WaitSlots == 0) {
                    m_MissLatchUs = t.latchUs;
                }
                ++m_WaitSlots;
                m_LastWaitLatchUs = t.latchUs;
            }
            notePrevTick(t);
            return;
        }
        const Rec* cur = find(t.seq);
        const int64_t dk = t.dispTick - m_LastNewTick;  // 與上一張新影像隔幾個顯示格
        if (m_LastSeq == 0 || t.seq < m_LastSeq || cur == nullptr || !m_HaveLastPts || dk < 1) {
            // 第一張，或對不上（紀錄已經被洗掉＝frame thread 停了很久；序號倒退＝影像路徑重建；顯示格序號沒有
            // 往前＝呼叫端的問題）：只重新對位，不分類
            m_Off = 0;
            m_OffBase = 0;
            m_Pardon = 0;
            m_NormalRun = 0;
            accept(t, cur);
            notePrevTick(t);
            return;
        }

        int64_t dpts = cur->pts90 - m_LastPts;
        if (dpts < -(static_cast<int64_t>(1) << 31)) {
            dpts += static_cast<int64_t>(1) << 32;  // pts 來自 32 bit 的 RTP 時戳，約 13 小時繞一圈
        }
        const double ptsPerPeriod = 90000.0 * t.periodUs / 1e6;
        // 兩張的 host Present 時刻隔幾個顯示格（週期不像話——不到一個 pts 刻度、或不是數字——就當成隔一格；
        // 夾在 ±1e12 是為了取整不溢位，正常的 pts 差不會超過 2^32）
        const double djRaw =
            ptsPerPeriod >= 1.0 ? (std::min)((std::max)(static_cast<double>(dpts) / ptsPerPeriod, -1e12), 1e12) : 1.0;
        int64_t dj = (std::max)(static_cast<int64_t>(std::llround(djRaw)), static_cast<int64_t>(1));
        const int64_t skipped = static_cast<int64_t>(t.seq - m_LastSeq) - 1;  // 發布了但沒被挑到
        // 顯示端正常（和上一張隔 m_Stride 格、沒有影像被跳過）時，pts 的間隔差不到 0.75 格就當成正常，不照四捨五入。
        // host 的 Present 時刻有抖動：某一張晚 Present 0.6 格、仍然趕上自己那一格的話，單看 pts 差會取整成隔兩格，
        // 憑空多出 gap 1＋early 1；餘裕（host 的 latch 目標）放到半格以上時特別容易發生。真的少一幀又剛好提早一格
        // 顯示的情況，pts 差會落在 m_Stride＋1 附近，不受影響
        if (dk == m_Stride && skipped == 0 && std::fabs(djRaw - static_cast<double>(dk)) < 0.75) {
            dj = dk;
        }
        // 串流一幀佔幾個顯示格（全速 1、半速 2）：取「每張發布影像」的 pts 間隔的眾數。被跳過的張數先除掉——
        // 相位貼著 latch 邊緣時一半的影像被跳過，直接拿 dj 取眾數會把全速估成半速
        pushDj((std::max)(static_cast<int64_t>(std::llround(static_cast<double>(dj) / static_cast<double>(skipped + 1))),
                          static_cast<int64_t>(1)));
        m_Stride = strideFromRecent();

        // 沒有發布的串流幀（host 沒出、掉包，或到了 client 卻沒發布）
        const int64_t gapSlots =
            (std::max)(static_cast<int64_t>(std::llround(static_cast<double>(dj) / static_cast<double>(m_Stride))) - 1 - skipped,
                       static_cast<int64_t>(0));
        const int64_t offDelta = dk - dj;  // 這一張比上一張多晚（正）或多早（負）幾格顯示
        // 上一張新影像之後有顯示格沒回報（frame thread 沒跑到，或那一格沒送影像層）：那幾格挑得到什麼不知道
        const bool unreported = dk - 1 > m_RepRun;
        add(m_C.gap, gapSlots);

        // late：被跳過、沒顯示過的影像。提早搶格（offDelta < 0）本來就會蓋掉前面的，那幾張不算晚到；其餘每一張都要有
        // 回報過的等待格對應——等待格是它晚到的證據，沒有（frame thread 漏格造成的跳過）就無從判斷，不算。
        // 對位同時往晚移（offDelta > 0）的那一次也照算：停了幾格才一起到的那幾張一樣沒趕上自己那一格
        const int64_t coveredByEarly = offDelta < 0 ? -offDelta / m_Stride : 0;
        const int64_t lateN = (std::min)((std::max)(skipped - coveredByEarly, static_cast<int64_t>(0)),
                                         (m_WaitSlots + m_Stride - 1) / m_Stride);
        add(m_C.late, lateN);
        uint64_t lateUs = 0;
        if (lateN > 0) {
            const Rec* first = find(m_LastSeq + 1);
            if (first != nullptr && first->pubUs > m_MissLatchUs) {
                lateUs = first->pubUs - m_MissLatchUs;
            }
        }

        // 對位的變化。跨過沒回報的格的那一次不分類，只記在 m_Pardon；之後反方向的變化先抵掉它（例：半速時
        // frame thread 漏掉該換新影像的那一格→那一張晚一格顯示、下一張回到原位，兩次都不是影像造成的）
        int64_t shift = offDelta;
        if (unreported) {
            m_Pardon += shift;
            shift = 0;
        }
        else if (shift != 0 && m_Pardon != 0 && (shift > 0) != (m_Pardon > 0)) {
            const int64_t used = (std::min)(shift > 0 ? shift : -shift, m_Pardon > 0 ? m_Pardon : -m_Pardon);
            m_Pardon += shift > 0 ? used : -used;
            shift += shift > 0 ? -used : used;
        }
        if (shift > 0) {
            if (m_Off + shift > m_OffBase) {
                add(m_C.lateShift, shift);
                if (lateUs == 0 && m_WaitSlots > 0 && cur->pubUs > m_MissLatchUs) {
                    lateUs = cur->pubUs - m_MissLatchUs;
                }
            }
            // 否則是早到滑格的後半（上一張提早搶了前一格，這一張回到原位），不另計
        }
        else if (shift < 0) {
            if (m_Off > m_OffBase) {
                add(m_C.catchup, -shift);
            }
            else {
                add(m_C.early, -shift);
            }
        }
        m_C.lateMaxUs = (std::max)(m_C.lateMaxUs, static_cast<uint32_t>((std::min)(lateUs, static_cast<uint64_t>(65535))));
        m_Off += shift;
        // 連續 120 張都正常：現在的對位就是基準（之後往晚算 lateShift、往早算 early），沒抵掉的 m_Pardon 也一筆勾銷
        m_NormalRun = (offDelta == 0 && skipped == 0 && gapSlots == 0 && dk == m_Stride) ? m_NormalRun + 1 : 0;
        if (m_NormalRun >= 120) {
            m_OffBase = m_Off;
            m_Pardon = 0;
        }

        // 不看基準的晚到計數（見 Counters）：不用 m_Off／m_OffBase，只看等待格與送出時刻。只在上一張新影像之後
        // 每一格都有回報時計。cand＝上一張之後第一張發布的：平常就是這一張；兩張擠在同一格才到的話是被跳過、沒顯示
        // 的那一張——在那個 latch 發布就顯示得到的是它。
        // 開場的第二張新影像不計（m_DjCount 還是 1）：串流一幀佔幾格是到這裡才第一次估出來的，它之前的重複格是照
        // 預設的 1 格判斷的——半速時第一張本來就該有的第二次顯示會被當成等待格
        const Rec* cand = find(m_LastSeq + 1);
        if (!unreported && m_DjCount > 1 && cand != nullptr && cand->submitUs != 0) {
            if (m_WaitSlots > 0) {
                // 之前有等待格：已經輪到它、它卻還沒發布
                add(m_C.waitNew, 1);
                const uint64_t by = cand->pubUs > m_LastWaitLatchUs ? cand->pubUs - m_LastWaitLatchUs : 0;
                add(m_C.waitBy[bucket8(by)], 1);
                add(m_C.waitSlots[(std::min)(m_WaitSlots, static_cast<int64_t>(3)) - 1], 1);
                if (cand->submitUs + kRescMarginUs <= m_LastWaitLatchUs) {
                    add(m_C.resc, 1);
                    add(m_C.rescBy[bucket6(by)], 1);
                }
            }
            else if (m_HavePrevTick && m_PrevTick == t.dispTick - 1 && cand->submitUs + kRescMarginUs <= m_PrevTickLatchUs) {
                // 上一格不是等待格，而它的 latch 當下這一張已經送出了
                add(m_C.flip, 1);
            }
        }
        accept(t, cur);
        notePrevTick(t);
    }

    // 取出這個視窗的計數並歸零（對位的狀態保留）
    Counters take()
    {
        Counters c = m_C;
        c.stride = static_cast<uint32_t>(m_Stride);
        m_C = Counters();
        return c;
    }

private:
    struct Rec {
        uint64_t seq = 0;
        int64_t pts90 = 0;
        uint64_t pubUs = 0;
        uint64_t submitUs = 0;  // 0＝沒有給
    };
    static constexpr size_t kRing = 64;
    static constexpr size_t kDjWindow = 32;

    const Rec* find(uint64_t seq) const
    {
        if (seq == 0) {
            return nullptr;
        }
        for (const Rec& r : m_Ring) {
            if (r.seq == seq) {
                return &r;
            }
        }
        return nullptr;
    }

    void accept(const Tick& t, const Rec* cur)
    {
        m_LastSeq = t.seq;
        m_HaveLastPts = cur != nullptr;
        m_LastPts = cur != nullptr ? cur->pts90 : 0;
        m_LastNewTick = t.dispTick;
        m_RepRun = 0;
        m_WaitSlots = 0;
    }

    // 每個回報的格（新影像、重複都算）結尾記下來：下一格用它判斷「上一格的 latch 當下送出了沒有」
    void notePrevTick(const Tick& t)
    {
        m_HavePrevTick = true;
        m_PrevTick = t.dispTick;
        m_PrevTickLatchUs = t.latchUs;
    }

    // Counters::waitBy 的格（µs）：<250、<500、<750、<1000、<1500、<2500、<4000、其餘
    static size_t bucket8(uint64_t us)
    {
        return us < 250 ? 0u : us < 500 ? 1u : us < 750 ? 2u : us < 1000 ? 3u : us < 1500 ? 4u : us < 2500 ? 5u : us < 4000 ? 6u : 7u;
    }

    // Counters::rescBy 的格（µs）：<250、<500、<750、<1000、<1500、其餘
    static size_t bucket6(uint64_t us)
    {
        return us < 250 ? 0u : us < 500 ? 1u : us < 750 ? 2u : us < 1000 ? 3u : us < 1500 ? 4u : 5u;
    }

    void pushDj(int64_t dj)
    {
        m_Dj[m_DjHead++ % kDjWindow] = dj;
        m_DjCount = (std::min)(m_DjCount + 1, kDjWindow);
    }

    // 最近 32 筆「每張發布影像的 pts 間隔」（見 onTick）的眾數（一樣多取小的），夾在 1～4
    int64_t strideFromRecent() const
    {
        int64_t best = 1;
        size_t bestN = 0;
        for (size_t i = 0; i < m_DjCount; i++) {
            size_t n = 0;
            for (size_t j = 0; j < m_DjCount; j++) {
                n += m_Dj[j] == m_Dj[i] ? 1u : 0u;
            }
            if (n > bestN || (n == bestN && m_Dj[i] < best)) {
                bestN = n;
                best = m_Dj[i];
            }
        }
        return (std::min)((std::max)(best, static_cast<int64_t>(1)), static_cast<int64_t>(4));
    }

    static void add(uint32_t& counter, int64_t v)
    {
        if (v > 0) {
            counter = static_cast<uint32_t>((std::min)(static_cast<uint64_t>(counter) + static_cast<uint64_t>(v),
                                                       static_cast<uint64_t>(0xffffffffu)));
        }
    }

    Rec m_Ring[kRing];
    uint64_t m_RingHead = 0;
    int64_t m_Dj[kDjWindow] = {};
    size_t m_DjHead = 0;
    size_t m_DjCount = 0;
    int64_t m_Stride = 1;
    uint64_t m_LastSeq = 0;
    bool m_HaveLastPts = false;
    int64_t m_LastPts = 0;
    int64_t m_LastNewTick = 0;
    int64_t m_RepRun = 0;          // 上一張新影像之後，回報過的重複格有幾格
    int64_t m_WaitSlots = 0;       // 其中已經輪到下一張的（等待格）有幾格
    uint64_t m_MissLatchUs = 0;    // 第一個等待格的 latch 時刻（m_WaitSlots > 0 才有意義）
    uint64_t m_LastWaitLatchUs = 0;  // 最後一個等待格的 latch 時刻（m_WaitSlots > 0 才有意義）
    bool m_HavePrevTick = false;   // 上一個回報的格：有沒有、它的顯示格序號與 latch 時刻（見 notePrevTick）
    int64_t m_PrevTick = 0;
    uint64_t m_PrevTickLatchUs = 0;
    int64_t m_Off = 0;             // 目前的對位（相對開場；正值＝晚）。不含 m_Pardon
    int64_t m_OffBase = 0;         // 基準對位
    int64_t m_Pardon = 0;          // 跨過沒回報的格造成、還沒被反方向抵掉的對位變化（不分類的那一部分）
    int m_NormalRun = 0;
    Counters m_C;
};

// dev：到達抖動注入（--vr-test-jitter <spec>）。影像 render thread 在畫每一張串流影像之前呼叫 nextUs()，
// 照回傳值延後——等同那一張晚到。
//
//   spec ＝ <mode>[,shift=<startSec>:<us>:<durSec>][,seed=<n>]
//   mode ＝ worn                      兩個狀態 calm／rough：每張以 8‰ 進 rough、20‰ 回 calm；calm 時 250‰ 的機率加一段
//                                     指數延遲（平均 700 µs），rough 時 750‰／平均 900 µs；上限 6000 µs
//          light                     30‰ 的機率加一段指數延遲（平均 500 µs），上限 4000 µs
//          early[=<baseUs>:<permille>]  每張固定延後 baseUs，其中 permille‰ 不延後（預設 5500:10）。host 的控制器會把
//                                     平均相位補回去，所以沒延後的那一張相對早到 baseUs
//          scan=<maxUs>              每張延後 U(0, maxUs)
//   shift：第一張影像後第 startSec 秒起 durSec 秒內，每張再多延後 us
//   seed ：亂數種子（xorshift32，預設 0x9E3779B9）
class Jitter
{
public:
    static constexpr uint32_t kMaxDelayUs = 20000;   // 單一參數的上限
    static constexpr uint32_t kMaxTotalUs = 30000;   // 一張影像最多延後這麼久

    // 空字串＝關。格式不對回 false，*err 是可以直接給命令列的英文說明；設定維持「關」
    bool configure(const std::string& spec, std::string* err)
    {
        *this = Jitter();
        if (spec.empty()) {
            return true;
        }
        static const char* const kUsage =
            "expected worn, light, early[=<baseUs>:<permille>] or scan=<maxUs>, optionally followed by "
            ",shift=<startSec>:<us>:<durSec> and ,seed=<n>";
        Jitter j;
        const std::vector<std::string> parts = split(spec, ',');
        std::string name, arg;
        bool hasArg = false;
        splitKeyValue(parts[0], &name, &arg, &hasArg);
        if (name == "worn" && !hasArg) {
            j.m_Mode = Mode::Worn;
        }
        else if (name == "light" && !hasArg) {
            j.m_Mode = Mode::Light;
        }
        else if (name == "early") {
            j.m_Mode = Mode::Early;
            if (hasArg) {
                const std::vector<std::string> f = split(arg, ':');
                uint64_t base = 0, pm = 0;
                if (f.size() != 2 || !parseUint(f[0], kMaxDelayUs, &base) || !parseUint(f[1], 1000, &pm)) {
                    return fail(err, "early needs <baseUs>:<permille> (baseUs 0-20000, permille 0-1000)");
                }
                j.m_EarlyBaseUs = static_cast<uint32_t>(base);
                j.m_EarlyPermille = static_cast<uint32_t>(pm);
            }
        }
        else if (name == "scan") {
            uint64_t maxUs = 0;
            if (!hasArg || !parseUint(arg, kMaxDelayUs, &maxUs) || maxUs == 0) {
                return fail(err, "scan needs =<maxUs> (1-20000)");
            }
            j.m_Mode = Mode::Scan;
            j.m_ScanMaxUs = static_cast<uint32_t>(maxUs);
        }
        else {
            return fail(err, kUsage);
        }
        for (size_t i = 1; i < parts.size(); i++) {
            splitKeyValue(parts[i], &name, &arg, &hasArg);
            if (name == "seed") {
                uint64_t seed = 0;
                if (!hasArg || !parseUint(arg, 0xffffffffull, &seed) || seed == 0) {
                    return fail(err, "seed needs =<n> (1-4294967295)");
                }
                j.m_Rng = static_cast<uint32_t>(seed);
            }
            else if (name == "shift") {
                const std::vector<std::string> f = split(arg, ':');
                uint64_t startSec = 0, us = 0, durSec = 0;
                if (!hasArg || f.size() != 3 || !parseUint(f[0], 3600, &startSec) || !parseUint(f[1], kMaxDelayUs, &us) ||
                    !parseUint(f[2], 3600, &durSec) || us == 0 || durSec == 0) {
                    return fail(err, "shift needs =<startSec>:<us>:<durSec> (startSec 0-3600, us 1-20000, durSec 1-3600)");
                }
                j.m_ShiftStartUs = startSec * 1000000ull;
                j.m_ShiftUs = static_cast<uint32_t>(us);
                j.m_ShiftDurUs = durSec * 1000000ull;
            }
            else {
                return fail(err, kUsage);
            }
        }
        j.m_Spec = spec;
        *this = j;
        return true;
    }

    bool enabled() const { return m_Mode != Mode::Off; }
    const std::string& spec() const { return m_Spec; }

    // 每張串流影像呼叫一次：這一張要延後幾微秒。nowUs＝呼叫時刻（shift 從第一次呼叫起算）
    uint32_t nextUs(uint64_t nowUs)
    {
        if (m_Mode == Mode::Off) {
            return 0;
        }
        if (!m_Started) {
            m_Started = true;
            m_FirstUs = nowUs;
        }
        uint32_t d = 0;
        switch (m_Mode) {
        case Mode::Worn:
            if (m_Rough) {
                if (permille(20)) {
                    m_Rough = false;
                }
            }
            else if (permille(8)) {
                m_Rough = true;
            }
            if (permille(m_Rough ? 750u : 250u)) {
                d = (std::min)(expoUs(m_Rough ? 900.0 : 700.0), static_cast<uint32_t>(6000));
            }
            break;
        case Mode::Light:
            if (permille(30)) {
                d = (std::min)(expoUs(500.0), static_cast<uint32_t>(4000));
            }
            break;
        case Mode::Early:
            d = permille(m_EarlyPermille) ? 0u : m_EarlyBaseUs;
            break;
        case Mode::Scan:
            d = rnd() % (m_ScanMaxUs + 1u);
            break;
        case Mode::Off:
            break;
        }
        if (m_ShiftUs != 0 && nowUs >= m_FirstUs) {
            const uint64_t t = nowUs - m_FirstUs;
            if (t >= m_ShiftStartUs && t - m_ShiftStartUs < m_ShiftDurUs) {
                d += m_ShiftUs;
            }
        }
        return (std::min)(d, kMaxTotalUs);
    }

private:
    enum class Mode { Off, Worn, Light, Early, Scan };

    static bool fail(std::string* err, const char* why)
    {
        if (err != nullptr) {
            *err = why;
        }
        return false;
    }

    static std::vector<std::string> split(const std::string& s, char sep)
    {
        std::vector<std::string> out;
        size_t start = 0;
        for (;;) {
            const size_t p = s.find(sep, start);
            if (p == std::string::npos) {
                out.push_back(s.substr(start));
                return out;
            }
            out.push_back(s.substr(start, p - start));
            start = p + 1;
        }
    }

    static void splitKeyValue(const std::string& s, std::string* key, std::string* value, bool* hasValue)
    {
        const size_t eq = s.find('=');
        *hasValue = eq != std::string::npos;
        *key = s.substr(0, eq);
        *value = *hasValue ? s.substr(eq + 1) : std::string();
    }

    // 只收十進位數字（不收正負號、空白、小數）
    static bool parseUint(const std::string& s, uint64_t maxValue, uint64_t* out)
    {
        if (s.empty() || s.size() > 10) {
            return false;
        }
        uint64_t v = 0;
        for (char ch : s) {
            if (ch < '0' || ch > '9') {
                return false;
            }
            v = v * 10u + static_cast<uint64_t>(ch - '0');
        }
        if (v > maxValue) {
            return false;
        }
        *out = v;
        return true;
    }

    uint32_t rnd()
    {
        m_Rng ^= m_Rng << 13;
        m_Rng ^= m_Rng >> 17;
        m_Rng ^= m_Rng << 5;
        return m_Rng;
    }

    bool permille(uint32_t p) { return ((static_cast<uint64_t>(rnd()) * 1000u) >> 32) < p; }

    // 指數分佈（平均 meanUs）
    uint32_t expoUs(double meanUs)
    {
        const double u = (static_cast<double>(rnd() >> 8) + 1.0) / 16777216.0;  // (0, 1]
        const double v = -meanUs * std::log(u);
        return v >= 1e6 ? 1000000u : static_cast<uint32_t>(v);
    }

    Mode m_Mode = Mode::Off;
    std::string m_Spec;
    uint32_t m_Rng = 0x9E3779B9u;
    bool m_Rough = false;
    uint32_t m_EarlyBaseUs = 5500;
    uint32_t m_EarlyPermille = 10;
    uint32_t m_ScanMaxUs = 0;
    uint64_t m_ShiftStartUs = 0;
    uint64_t m_ShiftDurUs = 0;
    uint32_t m_ShiftUs = 0;
    bool m_Started = false;
    uint64_t m_FirstUs = 0;
};

namespace detail {

struct ScriptFrame {
    int slot;       // host 的 Present 落在第幾個顯示格（pts＝(slot＋ptsAdj) × 一個顯示週期）
    double pubAt;   // 在 client 發布的時刻，單位＝顯示週期（第 n 格的 latch 在時刻 n）
    double ptsAdj;  // Present 時刻偏離格點多少（顯示週期；正＝晚）
    // GPU 工作送出的時刻在發布之前多久（顯示週期；0.18 個週期在 120 Hz 約 1.5 ms）。負值＝這一張不給送出時刻
    double subBefore = 0.18;
};

// 一段腳本。frames 依發布時刻遞增；第 i 張的發布序號是 i＋1
struct Script {
    std::vector<ScriptFrame> frames;
    int ticks = 0;                // 跑幾個顯示格
    std::vector<int> unreported;  // 這幾格 frame thread 沒有回報（沒跑到，或那一格沒送影像層）
    int64_t ptsBase = 0;          // pts 的起點（測 32 bit 繞回用）
    uint64_t noRecordSeq = 0;     // 這個序號的發布紀錄不給（抄錄時已經被洗掉）；0＝都給
};

// 照 frame thread 的規則跑一段腳本：第 n 格（latch 在 n × 週期）送出當時已經發布的最新一張。
// afterTake（可以不給）＝取完計數之後緊接著再取一次的結果（應該全部是 0，stride 照抄）
inline Counters runScript(const Script& s, double periodUs, Counters* afterTake = nullptr)
{
    Classifier c;
    const double ptsPerSlot = 90000.0 * periodUs / 1e6;
    const uint64_t baseUs = 1000000;  // 時刻不從 0 起算
    size_t next = 0;
    uint64_t seq = 0;
    for (int n = 0; n < s.ticks; n++) {
        if (std::find(s.unreported.begin(), s.unreported.end(), n) != s.unreported.end()) {
            continue;  // 這一格沒有回報；之前發布的等下一個有回報的格再一起交（和 XrContext::pcvrLatchDiag 一樣）
        }
        const uint64_t latchUs = baseUs + static_cast<uint64_t>(std::llround(n * periodUs));
        while (next < s.frames.size()) {
            const ScriptFrame& fr = s.frames[next];
            const uint64_t pubUs = baseUs + static_cast<uint64_t>(std::llround(fr.pubAt * periodUs));
            if (pubUs > latchUs) {
                break;
            }
            ++seq;
            if (seq != s.noRecordSeq) {
                // pts 和 RTP 時戳一樣只有低 32 bit
                const int64_t pts = s.ptsBase + static_cast<int64_t>(std::llround((fr.slot + fr.ptsAdj) * ptsPerSlot));
                const uint64_t submitUs =
                    fr.subBefore < 0.0 ? 0 : baseUs + static_cast<uint64_t>(std::llround((fr.pubAt - fr.subBefore) * periodUs));
                c.onPublished(seq, pts & static_cast<int64_t>(0xffffffffll), pubUs, submitUs);
            }
            ++next;
        }
        if (seq != 0) {
            Tick t;
            t.dispTick = n;
            t.latchUs = latchUs;
            t.seq = seq;
            t.periodUs = periodUs;
            c.onTick(t);
        }
    }
    const Counters got = c.take();
    if (afterTake != nullptr) {
        *afterTake = c.take();
    }
    return got;
}

// 準時的串流：第 i 張在第 i × stride 個顯示格 Present，發布在它該被挑到的那個 latch 之前半個週期
inline std::vector<ScriptFrame> onTime(int count, int stride)
{
    std::vector<ScriptFrame> f;
    f.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; i++) {
        f.push_back({i * stride, i * stride + 0.5, 0.0});
    }
    return f;
}

inline std::string describe(const Counters& c)
{
    char buf[160];
    std::snprintf(buf, sizeof(buf), "late=%u lateShift=%u gap=%u early=%u catchup=%u lateMax=%u us", c.late, c.lateShift, c.gap,
                  c.early, c.catchup, c.lateMaxUs);
    return buf;
}

inline bool matches(const Counters& c, uint32_t late, uint32_t lateShift, uint32_t gap, uint32_t early, uint32_t catchup,
                    double lateMaxUs)
{
    const double dmax = static_cast<double>(c.lateMaxUs) - lateMaxUs;
    return c.late == late && c.lateShift == lateShift && c.gap == gap && c.early == early && c.catchup == catchup &&
           dmax >= -3.0 && dmax <= 3.0;
}

constexpr int kCaseCount = 18;

// 腳本序列，以 periodUs 的顯示週期跑一遍。which＝0～kCaseCount−1（名稱在 selfTest 的 kCaseNames）；回傳是否符合預期，
// *got 是實際計數（一組裡有好幾段時，是第一段不符合的、或最後一段的）。
// 全速的腳本：第 i 張準時的話在第 i＋1 格被挑到；半速：第 i 張在第 2i＋1 格被挑到、第 2i＋2 格重複。
inline bool runCase(int which, double periodUs, Counters* got)
{
    const int kFrames = 300;
    const double T = periodUs;
    Script s;
    s.ticks = kFrames + 2;
    std::vector<ScriptFrame>& f = s.frames;
    // 跑目前的腳本並比對：late、lateShift、gap、early、catchup、lateMax（µs）
    auto run = [&](uint32_t late, uint32_t lateShift, uint32_t gap, uint32_t early, uint32_t catchup, double lateMaxUs) {
        *got = runScript(s, periodUs);
        return matches(*got, late, lateShift, gap, early, catchup, lateMaxUs);
    };
    // 第 from～to−1 張都比自己的 latch 晚 0.1 個週期才發布（全速：整條晚一格顯示）
    auto lateRun = [&](int from, int to) {
        for (int i = from; i < to; i++) {
            f[static_cast<size_t>(i)].pubAt = i + 1.1;
        }
    };
    bool ok = true;
    switch (which) {
    case 0:  // 正常：每一張都在自己那一格之前到
        f = onTime(kFrames, 1);
        return run(0, 0, 0, 0, 0, 0.0);
    case 1:  // 晚到被跳過：第 100 張比它的 latch 晚 0.1 個週期，下一格時第 101 張也到了，第 100 張沒顯示過
        f = onTime(kFrames, 1);
        f[100].pubAt = 101.1;
        return run(1, 0, 0, 0, 0, 0.1 * T);
    case 2:  // 早到滑格：第 100 張提早到、搶了第 99 張的那一格；下一格重複，之後回到原位
        f = onTime(kFrames, 1);
        f[100].pubAt = 99.9;
        return run(0, 0, 0, 1, 0, 0.0);
    case 3:  // host 少一幀：第 100 個顯示格沒有影像（序號連續，pts 跳一格）
        f = onTime(kFrames, 1);
        f.erase(f.begin() + 100);
        return run(0, 0, 1, 0, 0, 0.0);
    case 4:  // 連續晚到後追回：第 100～109 張都晚 0.1 個週期（整條晚一格），第 110 張恢復準時、跳掉第 109 張
        f = onTime(kFrames, 1);
        lateRun(100, 110);
        return run(0, 1, 0, 0, 1, 0.1 * T);
    case 5:  // 半速（stride 2）：每張顯示兩格不算重複。第 50 張晚到被第 51 張蓋掉、第 100 張 host 沒出
        f = onTime(kFrames / 2, 2);
        f[50].pubAt = 102.7;  // 該在第 101 格被挑到
        f[51].pubAt = 102.8;
        f.erase(f.begin() + 100);
        return run(1, 0, 1, 0, 0, 1.7 * T);
    case 6:  // 基準更新：晚一格的狀態維持到「連 120 張正常」，它就變成基準，之後回到原位算 early 而不是 catchup
        f = onTime(kFrames, 1);
        lateRun(100, 222);  // 第 100～220 張晚一格顯示（第 221 張被第 222 張蓋掉）：之間剛好 120 次正常
        ok = run(0, 1, 0, 1, 0, 0.1 * T);
        f = onTime(kFrames, 1);
        lateRun(100, 221);  // 少一張，只有 119 次正常：基準沒變，回到原位算 catchup
        return ok && run(0, 1, 0, 0, 1, 0.1 * T);
    case 7:  // pts 是 32 bit 的 RTP 時戳：跨過 2^32 的正常串流不能算出任何東西；剛好在繞回的地方少一幀仍然是 gap 1
        s.ptsBase = (static_cast<int64_t>(1) << 32) - static_cast<int64_t>(std::llround(150.0 * 90000.0 * periodUs / 1e6));
        f = onTime(kFrames, 1);
        ok = run(0, 0, 0, 0, 0, 0.0);
        f.erase(f.begin() + 150);
        return ok && run(0, 0, 1, 0, 0, 0.0);
    case 8:  // frame thread 漏格（那幾格沒有回報）：被跳過的影像不是晚到，不能算出任何東西
        f = onTime(kFrames, 1);
        s.unreported = {100, 150, 151, 152};
        ok = run(0, 0, 0, 0, 0, 0.0);
        // 漏格之前已經有一格在等（第 100、101 張晚到，漏掉的那一格過後才一起被蓋掉）：回報過的等待格只有一格，late 算 1
        f = onTime(kFrames, 1);
        f[100].pubAt = 102.1;
        f[101].pubAt = 102.2;
        s.unreported = {102};
        return ok && run(1, 0, 0, 0, 0, 1.1 * T);
    case 9:  // 半速的 frame thread 漏格：漏掉該換新影像的那一格（那一張晚一格顯示、下一張回到原位）、漏掉重複的那一格、
             // 連漏兩格（中間那一張被跳過）——都不是影像造成的，不能算出任何東西
        f = onTime(kFrames / 2, 2);
        s.unreported = {101, 140, 183, 184};
        return run(0, 0, 0, 0, 0, 0.0);
    case 10:  // 停了三格才到（三張一起到，前兩張沒顯示過），而且之後整條晚一格：late 2＋lateShift 1
        f = onTime(kFrames, 1);
        for (int i = 100; i < kFrames; i++) {
            f[static_cast<size_t>(i)].pubAt = i + 1.5;
        }
        f[100].pubAt = 103.1;
        f[101].pubAt = 103.2;
        f[102].pubAt = 103.3;
        return run(2, 1, 0, 0, 0, 2.1 * T);
    case 11:  // 半速：一張只晚一格（它只顯示了一格，沒有任何一張被跳過）→ lateShift 1，下一張回到原位 → catchup 1
        f = onTime(kFrames / 2, 2);
        f[50].pubAt = 101.1;
        return run(0, 1, 0, 0, 1, 0.1 * T);
    case 12:  // 幾張一起晚到、同一格才到齊：每一張都算 late，晚多久從第一張該被挑到的那一格起算
        f = onTime(kFrames, 1);
        f[100].pubAt = 102.1;
        f[101].pubAt = 102.2;
        ok = run(2, 0, 0, 0, 0, 1.1 * T);
        f = onTime(kFrames, 1);
        for (int i = 100; i < 109; i++) {  // 九張：第一張的發布紀錄要還查得到
            f[static_cast<size_t>(i)].pubAt = 109.1 + 0.01 * (i - 100);
        }
        return ok && run(9, 0, 0, 0, 0, (std::min)(8.1 * T, 65535.0));
    case 13:  // 「串流一幀佔幾格」的估計
        f = onTime(kFrames, 1);
        f.erase(f.begin() + 2);  // 開場第三張就少一幀：間隔的紀錄只有 1、2 各一筆，一樣多取小的（全速）
        ok = run(0, 0, 1, 0, 0, 0.0);
        f = onTime(kFrames / 2, 2);
        f.erase(f.begin() + 52);  // 半速連續兩次少一幀（間隔 4、4）：眾數仍然是 2，兩次都是 gap
        f.erase(f.begin() + 50);
        ok = ok && run(0, 0, 2, 0, 0, 0.0);
        f = onTime(kFrames, 1);
        for (int i = 100; i < 140; i += 2) {  // 全速、連 20 次每隔一張就有一張晚到被跳過：不可以因此估成半速
            f[static_cast<size_t>(i)].pubAt = i + 1.1;
        }
        return ok && run(20, 0, 0, 0, 0, 0.1 * T);
    case 14:  // 發布紀錄有缺（frame thread 停太久，紀錄被洗掉）
        f = onTime(kFrames, 1);
        f[100].pubAt = 101.1;
        s.noRecordSeq = 101;  // 缺的是被跳過的那一張：照樣算 late，只是不知道它晚多久
        ok = run(1, 0, 0, 0, 0, 0.0);
        f = onTime(kFrames, 1);
        s.noRecordSeq = 150;  // 缺的是挑到的那一張：只重新對位，不能算出任何東西
        return ok && run(0, 0, 0, 0, 0, 0.0);
    case 15:  // 漏格的那一次同時對位晚了一格：分不出是誰造成的，不算；之後回到原位那一次也不算
        f = onTime(kFrames, 1);
        lateRun(100, 110);
        f[150].pubAt = 149.9;  // 再之後的早到滑格照算（抵過一次就沒有了）
        s.unreported = {101};
        ok = run(0, 0, 0, 1, 0, 0.0);
        f = onTime(kFrames, 1);
        lateRun(100, 222);  // 晚一格的狀態維持到連 120 張正常：它變成基準、沒抵掉的那一筆作廢，回到原位算 early
        return ok && run(0, 0, 0, 1, 0, 0.0);
    case 16:  // host 的 Present 時刻不在格點上
        f = onTime(kFrames, 1);
        f[100].ptsAdj = 0.6;  // 晚 Present 0.6 格、照樣趕上自己那一格：顯示端正常，不能算出任何東西
        ok = run(0, 0, 0, 0, 0, 0.0);
        f = onTime(kFrames, 1);
        f[100].ptsAdj = 0.8;  // 晚 0.8 格：超過放寬的範圍，照 pts 算成前面少一格、這一張提早一格（已知的限制，檔頭第 3 點）
        ok = ok && run(0, 0, 1, 1, 0, 0.0);
        f = onTime(kFrames, 1);
        f[101].pubAt = 100.9;  // 真的少一幀、下一張又提早一格到：顯示端每格都有新影像，但 pts 隔了整整兩格，照算
        f.erase(f.begin() + 100);
        ok = ok && run(0, 0, 1, 1, 0, 0.0);
        f = onTime(kFrames, 1);
        f[102].pubAt = 101.9;  // 少一幀、等了一格，再下一張提早到並蓋掉中間那一張：被蓋掉的那一張不算 late
        f.erase(f.begin() + 100);
        ok = ok && run(0, 0, 1, 1, 0, 0.0);
        f = onTime(kFrames, 1);
        lateRun(100, 110);
        f[100].ptsAdj = 0.4;  // 晚 Present 0.4 格、而且整條晚一格顯示：顯示端不正常，不放寬，照四捨五入算成 lateShift
        ok = ok && run(0, 1, 0, 0, 1, 0.1 * T);
        f = onTime(kFrames, 1);
        f[100].pubAt = 99.9;
        f[100].ptsAdj = -0.3;  // 提早 Present 0.3 格的那一張搶了前一格：有影像被跳過，不放寬，照算 early
        return ok && run(0, 0, 0, 1, 0, 0.0);
    default:  // 半速、整條晚一格之後，host 有一張自己晚了一格才 Present（前後的間隔變成 3 格、1 格）。間隔不是 2 格的
              // 那兩次不算「正常」，所以到回位為止只連續了 119 次正常、基準沒變：回到原位算 catchup 而不是 early
        f = onTime(kFrames / 2, 2);
        for (int i = 20; i < 146; i++) {
            f[static_cast<size_t>(i)].pubAt = 2.0 * i + 1.1;
        }
        f[25].slot = 51;
        f[25].pubAt = 52.1;
        return run(0, 1, 1, 0, 1, 0.1 * T);
    }
}

// ── 不看基準的晚到計數（Counters 的 waitNew 以下）──

inline std::string describeWait(const Counters& c)
{
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "stride=%u waitNew=%u waitBy=%u,%u,%u,%u,%u,%u,%u,%u waitSlots=%u,%u,%u resc=%u rescBy=%u,%u,%u,%u,%u,%u flip=%u",
                  c.stride, c.waitNew, c.waitBy[0], c.waitBy[1], c.waitBy[2], c.waitBy[3], c.waitBy[4], c.waitBy[5], c.waitBy[6],
                  c.waitBy[7], c.waitSlots[0], c.waitSlots[1], c.waitSlots[2], c.resc, c.rescBy[0], c.rescBy[1], c.rescBy[2],
                  c.rescBy[3], c.rescBy[4], c.rescBy[5], c.flip);
    return buf;
}

// waitNew、resc、flip 的值；byIdx／slotIdx／rescIdx＝waitBy／waitSlots／rescBy 裡唯一有計數的那一格（-1＝整組都是 0）
inline bool waitMatches(const Counters& c, uint32_t waitNew, int byIdx, int slotIdx, uint32_t resc, int rescIdx, uint32_t flip)
{
    bool ok = c.waitNew == waitNew && c.resc == resc && c.flip == flip;
    for (int i = 0; i < 8; i++) {
        ok = ok && c.waitBy[i] == (i == byIdx ? waitNew : 0u);
    }
    for (int i = 0; i < 3; i++) {
        ok = ok && c.waitSlots[i] == (i == slotIdx ? waitNew : 0u);
    }
    for (int i = 0; i < 6; i++) {
        ok = ok && c.rescBy[i] == (i == rescIdx ? resc : 0u);
    }
    return ok;
}

// 這幾個計數自己對得上：分格的總和等於總數、resc 不超過 waitNew
inline bool waitConsistent(const Counters& c)
{
    // （變數不可以叫 slots：這個檔會被有 Qt 標頭的原始檔 include，那是 Qt 的關鍵字巨集）
    uint32_t bySum = 0, slotSum = 0, rescBySum = 0;
    for (uint32_t v : c.waitBy) {
        bySum += v;
    }
    for (uint32_t v : c.waitSlots) {
        slotSum += v;
    }
    for (uint32_t v : c.rescBy) {
        rescBySum += v;
    }
    return bySum == c.waitNew && slotSum == c.waitNew && rescBySum == c.resc && c.resc <= c.waitNew;
}

constexpr int kWaitCaseCount = 15;

// 腳本序列（120 Hz，時刻寫成「第 k 格的 latch 之後幾微秒」，負值＝之前）。which＝0～kWaitCaseCount−1（名稱在 selfTest
// 的 kWaitCaseNames）；回傳是否符合預期，*got 是實際計數（一組裡有好幾段時，是第一段不符合的、或最後一段的）。
// 全速的腳本：第 i 張準時的話在第 i＋1 格被挑到；半速：第 i 張在第 2i＋1 格被挑到、第 2i＋2 格重複。
inline bool runWaitCase(int which, Counters* got)
{
    const int kFrames = 300;
    const double T = 8333.0;
    Script s;
    s.ticks = kFrames + 2;
    std::vector<ScriptFrame>& f = s.frames;
    // 第 i 張改成：在第 k 格的 latch 之後 pubUs 發布、subUs 送出
    auto place = [&](int i, int k, double pubUs, double subUs) {
        f[static_cast<size_t>(i)].pubAt = k + pubUs / T;
        f[static_cast<size_t>(i)].subBefore = (pubUs - subUs) / T;
    };
    // 第 from 張起整條晚 lateBy 格（照新的對位每一張都準時：發布在自己的 latch 之前半個週期）
    auto shiftFrom = [&](int from, int stride, int lateBy) {
        for (size_t i = static_cast<size_t>(from); i < f.size(); i++) {
            f[i].pubAt = static_cast<double>(i) * stride + lateBy + 0.5;
        }
    };
    auto noSubmitTimes = [&]() {
        for (ScriptFrame& fr : f) {
            fr.subBefore = -1.0;
        }
    };
    auto run = [&](uint32_t waitNew, int byIdx, int slotIdx, uint32_t resc, int rescIdx, uint32_t flip) {
        *got = runScript(s, T);
        return waitMatches(*got, waitNew, byIdx, slotIdx, resc, rescIdx, flip);
    };
    bool ok = true;
    switch (which) {
    case 0:  // 全速，差一點：第 100 張在第 101 格的 latch 之前 1200 µs 送出、之後 300 µs 才發布；那一格重複、下一格顯示它
        f = onTime(kFrames, 1);
        shiftFrom(101, 1, 1);
        place(100, 101, 300.0, -1200.0);
        return run(1, 1, 0, 1, 1, 0);
    case 1:  // 全速，真的晚：latch 之後 500 µs 才送出、2000 µs 發布——等待格有，但那個 latch 當下還沒送出
        f = onTime(kFrames, 1);
        shiftFrom(101, 1, 1);
        place(100, 101, 2000.0, 500.0);
        return run(1, 5, 0, 0, -1, 0);
    case 2:  // 「已經送出」的邊界：早 100 µs、149 µs 不算，正好 150 µs（kRescMarginUs）才算
        f = onTime(kFrames, 1);
        shiftFrom(101, 1, 1);
        place(100, 101, 1400.0, -100.0);
        ok = run(1, 4, 0, 0, -1, 0);
        place(100, 101, 1400.0, -149.0);
        ok = ok && run(1, 4, 0, 0, -1, 0);
        place(100, 101, 1400.0, -150.0);
        return ok && run(1, 4, 0, 1, 4, 0);
    case 3:  // 全速、沒有等待格：第 101 張在第 101 格（顯示第 100 張）的 latch 之前 1000 µs 送出、之後 500 µs 發布，
             // 照常在第 102 格顯示。一送出就發布的話它會搶掉第 100 張的那一格 → flip
        f = onTime(kFrames, 1);
        place(101, 101, 500.0, -1000.0);
        return run(0, -1, -1, 0, -1, 1);
    case 4:  // 全速、準時：latch 之後 2000 µs 送出、3600 µs 發布 → 什麼都不計
        f = onTime(kFrames, 1);
        place(101, 101, 3600.0, 2000.0);
        return run(0, -1, -1, 0, -1, 0);
    case 5:  // 半速、晚一格：第 50 張顯示在第 101、102 格；第 51 張在第 103 格的 latch 之前 1300 µs 送出、之後 200 µs 發布
             // → 第 103 格重複（等待格）、第 104 格才顯示
        f = onTime(kFrames / 2, 2);
        place(51, 103, 200.0, -1300.0);
        return run(1, 0, 0, 1, 0, 0) && got->stride == 2;
    case 6:  // 半速、第二次顯示的那一格：第 51 張在第 102 格（第 50 張第二次顯示，不是等待格）的 latch 之前 800 µs 送出、
             // 之後 700 µs 發布，照常在第 103 格顯示。一送出就發布的話會搶掉那一格（補幀開著時就是合成的那一格）→ flip，
             // 不算救得到的
        f = onTime(kFrames / 2, 2);
        place(51, 102, 700.0, -800.0);
        return run(0, -1, -1, 0, -1, 1) && got->stride == 2;
    case 7:  // 半速、晚兩格：第 103、104 格都重複，第 51 張在第 104 格的 latch 之前送出、之後 100 µs 發布，第 105 格顯示。
             // 晚多久、送出了沒有都對最後一個等待格算
        f = onTime(kFrames / 2, 2);
        shiftFrom(52, 2, 2);
        place(51, 104, 100.0, -1400.0);
        return run(1, 0, 1, 1, 0, 0);
    case 8:  // 提早搶格之後的那一張：第 100 張早到、在第 100 格就顯示，第 101 格重複它；第 101 張照原本的節拍
             // （latch 之後 1900 µs 送出、3500 µs 發布）來。有等待格、發布得很晚，但它其實準時——waitBy 後面幾格
             // 不能當成晚到的張數
        f = onTime(kFrames, 1);
        f[100].pubAt = 99.9;
        place(101, 101, 3500.0, 1900.0);
        return run(1, 6, 0, 0, -1, 0);
    case 9:  // 上一張新影像之後有一格沒有回報：那一格挑得到什麼不知道，都不計
        f = onTime(kFrames, 1);
        shiftFrom(101, 1, 1);
        place(100, 101, 300.0, -1200.0);
        s.unreported = {101};
        ok = run(0, -1, -1, 0, -1, 0);
        f = onTime(kFrames, 1);
        place(101, 101, 500.0, -1000.0);
        ok = ok && run(0, -1, -1, 0, -1, 0);
        // 等待格有回報（第 101 格），但它和新影像之間的第 102 格沒有回報：一樣不計
        f = onTime(kFrames, 1);
        shiftFrom(101, 1, 2);
        place(100, 102, 300.0, -1200.0);
        s.unreported = {102};
        return ok && run(0, -1, -1, 0, -1, 0);
    case 10: {  // 基準停在晚的那一邊：開場起每一張都在 latch 之後 300 µs 才發布（之前 1200 µs 送出），整條對位就是晚一格，
                // 這也就是 Classifier 的基準。第 150 張難得趕上（提早一格顯示）→ 下一格重複 → 第 151 張照舊晚 300 µs。
                // 原本的分類把這一次記成 early 1、lateShift 0、lateMax 0；這幾個計數照樣看得到那一張晚了 300 µs、救得到。
                // flip 很大：每一張都在上一格的 latch 之前就送出了（300 張扣掉開場兩張、事件那兩格＝296）
        f = onTime(kFrames, 1);
        for (int i = 0; i < kFrames; i++) {
            place(i, i + 1, 300.0, -1200.0);
        }
        f[150].pubAt = 150.9;
        *got = runScript(s, T);
        return matches(*got, 0, 0, 0, 1, 0, 0.0) && waitMatches(*got, 1, 1, 0, 1, 1, 296);
    }
    case 11:  // 兩張擠在同一格：第 100 張晚了 200 µs（latch 之前 1000 µs 送出），下一格時第 101 張也到了，顯示的是
              // 第 101 張。算的是第 100 張——在那個 latch 發布就顯示得到的是它
        f = onTime(kFrames, 1);
        place(100, 101, 200.0, -1000.0);
        return run(1, 0, 0, 1, 0, 0);
    case 12:  // 沒有送出時刻的發布紀錄：這幾個計數全部不動
        f = onTime(kFrames, 1);
        shiftFrom(101, 1, 1);
        place(100, 101, 300.0, -1200.0);
        noSubmitTimes();
        ok = run(0, -1, -1, 0, -1, 0);
        f = onTime(kFrames, 1);
        place(101, 101, 500.0, -1000.0);
        noSubmitTimes();
        return ok && run(0, -1, -1, 0, -1, 0);
    case 13: {  // 分格的邊界：每一條邊界的前、後一微秒各一筆——waitBy 的 250／500／750／1000／1500／2500／4000 µs，
                // rescBy 的 250／500／750／1000／1500 µs（之後都在最後一格）；等了 3 格與 4 格都記在 waitSlots 的「3 以上」
        const struct {
            double pubUs;
            int byIdx, rescIdx;
        } kEdge[] = {{249.0, 0, 0},  {250.0, 1, 1},  {499.0, 1, 1},  {500.0, 2, 2},  {749.0, 2, 2},  {750.0, 3, 3},  {999.0, 3, 3},
                     {1000.0, 4, 4}, {1499.0, 4, 4}, {1500.0, 5, 5}, {2499.0, 5, 5}, {2500.0, 6, 5}, {3999.0, 6, 5}, {4000.0, 7, 5}};
        for (const auto& e : kEdge) {
            f = onTime(kFrames, 1);
            shiftFrom(101, 1, 1);
            place(100, 101, e.pubUs, -1200.0);
            ok = ok && run(1, e.byIdx, 0, 1, e.rescIdx, 0);
        }
        for (int waited = 3; waited <= 4; waited++) {  // 第 100 張在最後一個等待格的 latch 之後 300 µs 才發布
            f = onTime(kFrames, 1);
            shiftFrom(101, 1, waited);
            place(100, 100 + waited, 300.0, -1200.0);
            ok = ok && run(1, 1, 2, 1, 1, 0);
        }
        return ok;
    }
    default: {  // 固定種子的隨機腳本（全速、半速各 2000 張，有剛錯過、晚到、早到、沒回報的格、沒有送出時刻的影像）：
                // 分格的總和對得上、取完歸零、stride 有抄到；拿掉送出時刻之後原本的分類一個數字都不變
        for (int stride = 1; stride <= 2; stride++) {
            uint32_t rng = stride == 1 ? 0x2545F491u : 0x9E3779B9u;
            auto uni = [&rng]() {  // [0, 1)
                rng ^= rng << 13;
                rng ^= rng >> 17;
                rng ^= rng << 5;
                return static_cast<double>(rng >> 8) / 16777216.0;
            };
            const int n = 2000;
            f.clear();
            double prevPub = -1.0;
            for (int i = 0; i < n; i++) {
                const double due = static_cast<double>(i) * stride + 1.0;  // 它該被挑到的那個 latch
                const double r = uni();
                double pub = due - 0.5;
                if (r < 0.10) {
                    pub = due + uni() * 0.12;  // 剛錯過：latch 之後 1 ms 內
                }
                else if (r < 0.14) {
                    pub = due + uni() * 1.4;   // 晚到
                }
                else if (r < 0.17) {
                    pub = due - 0.5 - uni() * 0.8;  // 早到
                }
                pub = (std::max)(pub, prevPub + 0.01);  // 發布的順序就是序號的順序
                prevPub = pub;
                ScriptFrame fr = {i * stride, pub, 0.0, 0.03 + uni() * 0.4};
                if (uni() < 0.02) {
                    fr.subBefore = -1.0;
                }
                f.push_back(fr);
            }
            s.ticks = n * stride + 4;
            s.unreported.clear();
            for (int t = 50; t < s.ticks; t += 97) {
                s.unreported.push_back(t);
            }
            Counters after;
            *got = runScript(s, T, &after);
            const bool cleared = waitMatches(after, 0, -1, -1, 0, -1, 0) && matches(after, 0, 0, 0, 0, 0, 0.0);
            if (!waitConsistent(*got) || got->stride != static_cast<uint32_t>(stride) || after.stride != got->stride || !cleared ||
                got->waitNew < 50 || got->resc < 10 || got->flip < 1) {
                return false;
            }
            const Counters with = *got;
            noSubmitTimes();
            const Counters without = runScript(s, T);
            if (!waitMatches(without, 0, -1, -1, 0, -1, 0) || with.late != without.late || with.lateShift != without.lateShift ||
                with.gap != without.gap || with.early != without.early || with.catchup != without.catchup ||
                with.lateMaxUs != without.lateMaxUs) {
                *got = without;
                return false;
            }
        }
        return true;
    }
    }
}

}  // namespace detail

// 自測；回傳失敗數。每個案例呼叫 report(名稱, 是否通過, 細節)
inline int selfTest(const std::function<void(const char* name, bool ok, const std::string& detail)>& report)
{
    int failures = 0;
    auto caseDone = [&](const char* name, bool ok, const std::string& detail) {
        if (!ok) {
            failures++;
        }
        if (report) {
            report(name, ok, detail);
        }
    };

    // ── Classifier：腳本序列（120 Hz）。前六組是基本的分類，其餘是邊界情況（見 detail::runCase）──
    static const char* const kCaseNames[detail::kCaseCount] = {
        "latch-normal",          "latch-late-skipped",       "latch-early-slip",       "latch-host-gap",
        "latch-late-run-catchup", "latch-half-rate",          "latch-rebaseline",       "latch-pts-wrap",
        "latch-frame-thread-miss", "latch-half-frame-thread-miss", "latch-stall-then-shift", "latch-half-late-one-slot",
        "latch-burst",           "latch-stride-estimate",    "latch-missing-record",   "latch-pardon",
        "latch-present-jitter",  "latch-half-irregular-cadence"};
    for (int i = 0; i < detail::kCaseCount; i++) {
        Counters got;
        const bool ok = detail::runCase(i, 8333.0, &got);
        caseDone(kCaseNames[i], ok, detail::describe(got));
    }
    {
        // 其他更新率：同樣的腳本，預期相同（晚到的時間照週期比例）
        bool ok = true;
        std::string where = "72/90/144 Hz";
        for (double periodUs : {13889.0, 11111.0, 6944.0}) {
            for (int i = 0; i < detail::kCaseCount; i++) {
                Counters got;
                if (!detail::runCase(i, periodUs, &got)) {
                    ok = false;
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%s at period %.0f us: ", kCaseNames[i], periodUs);
                    where = buf + detail::describe(got);
                }
            }
        }
        caseDone("latch-other-rates", ok, where);
    }
    {
        // 對不上的序號（沒有 onPublished 過）只重新對位，不能算出任何一類
        Classifier c;
        for (uint64_t s = 1; s <= 5; s++) {
            c.onPublished(s, static_cast<int64_t>(s) * 750, 1000000 + s * 8333);
        }
        Tick t;
        t.periodUs = 8333.0;
        const uint64_t order[6] = {1, 2, 9, 9, 3, 4};  // 9 沒有紀錄；之後序號倒退
        for (int n = 0; n < 6; n++) {
            t.dispTick = n;
            t.latchUs = 1000000 + static_cast<uint64_t>(n + 1) * 8333;
            t.seq = order[n];
            c.onTick(t);
        }
        const Counters got = c.take();
        caseDone("latch-unknown-seq", detail::matches(got, 0, 0, 0, 0, 0, 0.0), detail::describe(got));
    }

    // ── Jitter：格式 ──
    {
        static const char* const kGood[] = {
            "worn", "light", "early", "early=5500:10", "early=0:0", "scan=6000", "worn,seed=7", "light,shift=60:3000:60",
            "scan=1,seed=1", "worn,seed=4294967295", "scan=6000,shift=10:500:5,seed=3"};
        static const char* const kBad[] = {
            "foo", "worn=1", "light=2", "scan", "scan=", "scan=0", "scan=20001", "scan=1e3", "early=5500", "early=5500:1001",
            "early=:10", "worn,", "worn,bogus=1", "worn,seed", "worn,seed=0", "worn,seed=-1", "worn,shift=1:2",
            "worn,shift=1:0:5", "worn,shift=1:5:0", "worn,shift=3601:5:5", ",worn", " worn", "worn ,seed=1",
            "light,seed=4294967296"};
        std::string bad;
        for (const char* s : kGood) {
            Jitter j;
            std::string err;
            if (!j.configure(s, &err) || !j.enabled() || j.spec() != s) {
                bad += std::string(" rejected:'") + s + "'";
            }
        }
        for (const char* s : kBad) {
            Jitter j;
            std::string err;
            if (j.configure(s, &err) || j.enabled() || err.empty()) {
                bad += std::string(" accepted:'") + s + "'";
            }
        }
        Jitter off;
        std::string err;
        if (!off.configure("", &err) || off.enabled() || off.nextUs(1) != 0) {
            bad += " empty-spec-not-off";
        }
        caseDone("jitter-spec", bad.empty(), bad.empty() ? "11 accepted, 24 rejected, empty = off" : bad);
    }

    // ── Jitter：分佈（固定種子，數字是可重現的）──
    auto draw = [](const char* spec, int n, double* fracDelayed, double* meanDelayed, uint32_t* maxUs) {
        Jitter j;
        std::string err;
        j.configure(spec, &err);
        uint64_t sum = 0;
        int delayed = 0;
        *maxUs = 0;
        for (int i = 0; i < n; i++) {
            const uint32_t d = j.nextUs(1000000ull + static_cast<uint64_t>(i) * 8333ull);
            if (d > 0) {
                delayed++;
                sum += d;
            }
            *maxUs = (std::max)(*maxUs, d);
        }
        *fracDelayed = static_cast<double>(delayed) / n;
        *meanDelayed = delayed > 0 ? static_cast<double>(sum) / delayed : 0.0;
    };
    auto statLine = [](double frac, double mean, uint32_t maxUs) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "delayed=%.1f%% mean=%.0f us max=%u us", frac * 100.0, mean, maxUs);
        return std::string(buf);
    };
    {
        // worn：rough 佔 8/(8+20)，延後的比例 ≈ 0.714×25% ＋ 0.286×75% ＝ 39.3%，平均約 800 µs
        double frac, mean;
        uint32_t maxUs;
        draw("worn", 200000, &frac, &mean, &maxUs);
        caseDone("jitter-worn", frac > 0.33 && frac < 0.45 && mean > 650.0 && mean < 950.0 && maxUs <= 6000 && maxUs > 3000,
              statLine(frac, mean, maxUs));
    }
    {
        double frac, mean;
        uint32_t maxUs;
        draw("light", 200000, &frac, &mean, &maxUs);
        caseDone("jitter-light", frac > 0.02 && frac < 0.04 && mean > 400.0 && mean < 600.0 && maxUs <= 4000, statLine(frac, mean, maxUs));
    }
    {
        // early：99% 的影像延後 5500 µs，1% 不延後
        double frac, mean;
        uint32_t maxUs;
        draw("early", 200000, &frac, &mean, &maxUs);
        caseDone("jitter-early", frac > 0.985 && frac < 0.995 && mean == 5500.0 && maxUs == 5500, statLine(frac, mean, maxUs));
    }
    {
        double frac, mean;
        uint32_t maxUs;
        draw("scan=6000", 200000, &frac, &mean, &maxUs);
        caseDone("jitter-scan", frac > 0.99 && mean > 2850.0 && mean < 3150.0 && maxUs <= 6000 && maxUs > 5900, statLine(frac, mean, maxUs));
    }
    {
        // shift：第一次呼叫後第 10 秒起 5 秒內每張多 3000 µs（early=0:0＝本身不延後）
        Jitter j;
        std::string err;
        j.configure("early=0:0,shift=10:3000:5", &err);
        const uint64_t t0 = 5000000;
        const uint32_t a = j.nextUs(t0);
        const uint32_t b = j.nextUs(t0 + 9999999);
        const uint32_t c = j.nextUs(t0 + 10000000);
        const uint32_t d = j.nextUs(t0 + 14999999);
        const uint32_t e = j.nextUs(t0 + 15000000);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "0s=%u 9.99s=%u 10s=%u 14.99s=%u 15s=%u", a, b, c, d, e);
        caseDone("jitter-shift", a == 0 && b == 0 && c == 3000 && d == 3000 && e == 0, buf);
    }
    {
        // 同一個種子的序列相同、不同種子不同；總延後不超過 kMaxTotalUs
        Jitter a, b, c, big;
        std::string err;
        a.configure("worn,seed=12345", &err);
        b.configure("worn,seed=12345", &err);
        c.configure("worn,seed=54321", &err);
        big.configure("scan=20000,shift=0:20000:10", &err);
        bool same = true, differs = false, capped = true;
        for (int i = 0; i < 5000; i++) {
            const uint64_t now = 1000000ull + static_cast<uint64_t>(i) * 100ull;
            const uint32_t x = a.nextUs(now);
            same = same && x == b.nextUs(now);
            differs = differs || x != c.nextUs(now);
            capped = capped && big.nextUs(now) <= Jitter::kMaxTotalUs;
        }
        caseDone("jitter-seed", same && differs && capped, same && differs && capped ? "repeatable, seed-dependent, capped at 30000 us" : "mismatch");
    }

    // ── Classifier：不看基準的晚到計數（120 Hz 的腳本序列，見 detail::runWaitCase）。排在最後，前面那些案例的輸出不動 ──
    static const char* const kWaitCaseNames[detail::kWaitCaseCount] = {
        "latch-wait-just-missed",      "latch-wait-truly-late",      "latch-wait-submit-margin",  "latch-flip-no-wait-slot",
        "latch-wait-on-time",          "latch-half-wait-just-missed", "latch-half-flip-second-slot", "latch-half-wait-two-slots",
        "latch-wait-after-early-grab", "latch-wait-unreported-slot", "latch-wait-stale-baseline", "latch-wait-two-in-one-slot",
        "latch-wait-no-submit-time",   "latch-wait-bucket-edges",    "latch-wait-random-invariants"};
    for (int i = 0; i < detail::kWaitCaseCount; i++) {
        Counters got;
        const bool ok = detail::runWaitCase(i, &got);
        // 基準過期的那一案兩組都印：原本的分類看不到（early 1、lateMax 0），新的計數看得到
        caseDone(kWaitCaseNames[i], ok,
                 i == 10 ? detail::describe(got) + " | " + detail::describeWait(got) : detail::describeWait(got));
    }
    return failures;
}

}  // namespace xrlatch
