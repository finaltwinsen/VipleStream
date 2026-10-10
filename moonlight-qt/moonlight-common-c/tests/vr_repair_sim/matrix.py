# -*- coding: utf-8 -*-
# §VR-REPAIR-RTT：行為表（模型，不是實測）。往返 5／10／15／20／30／50 ms × 幾種掉法，現行規則與 RTT 規則各跑一次，
# 再用一個簡單的顯示模型算「頭盔上多幾格沒有換到新影像」。估計值＝真實值（不抖動）；頭盔顯示 120 Hz。
#
#   python matrix.py             印出來，並和 matrix_expected.txt 逐字比
#   python matrix.py --update    重寫 matrix_expected.txt（規則有意改動之後，看過差異再做）
#
# 格子的讀法：
#   FEC·0          FEC 當場補回、多 0 格
#   補 +L·n        補包救回，那一幀晚 L ms、多 n 格沒有換到新影像
#   棄k·n·破W      放棄 k 張、多 n 格、破圖到 intra refresh 那一波結束約 W ms
#   ·白等          回報過、等到期限還是放棄
#   ·不補          後面缺太多幀、直接放棄
import os
import sys

import scenarios as S

HERE = os.path.dirname(os.path.abspath(__file__))
EXPECTED = os.path.join(HERE, "matrix_expected.txt")
SIM = os.path.join(S.default_build_dir(), "sim_final" + (".exe" if os.name == "nt" else ""))
OUT = []

TV = 1000.0 / 120.0      # 頭盔顯示週期
PIPE = 6.0               # 收齊→可以被挑走（解碼＋上傳；常數，會消掉）
SLACK = 4.0              # 平常一張影像比它那一格早多久備妥（LATCH 的目標約 40～50% 週期）


def run(mode, fps, rtt, extra, frames, D, more=()):
    args = ["mode=%s" % mode, "fps=%d" % fps, "rtt=%g" % rtt, "frames=%d" % frames, "D=%d" % D, "quiet=1", "origin=3"] + extra + list(more)
    out, rc = S.run_sim(SIM, args)
    if out is None or rc != 0:
        sys.exit("simulator failed: " + " ".join(args))
    res, summ, tot = S.parse_result(out)
    return {f: (r["nominal"], r["done"]) for f, r in res.items()}, summ, tot


def shown_sequence(res, D, last_frame, on_time=False):
    """每一個顯示格（120 Hz）挑到哪一張串流影像"""
    c0 = D * 0.07 + 0.01
    n1 = res[1][0]
    base = n1 + c0 + PIPE + SLACK            # 第 1 幀在它自己那一格被挑走，提早 SLACK 備妥
    ready = {}
    for f, (nom, done) in res.items():
        if on_time:
            ready[f] = nom + c0 + PIPE
        elif done is not None:
            ready[f] = done + PIPE
    t_end = res[last_frame][0] + c0 + PIPE + SLACK + 0.01
    seq = []
    k = 0
    while True:
        latch = base + k * TV
        if latch > t_end:
            break
        cur = 0
        for f in sorted(ready):
            if ready[f] <= latch + 1e-6:
                cur = f
        seq.append(cur)
        k += 1
    return seq


def display(res, D, last_frame):
    """回傳（比全部準時多出來的「沒有新影像」格數, 最晚的一張晚了幾 ms）"""
    a = shown_sequence(res, D, last_frame)
    b = shown_sequence(res, D, last_frame, on_time=True)
    rep_a = sum(1 for i in range(1, len(a)) if a[i] == a[i - 1])
    rep_b = sum(1 for i in range(1, len(b)) if b[i] == b[i - 1])
    worst_late = 0.0
    for f, (nom, done) in res.items():
        if done is not None and f <= last_frame:
            worst_late = max(worst_late, done - nom)
    return rep_a - rep_b, worst_late


def cell(fps, rtt, extra, D, mode, more=()):
    frames = 26
    last = 20 if fps == 120 else 14
    res, s, tot = run(mode, fps, rtt, extra, frames, D, more)
    rep, late = display(res, D, last)
    lost = sum(1 for f, (nom, done) in res.items() if done is None and f <= last)
    nacks = int(s.get("nacks", 0))
    if lost == 0 and nacks == 0:
        return "FEC·0"
    if lost == 0:
        return "補 +%.0f·%d" % (late, rep)
    rec = int(round(s["waveDone"] - s["firstLoss"])) if s.get("waveDone", -1) > 0 else 0
    t = "棄%d·%d·破%d" % (lost, rep, rec)
    if nacks:
        t += "·白等"
    elif int(tot.get("tooMany", 0)):
        t += "·不補"
    return t


def table(title, fps, D, scen, more=()):
    rtts = (5, 10, 15, 20, 30, 50)
    OUT.append("")
    OUT.append("**%s**" % title)
    OUT.append("")
    OUT.append("| 掉法 | 規則 | " + " | ".join("%d ms" % r for r in rtts) + " |")
    OUT.append("|---|---|" + "---|" * len(rtts))
    for name, extra in scen:
        for mode, mname in (("legacy", "現行"), ("rtt", "RTT")):
            OUT.append("| %s | %s | %s |" % (name if mode == "legacy" else "", mname, " | ".join(cell(fps, r, extra, D, mode, more) for r in rtts)))


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except AttributeError:
        pass
    if not os.path.exists(SIM):
        sys.exit("missing %s - run build.sh first" % SIM)
    low = [("掉單一封包", ["drop=3:1-1"]), ("一段掉 5 個（尾端有到）", ["drop=3:0-4"]), ("尾端掉 5 個", ["drop=3:4-8"]),
           ("空白 5 ms（一幀整個沒到）", ["gap=-1:4"]), ("空白 20 ms（120 Hz 三幀、60 Hz 兩幀沒到）", ["gap=-1:19"]),
           ("空白 40 ms（120 Hz 五幀、60 Hz 三幀沒到）", ["gap=-1:39"])]
    table("串流 120 Hz、每幀 6 data＋3 parity（位元率在下限時的典型大小）", 120, 6, low)
    table("串流 60 Hz（半速）、每幀 6 data＋3 parity", 60, 6, low)
    hi = [("中段掉 40 個（尾端有到）", ["drop=3:10-49"]), ("尾端掉 45 個", ["drop=3:56-100"]), ("整幀沒到（空白 5.5 ms）", ["gap=-0.5:5"]),
          ("空白 20 ms", ["gap=-0.5:19.5"])]
    table("串流 120 Hz、每幀 72 data＋29 parity（約 89 Mbps；封包間隔 49 µs）", 120, 72, hi, more=("spacing=49",))

    text = "\n".join(OUT) + "\n"
    sys.stdout.write(text)
    if "--update" in sys.argv:
        with open(EXPECTED, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("wrote " + EXPECTED)
        return 0
    with open(EXPECTED, encoding="utf-8") as f:
        want = f.read()
    ok = want == text
    print("matrix: %s" % ("PASS" if ok else "FAIL (differs from matrix_expected.txt; review, then rerun with --update)"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
