# -*- coding: utf-8 -*-
# §VR-REPAIR-RTT：逐項情境（模型，不是實測）。設計審查時提出的每一個反例各跑一次現行規則與 RTT 規則，印出：
# 沒收齊幾幀、第一次 LOSS 的時刻、intra refresh 那一波結束的時刻、回報數、補送封包數、受影響的幾幀各晚多久
# （相對它第一個封包該到的時刻；時間都相對第 3 幀第一個封包該到的時刻，單位 ms）。
#
#   python scen.py [組別…]         印出來（不給組別＝全部），並和 scen_expected.txt 逐字比
#   python scen.py --update        重寫 scen_expected.txt（規則有意改動之後，看過差異再做）
#
# 組別：j2a 估計值偏低｜j2b 補包那一輪掉了｜j2c 估計取錯連線｜j2d 目前的 block 先報過、後面才發現整幀沒到｜
#       j2e 長空白｜j2f 幀內停頓（沒有掉包）之後才有一張真的尾端掉｜j2g 規則切換的那一刻｜j1a 整段空白與 60 Hz｜
#       brk 補包永遠到不了（保險）｜hold 放棄之後的暫停｜chain 成串掉包（連續好幾幀各缺一段）｜
#       gone 預先報的幀收到 REPAIR_GONE
import os
import sys

import scenarios as S

HERE = os.path.dirname(os.path.abspath(__file__))
EXPECTED = os.path.join(HERE, "scen_expected.txt")
SIM = os.path.join(S.default_build_dir(), "sim_final" + (".exe" if os.name == "nt" else ""))
OUT = []


def emit(s=""):
    OUT.append(s)


def run(mode, args):
    out, rc = S.run_sim(SIM, ["mode=%s" % mode, "quiet=1", "origin=3"] + args.split())
    if out is None or rc != 0:
        return None, None, None
    return S.parse_result(out)


def desc(res, summ, tot, upto=20):
    if res is None:
        return "沒跑完"
    notdone = [f for f, r in sorted(res.items()) if r["done"] is None and f <= upto]
    late = []
    for f, r in sorted(res.items()):
        if r["done"] is not None and f <= upto and r["done"] - r["nominal"] > 1.0:
            late.append("%d:+%.1f" % (f, r["done"] - r["nominal"]))
    s = "沒收齊 %d 幀%s" % (len(notdone), ("（" + ",".join(map(str, notdone)) + "）") if notdone else "")
    if summ.get("firstLoss", -1) > -1 and notdone:
        s += "｜LOSS %+.1f、wave 結束 %+.1f" % (summ["firstLoss"], summ["waveDone"])
    s += "｜回報 %d、補送 %d（拒收 %d）" % (summ.get("nacks", 0), summ.get("repairPkts", 0), summ.get("rejected", 0))
    extra = []
    for k, name in (("late", "太晚不報"), ("afterLoss", "放棄之後不報"), ("tooMany", "缺太多不補"), ("spurious", "多餘回報")):
        if int(tot.get(k, 0)):
            extra.append("%s %d" % (name, int(tot[k])))
    if summ.get("trips", 0):
        extra.append("保險 %d" % summ["trips"])
    if extra:
        s += "｜" + "、".join(extra)
    if late:
        s += "｜晚到 " + " ".join(late[:8])
    return s


def table(title, rows, modes=("legacy", "rtt"), upto=20):
    emit("")
    emit("### " + title)
    for name, args, est_legacy, est_rtt in rows:
        emit("- **%s**" % name)
        for m in modes:
            est = est_legacy if m == "legacy" else est_rtt
            a = args + ((" est=" + est) if est else "")
            res, summ, tot = run(m, a)
            emit("  - %s：%s" % ("現行" if m == "legacy" else "RTT ", desc(res, summ, tot, upto)))


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except AttributeError:
        pass
    if not os.path.exists(SIM):
        sys.exit("missing %s - run build.sh first" % SIM)
    sel = [a for a in sys.argv[1:] if not a.startswith("--")]

    def want(k):
        return not sel or k in sel

    if want("j2a"):
        rows = []
        for rtt in (14, 18, 24, 32):
            for nm, d in (("中段掉", "drop=3:0-4"), ("尾端掉", "drop=3:4-8"), ("整幀沒到", "drop=3:0-8")):
                rows.append(("估計值偏低：視窗 lo/mid/hi=8.1/9.6/10.9、真實往返 %d ms、%s" % (rtt, nm), "fps=120 rtt=%d frames=22 %s" % (rtt, d),
                             "8.1:10.9:10.9", "8.1:9.6:10.9"))
        table("估計值偏低（事件當下往返突然變大）", rows)
    if want("j2b"):
        rows = [("真實往返 10、視窗 hi 15（lo 9／mid 10）、中段掉＋第一輪補包掉", "fps=120 rtt=10 frames=24 drop=3:0-4 repairloss=1", "9:10:15", "9:10:15"),
                ("同上，補包沒掉", "fps=120 rtt=10 frames=24 drop=3:0-4", "9:10:15", "9:10:15"),
                ("真實往返 10、視窗 lo 9／mid 12／hi 26、整幀沒到", "fps=120 rtt=10 frames=24 drop=3:0-8", "9:12:26", "9:12:26"),
                ("真實往返 10、視窗 lo 9／mid 24／hi 26（路徑真的變慢過）、整幀沒到", "fps=120 rtt=10 frames=24 drop=3:0-8", "9:24:26", "9:24:26"),
                ("真實往返 10、估計準確、整幀沒到＋第一輪補包掉", "fps=120 rtt=10 frames=24 drop=3:0-8 repairloss=1", "", ""),
                ("真實往返 10、估計準確、尾端掉＋第一輪補包掉", "fps=120 rtt=10 frames=24 drop=3:4-8 repairloss=1", "", "")]
        table("補包那一輪掉了／重報趕不趕得上", rows)
    if want("j2c"):
        rows = []
        for rtt in (6.8, 9, 11.8, 16.5):
            rows.append(("估計取自沒在送的快連線（lo/mid/hi=3.0/3.3/3.5）、補包實際 %.1f ms 才到、中段掉" % rtt, "fps=120 rtt=%g frames=22 drop=3:0-4" % rtt, "3:3.5:3.5", "3:3.3:3.5"))
            rows.append(("同一個情境、估計取自正在送的連線（＝真實值）", "fps=120 rtt=%g frames=22 drop=3:0-4" % rtt, "", ""))
        table("往返估計取錯連線", rows)
    if want("j2d"):
        rows = []
        for rtt in (10, 13, 15, 18):
            rows.append(("空白 +0.3～+19 ms（第 3 幀尾端＋第 4、5 幀整幀）、往返 %d" % rtt, "fps=120 rtt=%d frames=24 gap=0.3:19" % rtt, "", ""))
        table("目前的 block 先報過、後面才發現整幀沒到", rows)
    if want("j2e"):
        rows = []
        for D in (6, 75):
            for ln in (20, 45, 60, 80, 110):
                rows.append(("空白 −1～%d ms、每幀 %d 個 data、往返 10" % (ln - 1, D), "fps=120 rtt=10 frames=30 D=%d gap=-1:%d" % (D, ln - 1), "", ""))
        table("長空白（缺的幀多到補不完）", rows)
    if want("j2f"):
        st = " ".join("pause=%d:20:%g" % (f, 4 + (f % 2)) for f in range(3, 9))
        for idle in ("fixed", "frame", "learned", "size"):
            rows = [("每幀 40 個 data、第 3～8 幀各在第 20 個封包之後停 4～5 ms、第 10 幀尾端掉；idle=%s" % idle,
                     "fps=120 rtt=10 frames=22 D=40 %s drop=10:30-55 idle=%s" % (st, idle), "", "")]
            table("幀內停頓（沒有掉包）之後才有一張真的尾端掉（120 Hz）｜idle=%s" % idle, rows, modes=("legacy", "rtt") if idle == "fixed" else ("rtt",))
        for idle in ("fixed", "frame", "learned"):
            st60 = " ".join("pause=%d:30:%g" % (f, 4 + (f % 2)) for f in range(3, 9))
            rows = [("60 Hz、每幀 60 個 data、第 3～8 幀各停 4～5 ms、第 10 幀尾端掉；idle=%s" % idle,
                     "fps=60 rtt=10 frames=18 D=60 fec=14 %s drop=10:50-68 idle=%s" % (st60, idle), "", "")]
            table("幀內停頓（60 Hz）｜idle=%s" % idle, rows, modes=("legacy", "rtt") if idle == "fixed" else ("rtt",))
    if want("j2g"):
        rows = [("等到一半切回現行規則（A/B 換邊，+6 ms）、中段掉、往返 12", "fps=120 rtt=12 frames=22 drop=3:0-4 sched=6:0:0", "", ""),
                ("等到一半保險暫停（+6 ms）", "fps=120 rtt=12 frames=22 drop=3:0-4 sched=6:4:1", "", ""),
                ("等到一半第二條連線開始送（+6 ms）", "fps=120 rtt=12 frames=22 drop=3:0-4 sched=6:1:1", "", ""),
                ("三整幀補到一半（+30 ms）切到「兩條在送」", "fps=120 rtt=10 frames=24 gap=-1:19 sched=30:1:1", "", ""),
                ("三整幀補到一半（+30 ms）切回現行規則", "fps=120 rtt=10 frames=24 gap=-1:19 sched=30:0:0", "", ""),
                ("等到一半估計值暴增到 60 ms（+6 ms；門檻關）", "fps=120 rtt=12 frames=22 drop=3:0-4 sched=6:3:60 sched=6:2:60", "", "")]
        table("規則切換的那一刻（已經照 RTT 規則回報、還在等的 block 沿用到結束）", rows, modes=("rtt",))
    if want("j1a"):
        rows = []
        for ln in (20, 40, 60, 100):
            rows.append(("下行整段空白 %d ms（120 Hz、往返 10、每幀 6＋3）" % ln, "fps=120 rtt=10 frames=30 gap=-1:%d" % (ln - 1), "", ""))
        table("最壞情況（整段空白）", rows)
        rows = [("60 Hz、往返 10、空白 20 ms", "fps=60 rtt=10 frames=20 gap=-1:19", "", ""),
                ("60 Hz、往返 10、空白 40 ms", "fps=60 rtt=10 frames=20 gap=-1:39", "", ""),
                ("60 Hz、往返 25、整幀沒到", "fps=60 rtt=25 frames=20 drop=3:0-8", "", ""),
                ("60 Hz、往返 25、尾端掉＋第一輪補包掉", "fps=60 rtt=25 frames=20 drop=3:4-8 repairloss=1", "", "")]
        table("60 Hz 的最壞情況", rows)
    if want("brk"):
        # 每 20 幀一個事件、補包永遠到不了：前 3 個事件各白等一個上限，之後保險暫停（暫停期間等同門檻關著）
        ev = ",".join("%d:0-4" % f for f in range(3, 200, 20))
        rows = [("補包永遠到不了（每 20 幀中段掉 5 個、10 個事件）、120 Hz、往返 10", "fps=120 rtt=10 frames=200 drop=%s repairloss=99" % ev, "", ""),
                # 第三個事件是幀內停頓（沒有掉包）：報了、它自己的封包接著就到齊——那不算「補包用上了」，連續失敗的計數不歸零，
                # 所以第三次救不回（第 63 幀）就暫停
                ("兩次救不回、一次幀內停頓（報了、自己的封包接著到齊）、再一次救不回",
                 "fps=120 rtt=10 frames=120 drop=3:0-4,23:0-4,63:0-4 pause=43:3:5 repairloss=99", "", "")]
        table("保險：等了卻救不回就暫停", rows, upto=200)
    if want("hold"):
        # 重度掉包：連續 6 幀每幀只到前面一小段。補包也到不了時，第一幀等到上限才放棄，其餘在「放棄之後的暫停」裡立刻放棄
        # （不一幀接一幀地等）；補包到得了時依序補回來，補不完的那幾張同樣不再等
        ev = ",".join("%d:20-100" % f for f in range(3, 9))
        rows = [("50 ms 的重度掉包（每幀 72 個 data、每幀只到前 20 個）、補包也全掉；120 Hz、往返 10",
                 "fps=120 rtt=10 frames=30 D=72 spacing=49 drop=%s repairloss=99" % ev, "", ""),
                ("同上，補包到得了", "fps=120 rtt=10 frames=30 D=72 spacing=49 drop=%s" % ev, "", "")]
        table("放棄之後的暫停", rows)
    if want("chain"):
        # 成串掉包：連續十幀各掉一段（每幀 33 個 data＋14 個 parity 裡掉 20 個）。前三輪補包也掉時，第一幀等到上限才放棄；
        # 接著的暫停只算一次（最短往返＋一個幀週期＋2 ms，從這一次放棄起算）——暫停裡的幾幀立刻放棄，之後的照常回報、補得
        # 回來。暫停若是每放棄一幀就重新起算，第一次放棄之後整段都不再回報（十幀全掉，intra refresh 那一波一再重來）
        ev = ",".join("%d:5-24" % f for f in range(3, 13))
        rows = [("60 Hz（半速）、往返 10、連續十幀各掉 20 個（每幀 33＋14）、前三輪補包掉", "fps=60 rtt=10 D=33 frames=20 drop=%s repairloss=3" % ev, "", ""),
                ("120 Hz、往返 4（區網）、同樣的掉法", "fps=120 rtt=4 D=33 frames=20 drop=%s repairloss=3" % ev, "", ""),
                ("120 Hz、往返 10、同樣的掉法", "fps=120 rtt=10 D=33 frames=20 drop=%s repairloss=3" % ev, "", ""),
                ("60 Hz、往返 10、同樣的掉法、補包沒掉", "fps=60 rtt=10 D=33 frames=20 drop=%s" % ev, "", ""),
                ("120 Hz、往返 10、同樣的掉法、補包沒掉", "fps=120 rtt=10 D=33 frames=20 drop=%s" % ev, "", "")]
        table("成串掉包（連續好幾幀各缺一段）：放棄之後的暫停只算一次", rows)
    if want("gone"):
        # 每幀 180 個 data、三整幀沒到：server 的補包額度（最多累積 600 個）只夠補前兩幀，第三幀回 REPAIR_GONE。
        # 那一幀是預先報的——輪到它的時候直接不等、立刻放棄（不再多等一個上限）
        rows = [("空白 −1～27 ms（第 3～5 幀整幀沒到）、每幀 180 個 data、往返 10：預先報的第 5 幀 server 回補不了",
                 "fps=120 rtt=10 frames=20 D=180 spacing=20 gap=-1:27", "", "")]
        table("預先報的幀收到 REPAIR_GONE", rows)

    text = "\n".join(OUT) + "\n"
    sys.stdout.write(text)
    if "--update" in sys.argv:
        if sel:
            sys.exit("--update needs the full run (no group selection)")
        with open(EXPECTED, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("wrote " + EXPECTED)
        return 0
    if sel:
        return 0
    with open(EXPECTED, encoding="utf-8") as f:
        want_text = f.read()
    ok = want_text == text
    print("scen: %s" % ("PASS" if ok else "FAIL (differs from scen_expected.txt; review, then rerun with --update)"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
