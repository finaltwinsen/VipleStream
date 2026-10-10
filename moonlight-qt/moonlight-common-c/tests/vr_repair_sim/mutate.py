# -*- coding: utf-8 -*-
# §VR-REPAIR-RTT：變異測試——把每一條規則各弄壞一次，確認這個目錄的測試（與 server 的 googletest）抓得到。
#
#   python mutate.py                  全部跑一遍，結果和 mutation_expected.txt 比
#   python mutate.py --update         把結果寫進 mutation_expected.txt
#   python mutate.py --only a,b       只跑這幾個（不比對、不寫檔）
#   python mutate.py --list           列出全部變異
#   python mutate.py --jobs N         同時跑幾個（預設 4）
#   python mutate.py --keep           留著每個變異的複本與產物（預設跑完就刪）
#
# 做法：每個變異把要改的原始碼複製到 $VRSIM_BUILD/mut/<名稱>/（common-c 的 src、vr_multilink_logic.h、S3 的兩個
# 標頭），在複本上做一處替換，用 build.sh／srv_gtest.sh 的 VRSIM_Q、VRSIM_S 指到複本去編，再依序跑測試，記下第一個
# 失敗的是哪一組。repo 裡的檔案不會被改。「沒有改動」那一列（baseline）走同一條路把每一組測試都跑過，確認這條路本身
# 是通的——不然每個變異都會「被抓到」。
#
# 每個變異只換一行裡的一小段（不跨行：工作樹的行尾可能是 CRLF）；要換的字串在檔案裡必須剛好出現一次，或用 after
# 指定「從哪一行之後的第一個」。原始碼改到對不上時會直接報錯，那時要回來更新這張表。
#
# 需要 gcc／g++、bash、python 3；全部跑完約 10～15 分鐘。
import argparse
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

import scenarios as S

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
Q = os.path.join(ROOT, "moonlight-qt", "moonlight-common-c", "moonlight-common-c", "src")
SUN = os.path.join(ROOT, "Sunshine")
EXPECTED = os.path.join(HERE, "mutation_expected.txt")
EXE_SUFFIX = ".exe" if os.name == "nt" else ""
# Windows 上直接寫 "bash" 會先找到 System32 的那一個（WSL），不是正在用的 MSYS2／Git 的：照 PATH 的順序找
BASH = shutil.which("bash") or "bash"

# 改哪個檔（複本裡的相對路徑；VrRepairRtt.h 的兩份一起改，和 repo 裡三份必須相同一樣）
TARGETS = {
    "VrRepairRtt.h": [("q", "VrRepairRtt.h"), ("s", "third-party/moonlight-common-c/src/VrRepairRtt.h")],
    "RtpVideoQueue.c": [("q", "RtpVideoQueue.c")],
    "vr_multilink_logic.h": [("s", "src/vr/vr_multilink_logic.h")],
}
# 依序跑、第一個失敗就停。client 的檔先跑單元測試；server 的標頭先跑 googletest
ORDER = {
    "VrRepairRtt.h": ["unit", "scen", "matrix", "golden", "fuzz", "gtest"],
    "RtpVideoQueue.c": ["unit", "scen", "matrix", "golden", "fuzz"],
    "vr_multilink_logic.h": ["gtest", "scen", "matrix", "golden", "fuzz"],
    None: ["unit", "gtest", "scen", "matrix", "golden", "fuzz"],
}


# 「現行規則不變」那幾個變異先跑 golden：要確認的就是 golden 守得住開關關著時的時間軸
GOLDEN_FIRST = ["golden", "unit", "scen", "matrix", "fuzz"]


def M(name, rule, target, old, new, what, after=None, order=None, equiv=None):
    # equiv：這個變異在走得到的狀態下不改變行為（等價的變異），預期沒有測試失敗；字串是理由
    return {"name": name, "rule": rule, "target": target, "old": old, "new": new, "what": what, "after": after, "order": order,
            "equiv": equiv}


MUTANTS = [
    M("baseline", "（對照）", None, None, None, "沒有改動"),

    # ── VrRepairRtt.h：純邏輯 ──
    M("gate-limit", "門檻", "VrRepairRtt.h", "#define VRRR_GATE_US              25000u", "#define VRRR_GATE_US              35000u",
      "門檻由 25 ms 放寬到 35 ms"),
    M("gate-hyst", "門檻", "VrRepairRtt.h", "return wasOpen ? etaTryUs <= limit : (uint64_t)etaTryUs + VRRR_GATE_HYST_US <= limit;",
      "return etaTryUs <= limit;", "門檻沒有遲滯"),
    M("eta", "估計 eta", "VrRepairRtt.h", "return rttHiUs + rttHiUs / 4u + VRRR_ETA_MARGIN_US;", "return rttHiUs + VRRR_ETA_MARGIN_US;",
      "eta 少了四分之一的餘裕"),
    M("eta-try", "估計 etaTry", "VrRepairRtt.h", "return rttMidUs + VRRR_ETA_MARGIN_US;", "return rttMidUs;", "etaTry 少了 2 ms"),
    M("eta-fast", "估計 etaFast", "VrRepairRtt.h", "return rttLoUs + VRRR_ETA_MARGIN_US;", "return rttLoUs;", "etaFast 少了 2 ms"),
    M("rtt-hi", "往返的高估值", "VrRepairRtt.h", "out->hiUs = (uint32_t)s[n >= 3 ? n - 2 : n - 1] * 100u;",
      "out->hiUs = (uint32_t)s[n - 1] * 100u;", "hi 取最大值而不是第二大"),
    M("rtt-carry", "只看在送的連線", "VrRepairRtt.h", "unsigned use = haveMask & (carryMask != 0 ? carryMask : ~0u);",
      "unsigned use = haveMask;", "往返的統計不分連線有沒有在送影像"),
    M("cap-max", "等待上限", "VrRepairRtt.h", "#define VRRR_CAP_MAX_US           33000u", "#define VRRR_CAP_MAX_US           48000u",
      "等待上限的封頂由 33 ms 改成 48 ms"),
    M("cap-floor", "等待上限", "VrRepairRtt.h", "if (cap < 2ull * holdUs) {", "if (cap < 1ull * holdUs) {",
      "等待上限的下限由 2×hold 改成 1×hold"),
    M("retry-feasible", "重報", "VrRepairRtt.h", "return vrrrFeasible(at, etaFastUs, limitUs) ? at : 0;", "return at;",
      "重報不看趕不趕得上期限"),
    M("retry-interval", "重報", "VrRepairRtt.h", "at = lastNackUs != 0 ? lastNackUs + etaUs : nowUs;",
      "at = lastNackUs != 0 ? lastNackUs + etaFastUs : nowUs;", "重報的間隔由 eta 改成 etaFast"),
    M("deadline-unasked", "等到什麼時候", "VrRepairRtt.h", "return vrrrFeasible(nowUs, etaTryUs, cap) ? cap : headArrivalUs;",
      "return cap;", "沒報過、現在報也趕不上的 block 照樣等到上限"),
    M("holdoff", "放棄後暫停", "VrRepairRtt.h", "return rttLoUs + framePeriodUs + VRRR_ETA_MARGIN_US;", "return 0;",
      "放棄之後的暫停長度變成 0"),
    M("ahead-max", "預先回報", "VrRepairRtt.h", "#define VRRR_AHEAD_MAX            3u", "#define VRRR_AHEAD_MAX            5u",
      "一次最多預先回報 5 幀"),
    M("too-many", "預先回報", "VrRepairRtt.h", "if (gapFrames > vrrrAheadMaxFrames(framePeriodUs)) {", "if (false) {",
      "缺的幀數不設上限"),
    M("breaker-streak", "保險", "VrRepairRtt.h", "#define VRRR_BRK_STREAK      3u", "#define VRRR_BRK_STREAK      4u",
      "連續 4 次失敗才暫停"),
    M("breaker-backoff", "保險", "VrRepairRtt.h", "b->pausedUntilUs = nowUs + ((uint64_t)VRRR_BRK_PAUSE_US << b->level);",
      "b->pausedUntilUs = nowUs + (uint64_t)VRRR_BRK_PAUSE_US;", "暫停的時間不加倍"),
    M("switch-latched", "規則切換", "VrRepairRtt.h",
      "if (!rtt && in->latched && s->lastRttMode && in->negotiatedRepair && in->negotiatedRtt) {", "if (false) {",
      "規則切換時不沿用到正在等的 block 結束"),
    M("single-link-only", "規則切換", "VrRepairRtt.h",
      "in->armRtt && in->legacyRttMs >= 0 && !dual && in->haveStats;", "in->armRtt && in->legacyRttMs >= 0 && in->haveStats;",
      "兩條連線在送時也用 RTT 規則"),
    M("legacy-gate", "現行規則不變", "VrRepairRtt.h", "(uint32_t)in->legacyRttMs * 1000u + 2000u <= in->holdUs + in->holdUs / 2u;",
      "(uint32_t)in->legacyRttMs * 1000u + 3000u <= in->holdUs + in->holdUs / 2u;", "現行規則的門檻多 1 ms", order=GOLDEN_FIRST),
    M("idle-auto", "第一份回報", "VrRepairRtt.h", "return framePeriodUs <= 10000u ? VRRR_IDLE_SIZE : VRRR_IDLE_FIXED3;",
      "return VRRR_IDLE_FIXED3;", "idle 的預設做法一律 3 ms"),
    M("idle-size", "第一份回報", "VrRepairRtt.h", "#define VRRR_IDLE_SMALL_SHARDS    32u", "#define VRRR_IDLE_SMALL_SHARDS    64u",
      "「小的 block」由 32 個 shard 改成 64 個"),
    M("nack-flags", "NACK 的旗標", "VrRepairRtt.h", "uint32_t ms = (etaUs + 999u) / 1000u;", "uint32_t ms = etaUs / 1000u;",
      "eta 換成毫秒時無條件捨去"),
    M("cannot-be-repair", "量測", "VrRepairRtt.h", "return firstNackUs != 0 && rttLoUs > guard && rxUs + guard < firstNackUs + rttLoUs;",
      "return false;", "回報之後到的封包一律當成補包"),
    M("stall-min", "第一份回報", "VrRepairRtt.h", "if (stallUs < VRRR_STALL_MIN_US || nowUs == 0) {", "if (nowUs == 0) {",
      "短於 3 ms 的間隔也記成停頓"),

    # ── RtpVideoQueue.c：佇列 ──
    M("q-retry-silent", "重報", "RtpVideoQueue.c", "if (queue->nkLastUs != 0 && queue->nkAnyRxUs <= queue->nkLastUs) {", "if (false) {",
      "鏈路靜默時照樣重報"),
    M("q-refund", "重報", "RtpVideoQueue.c", "if (queue->mlRttMode) {", "if (false) {", "RTT 規則也用「退還」",
      after="static void nkNoteArrival("),
    M("q-deadline-unasked", "等到什麼時候", "RtpVideoQueue.c", "queue->nkCount + queue->nkRefunds > 0);", "true);",
      "沒報過的 block 也當成報過（現在報趕不上的照樣等到上限）"),
    M("q-first-feasible", "第一份回報", "RtpVideoQueue.c", "if (!vrrrFeasible(at, queue->mlRepairEtaTryUs, nkRttLimitUs(queue, at))) {",
      "if (false) {", "nkNextDueUs 裡第一份回報不看值不值得",
      equiv="有後面的封包在排隊時，同一個條件先讓 mlHeadDeadlineUs 不等、直接放棄，走不到回報；沒有時門檻保證 etaTry ≤ cap，恆為值得"),
    M("q-idle-policy", "第一份回報", "RtpVideoQueue.c",
      "? queue->nkLastRxUs + vrrrIdleUs(queue->mlIdlePolicy,", "? queue->nkLastRxUs + RTPV_NACK_IDLE_US + 0 * vrrrIdleUs(queue->mlIdlePolicy,",
      "「多久沒有新封包」一律 3 ms"),
    M("q-ahead-send", "預先回報", "RtpVideoQueue.c", "for (k = 1; k <= gap && k <= VRRR_AHEAD_MAX; k++) {",
      "for (k = 1; k <= gap && k <= 0; k++) {", "後面整幀沒到的幀不預先回報"),
    M("q-too-many", "預先回報", "RtpVideoQueue.c", "queue->nkTooMany = true;", "queue->nkTooMany = false;",
      "缺太多幀時照樣一幀一幀補", after="static void nkAheadPlan("),
    M("q-pre-consume", "預先回報", "RtpVideoQueue.c", "queue->nkCount = 1;", "queue->nkCount = 0;",
      "預先報過的幀輪到時不算報過"),
    M("q-pre-gone", "預先回報", "RtpVideoQueue.c", "queue->nkPreGone[i] = true;", "queue->nkPreGone[i] = false;",
      "預先報的幀收到 REPAIR_GONE 不記", after="void RtpvRepairGone("),
    M("q-holdoff", "放棄後暫停", "RtpVideoQueue.c",
      "queue->mlLossHoldoffUs = nowUs + vrrrLossHoldoffUs(queue->mlRttLoUs, queue->mlFramePeriodUs);", "queue->mlLossHoldoffUs = 0;",
      "放棄之後不暫停"),
    M("q-holdoff-rearm", "放棄後暫停", "RtpVideoQueue.c", "if (!afterLoss) {", "if (true) {",
      "暫停裡每放棄一次就重新起算（成串掉包時第一次放棄之後整段不再回報）"),
    M("q-deadline-cap", "等待上限", "RtpVideoQueue.c", "return vrrrHeadDeadlineUs(nowUs, h->arrivalUs, queue->mlHoldCapUs,",
      "return vrrrHeadDeadlineUs(nowUs, h->arrivalUs, queue->mlHoldCapUs * 2,", "等待的期限變成兩倍的上限"),
    M("q-latched", "規則切換", "RtpVideoQueue.c", "return true;", "return false;", "正在等的 block 不回報「沿用規則」",
      after="bool RtpvRepairLatched("),
    M("q-nack-flags", "NACK 的旗標", "RtpVideoQueue.c", "n.flags = queue->mlRttMode ? vrrrNackFlags(queue->mlRepairEtaUs) : 0;",
      "n.flags = 0;", "回報不帶旗標（server 照原本的年齡上限）"),
    M("q-used", "保險", "RtpVideoQueue.c", "const bool usedRepair = queue->nkUsedRepair || usedNow;", "const bool usedRepair = true;",
      "自己晚到湊齊的 block 也算「補包用上了」"),
    M("q-latch-own", "LATCH 的靜默", "RtpVideoQueue.c", "if (nkCur && usedRepair) {", "if (nkCur) {",
      "報了、補包沒用上就自己收齊的 block 也觸發靜默"),
    M("q-latch-wait", "LATCH 的靜默", "RtpVideoQueue.c", "queue->mlDelayedReleaseUs = nowUs;", "queue->mlDelayedReleaseUs = 0;",
      "跨幀等待結束時不觸發靜默", after="static void mlNoteDelayedRelease("),
    M("q-late-seen", "量測", "RtpVideoQueue.c", "if (hadPending && have != NULL) {", "if (false) {",
      "放棄前已經收到的 shard 再到一份也算進度（lateDone）"),
    M("q-late-gone", "量測", "RtpVideoQueue.c", "if (!queue->nkGone) {", "if (true) {",
      "server 回補不了的 block 也算進「沒到齊」（never）", after="static void msNoteLost("),
    M("q-stall-hole", "量測", "RtpVideoQueue.c", "if (gap >= VRRR_STALL_MIN_US && !afterHole) {", "if (gap >= VRRR_STALL_MIN_US) {",
      "掉包的洞也算成 block 內的停頓（stall）"),
    M("q-ring-settle", "保險", "RtpVideoQueue.c", "else if (queue->mlRttMode && queue->mlRepair) {", "else if (false) {",
      "預先報過、在環裡收齊的幀不結算（repaired／used 少算，保險看不到成功）"),
    M("q-gap-carry", "預先回報", "RtpVideoQueue.c",
      "if (carry != 0 && (e->link < 0 || e->link >= RTPV_ML_LINKS || !(carry & (1u << e->link)))) {", "if (false) {",
      "「後面整幀沒到」不分是哪條連線送來的封包"),
    M("q-legacy-idle", "現行規則不變", "RtpVideoQueue.c", "#define RTPV_NACK_IDLE_US    3000", "#define RTPV_NACK_IDLE_US    4000",
      "現行規則的 3 ms 改成 4 ms", order=GOLDEN_FIRST),
    M("q-legacy-clock", "現行規則不變", "RtpVideoQueue.c", "const bool ahead = mlIsAhead(queue, packet, length);",
      "const bool ahead = (PltGetMicroseconds(), mlIsAhead(queue, packet, length));", "每個封包多讀一次時鐘（時間軸差 1 µs）",
      order=GOLDEN_FIRST),

    # ── vr_multilink_logic.h：server ──
    M("s-max-age", "server 的年齡上限", "vr_multilink_logic.h", "inline constexpr int64_t kRepairRttMaxAgeNs = 100'000'000;",
      "inline constexpr int64_t kRepairRttMaxAgeNs = 50'000'000;", "帶旗標的回報的年齡上限由 100 ms 改成 50 ms"),
    M("s-max-age-flag", "server 的年齡上限", "vr_multilink_logic.h", "return rtt_mode ? kRepairRttMaxAgeNs : video_max_age_ns * 3 / 2;",
      "return kRepairRttMaxAgeNs;", "沒帶旗標的回報也放寬到 100 ms"),
    M("s-gap", "server「還在路上」的視窗", "vr_multilink_logic.h", "(int64_t) eta_ms * 750'000", "(int64_t) eta_ms * 1'000'000",
      "視窗由 eta 的四分之三改成整個 eta"),
    M("s-gap-flag", "server「還在路上」的視窗", "vr_multilink_logic.h", "if (!rtt_mode || eta_ms == 0) {", "if (eta_ms == 0) {",
      "沒帶旗標的回報也用 eta 算視窗"),
    M("s-master-switch", "協商", "vr_multilink_logic.h", "if (repair_rtt && (offered & VIPLE_VR_LINK_F_REPAIR_RTT)) {",
      "if (offered & VIPLE_VR_LINK_F_REPAIR_RTT) {", "server 的主開關關著也同意"),
    M("s-needs-repair", "協商", "vr_multilink_logic.h", "if (repair && (offered & VIPLE_VR_LINK_F_REPAIR)) {", "if (repair) {",
      "client 沒提 REPAIR 也同意（RTT 跟著成立）"),
    M("s-flag-negotiated", "NACK 的旗標", "vr_multilink_logic.h",
      "return (features & VIPLE_VR_LINK_F_REPAIR_RTT) != 0 && (nack_flags & VIPLE_VR_NACK_F_RTT) != 0;",
      "return (nack_flags & VIPLE_VR_NACK_F_RTT) != 0;", "沒協商也看旗標"),
    M("s-parse-flags", "NACK 的旗標", "vr_multilink_logic.h", "out.flags = n.flags;", "out.flags = 0;", "解析 NACK 時不讀 flags"),
]


def fwd(p):
    return p.replace("\\", "/")


def apply_mutation(text, m, path):
    old = m["old"]
    if m["after"]:
        if text.count(m["after"]) != 1:
            raise RuntimeError("%s: anchor %r appears %d times in %s" % (m["name"], m["after"], text.count(m["after"]), path))
        i = text.find(old, text.index(m["after"]))
    else:
        if text.count(old) != 1:
            raise RuntimeError("%s: %r appears %d times in %s (must be exactly once)" % (m["name"], old, text.count(old), path))
        i = text.index(old)
    if i < 0:
        raise RuntimeError("%s: %r not found after the anchor in %s" % (m["name"], old, path))
    return text[:i] + m["new"] + text[i + len(old):]


def prepare(m, out):
    """把要用到的原始碼複製到 <out>/mut/<名稱>/ 並套用變異；回傳 (複本根目錄, common-c src, Sunshine 根)"""
    d = os.path.join(out, "mut", m["name"])
    shutil.rmtree(d, ignore_errors=True)
    q = os.path.join(d, "q")
    s = os.path.join(d, "s")
    os.makedirs(q)
    for f in os.listdir(Q):
        if f.endswith((".c", ".h")):
            shutil.copyfile(os.path.join(Q, f), os.path.join(q, f))
    for rel in ("src/vr/vr_multilink_logic.h", "third-party/moonlight-common-c/src/VipleVr.h", "third-party/moonlight-common-c/src/VrRepairRtt.h"):
        dst = os.path.join(s, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(os.path.join(SUN, rel), dst)
    if m["target"]:
        for base, rel in TARGETS[m["target"]]:
            p = os.path.join(q if base == "q" else s, rel)
            with open(p, encoding="utf-8", newline="") as f:
                text = f.read()
            with open(p, "w", encoding="utf-8", newline="") as f:
                f.write(apply_mutation(text, m, rel))
    return d, q, s


def run(cmd, env, timeout):
    try:
        p = subprocess.run(cmd, cwd=HERE, env=env, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except subprocess.TimeoutExpired:
        return -1, "timeout"


def one(m, out, gtest_obj, keep):
    d, q, s = prepare(m, out)
    build = os.path.join(d, "build")
    env = dict(os.environ)
    env.update({"VRSIM_BUILD": fwd(build), "VRSIM_Q": fwd(q), "VRSIM_S": fwd(s), "VRSIM_GTEST_OBJ": fwd(gtest_obj), "PYTHONIOENCODING": "utf-8"})
    rc, log = run([BASH, "build.sh"], env, 600)
    if rc != 0:
        return m, "BUILD FAILED", log[-1500:]
    tests = {
        "unit": [os.path.join(build, "test_vrrr" + EXE_SUFFIX)],
        "gtest": [BASH, "srv_gtest.sh"],
        "scen": [sys.executable, "scen.py"],
        "matrix": [sys.executable, "matrix.py"],
        "golden": [sys.executable, "golden.py", "check"],
        "fuzz": [sys.executable, "fuzz.py", "all"],
    }
    killed = None
    tail = ""
    for t in (m["order"] or ORDER[m["target"]]):
        rc, log = run(tests[t], env, 900)
        if rc != 0:
            killed = t
            tail = log[-600:]
            break
    if not keep:
        shutil.rmtree(d, ignore_errors=True)
    return m, killed, tail


def problem(m, killed):
    """這一列的結果算不算有問題：編不過、對照組有測試失敗、該被抓到的沒被抓到、標成等價的卻被抓到"""
    if killed == "BUILD FAILED":
        return True
    if m["target"] is None or m["equiv"]:
        return killed is not None
    return killed is None


def line(m, killed):
    if m["target"] is None:
        verdict = "全部通過" if killed is None else "FAILED: " + killed
    elif m["equiv"]:
        verdict = ("沒有測試失敗（等價的變異：%s）" % m["equiv"]) if killed is None else "標成等價卻失敗了: " + killed
    else:
        verdict = killed if killed else "SURVIVED"
    return "%s | %s | %s | %s | %s" % (m["name"], m["rule"], m["target"] or "—", m["what"], verdict)


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except AttributeError:
        pass
    ap = argparse.ArgumentParser()
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--only", default="")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()

    names = [m["name"] for m in MUTANTS]
    assert len(set(names)) == len(names), "duplicate mutant names"
    if a.list:
        for m in MUTANTS:
            print("%-18s %-22s %-22s %s" % (m["name"], m["rule"], m["target"] or "—", m["what"]))
        return 0
    todo = MUTANTS
    if a.only:
        want = a.only.split(",")
        unknown = [w for w in want if w not in names]
        if unknown:
            sys.exit("unknown mutant(s): %s" % ", ".join(unknown))
        todo = [m for m in MUTANTS if m["name"] in want]

    out = S.default_build_dir()
    os.makedirs(out, exist_ok=True)
    # googletest 本身先編好一份，大家共用（srv_gtest.sh 看到已經有就不重編）
    gtest_obj = os.path.join(out, "gtest")
    env = dict(os.environ)
    env.update({"VRSIM_BUILD": fwd(out), "VRSIM_GTEST_OBJ": fwd(gtest_obj)})
    rc, log = run([BASH, "srv_gtest.sh"], env, 900)
    if rc != 0:
        print(log[-2000:])
        sys.exit("srv_gtest.sh does not pass on the unmodified tree - fix that first")

    with ThreadPoolExecutor(max_workers=max(1, a.jobs)) as ex:
        results = list(ex.map(lambda m: one(m, out, gtest_obj, a.keep), todo))

    lines = []
    bad = 0
    for m, killed, tail in results:
        lines.append(line(m, killed))
        print(lines[-1])
        if problem(m, killed):
            bad += 1
            if tail:
                print("    " + tail.strip().replace("\n", "\n    "))
    n = sum(1 for m in todo if m["target"] and not m["equiv"])
    killed_n = sum(1 for m, k, _ in results if m["target"] and not m["equiv"] and k is not None and k != "BUILD FAILED")
    print("mutants: %d (plus %d marked equivalent), killed: %d, problems: %d" % (n, sum(1 for m in todo if m["equiv"]), killed_n, bad))

    if a.only:
        return 1 if bad else 0
    if a.update:
        with open(EXPECTED, "w", encoding="utf-8", newline="\n") as f:
            f.write("# §VR-REPAIR-RTT 變異測試的結果（python mutate.py --update 產生，不要手改）。\n")
            f.write("# 每行：<變異> | <規則> | <改的檔> | <改了什麼> | <第一個失敗的測試>\n")
            f.write("# 測試的順序：VrRepairRtt.h＝unit→scen→matrix→golden→fuzz→gtest；RtpVideoQueue.c＝unit→scen→matrix→golden→fuzz；\n")
            f.write("# vr_multilink_logic.h＝gtest→scen→matrix→golden→fuzz；「現行規則不變」那幾列先跑 golden。\n")
            f.write("# SURVIVED＝沒有任何測試失敗（不該出現）；標成「等價」的那幾列本來就不該有測試失敗，理由寫在同一列。\n")
            f.write("\n".join(lines) + "\n")
        print("wrote %s" % EXPECTED)
        return 1 if bad else 0
    try:
        with open(EXPECTED, encoding="utf-8") as f:
            want = [l.rstrip("\n") for l in f if l.strip() and not l.startswith("#")]
    except OSError:
        sys.exit("mutation_expected.txt is missing - run with --update")
    same = want == lines
    if not same:
        for w, g in zip(want, lines):
            if w != g:
                print("expected: %s\n     got: %s" % (w, g))
        if len(want) != len(lines):
            print("expected %d rows, got %d" % (len(want), len(lines)))
    ok = same and bad == 0
    print("mutate: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
