# VipleStream Client Log Tag 字典

Client log 位置：`%TEMP%\VipleStream-*.log`（檔名的數字是啟動時的 Unix epoch）。

**重要：log 行首的 `HH:MM:SS` 是「程式啟動後經過的時間」，不是牆鐘時間。**
超過 24 小時的 session 會繞回 `00:00:00`，跨場比對時要注意。

一份 log 可能含**多場串流**（使用者退出再重連）。跨場分析請先用
`[VIPLE-SESSION]` 切段，否則場與場之間的閒置會被誤算成串流中斷。

分析工具：`scripts\benchmark\analyze_client_log.ps1`（見本文件最後一節）。

---

## 1. 場次識別

### `[VIPLE-SESSION]`
`session.cpp:2507`

```
[VIPLE-SESSION] host=%s app=%d fps=%d bitrate=%d codecs=0x%x
```

每場串流開始時印一次。設計目的是讓每秒遙測行（不帶 host/codec）能在事後
歸戶。**v1.5.268 才加入**，更早的 log 要改用 `Video stream is WxHxFPS` 當起點。

| 欄位 | 意義 |
|---|---|
| `host` | 連線目標位址 |
| `app` | 應用程式 ID |
| `fps` | 向 server 要求的 fps |
| `bitrate` | 設定碼率（kbps） |
| `codecs` | 視訊格式 bitmask（`0x1000`=AV1、`0x200`=HEVC 等） |

### `[VIPLE-RES]` / `[VIPLE-NAT]` / `[VIPLE-FRUC]`（啟動階段）
這三個都在**串流真正開始之前**印，切段時要往前回看才抓得到：

- `[VIPLE-RES] Stream config requested: WxH; host advertises max: WxH`（`session.cpp:807`）
- `[VIPLE-NAT] Skipping hole punch: direct private address ...` ＝ LAN 直連；
  `Attempting hole punch to ...` ＝ 遠端（`session.cpp:2304` 一帶）
- `[VIPLE-FRUC] FRUC disabled — requesting N FPS from server (pass-through)`
  → **這行才是 FRUC 開關的真相**。v1.5.267（含）以前 `[VIPLE-RATIO]` 在 FRUC
  全關時會誤印 `ACTIVE/2x`（§LOGHYG-A3 於 v1.5.268 修正），不可採信。

### 非 tag 但關鍵的啟動行
- `Interface MTU: N` —— `nvcomputer.cpp:563`。**只在 NIC 簽章變動時印一次**
  （`logOnceOnChange`），所以同一份 log 的第 2 場之後看不到，要往前找。
  ⚠ `nvcomputer.cpp:572`：**MTU < 1500 會被判定成 VPN**，連帶讓
  `session.cpp:2259` 把 `packetSize` 壓到 1024 並標記 `STREAM_CFG_REMOTE`。
- `V-sync %s`（`session.cpp:358`）—— 每次 `chooseDecoder` 都印，含 testOnly 探測，
  所以一場會出現多次。
- `Frame pacing disabled: target %d Hz with %d FPS stream`（`pacer.cpp:314`）
  或 `Frame pacing: target ...`（`pacer.cpp:272`）—— **每場只印一次**
  （Pacer 只在非 testOnly 建立），是判斷節拍有沒有生效的唯一依據。

---

## 2. 網路與解碼統計

### `[VIPLE-NET]` —— 每秒統計（**v1.5.268 起只在異常秒才印**）
`ffmpeg.cpp:3007`（`logNetSecondLine`）

```
[VIPLE-NET] %sreceived=%u decoded=%u networkDropped=%u total=%u decodeMeanMs=%.2f hostLatencyAvgMs=%s%s
```

| 欄位 | 意義 |
|---|---|
| 前綴 `%s` | 空 = 異常秒本身；`(pre) ` = 進入異常時回補的前 3 秒；`(tail) ` = 收尾時沖出的殘餘 |
| `received` | 該秒從網路收到的幀數 |
| `decoded` | 該秒成功解碼的幀數 |
| `networkDropped` | **幀號 gap 計數，不是封包遺失**（`du->frameNumber` 的跳號） |
| `total` | received + dropped |
| `decodeMeanMs` | 該秒平均解碼耗時 |
| `hostLatencyAvgMs` | 主機端處理延遲；無回報印 `n/a`（用 -1 哨兵與真 0.00 區分） |
| 後綴 `%s` | ` warmup` = 前 2 個視窗，含 init 佇列假值，**不進 ring、不計彙總** |

**異常判定**（`ffmpeg.cpp:3086`）：`received < 0.8×fps` 或 `networkDropped > 0`
或 `decodeMeanMs > 8.0` 或 `hostLat > 3×近期中位數`。

> ⚠ **這個 tick 是由 `submitDecodeUnit()` 驅動的**（`ffmpeg.cpp:3041`），不是計時器。
> 真正凍結（沒有幀抵達）時**完全不會印任何統計行** —— 這正是偵測停擺的依據。

### `[VIPLE-NET10]` —— 10 秒彙總（正常時的主要輸出）
`ffmpeg.cpp:3163`

```
[VIPLE-NET10] secs=%d received=%u decoded=%u networkDropped=%u decodeMeanMs p50=%.2f max=%.2f hostLatencyAvgMs p50=%.2f max=%.2f recvPct min=%.0f avg=%.0f fmt=0x%x
```

| 欄位 | 意義 |
|---|---|
| `secs` | 本批涵蓋的統計秒數（穩態恆為 10） |
| `received`/`decoded`/`networkDropped` | 10 秒**總和**（不是平均） |
| `decodeMeanMs p50/max` | 10 個秒級樣本的中位數與最大值 |
| `hostLatencyAvgMs p50/max` | 同上；全無回報則整段印 `n/a` |
| `recvPct min/avg` | 每秒 `received / fps × 100`。**`min` 是最有價值的欄位** —— 平均漂亮但 min 掉到 70 幾就代表有短促的收幀凹陷 |
| `fmt` | 視訊格式 bitmask |

**停擺重建法**：NET10 每 10 個「視窗翻轉」印一次，而翻轉由幀抵達驅動，
所以健康串流兩行相距 ~10 秒。超出的部分就是停擺：
`stall = (t_n − t_{n−1}) − secs_n`（`failover_flap_test.ps1` 用的就是這個）。

### `[VIPLE-RATIO10]` / `[VIPLE-RATIO]` —— FRUC ratio
`ffmpeg.cpp:3181` / `:3297`

```
[VIPLE-RATIO10] mode=OFF|PASSIVE|ACTIVE ratio=%dx server_fps=%d recvPct min=%.0f avg=%.0f
[VIPLE-RATIO] PASSIVE switch %dx → %dx (recv_of_display=..., below40s=.., below70s=.., above95s=.., display_Hz=.., ...)
```

⚠ `[VIPLE-RATIO] PASSIVE switch` 的格式字串寫 `above95s`，實際印的是
`m_RecvAbove80Seconds`（`ffmpeg.cpp:3305`）—— 命名不一致，別被誤導。

### `[VIPLE-RATIO-WARN]` —— ACTIVE 低收警告
`ffmpeg.cpp:3374`。60 秒滑動視窗內 `recv<80%` 達 42/60 秒才觸發，冷卻 300 秒。

- `cause=SERVER_LIMITED` —— 丟包 < 0.5%，代表 **server 根本沒產那麼多幀**
  （遊戲效能不足或靜態畫面省流），切被動補幀救不了。
- `cause=NETWORK` —— 真的是網路收幀不足。

### `[VIPLE-DEC-DEPTH]` —— 解碼器背壓
`ffmpeg.cpp:2916`（事件）／`:2925`（heartbeat）

```
[VIPLE-DEC-DEPTH] queueDepth=%d frameLatencyUs=%lld latRingMaxMs=%.2f [heartbeat]
```

- `queueDepth` > 0 代表**背壓**（送進解碼器但還沒取回）。HEVC 期望 ~1，
  AV1 期望 ~12 屬 pipeline-deep，不算異常。
- **帶 `heartbeat` 字尾 = 一切正常**，只是 60 秒一次的存活確認。
  沒有 heartbeat 字尾的才是真事件（§LOGHYG-A4 改成事件驅動，同況每秒最多 1 行）。

---

## 3. 呈現節奏（判斷「順不順」的主要依據）

### `[VIPLE-PRESENT-Stats]`
`d3d11va.cpp:1482`，每 **5 秒**一批。

```
[VIPLE-PRESENT-Stats] real|interp n=%zu fps=%.2f ft_mean=%.3fms p50=%.3f p95=%.3f p99=%.3f p99.9=%.3f call_mean=%.3fms p95=%.3f
[VIPLE-PRESENT-Stats] cumul real=%u [interp=%u]
```

| 欄位 | 意義 |
|---|---|
| label | `real` = 真實幀；`interp` = FRUC 內插幀（§LOGHYG-A6：整場沒有 interp 時會抑制該行） |
| `n` | 該 5 秒內的 Present 次數 |
| `fps` | `1000 / ft_mean`，**有效呈現速率** |
| `p50/p95/p99/p99.9` | **inter-Present 間隔**（幀間隔）的分位數 —— 卡頓看這裡 |
| `call_mean` / 末尾 `p95` | `Present()` 呼叫本身阻塞的時間。⚠ 末尾那個 `p95` 是 **callLat 的 p95**，與前面 interval 的 p95 同名不同義 |

**判讀**：p99 幀間隔 > 33.3ms ＝ 60Hz 下連掉兩幀，是常用的「看得出來卡」門檻。
Vulkan 對應版是 `[VIPLE-VKFRUC-Stats]`（`vkfruc.cpp:15958`，格式刻意鏡像）。

---

## 4. 遺失、FEC 與凍結

### `[VIPLE-DEPACK]`
`VideoDepacketizer.c`

- `:1251` `notifyFrameLost: frame=%u spec=%d (+%u more since last log, frames %u..%u)`
  —— §LOG-NFL-AGG 節流：每秒最多一筆 + 區間彙總。`spec` = speculative。
- `:144` `dropFrameState #%u from L%d: strict=%d idrProc=%d wasWait=%d → idr=%d rfi=%d consec=%u`
- `:834` `§FRZ-WATCHDOG: frame number resync %u → %u`
- `:1238` `notifyFrameLost: stale frame=%u — ignored`（§FRZ-B2 防幀號倒帶）

### `§FRZ-WATCHDOG` / `§FRZ-ESCALATE`（`[VIPLE-VIDEO]`）
`VideoStream.c:332` / `:383`

```
[VIPLE-VIDEO] §FRZ-WATCHDOG: packets flowing but no decode unit for %.1fs (queue frame=%u, pkts last 1s=%u) — resetting RTP queue and requesting IDR
[VIPLE-VIDEO] §FRZ-ESCALATE: %u consecutive ineffective resets, sentinel unclaimed — escalating IDR via QUIC marker
```

`pkts last 1s` 用來區分兩種凍結：**有流量組不出幀**（毒化）vs **幾乎無流量**（斷流）。
連續 fire 且 `pkts last 1s` 仍有值 ＝ reset 沒救到，通常代表根因不在佇列。

### 非 tag 的 FEC 證據行
```
Unrecoverable frame %d: %d+%d=%d received < %d needed
```
`X+Y=Z`：X = 收到的 data shard，**Y = 收到的 parity shard**，Z = 合計，
需要 W 個才能重建。`Y=0`（parity0）大量出現代表 parity 整批遺失。
`Z/W` 就是 shard 到達率 —— 這是判斷 FEC 夠不夠力的直接指標。

> QUIC 層另有獨立的 FEC decoder，目前是**編譯期固定 RS 4+2**
> （`QuicTransport.c:382-384`），不隨丟包自適應。

### 控制通道 RTT（非 tag，但很重要）
```
Control message took over 10 ms to send (net latency: %u ms | packet loss: %f%%)
```
`ControlStream.c:966`。⚠ **只在送出 >10ms 時才印，是偏誤取樣** ——
`min` 不代表健康基線，不能拿來當倍率分母。RTT 數百 ms 以上代表管線積壓
（bufferbloat）。要做真正的 RTT 基線追蹤請用
`LiGetEstimatedRttInfo()`（`ControlStream.c:2367`，讀 `peer->roundTripTime`）。

---

## 5. 其他

| Tag | 位置 | 用途 |
|---|---|---|
| `[VIPLE-DIAG]` | `session.cpp` / `d3d11va.cpp` | 解碼器探測與 init 里程碑；`handleKeyEvent` 那組是 IME 調查遺留，量很大 |
| `[VIPLE-XFER]` | `filetransferclient.cpp` | 檔案傳輸（poll / download / upload） |
| `[VIPLE-OVERLAY]` | `overlaymanager.cpp:344` | CJK 字型載入 |
| `[VIPLE-INPUT]` | `input.cpp:421` / `mouse.cpp:311` | 滑鼠擷取狀態、指標區域鎖定 |
| `[VIPLE-CACHE]` | `session.cpp:602` | 解碼器探測快取命中 |
| `[VIPLE-VK-VIDEO]` | `vulkanvideo.cpp` | Vulkan Video 解碼路徑 |
| `[VIPLE-PREFS]` | 多處 | 設定載入／存檔 |

---

## 6. 分析工具

```powershell
# 最新一份 log
pwsh scripts\benchmark\analyze_client_log.ps1

# 指定檔案或整個目錄，並輸出 JSON 供回歸比對
pwsh scripts\benchmark\analyze_client_log.ps1 -Path C:\Users\me\Downloads -Json out.json
```

輸出的兩個節奏指標要一起看：

| 指標 | 定義 | 判讀 |
|---|---|---|
| `judder(對設定)` | p99 幀間隔 > 2 × (1000 / (設定fps × FRUC ratio)) | 高 ＝ 沒達到設定的速率 |
| `stutter(絕對)` | p99 幀間隔 > 33.3ms（60Hz 掉兩幀） | 高 ＝ **使用者看得出來在頓**，跨設定可比 |

- `judder` 高但 `stutter` 低 ＝ 穩定，只是達不到設定 fps（server 餵不飽／路徑上限）。
- 兩者都高 ＝ 節拍真的不穩。

**不要**用「相對本場實測中位數」當卡頓門檻：場次愈爛中位數愈低、門檻反而愈鬆，
會把最嚴重的場次算成最順（實測踩過這個坑）。
