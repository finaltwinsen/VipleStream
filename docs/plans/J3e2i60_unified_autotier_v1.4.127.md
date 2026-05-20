# §J.3.e.2.i.60 / v1.4.127 — Unified Vulkan FRUC Autotier

## Context

**Why this change is being made:**

Today VipleStream 的 Vulkan FRUC 有 5+ 個獨立 UI 子選項（master toggle + NVOF + TRIPLE + Native RIFE + inferDim + auto-tier flag），每個都可單獨開關，加上 4 條 runtime autotier 分支（DUAL↔TRIPLE / chain_lv 1-3 / ASYNC↔SYNC / NVOF demote）彼此**不協調**：

- NVOF 30 連續 fail 才整個 demote，沒有「降載」概念
- chain_lv downgrade 跟 NVOF 狀態無關，可能 NVOF on + chain_lv 1 同時發生（不合理）
- TRIPLE / ASYNC 各自為政，user 看不出某 GPU 應該在哪個組合

**Goal:** 把 Vulkan 路徑下所有 FRUC 子選項收掉，UI 只剩 `enableFrameInterpolation` 一個 master checkbox。背後是統一 autotier 階梯（T-1..T5）：初始位置根據 GPU 能力 + benchmark 決定，runtime 根據 chain_mean / latency / NVOF health 沿階梯升降，最糟自動進 T-1（暫時停用 FRUC，純 pass-through decode + present）。

各 GPU class 都能達到「在能力內最高品質 + 低延遲 + 不卡頓」。

---

## Tier hierarchy

**Axes:**
- **Tier**（T-1..T5）：feature combo（NVOF / chain_lv / ASYNC / RIFE）
- **TRIPLE/DUAL**：orthogonal axis（任何 tier 內，chain_mean<3ms 持續 300 frame → TRIPLE；>5ms 持續 30 frame → DUAL）

**Tier table:**

| Tier | NVOF | chain_lv | ASYNC | RIFE | 適用 GPU |
|------|------|----------|-------|------|----------|
| **T5** | ✅ | 3 | ✅ | dim=256 | RTX 4070+ (QUALITY) |
| **T4** | ✅ | 3 | ✅ | dim=128 | RTX 3060+ (BALANCED + NVOF) |
| **T3** | ✅ | 2 | ✅ | dim=128 | NVOF GPU 中 chain 偏重 |
| **T2** | ❌ | 2 | ✅ | OFF | 無 NVOF GPU (AMD/Intel) |
| **T1** | ❌ | 1 | ✅ | OFF | PERFORMANCE GPU (GTX 1650, RX 5500) |
| **T0** | ❌ | 1 | SYNC | OFF | 最後 FRUC fallback |
| **T-1** | — | — | — | — | **FRUC 停用**（純 decode + present，no chain）|

**Throttle (frame-drop, v1.4.122-126)** 是獨立第 3 軸，套用在任何 Tier 上（T-1 例外）。Throttle 偵測 latency spike 主動 drop frame，跟 Tier 不衝突。

---

## Initial tier (capability cap)

工廠檢測決定 **可達最高 tier**（cap），不直接設運行 tier。Runtime autotier 從 cap 開始往下降。

**Capability cap rule:**

```
if (!hasComputeQF) cap = T0           // 沒 dedicated compute QF → 只能 SYNC
else if (!hasFRUC) cap = T_DISABLED   // 不支援 FRUC (整合顯卡老舊) → 強制 T-1
else if (vkfrucDetectedTier == ENTRY) cap = T1
else if (vkfrucDetectedTier == PERFORMANCE) cap = T2
else if (vkfrucDetectedTier == BALANCED) {
    cap = hasOpticalFlowQF ? T4 : T2
}
else if (vkfrucDetectedTier == QUALITY) {
    cap = hasOpticalFlowQF ? T5 : T2
}
else cap = T1                          // unknown discrete fallback
```

**Run-time start tier:** session start = cap. 之後 autotier 動態調整。

**Override:** env var `VIPLE_VKFRUC_TIER_CAP=N`（N=-1..5）可強制 cap。Dev 用，不暴露 UI。

---

## Transition rules（已存在的全部沿用 + 整合）

### 降階（T_N → T_{N-1}）

**從 T5/T4 → T3：** `chain_mean > 8ms` 持續 30 frame（chain_lv 3→2 既有條件）

**從 T3 → T2：** `nvof_consec_fail >= 30` OR `chainBusy mean > 14ms 持續 30 frame`（NVOF GPU work 也是 chain 一部分，太重就 demote 掉 NVOF）

**從 T2 → T1：** `chain_mean > 8ms` 持續 30 frame（chain_lv 2→1 既有條件）

**從 T1 → T0：** `chainBusy mean > frameBudgetMs * 0.72` 持續 30 frame（既有 ASYNC→SYNC 條件）

**從 T0 → T-1：** **NEW**: 60s 滾動視窗內進入 T0 三次以上 → 推論 GPU 結構性跑不動 FRUC → T-1（停用 FRUC，純 pass-through）。Recovery: T-1 維持 5 分鐘後重試 cap tier。

### 升階（T_N → T_{N+1}）

統一條件：`chain_mean < 2.5ms` 持續 300 frame（5 秒）+ 30 秒 cool-down 已過 + 當前 tier < cap。

True-idle bypass（chain_mean < 1ms 連 600 frame）跳 cool-down 直接升回 cap（既有 alt-tab 快速恢復邏輯）。

T-1 → T0：等 5 分鐘 hold 後重試 cap，不直接 T0。

### TRIPLE/DUAL orthogonal axis

任何 Tier 內：
- `chain_mean < 3ms` 持續 300 frame → TRIPLE
- `chain_mean > 5ms` 持續 30 frame → DUAL

T-1 時不適用（沒 chain）。

---

## Critical files

### `moonlight-qt/app/streaming/video/ffmpeg-renderers/vkfruc.h`

新增（在 line ~770 throttle 區域附近）：

```cpp
// §J.3.e.2.i.60 (v1.4.127) — unified autotier.
// 7 tiers: T-1 (FRUC disabled) ... T5 (max quality with NVOF+TRIPLE+chain_lv3).
// Tier 決定 4 個 feature axis (NVOF/chain_lv/ASYNC/RIFE), TRIPLE/DUAL 是 orthogonal.
// 取代 v1.4.121 之前的 4 條獨立 autotier 分支.
enum class VkFrucTier : int {
    DISABLED = -1,  // FRUC off, pass-through decode + present
    T0       =  0,  // last-resort: SYNC, chain_lv 1, no NVOF, no RIFE
    T1       =  1,  // ASYNC, chain_lv 1, no NVOF, no RIFE
    T2       =  2,  // ASYNC, chain_lv 2, no NVOF, no RIFE
    T3       =  3,  // ASYNC, chain_lv 2, NVOF, RIFE dim=128
    T4       =  4,  // ASYNC, chain_lv 3, NVOF, RIFE dim=128
    T5       =  5,  // ASYNC, chain_lv 3, NVOF, RIFE dim=256
};
std::atomic<int> m_CurrentTier   { (int)VkFrucTier::DISABLED };  // current runtime tier
int              m_TierCap       = (int)VkFrucTier::DISABLED;     // max achievable by GPU
int64_t          m_TierEnteredMs = 0;                              // current tier's enter time
// T0 demote tracking for T0 → T-1 trigger
int64_t          m_T0EnterTimes[3] = {0, 0, 0};  // ring buffer of last 3 T0 enter times
int              m_T0EnterIdx       = 0;
// T-1 hold (5 min before retry)
int64_t          m_TierDisabledEnteredMs = 0;

// Tier → feature config helper (read in renderFrame to decide which path to take)
struct VkFrucTierConfig {
    bool useNvOf;
    bool useAsync;
    int  chainLevel;     // 1, 2, or 3
    bool useRife;
    int  rifeInferDim;   // 128 or 256
};
VkFrucTierConfig tierConfig(VkFrucTier t) const;
void runAutotierTransition();  // 每 frame 末叫, decide upgrade/downgrade
```

**刪除/廢棄（保留 .cpp/.h 標記為 DEPRECATED 但不馬上刪以便 incremental migration）：**
- `m_FrucChainAsyncTempDisabled` → tier transition 取代
- `m_EffectiveChainLevel` / `m_InitialChainLevel` → tier 內含
- `m_DynamicDualDowngrade` → TRIPLE/DUAL 仍是獨立軸，這個保留但跟 tier 解耦
- `m_FramesAboveHandoffThresh` → tier transition 取代

### `moonlight-qt/app/streaming/video/ffmpeg-renderers/vkfruc.cpp`

新實作 `tierConfig()` helper（純 lookup table，no side effect）：

```cpp
VkFrucTierConfig VkFrucRenderer::tierConfig(VkFrucTier t) const {
    switch (t) {
        case VkFrucTier::T5: return {true,  true,  3, true,  256};
        case VkFrucTier::T4: return {true,  true,  3, true,  128};
        case VkFrucTier::T3: return {true,  true,  2, true,  128};
        case VkFrucTier::T2: return {false, true,  2, false, 0};
        case VkFrucTier::T1: return {false, true,  1, false, 0};
        case VkFrucTier::T0: return {false, false, 1, false, 0};
        case VkFrucTier::DISABLED:
        default:             return {false, false, 0, false, 0};  // chain skipped
    }
}
```

新實作 `runAutotierTransition()`（在 chain measurement block，line ~8800 area）：

```cpp
void VkFrucRenderer::runAutotierTransition() {
    // 取 chain_mean (60-frame avg) + chainBusy mean (60-frame) + NVOF consec fails
    const double chainMean = computeChainMean();      // existing m_ChainMeanMsRing logic
    const double chainBusy = computeChainBusyMean();  // existing m_HandoffMsRing logic
    const uint64_t nvOfFails = m_NvOfProfConsecFails.load();
    const int cur = m_CurrentTier.load();
    const int64_t nowMs = ...;
    const double frameBudgetMs = 1000.0 / m_StreamFps;

    // --- 1) Determine target tier ---
    int target = cur;

    // Downgrade conditions (順序: 最嚴格→最寬鬆, 第一個觸發就 break)
    if (cur == 0) {
        // 在 T0 內仍有問題 → T-1?
        // 60s 內進入 T0 三次 → T-1
        bool eligibleForDisable = false;
        int hits = 0;
        for (int i = 0; i < 3; ++i) {
            if (nowMs - m_T0EnterTimes[i] < 60000) hits++;
        }
        if (hits >= 3) eligibleForDisable = true;
        if (eligibleForDisable) {
            target = (int)VkFrucTier::DISABLED;
            m_TierDisabledEnteredMs = nowMs;
        }
    }
    else if (cur >= 1 && chainBusy > frameBudgetMs * 0.72 && m_FramesAboveHandoffThresh >= 30) {
        // T1+ chainBusy 超 → 降一階
        target = cur - 1;
    }
    else if (cur >= 3 && (nvOfFails >= 30 || chainBusy > 14.0)) {
        // T3+ NVOF demote OR chain 太重 → T2 (no NVOF)
        target = 2;
    }
    else if (cur >= 4 && chainMean > 8.0 && m_FramesAboveChainLevelThresh >= 30) {
        // T4+ chain_lv 3→2 trigger → T3
        target = 3;
    }
    // 注意 chain_lv 2→1 trigger 在 T2→T1 (chain_mean > 8ms 30 幀, 既有條件)
    else if (cur >= 2 && cur <= 2 && chainMean > 8.0 && m_FramesAboveChainLevelThresh >= 30) {
        target = 1;
    }

    // Upgrade conditions (同一條件適用任何 tier→tier+1)
    if (target == cur && cur < m_TierCap) {
        // 沒被 downgrade, 看能不能升
        const int64_t sinceEntered = nowMs - m_TierEnteredMs;
        if (sinceEntered >= 30000 && chainMean < 2.5 && m_FramesBelowThreshold >= 300) {
            target = cur + 1;
        }
        // True-idle bypass
        else if (chainMean < 1.0 && m_FramesBelowIdleThreshold >= 600) {
            target = m_TierCap;  // 跳到 cap
        }
    }

    // T-1 hold (5 min before retry)
    if (cur == (int)VkFrucTier::DISABLED) {
        if (nowMs - m_TierDisabledEnteredMs >= 300000) {
            target = m_TierCap;  // 重試 cap
        } else {
            target = cur;  // 留在 T-1
        }
    }

    // --- 2) Apply transition ---
    if (target != cur) {
        m_CurrentTier.store(target);
        m_TierEnteredMs = nowMs;
        if (target == 0) {  // entered T0, record for T-1 trigger
            m_T0EnterTimes[m_T0EnterIdx] = nowMs;
            m_T0EnterIdx = (m_T0EnterIdx + 1) % 3;
        }
        // Log + reset hysteresis counters
        SDL_LogInfo(..., "[VIPLE-VKFRUC-TIER] T%d → T%d (chainMean=%.2fms "
                          "chainBusy=%.2fms nvofFails=%llu cap=T%d)",
                          cur, target, chainMean, chainBusy, nvOfFails, m_TierCap);
        m_FramesAboveThreshold = 0;
        m_FramesBelowThreshold = 0;
        m_FramesAboveHandoffThresh = 0;
        m_FramesAboveChainLevelThresh = 0;
    }
}
```

**Initial cap (line ~3000 init)：**

```cpp
// §J.3.e.2.i.60 — compute initial tier cap from GPU capabilities + detected tier
int detectInitialTierCap() {
    if (!m_FrucChainAsyncAvailable) return (int)VkFrucTier::T0;  // no compute QF

    const StreamingPreferences* prefs = ...;
    const auto detectedTier = (StreamingPreferences::VKFrucGpuTier)prefs->vkfrucDetectedTier;
    const bool hasOF = (m_OpticalFlowQueueFamily != UINT32_MAX);

    switch (detectedTier) {
        case VGT_QUALITY:     return hasOF ? (int)VkFrucTier::T5 : (int)VkFrucTier::T2;
        case VGT_BALANCED:    return hasOF ? (int)VkFrucTier::T4 : (int)VkFrucTier::T2;
        case VGT_PERFORMANCE: return (int)VkFrucTier::T2;
        case VGT_ENTRY:       return (int)VkFrucTier::T1;
        case VGT_UNKNOWN:
        default:              return (int)VkFrucTier::T1;
    }
}

m_TierCap = detectInitialTierCap();
// Env var override
if (qEnvironmentVariableIsSet("VIPLE_VKFRUC_TIER_CAP")) {
    m_TierCap = qEnvironmentVariableIntValue("VIPLE_VKFRUC_TIER_CAP");
}
m_CurrentTier.store(m_TierCap);  // start at cap
m_TierEnteredMs = nowMs;
```

**Replace per-feature decisions with tierConfig() reads:**

```cpp
// 舊 (各處散落):
if (m_NvOfReady && ...) { ... }
if (m_FrucChainAsyncTempDisabled.load() == false) { ... }
const int chainLevel = m_EffectiveChainLevel.load();

// 新 (集中):
const auto cfg = tierConfig((VkFrucTier)m_CurrentTier.load());
if (cfg.useNvOf && m_NvOfFuncList) { ... }
if (cfg.useAsync) { ... }
const int chainLevel = cfg.chainLevel;
```

T-1 處理：renderFrame 開頭 `if (m_CurrentTier.load() == (int)VkFrucTier::DISABLED) { renderFrameDecodeOnly(frame); return; }`

`renderFrameDecodeOnly()` 是新 helper：pass-through decode → 單一 render pass present。沒 FRUC，沒 chain，純把 decode 結果 blit 到 swapchain。

### `moonlight-qt/app/gui/SettingsView.qml`

**刪除：**
- `vkfrucEnableNvOf` checkbox
- `vkfrucEnableTriple` checkbox
- `vkfrucEnableNativeRife` checkbox + visible-when 邏輯
- `vkfrucNativeRifeInferDim` dropdown + visible-when 邏輯
- `vkfrucRifeAutoTier` checkbox（直接視為永遠 on）

**保留：**
- `enableFrameInterpolation` (master, 重新 label 為「Frame interpolation (auto-tier)」)
- `vkfrucDetectedTier` / `vkfrucDetectedGpuName` / `vkfrucBenchmarkNs` 顯示（read-only diagnostic info）
- D3D11 用的 `frucBackend` / `frucQuality`（這些是 D3D11 path, 不在本 plan）

### `moonlight-qt/app/settings/streamingpreferences.h` / `.cpp`

**Settings key deprecation pattern**: 不刪 key 以免炸舊 user QSettings；但停止讀寫並加 deprecated 註解。新 vkfruc 完全忽略這些 keys：
- `vkfrucEnableNvOf` (deprecated, vkfruc 改讀 tier)
- `vkfrucEnableTriple` (deprecated)
- `vkfrucEnableNativeRife` (deprecated)
- `vkfrucNativeRifeInferDim` (deprecated)
- `vkfrucRifeAutoTier` (deprecated, 視為 always on)

Env vars 保留作 dev override，行為改為 **直接 set tier**：
- `VIPLE_VKFRUC_TIER_CAP=N` (新): force tier cap 0..5
- `VIPLE_VKFRUC_TIER=N` (新): force current tier (跳過 autotier)
- 舊 `VIPLE_VKFRUC_NV_OF`, `VIPLE_VKFRUC_TRIPLE`, `VIPLE_VKFRUC_NATIVE_RIFE` 改 log 一個 warning「deprecated, use VIPLE_VKFRUC_TIER」但保留行為 5 個版本後再刪。

### Reuse existing patterns

- 60-frame chain ring: `m_ChainMeanMsRing` 既有, transition 用 mean
- 60-frame chainBusy ring: `m_HandoffMsRing` 既有
- Cool-down 30s: `m_LastDowngradeTime` + `s_CooldownMs` 既有
- 300-frame upgrade hold: `s_UpgradeFrames` 既有
- True-idle bypass 600 frame: `m_FramesBelowIdleThreshold` 既有
- Hysteresis counter: `m_FramesAboveThreshold` / `m_FramesBelowThreshold` 既有
- Atomic for cross-thread tier state: 同 `m_NvOfReady` / `m_DynamicDualDowngrade` 既有 pattern

---

## Verification

### Build / smoke
1. `build_moonlight.cmd`（auto bump v1.4.126 → v1.4.127）
2. `temp\moonlight\VipleStream.exe --help` cold + hot rc=0

### GPU 路徑驗測（針對各 GPU class）

| GPU Class | Expected Cap | Expected Settled Tier (PixArk 60fps) | 驗測重點 |
|---|---|---|---|
| RTX 4070+ (QUALITY+OF) | T5 | T4-T5（chain 不太可能持續 <2.5ms 都 T5）| TRIPLE 應出現, NVOF 應 on |
| **RTX 3060 Laptop (BALANCED+OF, user 主測機)** | T4 | T3-T4（heavy scene 短暫 T3）| 重場景時 T4→T3 應觸發, NVOF on |
| RTX 1650 Mobile (PERFORMANCE) | T2 | T1-T2 | 無 NVOF, chain_lv 1-2 |
| AMD RX 6700 (BALANCED, no OF) | T2 | T2 | 無 NVOF, 不應升 T3+ |
| Intel Arc A380 (PERFORMANCE) | T2 | T1-T2 | 同 AMD |
| Old GTX 1060 (PERFORMANCE) | T2 | T1-T2 | block-match, async |
| Integrated Iris Xe (ENTRY) | T1 | T0-T1 | 重 scene 可能 T-1 |

### Regression guards

1. **舊 user QSettings 有殘留 `vkfrucEnableNvOf=true`**: vkfruc 不讀此 key, 用 tier 決定. 不會 break.
2. **Env var `VIPLE_VKFRUC_NV_OF=0`** (舊): 透過 `VIPLE_VKFRUC_TIER_CAP <= 2` 等價達成. 加 log warn deprecated.
3. **T0 卡住**: 60s 三次 T0 觸發 T-1. user 看到 5 分鐘 FRUC off 後自動 retry. 不會永久卡死.
4. **T-1 完全 pass-through**: 仍要保證 video display + decoder 正常運作 (no chain, no compute). Renderer 還是要 decode + present.

### Commit format
```
v1.4.127: §J.3.e.2.i.60 — unified Vulkan FRUC autotier (NVOF + chain_lv + ASYNC + RIFE)

User 要求: 把 Vulkan 底下 5 個 FRUC 子選項拿掉, 啟用 FRUC = 直接 autotier.
NVOF 整合進階梯, runtime 沿階梯升降, 最糟 fallback 為「暫時停用 FRUC」.

新 enum VkFrucTier { DISABLED, T0..T5 }. Tier 決定 4 個 feature axis:
NVOF on/off, chain_lv 1-3, ASYNC/SYNC, RIFE on/off (dim 128 or 256).
TRIPLE/DUAL 是 orthogonal axis (chain_mean 觸發).

初始 cap 根據 GPU 能力 + benchmark 決定:
- QUALITY+OF QF → T5; QUALITY no OF → T2
- BALANCED+OF QF → T4; BALANCED no OF → T2
- PERFORMANCE → T2
- ENTRY → T1
- 沒 compute QF → T0; 不支援 FRUC → T-1

Runtime autotier 沿階梯升降:
- T5→T4: chain_lv 3→2 trigger (chain mean > 8ms 30 frame)
- T4→T3: same
- T3→T2: NVOF 30 連 fail OR chainBusy > 14ms 30 frame
- T2→T1: chain_lv 2→1 trigger
- T1→T0: chainBusy > frameBudgetMs * 0.72 30 frame
- T0→T-1: 60s 內進 T0 三次 (結構性跑不動 FRUC)
- 升階: chain_mean < 2.5ms 300 frame + 30s cool-down
- T-1 hold: 5 分鐘後重試 cap

UI 改動: SettingsView.qml 刪掉 vkfrucEnableNvOf / vkfrucEnableTriple /
vkfrucEnableNativeRife / vkfrucNativeRifeInferDim / vkfrucRifeAutoTier.
留 enableFrameInterpolation 一個 toggle. 顯示 read-only detected tier.

QSettings keys 標 deprecated 不刪 (保留向後相容). Env vars 標 deprecated
但保留行為, 加 deprecated warning log.

新 env vars (dev 用):
- VIPLE_VKFRUC_TIER_CAP=N (force cap, 0..5)
- VIPLE_VKFRUC_TIER=N (force current tier, skip autotier)

驗證: build OK + smoke --help cold/hot rc=0.
待 user stream PixArk on RTX 3060 + 看 log [VIPLE-VKFRUC-TIER] T_X → T_Y
轉換 + heavy scene 是否 T4→T3 + 是否完全跳開 server-side IDR drop.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
```

---

## 風險評估

- **大規模重構觸碰 render path**: 多處 if/else 改成 tierConfig() lookup, 可能引入 bug. Mitigation: incremental migration, 舊 state vars 標 deprecated 但保留 ~5 版本, env var override 隨時可退回. Risk: medium
- **T-1 pass-through path 沒先 prototype**: `renderFrameDecodeOnly()` 是新 code path. 必須 fence wait + acquire + 純 blit + present. 抄 single-cmd path 開頭, 去掉 chain. Risk: medium
- **舊 user 升級 break**: QSettings keys 都 deprecated 但不刪, 舊設定不會炸. 但 user 看 UI 找不到 NVOF / TRIPLE 開關會困惑. Mitigation: release notes + UI 顯示「auto-tier handles this」tooltip. Risk: low
- **PERFORMANCE+OF GPU 沒位置**: 我的 cap 表 PERFORMANCE → T2 不管有沒有 OF. 是否該 PERFORMANCE+OF → T3? 沒人這樣配 (RTX 16xx 沒 OF, 只 RTX 20+). 沒風險. Risk: none
- **RIFE inferDim 變化**: T5 (256) vs T4 (128) 切換時 RIFE pipeline 要重 init. 切換 cost. Mitigation: 改 dim 時 invalidate descriptor set, 下幀重 build. ~10ms cost on transition, 可接受. Risk: low
- **T0 → T-1 觸發太敏感**: 60s 進 T0 三次. PixArk loading screen / 切場景可能短時間連觸發. Mitigation: 加 condition「進 T0 後須維持 5+ 秒才算一次」避免 thrash. Risk: low
- **51-drop network event 仍存在**: 跟本 plan 無關. 屬 wire-loss 範疇.
