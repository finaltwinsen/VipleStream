// VipleStream 2.0 §VR-REPAIR-RTT：影像接收佇列的模擬器（模型，不是實測）。
//
// 把真的 RtpVideoQueue.c 接上假的時鐘與封包事件，照 VideoStream.c 接收迴圈的做法餵它：每個封包先做「這個封包的
// 決定」（vrrrDecide）、填進佇列、再交給 RtpvAddPacket；沒有封包時照 RtpvHoldTimeoutMs 的期限呼叫 RtpvPollHold。
// server 端的補包決策在 server_model.cpp（Sunshine/src/vr/vr_multilink_logic.h 的真邏輯）。
//
// 用途：
//   1. 「開關關著時逐步相同」：現行規則下整份時間軸（含佇列印的每一行 log）要和改動前的佇列逐字相同（golden.py）；
//   2. 亂數情境的不變式（fuzz.py）、逐項情境（scen.py）、行為表（matrix.py）。
// 假時鐘每讀一次前進 1 µs：佇列多讀一次時鐘，時間軸就會不同（改動不可以在現行規則的路徑上多讀時鐘）。
//
// 編譯開關（build.sh 會用到）：
//   -DSIM_QUEUE_ORIG   佇列是還沒有 §VR-REPAIR-RTT 欄位的版本（從某個舊提交取出來產生 golden 用）
//   -DLEGACY_INLINE    每個封包的決定用「VideoStream.c 原本寫在接收迴圈裡的那幾行」（原樣搬來），不經過 vrrrDecide()
//
// 參數（key=value，可重複的有 drop／gap／pause／sched）：
//   mode=legacy|rtt          現行規則／§VR-REPAIR-RTT（對應 vrrrDecide 的 armRtt）
//   fps=120 rtt=12           串流的更新率、真實的往返時間（ms；上下行各一半）
//   est=lo:mid:hi            client 手上的往返估計（ms）；預設＝真實值
//   D=6 blocks=1 fec=40      每個 FEC block 的 data shard 數、每幀幾個 block、FEC 百分比
//   frames=12 spacing=70     模擬幾幀、同一幀相鄰封包的間隔（µs）
//   turn=300                 server 收到回報到補包開始送出（µs）
//   repairloss=N             前 N 輪補包在路上全掉
//   capms=N idle=… ahead=0|1 等待上限（0＝自動）、「多久沒有新封包才回報」的做法、後面整幀沒到的幀要不要一起報
//   links=2 lag2=5           兩條連線（每個封包各一份）、第二條晚幾毫秒
//   drop=F:a-b[,F:a-b…]      第一條連線上第 F 幀的第 a～b 個封包掉了；drop2＝第二條
//   gap=a:b                  下行在相對原點 a～b ms 之間到達的封包全掉；gap2＝第二條；upgap＝上行（回報）
//   pause=F:k:ms             第 F 幀從第 k 個封包起整個往後延 ms（鏈路停頓，不是掉包）
//   sched=ms:kind:val        相對原點 ms 的時刻切換：kind 0＝規則（0 現行／1 RTT）、1＝「兩條在送」、2＝估計 hi、
//                            3＝估計 mid、4＝保險暫停（1）／恢復（0）、5＝估計 lo
//   origin=3                 時間原點＝第幾幀第一個封包該到的時刻
#include "Limelight-internal.h"
#include "VrMultiLink.h"
#include "rswrapper.h"
#include "VrRepairRtt.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

int AppVersionQuad[4] = {7, 1, 431, -1};
STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;

static uint64_t g_now;
static uint64_t g_t0;
uint64_t PltGetMicroseconds(void) { return g_now++; }

static int P_quiet = 0;
static double rel(uint64_t t) { return ((double) t - (double) g_t0) / 1000.0; }

static void logAt(const char* fmt, ...) {
    va_list ap;
    if (P_quiet) return;
    printf("  [%+8.2f ms] ", rel(g_now));
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static void limelogCb(const char* fmt, ...) {
    va_list ap;
    size_t n;
    if (P_quiet) return;
    printf("  [%+8.2f ms] (client log) ", rel(g_now));
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    n = strlen(fmt);
    if (n == 0 || fmt[n - 1] != '\n') printf("\n");
}

void srv_reset(void);
int srv_put(uint32_t frame, uint8_t block, uint16_t first, uint16_t data, uint16_t total, const uint8_t* pkts, size_t pkt_size, size_t count, int64_t now_ns);
int srv_on_nack(const uint8_t* body, size_t len, int64_t now_ns, int rtt_ms10, int64_t video_max_age_us, double video_pps, int rtt_feature,
                uint8_t* out_shards, int* out_needed, double* out_age_ms, int* out_round, char* why, size_t whylen);

// ── 參數 ──
static int P_mode = 0;              // 0＝現行規則、1＝§VR-REPAIR-RTT
static int P_fps = 120;
static double P_rttMs = 12.0;       // 真實往返（上下行各一半）
static double P_estLo = -1, P_estMid = -1, P_estHi = -1;  // client 手上的估計（ms）；預設＝真實值
static int P_D = 6;                 // 每個 block 的 data shard 數
static int P_blocks = 1;            // 每幀幾個 FEC block
static int P_fec = 40;
static int P_minPar = 2;
static int P_frames = 12;
static int P_spacingUs = 70;
static int P_turnUs = 300;
static int P_repairLoss = 0;
static int P_ir = 8;
static int P_origin = 3;
static int P_capMs = 0;
static int P_ahead = 1;             // 後面整幀沒到的幀一起回報
static int P_idle = 0;              // VRRR_IDLE_*
static int P_links = 1;             // 1 或 2
static double P_lag2Ms = 0;         // 第二條連線每個封包晚多久
// 時間表（相對原點的毫秒）：kind 見檔頭
static struct { double ms; int kind; double val; } g_sched[32];
static int g_schedCount;
static int g_forceDual = 0;
static int g_forcePause = 0;

#define MAXF 220
#define MAXGAP 64
typedef struct {
    int D, P, bt, total, pct;   // 每個 block：D data＋P parity＝bt；total＝blocks × bt
    uint16_t lowseq;
    uint64_t sendUs;
    uint8_t* bytes;
    int put;
    uint64_t doneUs;   // 0＝沒收齊
    uint64_t ownUs;    // 這一幀自己的最後一個封包（該）到的時刻
    int dropped;       // 第一次送出時在路上掉了幾個（第一條連線）
    int lost;          // bit0＝notifyFrameLost 涵蓋到；bit1＝有 data 在路上掉了
} FRAME;
static FRAME F[MAXF + 1];
static int g_blocksize;
static uint8_t g_drop[2][MAXF + 1][1024];
static struct { int frame, from; double ms; } g_pause[8];
static int g_pauseCount;
typedef struct { uint64_t a, b; } GAP;
static GAP g_downGap[2][MAXGAP], g_upGap[8];
static int g_downGapCount[2], g_upGapCount;

enum { EV_PKT = 1, EV_GONE = 2 };
typedef struct {
    uint64_t t;
    uint64_t order;
    int kind;
    uint32_t frame;
    int shard;      // 幀內的序號（跨 block 連續）
    int repair;
    int link;
} EVENT;
static EVENT* g_ev;
static int g_evCount, g_evCap;
static uint64_t g_order;

static void evAdd(uint64_t t, int kind, uint32_t frame, int shard, int repair, int link) {
    EVENT e;
    int i;
    if (g_evCount == g_evCap) {
        g_evCap = g_evCap ? g_evCap * 2 : 65536;
        g_ev = realloc(g_ev, (size_t) g_evCap * sizeof(EVENT));
    }
    e.t = t; e.order = g_order++; e.kind = kind; e.frame = frame; e.shard = shard; e.repair = repair; e.link = link;
    for (i = g_evCount; i > 0 && (g_ev[i - 1].t > e.t); i--) g_ev[i] = g_ev[i - 1];
    g_ev[i] = e;
    g_evCount++;
}

static int inGap(const GAP* g, int count, uint64_t t) {
    int i;
    for (i = 0; i < count; i++) if (t >= g[i].a && t < g[i].b) return 1;
    return 0;
}

static RTP_VIDEO_QUEUE q;
static uint32_t g_mlHoldUs;
static int g_nackRound;
static int g_lostFrames, g_doneFrames;
static int g_nacks, g_repairPkts, g_repairUsed, g_repairRejected;
static uint64_t g_firstWaveDone;
static uint64_t g_firstLossUs;
static int g_trips;
#ifndef LEGACY_INLINE
static VRRR_STATE g_state;
#endif
#ifndef SIM_QUEUE_ORIG
static uint32_t g_seenOk, g_seenBad;
#endif

static void applySched(void) {
    int i;
    for (i = 0; i < g_schedCount; i++) {
        if (g_sched[i].kind >= 0 && (double) g_now >= (double) g_t0 + g_sched[i].ms * 1000.0) {
            switch (g_sched[i].kind) {
                case 0: P_mode = (int) g_sched[i].val; break;
                case 1: g_forceDual = (int) g_sched[i].val; break;
                case 2: P_estHi = g_sched[i].val; break;
                case 3: P_estMid = g_sched[i].val; break;
                case 4: g_forcePause = (int) g_sched[i].val; break;
                case 5: P_estLo = g_sched[i].val; break;
            }
            if (!P_quiet) printf("  [%+8.2f ms] (sched) kind=%d val=%g applied\n", ((double) g_now - (double) g_t0) / 1000.0, g_sched[i].kind, g_sched[i].val);
            g_sched[i].kind = -1;
        }
    }
}

// VideoStream.c 接收迴圈裡「每個封包的決定」那一段
static void setQueueFields(int rxLink) {
    const uint32_t T = 1000000u / (uint32_t) P_fps;
    const int carrying = (P_links >= 2 || g_forceDual) ? 2 : 1;
    const int repairRttMs = (int) (P_estMid + 0.999);   // vrmlCtrlRttMs()：最近一筆、無條件進位
    int ml;
    applySched();
    q.multiLink = true;
    q.mlCarryMask = (uint8_t) (carrying >= 2 ? 3 : 1);
#ifdef LEGACY_INLINE
    // ── 從改動前的 VideoStream.c 原樣搬來 ──
    q.mlHoldDual = carrying >= 2;
    q.mlRepair = repairRttMs >= 0 && (uint32_t) repairRttMs * 1000u + 2000u <= g_mlHoldUs + g_mlHoldUs / 2;
    if (q.mlRepair) {
        uint32_t rtoUs = 2000u * (uint32_t) repairRttMs + 3000u;
        q.mlRepairRtoUs = rtoUs < 4000u ? 4000u : (rtoUs > 10000u ? 10000u : rtoUs);
        for (ml = 0; ml < RTPV_ML_LINKS; ml++) q.mlLinkLagUs[ml] = (ml == 1 && P_links >= 2) ? (uint32_t) (P_lag2Ms * 1000.0) : 0;
    }
    q.mlHoldMaxUs = (q.mlHoldDual || q.mlRepair) ? g_mlHoldUs : 0;
    (void) T;
#else
    {
        VRRR_IN in;
        VRRR_OUT out;
        memset(&in, 0, sizeof(in));
        in.negotiatedRepair = true;
#ifndef SIM_QUEUE_ORIG
        {
            // 上一個封包（與它之後的逾時處理）是照 RTT 規則跑的，那段時間的結果才交給保險
            const bool prevRtt = q.mlRttMode;
            in.negotiatedRtt = true;
            in.armRtt = P_mode == 1;
            in.latched = RtpvRepairLatched(&q);
            in.okDelta = prevRtt ? q.mlTot.repairedUsed - g_seenOk : 0;
            in.badDelta = prevRtt ? q.mlTot.lostNacked - g_seenBad : 0;
            g_seenOk = q.mlTot.repairedUsed;
            g_seenBad = q.mlTot.lostNacked;
        }
#endif
        in.carrying = carrying;
        in.legacyRttMs = repairRttMs;
        in.haveStats = true;
        in.st.loUs = (uint32_t) (P_estLo * 1000.0);
        in.st.midUs = (uint32_t) (P_estMid * 1000.0);
        in.st.hiUs = (uint32_t) (P_estHi * 1000.0);
        in.st.samples = 8;
        in.holdUs = g_mlHoldUs;
        in.framePeriodUs = T;
        in.capOverrideMs = (uint32_t) P_capMs;
        in.idlePolicy = (uint8_t) P_idle;
        in.nowUs = g_now;
        if (g_forcePause) {
            g_state.brk.pausedUntilUs = UINT64_MAX;
        }
        else if (g_state.brk.pausedUntilUs == UINT64_MAX) {
            g_state.brk.pausedUntilUs = 0;
        }
        vrrrDecide(&in, &g_state, &out);
        if (out.tripped) {
            g_trips++;
            logAt("(保險) 暫停 RTT 規則 %.0f ms（level %u）\n", (double) (g_state.brk.pausedUntilUs - g_now) / 1000.0, (unsigned) g_state.brk.level);
        }
#ifdef SIM_QUEUE_ORIG
        q.mlHoldDual = out.mlHoldDual;
        q.mlRepair = out.mlRepair;
        if (out.mlRepair) {
            q.mlRepairRtoUs = out.mlRepairRtoUs;
        }
        q.mlHoldMaxUs = out.mlHoldMaxUs;
#else
        RtpvSetRepairDecision(&q, &out, T, P_ahead != 0);
#endif
        if (out.mlRepair) {
            for (ml = 0; ml < RTPV_ML_LINKS; ml++) q.mlLinkLagUs[ml] = (ml == 1 && P_links >= 2) ? (uint32_t) (P_lag2Ms * 1000.0) : 0;
        }
    }
#endif
#ifndef SIM_QUEUE_ORIG
    RtpvSetRepairMeas(&q, true, g_now, (uint32_t) (P_estLo * 1000.0));
#endif
    q.mlRxLink = rxLink;
}

void connectionSawFrame(uint32_t frameIndex) { (void) frameIndex; }
void vrmlNoteVideoUsed(int linkIdx) { (void) linkIdx; }

static int g_fecStatusCount, g_fecStatusMissing;
void connectionSendFrameFecStatus(PSS_FRAME_FEC_STATUS s) {
    g_fecStatusCount++;
    g_fecStatusMissing += BE16(s->missingPacketsBeforeHighestReceived);
    logAt("FEC status → server：frame=%u missing=%u（data=%u parity=%u；收到 data=%u parity=%u）\n",
          (unsigned) BE32(s->frameIndex), (unsigned) BE16(s->missingPacketsBeforeHighestReceived), (unsigned) BE16(s->totalDataPackets),
          (unsigned) BE16(s->totalParityPackets), (unsigned) BE16(s->receivedDataPackets), (unsigned) BE16(s->receivedParityPackets));
}

static uint64_t sendUsOf(int n) { return F[1].sendUs + (uint64_t) (n - 1) * (1000000ull / (uint64_t) P_fps); }

void notifyFrameLost(unsigned int frameNumber, bool speculative) {
    const uint64_t up = (uint64_t) (P_rttMs * 500.0), down = up;
    (void) speculative;
    g_lostFrames++;
    if (frameNumber <= MAXF) F[frameNumber].lost |= 1;
    {
        // LOSS：VrSend 執行緒每 2 ms 一輪，取中間值 +1 ms 送出；server 收到時正在等的那一張照編，wave 從下一張開始
        const uint64_t ts = g_now + 1000 + up;
        int k = 1;
        while (sendUsOf(k) < ts) k++;
        {
            const int w = k + 1;
            const int done = w + P_ir - 1;
            const uint64_t tDone = sendUsOf(done) + down + (uint64_t) F[1].total * (uint64_t) P_spacingUs;
            logAt("notifyFrameLost(%u) → LOSS；(模型) wave＝第 %d～%d 幀，REFRESH_DONE 那一幀 %+.2f 到齊（recover＝%.1f ms）\n",
                  frameNumber, w, done, rel(tDone), ((double) tDone - (double) g_now) / 1000.0);
            if (g_firstWaveDone == 0 || tDone > g_firstWaveDone) g_firstWaveDone = tDone;
            if (g_firstLossUs == 0) g_firstLossUs = g_now;
        }
    }
}

void queueRtpPacket(PRTPV_QUEUE_ENTRY e) {
    PRTP_PACKET p = e->packet;
    PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET) (((char*) p) + sizeof(*p) + 4);
    uint32_t f = nv->frameIndex;
    if (f <= MAXF && F[f].doneUs == 0) {
        F[f].doneUs = g_now;
        g_doneFrames++;
        logAt("第 %u 幀收齊（比它第一個封包該到的時刻晚 %.2f ms）\n", f,
              ((double) g_now - (double) (F[f].sendUs + (uint64_t) (P_rttMs * 500.0))) / 1000.0);
    }
    free(p);
}

int vrmlSendCtrl(const unsigned char* tlv, int length) {
    VIPLE_VR_TLV_NACK n;
    const uint64_t up = (uint64_t) (P_rttMs * 500.0), down = up;
    uint8_t shards[256];
    int needed = 0, round = 0, r, i, b;
    double ageMs = 0;
    char why[200];
    const uint64_t ts = g_now + up;
    const int64_t maxAgeUs = 2 * (1000000 / P_fps) < 8000 ? 8000 : (2 * (1000000 / P_fps) > 40000 ? 40000 : 2 * (1000000 / P_fps));  // stream.cpp 的 opt.video_max_age

    if (length < 2 + (int) sizeof(n) || tlv[0] != VIPLE_VR_C2S_NACK) return 1;
    memcpy(&n, tlv + 2, sizeof(n));
    g_nacks++;
    logAt("NACK 送出：frame=%u block=%u need=%u total=%u attempt=%u flags=0x%02x\n", n.frame, (unsigned) n.block, (unsigned) n.need,
          (unsigned) n.total, (unsigned) n.attempt, (unsigned) n.flags);
    if (inGap(g_upGap, g_upGapCount, g_now)) {
        if (!P_quiet) printf("  %*s (模型) 這一份回報在上行的空白裡掉了\n", 13, "");
        return 1;
    }
    for (i = 1; i <= P_frames; i++) {
        if (!F[i].put && F[i].sendUs <= ts) {
            for (b = 0; b < P_blocks; b++) {
                srv_put((uint32_t) i, (uint8_t) b, 0, (uint16_t) F[i].D, (uint16_t) F[i].bt, F[i].bytes + (size_t) b * (size_t) F[i].bt * (size_t) g_blocksize,
                        (size_t) g_blocksize, (size_t) F[i].bt, (int64_t) F[i].sendUs * 1000);
            }
            F[i].put = 1;
        }
    }
    r = srv_on_nack(tlv + 2, (size_t) (length - 2), (int64_t) ts * 1000, (int) (P_rttMs * 10.0), maxAgeUs, (double) F[1].total * P_fps,
#ifdef SIM_QUEUE_ORIG
                    0,
#else
                    1,
#endif
                    shards, &needed, &ageMs, &round, why, sizeof(why));
    if (!P_quiet) printf("  %*s (模型) server %+.2f 收到：block 年齡 %.1f ms、第 %d 輪 → %s\n", 13, "", rel(ts), ageMs, round, why);
    if (r > 0) {
        g_nackRound++;
        g_repairPkts += r;
        if (g_nackRound <= P_repairLoss) {
            if (!P_quiet) printf("  %*s (模型) 這一輪補包在路上全掉（repairloss）\n", 13, "");
        } else {
            int kept = 0, l;
            const uint64_t tb = ts + (uint64_t) P_turnUs + down;
            for (l = 0; l < P_links; l++) {
                const uint64_t lagUs = l == 1 ? (uint64_t) (P_lag2Ms * 1000.0) : 0;
                for (i = 0; i < r; i++) {
                    const uint64_t ta = tb + lagUs + (uint64_t) i * (uint64_t) P_spacingUs;
                    if (inGap(g_downGap[l], g_downGapCount[l], ta)) continue;
                    evAdd(ta, EV_PKT, n.frame, (int) n.block * (n.frame <= MAXF ? F[n.frame].bt : 0) + shards[i], 1, l);
                    kept++;
                }
            }
            if (!P_quiet) printf("  %*s (模型) 補包 %d 個（%d 個沒掉）在 %+.2f～%+.2f 到 client\n", 13, "", r, kept, rel(tb),
                   rel(tb + (uint64_t) (r - 1) * (uint64_t) P_spacingUs));
        }
    } else if (r < 0) {
        evAdd(ts + down, EV_GONE, n.frame, (int) n.block, 0, 0);
    }
    return 1;
}

static int parityFor(int D, int* pctOut) {
    int par = (D * P_fec + 99) / 100, pct = P_fec;
    if (par < P_minPar && P_fec != 0) {
        par = P_minPar;
        pct = (100 * par) / D;
    }
    *pctOut = pct;
    return par;
}

static void buildFrame(int n, uint16_t lowseq, uint64_t sendUs) {
    FRAME* f = &F[n];
    int x, b, D = P_D, pct = 0, par = parityFor(P_D, &pct);
    const int bt = D + par;
    f->D = D; f->P = par; f->bt = bt; f->total = bt * P_blocks; f->pct = pct; f->lowseq = lowseq; f->sendUs = sendUs;
    f->bytes = calloc((size_t) f->total, (size_t) g_blocksize);
    for (b = 0; b < P_blocks; b++) {
        uint8_t** sp = calloc((size_t) bt, sizeof(uint8_t*));
        for (x = 0; x < bt; x++) sp[x] = f->bytes + (size_t) (b * bt + x) * (size_t) g_blocksize;
        for (x = 0; x < D; x++) {
            PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET) (sp[x] + 16);
            nv->frameIndex = (uint32_t) n;
            nv->streamPacketIndex = ((uint32_t) lowseq + (uint32_t) (b * bt + x)) << 8;
            nv->multiFecFlags = 0x10;
            nv->flags = FLAG_CONTAINS_PIC_DATA | ((b == 0 && x == 0) ? FLAG_SOF : 0) | ((b == P_blocks - 1 && x == D - 1) ? FLAG_EOF : 0);
            memset(sp[x] + 32, 0x40 + (x & 0x3f), (size_t) g_blocksize - 32);
        }
        if (par > 0) {
            reed_solomon* rs = reed_solomon_new(D, par);
            reed_solomon_encode(rs, sp, bt, g_blocksize);
            reed_solomon_release(rs);
        }
        for (x = 0; x < bt; x++) {
            PRTP_PACKET rtp = (PRTP_PACKET) sp[x];
            PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET) (sp[x] + 16);
            nv->fecInfo = ((uint32_t) x << 12) | ((uint32_t) D << 22) | ((uint32_t) pct << 4);
            rtp->header = 0x80 | FLAG_EXTENSION;
            rtp->sequenceNumber = (uint16_t) (lowseq + b * bt + x);
            rtp->timestamp = (uint32_t) (sendUs * 9 / 100);
            rtp->ssrc = 0;
            nv->multiFecFlags = 0x10;
            nv->multiFecBlocks = (uint8_t) ((b << 4) | ((P_blocks - 1) << 6));
            nv->frameIndex = (uint32_t) n;
        }
        free(sp);
    }
}

static int cmpStaged(const void* a, const void* b) {
    const EVENT* x = a;
    const EVENT* y = b;
    if (x->frame != y->frame) return (int32_t) (x->frame - y->frame) < 0 ? -1 : 1;
    {
        uint16_t sx = (uint16_t) (F[x->frame].lowseq + x->shard), sy = (uint16_t) (F[y->frame].lowseq + y->shard);
        if (sx != sy) return (int16_t) (uint16_t) (sx - sy) < 0 ? -1 : 1;
    }
    return x->order < y->order ? -1 : (x->order > y->order);
}

static void deliver(const EVENT* e) {
    const int bufferSize = g_blocksize + (int) sizeof(RTPV_QUEUE_ENTRY);
    char* buffer = malloc((size_t) bufferSize);
    int st;
    memcpy(buffer, F[e->frame].bytes + (size_t) e->shard * (size_t) g_blocksize, (size_t) g_blocksize);
    setQueueFields(P_links >= 2 ? e->link : 0);
    st = RtpvAddPacket(&q, (PRTP_PACKET) buffer, g_blocksize, (PRTPV_QUEUE_ENTRY) &buffer[g_blocksize]);
    if (e->repair) {
        if (st == RTPF_RET_REJECTED) g_repairRejected++; else g_repairUsed++;
        logAt("補包 frame=%u shard=%d 到 → %s\n", e->frame, e->shard,
              st == RTPF_RET_QUEUED ? "採用" : (st == RTPF_RET_HELD ? "排隊" : "被拒收（那一幀已經放棄或收齊）"));
    }
    if (st == RTPF_RET_REJECTED) free(buffer);
}

static void parseDrop(const char* s, int link) {
    while (*s) {
        int f = 0, a = 0, b = 0, i;
        if (sscanf(s, "%d:%d-%d", &f, &a, &b) == 3 && f >= 1 && f <= MAXF) {
            for (i = a; i <= b && i < 1024; i++) g_drop[link][f][i] = 1;
        }
        s = strchr(s, ',');
        if (!s) break;
        s++;
    }
}

int main(int argc, char** argv) {
    int i, x, l;
    uint64_t T, down;
    const char* title = "";
    static double gapA[2][MAXGAP], gapB[2][MAXGAP], upA[8], upB[8];
    int nGap[2] = {0, 0}, nUp = 0;
    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (!strncmp(a, "mode=", 5)) P_mode = !strcmp(a + 5, "rtt") ? 1 : 0;
        else if (!strncmp(a, "fps=", 4)) P_fps = atoi(a + 4);
        else if (!strncmp(a, "rtt=", 4)) P_rttMs = atof(a + 4);
        else if (!strncmp(a, "est=", 4)) { sscanf(a + 4, "%lf:%lf:%lf", &P_estLo, &P_estMid, &P_estHi); }
        else if (!strncmp(a, "D=", 2)) P_D = atoi(a + 2);
        else if (!strncmp(a, "blocks=", 7)) P_blocks = atoi(a + 7);
        else if (!strncmp(a, "fec=", 4)) P_fec = atoi(a + 4);
        else if (!strncmp(a, "frames=", 7)) P_frames = atoi(a + 7);
        else if (!strncmp(a, "spacing=", 8)) P_spacingUs = atoi(a + 8);
        else if (!strncmp(a, "turn=", 5)) P_turnUs = atoi(a + 5);
        else if (!strncmp(a, "repairloss=", 11)) P_repairLoss = atoi(a + 11);
        else if (!strncmp(a, "ir=", 3)) P_ir = atoi(a + 3);
        else if (!strncmp(a, "origin=", 7)) P_origin = atoi(a + 7);
        else if (!strncmp(a, "capms=", 6)) P_capMs = atoi(a + 6);
        else if (!strncmp(a, "ahead=", 6)) P_ahead = atoi(a + 6);
        else if (!strncmp(a, "idle=", 5)) P_idle = !strcmp(a + 5, "fixed") ? 1 : !strcmp(a + 5, "frame") ? 2 : !strcmp(a + 5, "learned") ? 3 : !strcmp(a + 5, "size") ? 4 : atoi(a + 5);
        else if (!strncmp(a, "links=", 6)) P_links = atoi(a + 6);
        else if (!strncmp(a, "lag2=", 5)) P_lag2Ms = atof(a + 5);
        else if (!strncmp(a, "quiet=", 6)) P_quiet = atoi(a + 6);
        else if (!strncmp(a, "drop=", 5)) parseDrop(a + 5, 0);
        else if (!strncmp(a, "drop2=", 6)) parseDrop(a + 6, 1);
        else if (!strncmp(a, "title=", 6)) title = a + 6;
        else if (!strncmp(a, "gap=", 4)) { if (nGap[0] < MAXGAP && sscanf(a + 4, "%lf:%lf", &gapA[0][nGap[0]], &gapB[0][nGap[0]]) == 2) nGap[0]++; }
        else if (!strncmp(a, "gap2=", 5)) { if (nGap[1] < MAXGAP && sscanf(a + 5, "%lf:%lf", &gapA[1][nGap[1]], &gapB[1][nGap[1]]) == 2) nGap[1]++; }
        else if (!strncmp(a, "upgap=", 6)) { if (nUp < 8 && sscanf(a + 6, "%lf:%lf", &upA[nUp], &upB[nUp]) == 2) nUp++; }
        else if (!strncmp(a, "sched=", 6)) {
            double ms = 0, val = 0; int kind = 0;
            if (sscanf(a + 6, "%lf:%d:%lf", &ms, &kind, &val) == 3 && g_schedCount < 32) {
                g_sched[g_schedCount].ms = ms; g_sched[g_schedCount].kind = kind; g_sched[g_schedCount].val = val; g_schedCount++;
            }
        }
        else if (!strncmp(a, "pause=", 6)) {
            int f = 0, k = 0; double ms = 0;
            if (sscanf(a + 6, "%d:%d:%lf", &f, &k, &ms) == 3 && g_pauseCount < 8) {
                g_pause[g_pauseCount].frame = f; g_pause[g_pauseCount].from = k; g_pause[g_pauseCount].ms = ms; g_pauseCount++;
            }
        }
        else {
            fprintf(stderr, "unknown argument: %s\n", a);
            return 2;
        }
    }
    if (P_frames > MAXF) P_frames = MAXF;
    if (P_blocks < 1) P_blocks = 1;
    if (P_blocks > 4) P_blocks = 4;
    if (P_links < 1) P_links = 1;
    if (P_links > 2) P_links = 2;
    if (P_estLo < 0) P_estLo = P_rttMs;
    if (P_estMid < 0) P_estMid = P_rttMs;
    if (P_estHi < 0) P_estHi = P_rttMs;

    memset(&StreamConfig, 0, sizeof(StreamConfig));
    StreamConfig.packetSize = 1024;
    StreamConfig.fps = P_fps;
    ListenerCallbacks.logMessage = limelogCb;
    g_blocksize = StreamConfig.packetSize + MAX_RTP_HEADER_SIZE;

    g_mlHoldUs = P_fps > 0 ? 1000000u / (uint32_t) P_fps : 11111u;
    if (g_mlHoldUs < 4000) g_mlHoldUs = 4000;
    if (g_mlHoldUs > 14000) g_mlHoldUs = 14000;

    T = 1000000ull / (uint64_t) P_fps;
    down = (uint64_t) (P_rttMs * 500.0);

    RtpvInitializeQueue(&q);
    srv_reset();

    {
        uint16_t lowseq = 1000;
        for (i = 1; i <= P_frames; i++) {
            buildFrame(i, lowseq, 1000000ull + (uint64_t) (i - 1) * T);
            lowseq = (uint16_t) (lowseq + F[i].total);
        }
    }
    g_t0 = F[P_origin].sendUs + down;
    for (l = 0; l < 2; l++) {
        for (i = 0; i < nGap[l]; i++) {
            g_downGap[l][i].a = g_t0 + (uint64_t) (int64_t) (gapA[l][i] * 1000.0);
            g_downGap[l][i].b = g_t0 + (uint64_t) (int64_t) (gapB[l][i] * 1000.0);
        }
        g_downGapCount[l] = nGap[l];
    }
    for (i = 0; i < nUp; i++) { g_upGap[i].a = g_t0 + (uint64_t) (int64_t) (upA[i] * 1000.0); g_upGap[i].b = g_t0 + (uint64_t) (int64_t) (upB[i] * 1000.0); }
    g_upGapCount = nUp;

    for (l = 0; l < P_links; l++) {
        uint64_t lastTa = 0;
        const uint64_t lagUs = l == 1 ? (uint64_t) (P_lag2Ms * 1000.0) : 0;
        for (i = 1; i <= P_frames; i++) {
            for (x = 0; x < F[i].total; x++) {
                // 同一條路徑照順序送達：停頓把這個封包和它後面的全部往後推（後面的幀也一樣），停頓結束後積著的封包緊接著到
                const int isPar = (x % F[i].bt) >= F[i].D;
                const uint64_t tb = F[i].sendUs + down + lagUs;
                uint64_t ta = tb + (uint64_t) x * (uint64_t) P_spacingUs;
                int pz;
                if (ta < lastTa + 20) ta = lastTa + 20;
                for (pz = 0; pz < g_pauseCount; pz++) {
                    if (l == 0 && g_pause[pz].frame == i && x == g_pause[pz].from) ta += (uint64_t) (g_pause[pz].ms * 1000.0);
                }
                lastTa = ta;
                if (l == 0 && ta > F[i].ownUs) F[i].ownUs = ta;
                if (g_drop[l][i][x] || inGap(g_downGap[l], g_downGapCount[l], ta)) {
                    if (l == 0) { F[i].dropped++; if (!isPar) F[i].lost |= 2; }
                    continue;
                }
                evAdd(ta, EV_PKT, (uint32_t) i, x, 0, l);
            }
        }
    }

    setQueueFields(0);
    if (!P_quiet) {
        printf("== %s\n", title);
        printf("   %s｜%d Hz（幀週期 %.2f ms）、真實往返 %.1f ms、client 估計 lo/mid/hi=%.1f/%.1f/%.1f ms；每幀 %d×(%d data＋%d parity)；連線 %d 條；mlRepair=%d mlHoldMaxUs=%u",
               P_mode ? "§VR-REPAIR-RTT" : "現行規則", P_fps, T / 1000.0, P_rttMs, P_estLo, P_estMid, P_estHi, P_blocks, F[1].D, F[1].P, P_links,
               (int) q.mlRepair, q.mlHoldMaxUs);
#ifndef SIM_QUEUE_ORIG
        if (q.mlRttMode) printf(" eta=%.2f try=%.2f fast=%.2f cap=%.1f ms idle=%d", q.mlRepairEtaUs / 1000.0, q.mlRepairEtaTryUs / 1000.0,
                                q.mlRepairEtaFastUs / 1000.0, q.mlHoldCapUs / 1000.0, (int) q.mlIdlePolicy);
#endif
        printf("\n");
    }

    g_now = F[1].sendUs;
    for (;;) {
        int pollMs, waitMs, n = 0;
        uint64_t tNext;
        static EVENT staged[4096];

        for (i = 0; i < g_evCount;) {
            if (g_ev[i].kind == EV_GONE && g_ev[i].t <= g_now) {
                EVENT e = g_ev[i];
                memmove(&g_ev[i], &g_ev[i + 1], (size_t) (g_evCount - i - 1) * sizeof(EVENT));
                g_evCount--;
                logAt("REPAIR_GONE 到（frame=%u block=%d）→ RtpvRepairGone\n", e.frame, e.shard);
                setQueueFields(0);
                RtpvRepairGone(&q, e.frame, (uint8_t) e.shard);
            } else {
                i++;
            }
        }
        pollMs = RtpvHoldTimeoutMs(&q);
        waitMs = (pollMs > 0 && pollMs < UDP_RECV_POLL_TIMEOUT_MS) ? pollMs : UDP_RECV_POLL_TIMEOUT_MS;
        tNext = g_evCount > 0 ? g_ev[0].t : UINT64_MAX;
        if (tNext == UINT64_MAX && pollMs == 0) break;
        if (tNext != UINT64_MAX && tNext <= g_now + (uint64_t) waitMs * 1000ull) {
            if (tNext > g_now) g_now = tNext;
            for (i = 0; i < g_evCount && g_ev[i].t <= g_now;) {
                if (g_ev[i].kind == EV_PKT && n < 4096) {
                    staged[n++] = g_ev[i];
                    memmove(&g_ev[i], &g_ev[i + 1], (size_t) (g_evCount - i - 1) * sizeof(EVENT));
                    g_evCount--;
                } else {
                    i++;
                }
            }
            qsort(staged, (size_t) n, sizeof(EVENT), cmpStaged);
            for (i = 0; i < n; i++) deliver(&staged[i]);
        } else {
            g_now += (uint64_t) waitMs * 1000ull;
            if (pollMs > 0) {
                setQueueFields(0);
                RtpvPollHold(&q);
            }
        }
        if (g_now > F[P_frames].sendUs + 400000ull) break;
    }
    if (!P_quiet) printf("   結果：%d 幀收齊、%d 次放棄；回報 %d 份、server 補送 %d 個封包（採用 %d、被拒收 %d）；FEC status %d 份、missing 合計 %d\n\n",
           g_doneFrames, g_lostFrames, g_nacks, g_repairPkts, g_repairUsed, g_repairRejected, g_fecStatusCount, g_fecStatusMissing);
    // 機器可讀：每幀「第一個封包該到的時刻」與「收齊的時刻」（相對原點，ms）
    for (i = 1; i <= P_frames; i++) {
        printf("RESULT f=%d nominal=%.3f done=%.3f ok=%d lost=%d dropped=%d own=%.3f total=%d\n", i, rel(F[i].sendUs + down),
               F[i].doneUs ? rel(F[i].doneUs) : 0.0, F[i].doneUs ? 1 : 0, F[i].lost & 1, F[i].dropped, rel(F[i].ownUs), F[i].total);
    }
    printf("SUMMARY done=%d lost=%d nacks=%d repairPkts=%d used=%d rejected=%d fecStatus=%d fecMissing=%d waveDone=%.3f firstLoss=%.3f trips=%d\n",
           g_doneFrames, g_lostFrames, g_nacks, g_repairPkts, g_repairUsed, g_repairRejected, g_fecStatusCount, g_fecStatusMissing,
           g_firstWaveDone ? rel(g_firstWaveDone) : -1.0, g_firstLossUs ? rel(g_firstLossUs) : -1.0, g_trips);
#ifndef SIM_QUEUE_ORIG
    // 佇列的累計數（另一行：和改動前的佇列比時間軸時不比這一行）
    printf("TOTALS nack=%u repaired=%u repairedUsed=%u used=%u spurious=%u lost=%u lostNacked=%u lostUnasked=%u lostWhole=%u late=%u afterLoss=%u"
           " tooMany=%u ahead=%u lateDone=%u/%u/%u/%u/%u lateNever=%u stall=%u/%u/%u/%u/%u\n",
           q.mlTot.nack, q.mlTot.repaired, q.mlTot.repairedUsed, q.mlTot.used, q.mlTot.spurious, q.mlTot.lost, q.mlTot.lostNacked,
           q.mlTot.lostUnasked, q.mlTot.lostWhole, q.mlTot.late, q.mlTot.afterLoss, q.mlTot.tooMany, q.mlTot.ahead,
           q.mlTot.lateDone[0], q.mlTot.lateDone[1], q.mlTot.lateDone[2], q.mlTot.lateDone[3], q.mlTot.lateDone[4], q.mlTot.lateNever,
           q.mlTot.stall[0], q.mlTot.stall[1], q.mlTot.stall[2], q.mlTot.stall[3], q.mlTot.stall[4]);
#endif
    return 0;
}
