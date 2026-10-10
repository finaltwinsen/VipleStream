# -*- coding: utf-8 -*-
# §VR-REPAIR-RTT：「開關關著時逐步相同」。
#
# 現行規則（mode=legacy）下，佇列的整份時間軸——每個封包怎麼處理、什麼時候回報、什麼時候放棄、佇列印的每一行 log——
# 要和改動前逐字相同。golden_legacy.txt 是「改動前的佇列＋改動前寫在 VideoStream.c 接收迴圈裡的逐封包設定」
# （sim_ref_inline）對 scenarios.py 那 2,844 個情境的輸出，每個情境一個 SHA-256。
#
#   python golden.py check             目前的佇列對 golden：sim_final（走 vrrrDecide）與 sim_final_inline（走原本那幾行）
#   python golden.py three             不看 golden 檔，三個執行檔直接逐字比（要先 build.sh --ref <提交>）：
#                                        A＝sim_ref_inline、B＝sim_ref（改動前的佇列＋vrrrDecide）、C＝sim_final
#   python golden.py generate <提交>   用 sim_ref_inline 重新產生 golden_legacy.txt（<提交> 只是寫進檔頭的說明）
#   python golden.py diff <編號>       印出第幾個情境（1 起）sim_ref_inline 與 sim_final 的時間軸差在哪裡
#
# 比的時候不看 TOTALS 那一行（佇列的累計數，改動前的佇列沒有）。雜湊算的是換行正規化成 LF 之後的 UTF-8 位元組。
# golden 是在 Windows（MSYS2 ucrt64 的 gcc）產生的；別的平台若因為 printf 的進位不同而對不上，改用 three。
import difflib
import hashlib
import os
import sys
from concurrent.futures import ThreadPoolExecutor

import scenarios as S

HERE = os.path.dirname(os.path.abspath(__file__))
GOLDEN = os.path.join(HERE, "golden_legacy.txt")
EXE_SUFFIX = ".exe" if os.name == "nt" else ""


def exe(name):
    return os.path.join(S.default_build_dir(), name + EXE_SUFFIX)


def need(*names):
    missing = [n for n in names if not os.path.exists(exe(n))]
    if missing:
        sys.exit("missing %s in %s - run build.sh%s first" % (", ".join(missing), S.default_build_dir(),
                                                              " --ref <commit>" if any("ref" in n for n in missing) else ""))


def run_all(names, sc):
    jobs = []
    for kind, s in sc:
        argv = ["mode=legacy"] + s.split()
        for n in names:
            jobs.append((exe(n), argv))

    def one(job):
        out, rc = S.run_sim(job[0], job[1])
        if out is None or rc != 0 or "SUMMARY" not in out:
            return None
        return S.timeline(out)

    with ThreadPoolExecutor(max_workers=12) as ex:
        outs = list(ex.map(one, jobs))
    k = len(names)
    return [outs[i * k:(i + 1) * k] for i in range(len(sc))]


def digest(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def load_golden():
    rows = []
    with open(GOLDEN, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            h, kind, args = line.rstrip("\n").split(" ", 2)
            rows.append((h, kind, args))
    return rows


def table(stat, cols):
    print("| 情境 | 個數 | " + " | ".join(cols) + " | 沒跑完 |")
    print("|---|---|" + "---|" * (len(cols) + 1))
    tot = None
    for kind, st in stat.items():
        print("| %s | %s |" % (kind, " | ".join(str(x) for x in st)))
        tot = st[:] if tot is None else [a + b for a, b in zip(tot, st)]
    print("| 合計 | %s |" % " | ".join(str(x) for x in tot))
    return tot


def cmd_check():
    need("sim_final", "sim_final_inline")
    sc = S.scenarios()
    gold = load_golden()
    if len(gold) != len(sc) or any((g[1], g[2]) != s for g, s in zip(gold, sc)):
        sys.exit("golden_legacy.txt does not match scenarios.py (count or order) - regenerate it")
    rows = run_all(["sim_final", "sim_final_inline"], sc)
    stat = {}
    shown = 0
    for i, ((kind, s), (h, _, _), (a, b)) in enumerate(zip(sc, gold, rows)):
        st = stat.setdefault(kind, [0, 0, 0, 0])
        st[0] += 1
        if a is None or b is None:
            st[3] += 1
            continue
        bad_a = digest(a) != h
        bad_b = digest(b) != h
        st[1] += bad_a
        st[2] += bad_b
        if (bad_a or bad_b) and shown < 8:
            shown += 1
            print("DIFF #%d (%s)%s%s: %s" % (i + 1, kind, " sim_final" if bad_a else "", " sim_final_inline" if bad_b else "", s))
    tot = table(stat, ["sim_final ≠ golden", "sim_final_inline ≠ golden"])
    ok = tot[1] == 0 and tot[2] == 0 and tot[3] == 0
    print("golden check: %s（%d 個情境）" % ("PASS" if ok else "FAIL", tot[0]))
    return 0 if ok else 1


def cmd_three():
    need("sim_ref_inline", "sim_ref", "sim_final")
    sc = S.scenarios()
    rows = run_all(["sim_ref_inline", "sim_ref", "sim_final"], sc)
    stat = {}
    shown = 0
    lost = nack = 0
    for i, ((kind, s), (a, b, c)) in enumerate(zip(sc, rows)):
        st = stat.setdefault(kind, [0, 0, 0, 0, 0])
        st[0] += 1
        if a is None or b is None or c is None:
            st[4] += 1
            continue
        st[1] += a != b
        st[2] += b != c
        st[3] += a != c
        if a != c and shown < 8:
            shown += 1
            print("DIFF #%d (%s): %s" % (i + 1, kind, s))
        _, summ, _ = S.parse_result(a)
        lost += summ.get("lost", 0) > 0
        nack += summ.get("nacks", 0) > 0
    tot = table(stat, ["A≠B（vrrrDecide 的現行那一支）", "B≠C（佇列的改動）", "A≠C"])
    print("有放棄幀的情境 %d 個、有回報的情境 %d 個（共 %d）" % (lost, nack, len(sc)))
    ok = tot[1] == 0 and tot[2] == 0 and tot[3] == 0 and tot[4] == 0
    print("three-way: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def cmd_generate(ref):
    need("sim_ref_inline")
    sc = S.scenarios()
    rows = run_all(["sim_ref_inline"], sc)
    if any(r[0] is None for r in rows):
        sys.exit("some scenarios did not finish")
    with open(GOLDEN, "w", encoding="utf-8", newline="\n") as f:
        f.write("# §VR-REPAIR-RTT：現行規則的 golden（golden.py generate 產生，不要手改）。\n")
        f.write("# 來源：%s 的 RtpVideoQueue.c＋改動前寫在 VideoStream.c 接收迴圈裡的逐封包設定（sim_ref_inline），mode=legacy。\n" % ref)
        f.write("# 每行：<時間軸的 SHA-256> <情境種類> <模擬器參數>；順序與個數＝scenarios.py（%d 個）。\n" % len(sc))
        for (kind, s), (a,) in zip(sc, rows):
            f.write("%s %s %s\n" % (digest(a), kind, s))
    print("wrote %s (%d scenarios)" % (GOLDEN, len(sc)))
    return 0


def cmd_diff(index):
    need("sim_ref_inline", "sim_final")
    sc = S.scenarios()
    kind, s = sc[index - 1]
    (a, c), = run_all(["sim_ref_inline", "sim_final"], [(kind, s)])
    print("#%d (%s): %s" % (index, kind, s))
    if a is None or c is None:
        print("did not finish")
        return 1
    d = list(difflib.unified_diff(a.splitlines(), c.splitlines(), "sim_ref_inline", "sim_final", lineterm="", n=2))
    print("\n".join(d) if d else "identical")
    return 1 if d else 0


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except AttributeError:
        pass
    cmd = sys.argv[1] if len(sys.argv) > 1 else "check"
    if cmd == "check":
        return cmd_check()
    if cmd == "three":
        return cmd_three()
    if cmd == "generate":
        return cmd_generate(sys.argv[2] if len(sys.argv) > 2 else "(unknown commit)")
    if cmd == "diff" and len(sys.argv) > 2:
        return cmd_diff(int(sys.argv[2]))
    sys.exit("usage: golden.py check | three | generate <commit> | diff <n>")


if __name__ == "__main__":
    sys.exit(main())
