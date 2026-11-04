#pragma once

#include <QString>

// §TS-PATH：串流走 Tailscale CGNAT（100.64.0.0/10）時，在背景看這條路徑是直連還是
// DERP 中繼。掉到中繼就依序 disco ping → restun → rebind，試著恢復直連。
// rebind 只重建 UDP socket，中繼那條（TCP）不斷，不會中斷串流。
// 不是 Tailscale 位址就什麼都不做。start／stop 可以從任何執行緒呼叫；stop 立刻返回。
class TailscalePath {
public:
    /// 串流開始（hostAddress 是 client 連的 host 位址字串）。不是 100.64.0.0/10 就什麼都不做。
    static void start(const QString& hostAddress);
    /// 串流結束：停止（必須很快返回）
    static void stop();
};
