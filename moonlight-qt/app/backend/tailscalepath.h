#pragma once

#include <QString>

// §TS-PATH：串流走 Tailscale CGNAT（100.64.0.0/10）時，在背景看這條路徑是直連還是
// DERP 中繼。掉到中繼就依序 disco ping → restun → rebind，試著恢復直連。
// rebind 有代價（tailscaled 會清掉所有 peer 已打通的直連位址，並 ping DERP、3 秒沒回就關掉重連），
// 所以排在最後、有間隔與次數上限；restun 只是重新問 STUN，看到中繼的當輪就做。
// 不是 Tailscale 位址就什麼都不做。start／stop 可以從任何執行緒呼叫；stop 立刻返回。
class TailscalePath {
public:
    /// 串流開始（hostAddress 是 client 連的 host 位址字串）。不是 100.64.0.0/10 就什麼都不做。
    static void start(const QString& hostAddress);
    /// 串流結束：停止（必須很快返回）
    static void stop();
    /// 監看中的那條路徑目前是不是走 DERP 中繼（TCP 轉送）。直連、UDP peer relay、沒在監看、連不上 tailscaled 都回 false；
    /// 暫時讀不到狀態時沿用上一次的結果，連續兩輪讀不到就回 false。
    /// 通常落後不超過一個輪詢週期（當輪的 ping 等動作比週期久時，是動作做完再 1 秒）。
    /// 只讀一個 atomic，可以從任何執行緒每拍呼叫。
    static bool onDerpRelay();
};
