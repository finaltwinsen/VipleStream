# -*- coding: utf-8 -*-
# §VR-REPAIR-RTT：亂數情境的不變式（模型，不是實測）。用 sim_final 各跑一次 RTT 規則與現行規則。
#
#   python fuzz.py <種類> [個數] [亂數種子]
#     design   估計值接近真實往返（設計時的產生器）                                預設 600 個
#     judge    對抗式：估計值與真實往返脫鉤、空白到 100 ms                          預設 500 個
#     toggle   同上，再加執行途中切換規則／「兩條在送」／保險暫停／估計值              預設 400 個
#     multi    多 block 的幀＋兩條連線（RTT 規則只在單一連線時生效；看會不會當掉、卡住）  預設 300 個
#     burst    成串掉包：一到三段 30～300 ms、段內逐包掉 3～45%（連續好幾幀各缺一部分），
#              估計值接近真實往返，沒有別的干擾                                      預設 400 個
#     frozen   judge 的情境，假時鐘不自動前進（同一批事件拿到同一個時刻；見 sim.c）       預設 300 個
#   python fuzz.py all            六種各跑預設的個數，並和 fuzz_expected.txt 逐行比（--update 重寫它）
#
# 不變式（違反任何一條就回傳非 0）：
#   1. 不當掉、不卡住，跑得完。
#   2. 沒被動到的幀（第一條連線上一個封包都沒掉）一定收齊。
#   3. 單一連線時，沒被動到的幀被補包機制壓住不超過「等待上限＋2 ms」。toggle（執行途中切換規則與估計值）用
#      可能出現的最大上限：33 ms 與指定值取大的。
# 另外印出和現行規則的對照（沒收齊的幀數、回報數、補送封包數、第一次 LOSS 晚多少）——這些是模型的數字，固定種子下
# 是確定的，all 會拿來和 fuzz_expected.txt 比：規則一有變動就看得到差在哪裡。
import os
import random
import sys
from concurrent.futures import ThreadPoolExecutor

import scenarios as S

HERE = os.path.dirname(os.path.abspath(__file__))
EXPECTED = os.path.join(HERE, "fuzz_expected.txt")
DEFAULT_N = {"design": 600, "judge": 500, "toggle": 400, "multi": 300, "burst": 400, "frozen": 300}
KINDS = ("design", "judge", "toggle", "multi", "burst", "frozen")
SIM = os.path.join(S.default_build_dir(), "sim_final" + (".exe" if os.name == "nt" else ""))


def cap_ms(fps, lo, hi, capms):
    """和 vrrrCapUs 同樣的算法（毫秒）"""
    if capms:
        return max(8, min(100, capms))
    hold = max(4.0, min(14.0, 1000.0 / fps))
    return min(33.0, max(2 * hold, (hi * 1.25 + 2.0) + (lo + 2.0) + 2.0))


def run(args):
    out, rc = S.run_sim(SIM, args, timeout=40)
    if out is None or rc != 0:
        return None
    res, summ, tot = S.parse_result(out)
    if not summ:
        return None
    return res, summ, tot


def gen(kind, rng):
    frames = 30 if kind != "design" else 26
    if kind == "design":
        fps = rng.choice([120, 120, 90, 60, 60, 72])
        rtt = rng.choice([2, 4, 6, 8, 9.6, 10, 12, 14, 17, 22, 28, 35, 45])
        lo = rtt * rng.uniform(0.8, 1.0)
        hi = rtt * rng.uniform(1.0, 1.6)
        mid = (lo + hi) / 2
        D = rng.choice([4, 6, 6, 8, 10, 14, 20, 40, 75, 110, 150])
        capms = rng.choice([0, 0, 0, 12, 20, 60])
    else:
        fps = rng.choice([120, 120, 120, 90, 60, 60])
        rtt = rng.choice([4, 7, 8, 9, 9.6, 10, 11, 12, 13, 15, 18, 22, 28, 35])
        if rng.random() < 0.5:
            lo = rtt * rng.uniform(0.85, 1.0)
            hi = rtt * rng.uniform(1.0, 1.4)
        else:
            lo = rtt * rng.uniform(0.4, 1.0)
            hi = rtt * rng.uniform(0.4, 2.5)
            if hi < lo:
                lo, hi = hi, lo
        mid = rng.uniform(lo, hi)
        D = rng.choice([5, 6, 6, 7, 8, 10, 14, 30, 60, 85, 110])
        capms = rng.choice([0, 0, 0, 0, 14, 25])
    blocks = 1
    links = 1
    extra_link = []
    if kind == "multi":
        blocks = rng.choice([1, 2, 2, 3, 4])
        links = rng.choice([1, 2, 2])
        D = rng.choice([4, 6, 8, 12, 20, 30])
        if links == 2:
            extra_link = ["links=2", "lag2=%g" % rng.choice([0, 2, 5, 9])]
    while blocks * (D + (D * 40 + 99) // 100) * 0.07 > 1000.0 / fps - 1.5:
        D = D * 2 // 3
    P = max(2, (D * 40 + 99) // 100)
    total = blocks * (D + P)
    common = ["fps=%d" % fps, "rtt=%g" % rtt, "D=%d" % D, "frames=%d" % frames, "quiet=1", "origin=3"]
    if blocks > 1:
        common.append("blocks=%d" % blocks)
    common += extra_link
    ex = []
    drops = []
    for _ in range(rng.randint(0, 4 if kind == "design" else 3)):
        f = rng.randint(3, 16)
        a = rng.randint(0, total - 1)
        b = rng.randint(a, min(total - 1, a + rng.randint(0, total)))
        drops.append("%d:%d-%d" % (f, a, b))
    if drops:
        ex.append("drop=" + ",".join(drops))
    if links == 2 and rng.random() < 0.6:
        f = rng.randint(3, 16)
        a = rng.randint(0, total - 1)
        b = rng.randint(a, min(total - 1, a + rng.randint(0, total)))
        ex.append("drop2=%d:%d-%d" % (f, a, b))
    lens = [1, 3, 5, 9, 12, 20, 30, 45] if kind == "design" else [1, 3, 5, 9, 12, 20, 30, 45, 70, 100]
    for _ in range(rng.randint(0, 2) if kind == "design" else rng.choice([0, 0, 1, 1, 2])):
        a = rng.uniform(-2, 110)
        ln = rng.choice(lens)
        ex.append("gap=%.2f:%.2f" % (a, a + ln))
    if rng.random() < 0.25:
        a = rng.uniform(-2, 110)
        ex.append("upgap=%.2f:%.2f" % (a, a + rng.choice([3, 8, 15, 30])))
    for _ in range(rng.randint(0, 3) if kind == "design" else rng.choice([0, 0, 1, 2])):
        ex.append("pause=%d:%d:%g" % (rng.randint(3, 16), rng.randint(1, total - 1), rng.choice([1.5, 3.5, 4, 6, 9, 14])))
    if rng.random() < (0.2 if kind == "design" else 0.3):
        ex.append("repairloss=%d" % rng.randint(1, 3))
    if rng.random() < 0.3:
        ex.append("idle=%s" % rng.choice(["fixed", "frame", "learned", "size"]))
    if capms:
        ex.append("capms=%d" % capms)
    tg = []
    if kind == "toggle":
        for _ in range(rng.randint(1, 4)):
            k = rng.choice([0, 1, 4, 2, 3])
            v = rng.choice([0, 1]) if k in (0, 1, 4) else rng.choice([5, 9, 14, 22, 40])
            tg.append("sched=%.1f:%d:%g" % (rng.uniform(-5, 150), k, v))
    est = "est=%.2f:%.2f:%.2f" % (lo, mid, hi)
    return dict(fps=fps, lo=lo, mid=mid, hi=hi, capms=capms, frames=frames, common=common, ex=ex, tg=tg, est=est, links=links, blocks=blocks)


def gen_burst(rng):
    """成串掉包：連續好幾幀各缺一部分（指定「哪幾幀掉一段」與空白的產生器做不出這種情境）。現行規則本來有效的條件
    （區網、60～90 Hz 而往返在它的門檻內）佔多數——新規則在這些條件下不可以比它差太多"""
    frames = 48
    fps = rng.choice([120, 120, 90, 72, 60, 60])
    rtt = rng.choice([2, 3, 4, 5, 6, 8, 10, 12, 14, 17])
    lo = rtt * rng.uniform(0.9, 1.0)
    hi = rtt * rng.uniform(1.0, 1.3)
    mid = (lo + hi) / 2
    D = rng.choice([6, 10, 20, 33, 33, 60, 85])
    fec = rng.choice([40, 40, 20, 10])
    while (D + max(2, (D * fec + 99) // 100)) * 0.07 > 1000.0 / fps - 1.5:
        D = D * 2 // 3
    common = ["fps=%d" % fps, "rtt=%g" % rtt, "D=%d" % D, "fec=%d" % fec, "frames=%d" % frames, "quiet=1", "origin=3",
              "seed=%d" % rng.randint(1, 999999)]
    ex = []
    span = (frames - 10) * 1000.0 / fps     # 掉包的時段落在前面，最後幾幀留給恢復
    for _ in range(rng.randint(1, 3)):
        a = rng.uniform(-2, span * 0.8)
        ln = rng.choice([30, 60, 100, 200, 300])
        ex.append("burst=%.2f:%.2f:%d" % (a, min(a + ln, span), rng.choice([3, 5, 10, 20, 30, 45])))
    if rng.random() < 0.25:
        a = rng.uniform(-2, span)
        ex.append("upgap=%.2f:%.2f" % (a, a + rng.choice([3, 8, 15, 30])))
    est = "est=%.2f:%.2f:%.2f" % (lo, mid, hi)
    return dict(fps=fps, rtt=rtt, lo=lo, mid=mid, hi=hi, capms=0, frames=frames, common=common, ex=ex, tg=[], est=est, links=1, blocks=1)


def burst_group(c):
    """burst 的對照另外依條件分組印：現行規則有效的條件與它幾乎不作用的條件要分開看"""
    if c["rtt"] <= 5:
        return "區網（往返 ≤5 ms）"
    return "%d Hz、往返 %s ms" % (c["fps"], "6～10" if c["rtt"] <= 10 else "12～17")


def fuzz(kind, n, seed, out):
    """回傳違反不變式的個數；說明文字寫進 out（list of str）"""
    rng = random.Random(seed)
    if kind == "burst":
        cases = [gen_burst(rng) for _ in range(n)]
    elif kind == "frozen":
        cases = [gen("judge", rng) for _ in range(n)]
        for c in cases:
            c["common"].append("clock=0")
    else:
        cases = [gen(kind, rng) for _ in range(n)]
    jobs = []
    for c in cases:
        jobs.append(["mode=rtt", c["est"]] + c["common"] + c["ex"] + c["tg"])
        jobs.append(["mode=legacy", c["est"]] + c["common"] + [x for x in c["ex"] if not x.startswith("capms=") and not x.startswith("idle=")])
    with ThreadPoolExecutor(max_workers=12) as ex:
        outs = list(ex.map(run, jobs))
    crash = untouched = held = 0
    worst = worst_auto = 0.0
    tot = dict(old_lost=0, new_lost=0, old_pk=0, new_pk=0, old_n=0, new_n=0, trips=0, toomany=0, late=0, afterloss=0)
    better = same = excused = 0
    worse = []
    later = []
    groups = {}   # burst：依條件分組的對照（組名 → [情境數, 現行沒收齊, RTT 沒收齊, RTT 較多的情境數, RTT 較少的情境數]）
    for i, c in enumerate(cases):
        new, old = outs[2 * i], outs[2 * i + 1]
        desc = " ".join([c["est"]] + c["common"] + c["ex"] + c["tg"])
        frames = c["frames"]
        if new is None or old is None or len(new[0]) != frames:
            crash += 1
            out.append("CRASH/TIMEOUT: " + desc)
            continue
        res, summ, t = new
        cap = cap_ms(c["fps"], c["lo"], c["hi"], c["capms"])
        if kind == "toggle":
            cap = max(33.0, float(c["capms"]))   # 途中會切換規則與估計值：用可能出現的最大上限
        for f in range(1, frames - 3):
            r = res[f]
            if r["dropped"]:
                continue
            if r["done"] is None:
                if c["links"] == 1:   # 兩條連線時「第一條沒掉」不代表沒被動到（另一條的延遲、去重）
                    untouched += 1
                    out.append("UNTOUCHED LOST f=%d: %s" % (f, desc))
                break
            late = r["done"] - r["own"]
            if kind != "multi" and c["links"] == 1:
                worst = max(worst, late)
                if not c["capms"]:
                    worst_auto = max(worst_auto, late)
                if late > cap + 2.0:
                    held += 1
                    out.append("HELD TOO LONG f=%d late=%.2f cap=%.1f: %s" % (f, late, cap, desc))
                    break
        ond = sum(1 for f in range(1, frames - 3) if old[0][f]["done"] is None)
        nnd = sum(1 for f in range(1, frames - 3) if res[f]["done"] is None)
        if kind == "burst":
            g = groups.setdefault(burst_group(c), [0, 0, 0, 0, 0])
            g[0] += 1
            g[1] += ond
            g[2] += nnd
            g[3] += nnd > ond
            g[4] += nnd < ond
        tot["old_lost"] += ond
        tot["new_lost"] += nnd
        tot["old_pk"] += int(old[1]["repairPkts"])
        tot["new_pk"] += int(summ["repairPkts"])
        tot["old_n"] += int(old[1]["nacks"])
        tot["new_n"] += int(summ["nacks"])
        tot["trips"] += int(summ.get("trips", 0))
        tot["toomany"] += int(t.get("tooMany", 0))
        tot["late"] += int(t.get("late", 0))
        tot["afterloss"] += int(t.get("afterLoss", 0))
        if kind != "toggle":
            hold2 = 2 * max(4.0, min(14.0, 1000.0 / c["fps"]))
            if nnd > ond and c["capms"] and c["capms"] < hold2:
                excused += 1     # 指定的等待上限比現行規則「有進展時」的上限還短：比現行差是指定值造成的
            elif nnd > ond:
                worse.append((ond, nnd, desc))
            elif nnd < ond:
                better += 1
            else:
                same += 1
                ofl = old[1]["firstLoss"]
                nfl = summ["firstLoss"]
                if ofl > 0 and nfl > ofl + 10.0:
                    later.append((nfl - ofl, ond, int(summ["repairPkts"]), desc))
    out.append("kind=%s runs=%d seed=%d crash=%d untouched_lost=%d held_too_long=%d worst_hold=%.2f ms (auto cap: %.2f ms)" % (
        kind, n, seed, crash, untouched, held, worst, worst_auto))
    out.append("totals: " + " ".join("%s=%d" % (k, v) for k, v in tot.items()))
    if kind != "toggle":
        later.sort(reverse=True)
        out.append("沒收齊的幀數：RTT 規則較少 %d、一樣 %d、較多 %d（另有 %d 個是指定的等待上限短於現行規則的上限）；"
                   "一樣但第一次 LOSS 晚超過 10 ms 的 %d 個%s" % (better, same, len(worse), excused, len(later),
                                                   "，最多晚 %.1f ms" % later[0][0] if later else ""))
        for name in sorted(groups):
            g = groups[name]
            out.append("  %s：%d 個情境，沒收齊 現行 %d／RTT %d（RTT 較多的 %d 個、較少的 %d 個）" % (name, g[0], g[1], g[2], g[3], g[4]))
        for o, w, a in worse[:10]:
            out.append("  較多（現行 %d → RTT %d）：%s" % (o, w, a))
    return crash + untouched + held


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except AttributeError:
        pass
    if not os.path.exists(SIM):
        sys.exit("missing %s - run build.sh first" % SIM)
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    kind = args[0] if args else "all"
    lines = []
    bad = 0
    if kind == "all":
        for k in KINDS:
            bad += fuzz(k, DEFAULT_N[k], 7, lines)
    else:
        if kind not in DEFAULT_N:
            sys.exit("usage: fuzz.py %s [n] [seed] | all [--update]" % "|".join(KINDS))
        bad += fuzz(kind, int(args[1]) if len(args) > 1 else DEFAULT_N[kind], int(args[2]) if len(args) > 2 else 7, lines)
    text = "\n".join(lines) + "\n"
    sys.stdout.write(text)
    rc = 1 if bad else 0
    if kind == "all":
        if "--update" in sys.argv:
            with open(EXPECTED, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
            print("wrote " + EXPECTED)
        else:
            with open(EXPECTED, encoding="utf-8") as f:
                want = f.read()
            if want != text:
                print("fuzz: output differs from fuzz_expected.txt (the rules changed? review the numbers, then rerun with --update)")
                rc = 1
    print("fuzz: %s（違反不變式 %d 個）" % ("PASS" if rc == 0 else "FAIL", bad))
    return rc


if __name__ == "__main__":
    sys.exit(main())
