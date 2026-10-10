# -*- coding: utf-8 -*-
# §VR-REPAIR-RTT：「開關關著時逐步相同」用的情境清單（golden.py 用；順序固定，golden 檔是照這個順序排的）。
#
# 2,844 個情境，全部以現行規則（mode=legacy）跑：
#   single            1,080  單一連線：3 種更新率 × 8 種往返 × 15 種掉法 × 3 種估計
#   flagdual            660  單一實體連線、但「兩條在送」的旗標是開的（遲滯期間會有這種狀態）
#   dual                720  真正兩條連線：每個封包兩條各一份，第二條晚 0～14 ms，各掉各的
#   multiblock          288  每幀 2～4 個 FEC block
#   multiblock+dual      96  多 block＋兩條連線
# 其中約四成有放棄的幀、六成多有回報。
import os
import subprocess

BASE_SC = ["drop=3:1-1", "drop=3:0-4", "drop=3:4-8", "drop=3:0-8", "drop=3:2-6,4:0-3", "drop=3:0-8,4:0-8", "gap=-1:4", "gap=-1:19",
           "drop=3:0-4 repairloss=1", "drop=3:3-8 pause=4:2:5", "drop=3:0-4 upgap=0:9", "D=60 drop=3:10-45", "D=60 drop=3:40-83",
           "D=60 pause=3:30:4", "D=60 pause=3:30:9 drop=4:5-40"]
EST = ["", "est=8:8:8", "est=9:9:9"]


def scenarios():
    out = []
    # (1) 單一連線
    for fps in (120, 90, 60):
        for rtt in (3, 5, 8, 10, 12, 16, 20, 30):
            for sc in BASE_SC:
                for est in EST:
                    out.append(("single", "fps=%d rtt=%g frames=14 %s %s" % (fps, rtt, sc, est)))
    # (2) 強制「兩條在送」的旗標（單一實體連線）
    for fps in (120, 60):
        for rtt in (3, 5, 8, 10, 12, 16, 20, 30, 45, 2, 6):
            for sc in BASE_SC:
                for est in ("", "est=8:8:8"):
                    out.append(("flagdual", "fps=%d rtt=%g frames=14 %s %s sched=-100:1:1" % (fps, rtt, sc, est)))
    # (3) 真正兩條連線：每個封包兩條各一份，第二條晚 lag2；各自掉各自的
    dual_sc = ["drop=3:0-4", "drop=3:0-4 drop2=3:0-4", "drop=3:0-8 drop2=3:5-8", "drop=3:0-8 drop2=3:0-8", "gap=-1:12", "gap=-1:12 gap2=3:16",
               "gap2=-1:30", "drop=3:0-8,4:0-8 drop2=4:0-8", "D=40 drop=3:5-30 drop2=3:20-50", "D=40 gap=2:9 gap2=12:19",
               "drop=3:2-6 drop2=3:2-6 repairloss=1", "D=40 pause=3:10:5 drop2=3:0-55"]
    for fps in (120, 90, 60):
        for rtt in (3, 8, 12, 20):
            for lag in (0, 2, 5, 8, 14):
                for sc in dual_sc:
                    out.append(("dual", "fps=%d rtt=%g frames=14 links=2 lag2=%g %s" % (fps, rtt, lag, sc)))
    # (4) 多個 FEC block 的幀（每幀 2～4 個 block），單一連線與兩條連線
    mb_sc = ["drop=3:0-4", "drop=3:9-13", "drop=3:5-12", "drop=3:0-17", "gap=-1:4", "gap=0.5:6", "drop=3:12-17,4:0-5", "drop=3:2-6 repairloss=1",
             "pause=3:10:5", "D=20 drop=3:10-45", "D=20 drop=3:30-70 pause=4:20:4", "D=20 gap=-1:19"]
    for fps in (120, 60):
        for rtt in (3, 8, 12, 20):
            for blocks in (2, 3, 4):
                for sc in mb_sc:
                    out.append(("multiblock", "fps=%d rtt=%g frames=14 blocks=%d %s" % (fps, rtt, blocks, sc)))
                    if blocks == 2:
                        out.append(("multiblock+dual", "fps=%d rtt=%g frames=14 blocks=%d links=2 lag2=5 %s drop2=3:3-9" % (fps, rtt, blocks, sc)))
    return out


def run_sim(exe, argv, timeout=60):
    """執行模擬器，回傳 (stdout 文字, 結束碼)；逾時回 (None, None)"""
    try:
        p = subprocess.run([exe] + argv, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, None
    return p.stdout, p.returncode


def timeline(text):
    """比時間軸用：拿掉 TOTALS 那一行（佇列的累計數，改動前的佇列沒有）"""
    return "".join(l for l in text.splitlines(True) if not l.startswith("TOTALS "))


def parse_result(text):
    """RESULT／SUMMARY／TOTALS 三種機器可讀的行 → (每幀的 dict, summary dict, totals dict)"""
    res = {}
    summ = {}
    tot = {}
    for line in text.splitlines():
        if line.startswith("RESULT"):
            kv = dict(p.split("=") for p in line.split()[1:])
            res[int(kv["f"])] = {"nominal": float(kv["nominal"]), "done": float(kv["done"]) if kv["ok"] == "1" else None,
                                 "dropped": int(kv["dropped"]), "own": float(kv["own"]), "lost": int(kv["lost"])}
        elif line.startswith("SUMMARY"):
            summ = {k: float(v) for k, v in (p.split("=") for p in line.split()[1:])}
        elif line.startswith("TOTALS"):
            tot = dict(p.split("=") for p in line.split()[1:])
    return res, summ, tot


def default_build_dir():
    return os.environ.get("VRSIM_BUILD") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "build")
