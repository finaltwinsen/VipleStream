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

- `:1258` `notifyFrameLost: frame=%u spec=%d (+%u more since last log, frames %u..%u)`
  —— §LOG-NFL-AGG 節流：每秒最多一筆 + 區間彙總。`spec` = speculative。
- `:144` `dropFrameState #%u from L%d: strict=%d idrProc=%d wasWait=%d → idr=%d rfi=%d consec=%u`
- `:834` `§FRZ-WATCHDOG: frame number resync %u → %u`
- `:1245` `notifyFrameLost: stale frame=%u (start=%u) — ignored`（§FRZ-B2 防幀號倒帶；
  §F1-DBG-ASSERT 起這道防衛排在所有 assert 之前，Android debug native 不再先 SIGABRT。
  同時段常伴隨 `[VIPLE-CTRL]` 的三行幀號倒帶防衛，見第 5 節）
- `VideoStream.c` -101 回報點前一行（§M01-D，只記錄）：
  `§M01-D no decodable frame: firstFrameIndex=<N|none> queueFrame=%u stalePkts=%u far=%u pkts/%u frames (frames <a..b|->, maxLag=%u) enc=%d decryptFail=%u preDecryptStale=%u preDecryptFar=%u`
  —— 判讀：`stalePkts` 含正常的尾端 FEC parity（一幀收齊 data 就完成，之後到的 parity
  一定被當成過期），**不能拿來判斷殭屍**；要看 `far`（落後超過 64 幀的封包）與 `maxLag` 的
  量級。殭屍 session 的特徵是 `far` 數百、`maxLag` 約等於舊 session 已跑的幀數（數萬）；
  bufferbloat 嚴重時 `far` 也可能 > 0，但 `maxLag` 只有數百。加密串流（`enc=1`）改看
  `preDecryptFar`／`decryptFail`（落後封包在解密前就被丟掉，佇列的計數一律是 0）。

### `§FRZ-WATCHDOG` / `§FRZ-ESCALATE`（`[VIPLE-VIDEO]`）
`VideoStream.c:332` / `:383`

```
[VIPLE-VIDEO] §FRZ-WATCHDOG: packets flowing but no decode unit for %.1fs (queue frame=%u, pkts last 1s=%u) — resetting RTP queue and requesting IDR
[VIPLE-VIDEO] §FRZ-ESCALATE: %u consecutive ineffective resets, sentinel unclaimed — escalating IDR via QUIC marker
```

`pkts last 1s` 用來區分兩種凍結：**有流量組不出幀**（毒化）vs **幾乎無流量**（斷流）。
連續 fire 且 `pkts last 1s` 仍有值 ＝ reset 沒救到。**§M01-B（2026-09-23）以前**這幾乎都是
reset 本身的 bug：reset 把 RTP 序號基準清成 0，序號 ≥ 32768 的封包全被當成過期，要等
16-bit 序號繞回才解開（凍結 20 秒到數分鐘，約一半的 reset 會中）。修正後 reset 由第一個
合法封包重新對齊序號基準，會印：

```
[VIPLE-VIDEO] §M01-B: RTP seq baseline re-anchored after queue reset (seq=%u lowest=%u frame=%u)
[VIPLE-VIDEO] §M01-B: seq jumped half-window (seq=%u nextContig=%u frame=%u) — re-anchored
[VIPLE-VIDEO] §M01-B: %u consecutive in-block packets behind nextContig=%u (seq=%u frame=%u)
```

第一行是正常的 reset 後恢復；第二、三行出現代表序號仍在大幅跳動，要對照 host 的 failover
與 QUIC path 事件看。修正後若仍連續 fire 且有流量、又沒有 re-anchored 行，才代表根因不在佇列。

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
`ControlStream.c:1032`。⚠ **只在送出 >10ms 時才印，是偏誤取樣** ——
`min` 不代表健康基線，不能拿來當倍率分母。RTT 數百 ms 以上代表管線積壓
（bufferbloat）。要做真正的 RTT 基線追蹤請用
`LiGetEstimatedRttInfo()`（`ControlStream.c:2453`，讀 `peer->roundTripTime`）。

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
| `AutoUpdateChecker` / `[AutoUpdateChecker]` | `backend/autoupdatechecker.cpp` | §UPDATE-HEAD 自動更新檢查（每次啟動、5 分鐘 debounce）：`HEAD https://github.com/…/releases/latest` → `HEAD 302 → …/releases/tag/vX`（免 API 配額）→ `up to date` / `update available (head)`；HEAD 失敗才退回 REST API：`GET … (If-None-Match)` → `API 200 OK` / `API 304 Not Modified`（**匿名 304 仍計入 60 次/小時/IP 配額**）/ `API rate limited … back-off until <epoch>`；不打網路時（`debounce`／`back-off`／`error`）用上次快取判斷。發佈當天第一次啟動就該看到 `update available`；沒有就看是哪一行 |
| `[SC-HID]` | `streaming/input/sc_hid.cpp` | Steam Controller 原生 HID 轉發（見下節；host 端同 tag） |
| `[VIPLE-INPUT-STALL]` | `session.cpp`（watchdog 執行緒） | 主迴圈（鍵鼠唯一入口）停頓 ≥50 ms 且期間有使用者輸入、本視窗在前景才記；`phase=SDL_WaitEventTimeout`＝卡在 SDL 內部，`phase=event 0x…`＝我們的 handler；停頓 ≥300 ms 另抄一份主執行緒堆疊（`module!symbol+offset`）。影像由 Pacer 執行緒繪製，所以「畫面正常但鍵鼠斷續」要看這行 |
| `[VIPLE-INPUT-GAP]` / `[VIPLE-INPUT-QUEUE-LAG]` / `[VIPLE-INPUT-SENDINPUT-SLOW]` | **host** `stream.cpp` / `input.cpp` / `platform/windows/input.cpp` | 輸入封包到達間隔 >150 ms（前 1 秒 ≥30 包才算）／task_pool 排隊 ≥50 ms／`SendInput` ≥20 ms（被其他行程的 low-level hook 卡住）。四者對時可分辨停頓在 client、傳輸還是 host 注入 |
| `[VIPLE-QLOG] §S.19 cooldown armed f=N (K.14 pending\|skipped)` / `IDR-REQ` / `IDR-EMIT` / `IDR-SUPPRESS` | **host** `video.cpp` encode_run | IDR cooldown 閘門：`armed` 標示本 encoder 生命期起點（`K.14 skipped: rebuilt encoder`＝顯示拓樸切換後重建）；client 要 IDR 卻只見 SUPPRESS、無 EMIT 就是 §S.19-INIT-FIX 修掉的永久凍結型態 |
| `[VIPLE-UPDATE]` | **host** `self_update.cpp` / `system_tray.cpp` | §SELF-UPDATE 常駐圖示兩段式「Check for updates...」→「Install update vX」與 CLI `--check-update`／`--self-update [--force] [package]`：`latest via HEAD: X`（github.com 302 取 tag，免 API 配額）／`falling back to API`／`up to date`／`update available: A → B`／`downloaded … bytes`／`sha256 verified`（GitHub asset digest，拿不到就拒裝）／`updater launched (pid N) … service=1`（Windows：PowerShell 腳本在 `<install>/config/update`，停 service→rename .old→覆蓋→啟 service，失敗回滾；細節在 `config/self_update.log`）／`sudo path rc=` `pkexec path rc=`（Linux：apt-get install .deb；細節在 `~/.config/sunshine/self_update.log`）／`self-update completed: now running X` 或 `last self-update failed: …`（重啟後讀 result 檔） |
| `[VIPLE-HID]` | `backend/hidprobe.cpp` + 四個閘控點（`gamepad.cpp` getUnmappedGamepads / `sdlgamepadkeynavigation.cpp` enable / `input.cpp` SdlInputHandler ctor / `sc_hid.cpp` prepare） | 啟動前與每場開始前的 HID 探測（§HID-PROBE，2026-09-19 Puck 卡死事故）。`probe (startup\|refresh) OK: N HID interface(s) responded in X ms (slowest VID:PID Y ms, limit 1000 ms)` = 正常；`UNRESPONSIVE <name> vid= pid= stuck_at=<CreateFile\|HidD_GetAttributes\|HidD_GetProductString\|enumeration> path=` = 該裝置無回應 → 手把偵測、手把 UI 導覽、串流手把與 SC-HID 停用，各閘控點各印一行 `skipped/refused`；重插裝置後下一次 refresh（開串流／回 UI）自動恢復，不必重啟。**刻意比 SDL 保守**：對所有 HID 介面查字串，卡死的普通鍵鼠也會觸發。`(simulated via VIPLE_HID_PROBE_SIMULATE_HANG, dev-only)` = 測試注入（`VID:PID` 直接注入；`VID:PID:stall` 對實體介面真的卡在字串查詢；`enumerate:stall` 整個列舉逾時），不是真故障 |
| `[VIPLE-CTRL]` | `ControlStream.c`（common-c，Q／A 兩份相同） | 控制通道的防衛，四種行都代表防衛生效（拒送、改送 IDR 或忽略），不是崩潰。**§F2**（`sendMessageEnet` 加密分支）：`§F2 payload too large (ptype=0x%04x len=%d) — limit 251, dropped (count=N)` ＝ 明文超過 256 B stack buffer（payload 上限 251 B）被拒送、回傳 false；前 10 次都印、之後每 1000 次一筆。現有呼叫端都是固定長度（最大是 input 132 B），**1.5.x 正常 log 不該出現**；出現代表有新呼叫端（2.0 VR 送出 API）組出超長封包。**§F1-DBG-ASSERT**（幀號狀態倒帶，三行都改送 IDR）：`Invalid frame loss range (%u to %u) — requesting IDR frame instead`（`queueFrameInvalidationTuple`，每秒最多 1 筆）、`Non-monotonic RFI ranges (start %u, last end %u) — requesting IDR frame instead`（`referenceFrameControlFunc` 聚合時）、`connectionSawFrame: stale frame=%u (lastSeen=%u) — ignored`（倒退幀號不計入統計，每秒最多 1 筆）。這三行與 `[VIPLE-DEPACK] notifyFrameLost: stale` 同時出現＝§FRZ-WATCHDOG 哨兵重置後採納了 failback 殘留的 stale 幀；舊版 Android（debug native、assert 生效）會在這裡 SIGABRT 閃退 |
| `[VIPLE-MPQUIC] §M01-C …` | `ControlStream.c`（common-c，Q／A 兩份相同） | §M01-C ENet 斷線後的重連。以前 §Q-ENET-RECONNECT 只在找到有線網卡時才重連，只有 Wi-Fi 的裝置（手機、筆電）斷線後永遠不重連、120 秒後放棄輸入。現在改看「到 host 的路由是否可用」：`route to host available (local=…)` / `route to host lost` / `route to host now via local=… (was …) — backoff reset`（換了出口介面就重設退避）；`host rejected ENet reconnect (stale peer?)` 是 host 端舊 peer 還在，會繼續重試；重連後約 5 秒印 `ENet healthy rtt=…`（或 `NOT healthy`）。`ENet still down after 120 s … input via QUIC datagram (unreliable), still retrying` 代表輸入暫時走 QUIC datagram、不保證送達；`QUIC transport dead while ENet down — terminating` 是兩條都斷、結束連線 |
| `[VIPLE-MPQUIC] §Q.path-recovery: thread …` | `QuicTransport.c`（common-c，Q／A 兩份相同） | §M01-E 路徑復原執行緒的生命週期：`thread started` → `thread exiting` → `thread joined`。**每個用過 QUIC 的 session 收尾時都要有一行 `thread joined`**，包括 QUIC 閒置逾時或 path 全滅走 `§K.13 IO loop bail` 的情況。2026-09-26 以前 bail 路徑會跳過 join：debug 版在 `Platform.c` 的 `activeThreads == 0` assert 中止，release 版洩漏執行緒。有 `thread started` 卻沒有 `thread joined` 就是 regression |
| `[VIPLE-AUDIO] §M01-A …` | `RtpAudioQueue.c`、`AudioStream.c`（common-c，Q／A 兩份相同） | §M01-A 音訊佇列對網路輸入的檢查。上游用 `LC_ASSERT_VT`，debug 建置（Android `com.piinsta.debug`、Qt Debug 組態）遇到異常封包直接 SIGABRT（2026-09-23 20:06:43 Pixel 5 AudioRecv 在 block size 檢查 abort）；§M01-A 起改成與 release 相同的 runtime 處理，加上這組 log（每個呼叫點每秒最多 1 筆，行為都是「丟包或照常繼續」，不是崩潰）。根因是 QUIC 接收 ring（`quicAudioRing`／`quicVideoRing`）在 ARM64 缺記憶體屏障、讀到 slot 上一輪殘留的 len，同版已修（`Limelight-internal.h` 的 `QUIC_RING_*_FENCE`）。**release 也會印**：`corrupt audio shard length (seq=%u pt=%u len=%u) — dropped, FEC kept` ＝ 音訊有加密（AES-CBC＋PKCS7，密文必為 16 的倍數）但 shard 長度不是 16 的倍數，必定是 client 端毀損，只丟這包、不停用 FEC；`block size mismatch detail: seq=%u pt=%u len=%u blockSize=%u enc=%d block=%u (had data=%u fec=%u)` ＝ 緊接在上游 `Audio block size mismatch (got X, expected Y)` 之後、`Audio FEC has been disabled due to an incompatibility with your host's old software!` 之前，每場最多一次（之後整場停用音訊 FEC）；判讀規則：**enc=1 且 blockSize%16≠0 ⇒ client 端毀損；enc=1 且 blockSize%16==0 ⇒ host 真的送了變長封包**（GFE 3.13、舊 Sunshine）；enc=0 無法只靠長度判斷；`FEC header mismatch in block %u (seq=%u pt=%u len=%u): pt a/b ts a/b ssrc a/b` ＝ 同一 FEC block 的封包 header 不一致，以最先到的封包為準照常收。**只在 debug**：`FEC block %u (ts size pt ssrc) inconsistent with block %u (…)` ＝ 佇列內各 block 的 timestamp／長度／pt／ssrc 不一致；`FEC validation mismatch: block %u shard %u, N byte errors (first at K), total failures M — keeping received packet` ＝ FEC_VALIDATION_MODE 合成丟包後的重建結果與實收封包不同（host parity 與 data 對不上、實收封包毀損，或我們的 RS 重建有 bug），改交付實收封包；`first Opus TOC byte is 0x00 (seq=%u), baseline deferred`／`unexpected Opus TOC byte 0x%02x (expected 0x%02x, seq=%u)` ＝ Opus TOC 一致性檢查（host 是 Sunshine 時只檢查第一包）。上游不帶 tag 的 `Failed to decrypt audio packet (sequence number: %u)` 在 debug 版也不再 abort。**一般串流、failover、outage 都不該出現任何 §M01-A 行**；測試時 grep `[VIPLE-AUDIO]`，連同 `Audio block size mismatch`、`Audio FEC has been disabled`、`Failed to decrypt audio packet` 都應為 0 |
| `[VIPLE-CTRL-TX]` | **host** `stream.cpp`（`control_server_t::send`） | §F4 server 控制訊息可指定 channel／flags：`§F4: channel N >= peer channelCount M, falling back to channel 0` ＝ 要求的 ENet channel 超過該 client 協商的 channel 數（舊 client），改走 channel 0、flags 不變；整個程序只記前 5 次。1.5.x 沒有呼叫端送非 0 channel（M1a 的 VR 訊息才會），**目前不該出現** |
| `[VIPLE-MPQUIC] §F3 …` | **host** `stream.cpp` | §F3 QUIC recv handler 提早註冊（只在開 mpquic 的 session）：`§F3 recv handler registered at session start (via=session-start\|quic-ready, peer=…)` 每條 session 一次，一般順序（RTSP 握手後才連 QUIC）是 `via=quic-ready`；同一條 session 因 QUIC 重連換了 QuicSession 時印 `§F3 recv handler re-registered on new QUIC session (via=…)`。`§Q-IDR-VIA-QUIC: recv handler registered on QUIC session via video loop (§F3 safety net — early registration missed, peer=…)` ＝ 兩個提早註冊點都漏接、由 video 迴圈補上，**不該出現**，出現就是 F3 有漏洞。開 mpquic 的 session 若三行都沒有，client 在 QUIC fallback 期間送的 IDR／FEC／input 會被丟掉 |
| `[VIPLE-MULTI] §M01-D …` / `[VIPLE-MPQUIC] §M01-D …` | **host** `rtsp.cpp`（`clear_for_client`、`session_raise`）/ `stream.cpp`（`recv_ping`）/ `quic_server.cpp`（`retireSession`） | §M01-D 同一個 client（同一張 TLS 憑證）被強制關閉後立刻重開：/resume、/launch 先收掉它自己殘留的 stream session（2026-09-23 事故：殭屍 session 的 P-frame 經同 IP 的新 QUIC 連線灌給新 client → `Network dropped 69411 frames` → -101）。`superseded N stale session(s) of uuid=… before /resume\|/launch (active before=M)` ＝ 收掉 N 條；單一 client 時之後應依序出現 `Listener stopped` → `Listener started` → `New streaming session started [active sessions: 1]` → `Session stored` → `§F3 … via=quic-ready`，**不該**再有 `active sessions: 2`、`§F3 safety net`、重連後的 `ping timeout suppressed`。`stale session of uuid=… has not started yet — marked superseded, not joined` ＝ 殘留的那條還在 `session::start` 中途，只標記、交給 ping timeout（極少見）。`replaced stale pending launch of same client (old id=… new id=…)` ＝ client 在 /resume 之後、ENet 連上之前被殺又重開，pending launch 換成新的（否則 RTSP 會拿到舊 rikey／ping payload）。`[VIPLE-MPQUIC] §M01-D retired QUIC connection of superseded session (peer=…)` ＝ 被取代的 session 擁有的 QUIC 連線從 listener map 移除（cnx 交給 picoquic 30 s idle 回收）。`recv_ping aborted — session stopping before first video\|audio ping` ＝ 被收掉的 session 還在等第一個 UDP ping，立刻放棄（否則 /resume 會被拖到 ping_timeout）。一般串流、failover、outage 都**不該出現任何 §M01-D 行**。B 線（縱深防禦：`claimed by another session — not extending grace`／`dropping stale-session frames\|audio`／`QUIC owned by older session — falling back`）尚未合入，目前版本不會出現 |
| `[VIPLE-DEVENV]` | `wm.cpp`（`Utils::logDevEnvOverride`）／**host** `config.cpp` | 環境變數覆寫了行為（環境變數只准當 dev-only 偵錯開關）。client：`dev-only override NAME=value`（SDL warn，每個名稱每個行程一次），涵蓋 `PREFER_VULKAN`、`GL_IS_SLOW`、`VULKAN_IS_SLOW`、`MATCH_DISPLAY_MODE_TO_VIDEO`、`SEPARATE_TEST_DECODER`、`FORCE_QT_GLES`、`VIPLE_USE_VK_DECODER`、`VIPLE_VK_FRUC_GENERIC`、`VIPLE_VKFRUC_*`、`*_AVOPTIONS`、`*_DECODER_HINT`。host（sunshine.log，啟動時一次，§F7）：`dev-only override VIPLE_SMOOTH_PACING=<v> （…smooth_pacing=<cfg> 被覆寫，生效值=<eff>…）`，優先序 env > `sunshine.conf` 的 `smooth_pacing` > 預設 false。**分析使用者回報的 log 前先 grep 這個 tag**：有這行就代表行為被環境變數改過，要先排除 |
| `[VIPLE-LNXFE]` | （僅 Linux）`settings/streamingpreferences.cpp`、`wm.cpp`、`ffmpeg.cpp`、`session.cpp` | §F6 Linux renderer 決策。`linuxVideoFrontend=auto\|vulkan\|egl -> frontend=PlVk-first\|legacy-order (reason=aarch64\|zink\|default\|user isGpuSlow=N)`（設定或結果改變時才印）；`DRM driver probe: [<driver>] (source=libdrm\|sysfs)`（決定 isGpuSlow；msm 判為不慢）；`EGL probe: vendor='…' driver='…' zink=N`（x86 AUTO 第一次選 renderer 時探一次 Zink）；G-α 驗收摘要 `decoder=%s fmt=%s frontend=%s backend=%s isGpuSlow=%d`、`windowMode=%d fullscreenFlag=FULLSCREEN\|FULLSCREEN_DESKTOP\|NONE isGpuSlow=%d`、`matchVideo=%d (isGpuSlow=%d videoDriver=%s)`。Steam Frame 預期：`DRM driver probe: [msm]`、`reason=aarch64`、`fmt=DRM_PRIME frontend=PlVk isGpuSlow=0`、`fullscreenFlag=FULLSCREEN_DESKTOP`、`matchVideo=0`。Windows 不印 |
| `[VIPLE-UPDATE]`（client updater） | `backend/updater.cpp`（決策在 `backend/updateassetrules.h`） | §F9 更新對話框的「立即更新」路徑（版本檢查本身是上面的 `AutoUpdateChecker`）。啟動時 `platform <os>/<arch> (cpu …, build …, appimage\|flatpak\|plain[, emulated]) install mode = auto-install\|notify-only\|unsupported`，模擬執行另印 `running under emulation — cpu X build Y; assets follow the build architecture`；只通知的平台按下更新印 `startUpdate refused — install mode …`。抓到 release 後逐筆 `skip asset <檔名> — <原因>`，接著 `matched asset <檔名> action = auto-install\|notify-only release tag vX` 或 `no asset for <platform> — plan not-newer\|no-assets\|no-match\|missing-tag\|bad-tag release tag … current … among …`；`fetched release vX differs from the version offered in the UI Y` ＝ UI 顯示的版號已過時，改裝抓到的 tag；之後 `selected asset … → <url>`、`install dir …`，失敗統一 `failed: …`。判讀：`plan not-newer` ＝ GitHub API 快取落後 HEAD 302，幾分鐘後再試；`no-match` ＝ 這個 release 沒有本架構的檔或檔名版號對不上 tag |
| `[VIPLE-COMMONC-SYNC]` | **build** `moonlight-qt/moonlight-common-c/check_commonc_sync.ps1`（本機 `build_moonlight.cmd`／`build_sunshine.cmd`／`build_android.cmd` 第一步呼叫） | §F1 三份 common-c 同步檢查（規則見 `docs/vr_protocol.md` §4.8），**是 build log，不是 runtime log**。`[ERROR] (1) Q/A …`（Qt 與 Android 兩份 `src/`、`enet/` 必須一致）／`[ERROR] (2) Q/S3 …`（server 實際編譯的指定檔必須一致）→ build 以 `[ERROR] moonlight-common-c sync check failed (rc=N)` 停下，發生在版號步驟之前，不會吃掉版號；`[WARN]` 只提示（未 stage 的刪除、Q/S3 其餘漂移）；結尾一行 `PASS：…` 或 `FAIL：…` |

### `[SC-HID]` —— Steam Controller 原生 HID 轉發（§SC-HID Round 1，2026-09-02 起）
client：`streaming/input/sc_hid.cpp`；host 端 `Sunshine/src/input.cpp` 與
`sc_hid_driver/VipleSCHid.cpp` 用同一個 tag（sunshine.log）。Round 1 起 client 的所有
`[SC-HID]` 行都走 `SDL_LOG_CATEGORY_APPLICATION`（舊碼把「非 0x45 丟棄」印在
INPUT 類別，app 從未設該類別等級，所以**永遠不會出現在 log**——別拿舊 log 推論
「沒有丟棄」）。下表格式以 Round 1 實際碼為準；欄位名是判讀依據。

**client 行**

| 行 | 何時印 | 判讀 |
|---|---|---|
| `Opened N Steam Controller vendor interface(s) (prepared; SDL ignore list = 0x28DE/0x1302,…)` ＋ 每介面 `dev=%d pid=0x%04x …path=…` | `prepare()`（`Session::start()`，建 SdlInputHandler 之前；§SC-THREAD-OWNER 2026-09-19） | Puck（PID 0x1304）開 4 個 slot；USB 直連（0x1302）1 個 |
| `passthrough prepared - SDL gamepad path disabled for 0x28DE/0x1302,0x28DE/0x1303,0x28DE/0x1304,0x28DE/0x1305 this session` | `SdlInputHandler` 建構（`input.cpp`），prepare 有開到介面時 | SC 家族已進 SDL 忽略清單（SDL 3.4.2 在 `SDL_hid_open_path` 之前就擋）；**沒這行**＝這場 SC 當一般 SDL 手把（沒插、或 Linux 沒 hidraw 權限） |
| `Steam Controller passthrough started (normalize42=1, gen=N, all hidapi calls on the SC-HID thread)` | `start()`（LiStartConnection 之後） | `gen` 每場 +1；被遺棄的舊執行緒看到世代不符會自行退出 |
| `Steam Controller passthrough stopped (read thread joined in N ms)` | `stop()` 正常路徑 | 健康裝置 ≤ ~150 ms（feature 輪詢上限 100 ms） |
| `[VIPLE-HID] [SC-HID] stop(): read thread did not exit within 1000 ms - stuck in <呼叫> for N ms …; thread + N HID handle(s) abandoned, not closed` | `stop()` 逾時 | 裝置**串流中**卡死（同 §HID-PROBE 的 Puck 事故）；執行緒與 handle 遺棄、session 照常收尾；重插後下一場 `HidProbe::refresh()` 重測。行程結束可能殘留到重插（troubleshooting.md） |
| `feature queue full (8) - dropped oldest request (dropped=N); is the read thread stuck?` | host 的 feature 請求排不進佇列 | 讀取執行緒沒在消化（通常緊接著上面的 stop() 逾時）；`qdrop=` 累計在 rx stats |
| `First report id=0x%02x on dev=%d (%d bytes): <全部 bytes hex>` | 每個 report id 首見（每場重置） | `0x42`＝controller state（Puck／USB 都是它）、`0x45`＝BLE-style state、`0x43`＝電量、`0x7B`＝遙測、`0x46/0x79`＝無線狀態、`0x47`＝帶時戳 state（佈局不同，本輪不轉發） |
| `rx stats(5s\|heartbeat\|final): total= fwd= norm42= drop= sendErr= \| id42= id45= id43= id7B= id47= other= \| feat req= ok= stolen= empty= cache= qdrop= lat avg/max=/ ms \| active= \| dev rx/fwd: dev0=rx/fwd …` | 每 5 s 有變才印（`5s`）；沒變則定期 `heartbeat`；`stop()` 印 `final` | **G1／G2 依據**：`id42 ≥ 500`／場、`fwd ≈ id42`；`norm42`＝0x42 改標成 0x45 轉發的筆數（`kNormalize42`）；`sendErr`＝`LiSendScHidInputReport` 非 0；`active`＝最近吐 state 的 slot；`final` 與 host `Session ended, final write stats` 對帳 |
| `No input report from any of %d dev(s) in %u ms — controller asleep (press the Steam button) or not paired to an opened slot` | 啟動後仍 `total=0`，一次 | 按 Steam 鍵喚醒 |
| `Feature req[ (warm-up)] id=0x%02x op=SET\|GET seq=%u type=0x%02x → resp type=0x%02x n=%d lat=%u ms dev=%d src=… stolen=%u cand=%d sent=%d` | 每次 host 轉來的 feature 請求（與 `start()` 暖機） | **G4 依據**：`lat` 應落在 13～21 ms；`resp type` 必須等於 `type`；`stolen`＝撿到 type 不符的回應（本機 Steam 搶走／未請求的 `0x87` ack）；`src`＝回應來源（live／client 端快取／empty）；`n=0`＝回零 |
| `Proactive cache prime sent (dev=%d, GET_ATTRIBUTES): …` ／ `Warm-up GET_ATTRIBUTES got no response on %d dev(s) — …` | 暖機（`start()` 排入佇列、讀取執行緒第一輪代跑） | 全失敗＝控制器睡眠或本機 Steam 獨占；握手仍會即時代理 |

**host 行（sunshine.log，同 tag）**

| 行 | 判讀 |
|---|---|
| `Polling thread started, handle open` ／ `Virtual Steam Controller device not found` | **G0**：driver 綁上與否（`Sunshine\src\platform\windows\sc_hid_driver\diag\sc_hid_host_check.ps1` 的 `bound`／`driverVersion` 同義） |
| `Polling thread: device NOT open (retry with backoff): <探測軌跡>`（刻意不含 `Polling thread started`，G0 可用兩個互斥字串直接判讀） |
| *(Android client，logcat tag `LimeLog`)* `[SC-HID] Steam Controller passthrough started (N HID interface(s), M input endpoint(s), normalize42=1)`／`First report id=0x.. on endpoint= iface= (n bytes): <hex>`／`rx stats(5s|heartbeat|final): total= fwd= norm42= drop= sendErr= \| id42= id45= id43= id7B= id47= \| feat req= ok= stolen= empty= cache= lat avg/max= \| active= \| ep rx/fwd: …`／`Feature req[ (warm-up)] id=0x01 op=SET|GET seq= type=0x.. -> resp type=0x.. lat= ms iface= src=live|cache stolen= cand= sent= poll= ms`／`No input report from any of N endpoint(s) in 3 s`／`Bluetooth Steam Controller detected as InputDevice ... Android blocks raw HID over GATT` | 與桌面 client 同語義（USB-C／Puck OTG 才會出現前四種；藍牙只會出現最後一行＋toast） | warning；poll thread 起來了但開不到虛擬裝置（每個失敗 streak 首次印，之後退避靜默）。舊碼這種情況零 log（F10）。看軌跡分「沒列舉到」（driver 沒綁）vs「開啟失敗 err=」（權限／獨占） |
| `First report injected (id=0x…)` | 本場第一筆**真實**（非 keepalive）report 進虛擬裝置；只在寫入真的到裝置（`wr==1`）時印 |
| `Keepalive active (4Hz idle replay of last real report, id=0x…; suppressed while real input flows)` | 本場首次進入 keepalive（500 ms 無真實注入 → 4 Hz 重放最後一筆真實 report 的原 id；尚無真實資料時重放零值 0x45）。整場只有這行、沒有 `First report injected`＝client 從未轉發任何 state（G2 失敗） |
| `Skipped undeclared report id=0x%02x` | 節流 warning；descriptor 沒宣告的 id（`VipleSCHidWrite` 回 2） |
| `write stats(10s): session okS= skippedS= failedS= featureEvtsS= responsesS= keepalive= \| process ok= skipped=(lastId=) failed= lastErr= featureEvts= responses= pollErrs= \| drvSet= drvGet= drvGated= drvGetEvt= drvDelivered= drvZeroDrop= drvPending= drvReady= drvLastId=0x.. drvRing= drvVer=1.5`（driver < 1.0.5.0 時最後一段為 `drv=n/a(err=N)`） | 每 10 s 有變才印。**G3**：用 **per-session 差值欄位** `okS − keepalive`＝真實注入數（`ok`／`skipped`／`failed` 是 process 級累計、跨 session 不歸零）、`skipped=0`、`failed=0`；**G4**：`drvSet>0`＝host Steam 的 SET 真的進了 driver ring（來自 driver **Feature 0x05 統計** report；0x03 自 driver 1.0.5.0 起是純事件、不再帶統計尾段；driver < 1.0.5.0 不宣告 0x05 → 恆 0） |
| `Session ended, final write stats: …` | session 解構時的同格式總結，與 client `rx stats(final)` 對帳 |
| `Steam feature op → client: op=…`（input 路徑 piggyback）／ `Poll-thread: Steam feature → client: op= reportId=0x… seq=`（20 ms poll thread） | host Steam 的 SET／GET 事件從 driver ring 轉發給 client；兩條路徑二選一出現，grep 用 `Steam feature (op )?→ client`。要與 `Feature response delivered: seq=…` 成對；只有前者＝client 沒回或回零（driver 會把全零回應忽略、`zeroDropped++`） |
| `Feature response delivered: seq=…` | client 的實體 SC 回應已 WriteFile(0x04) 進 driver；G4 成對條件的後半 |
| `Write fatal, reopening: …` | 寫入遇到 fatal 錯誤碼（含 433／55）→ 重開 handle |

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
