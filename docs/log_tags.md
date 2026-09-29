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

**Linux PlVk**（`plvk.cpp` `recordPresentStats`，§SF-PRESENT-STATS，2026-09-28 起）印同樣格式、只有 `real`：間隔取相鄰兩次
`pl_swapchain_submit_frame` 成功的時間差，`call_*` 是 submit 本身花的時間。Steam Frame 的 stutter 指標來自這裡；EGL／SDL
前端沒有這一行。

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

### `[VIPLE-FREEZE]` —— 解碼／render 凍結診斷（僅非 Windows，§SF-FREEZE，2026-09-28 起）
`ffmpeg.cpp`（watchdog 執行緒 `FreezeWD`）＋`pacer.cpp`（render 相位）

```
[VIPLE-FREEZE] no decoded frame for %llu ms: dec=<相位> %llu ms in=%d out=%d liveFrames=%d | render=<相位> %llu ms renderQueue=%d
[VIPLE-FREEZE] recovered after %llu ms (liveFrames=%d)
[VIPLE-FREEZE] beat liveFrames=%d max=%d in=%d out=%d
```

- 超過 1.5 s 沒有解出新幀就印第一行，之後每 5 s 重印；恢復時印 `recovered`；平常每 10 s 一行 `beat`。
- `dec` 相位：`waitInput`（等 host 送幀）、`submitPacket`（`avcodec_send_packet`）、`receiveFrame`（`avcodec_receive_frame`）、
  `toPacer`；`render` 相位：`waitToRender`（renderer 的 `waitToRender()`，PlVk 是 swapchain）、`waitQueue`（等幀）、`renderFrame`。
- `liveFrames`：目前還活著的解碼幀（標記串在 `opaque_ref` 上，libplacebo 的 clone、renderer 扣著的都算）。**判讀**：凍結時
  `liveFrames` 接近 v4l2m2m capture buffer 總數（10）＝ app 扣住或漏掉 buffer；很低（1～2）而 `dec=submitPacket`／`receiveFrame`
  長時間不動 ＝ 硬體解碼器自己停了（Steam Frame 2026-09-28 的參數集改變事故就是這個樣子，見 `steam_frame_client.md` §7.5）。
- 純診斷：不改變解碼或渲染行為。

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
| `[VIPLE-INPUT-STALL]` | `session.cpp`（watchdog 執行緒） | **非 Windows 會誤報**：只有 Windows 以 `GetLastInputInfo` 確認真的有輸入，其他平台一律當作有輸入，主迴圈閒置（例如 Linux PlVk 在 Pacer 執行緒繪製）就會被記成停頓（TODO）。主迴圈（鍵鼠唯一入口）停頓 ≥50 ms 且期間有使用者輸入、本視窗在前景才記；`phase=SDL_WaitEventTimeout`＝卡在 SDL 內部，`phase=event 0x…`＝我們的 handler；停頓 ≥300 ms 另抄一份主執行緒堆疊（`module!symbol+offset`）。影像由 Pacer 執行緒繪製，所以「畫面正常但鍵鼠斷續」要看這行 |
| `[VIPLE-INPUT-GAP]` / `[VIPLE-INPUT-QUEUE-LAG]` / `[VIPLE-INPUT-SENDINPUT-SLOW]` | **host** `stream.cpp` / `input.cpp` / `platform/windows/input.cpp` | 輸入封包到達間隔 >150 ms（前 1 秒 ≥30 包才算）／task_pool 排隊 ≥50 ms／`SendInput` ≥20 ms（被其他行程的 low-level hook 卡住）。四者對時可分辨停頓在 client、傳輸還是 host 注入 |
| `[VIPLE-QLOG] §S.19 cooldown armed f=N (K.14 pending\|skipped)` / `IDR-REQ` / `IDR-EMIT` / `IDR-SUPPRESS` | **host** `video.cpp` encode_run | IDR cooldown 閘門：`armed` 標示本 encoder 生命期起點（`K.14 skipped: rebuilt encoder`＝顯示拓樸切換後重建）；client 要 IDR 卻只見 SUPPRESS、無 EMIT 就是 §S.19-INIT-FIX 修掉的永久凍結型態 |
| `[VIPLE-UPDATE]` | **host** `self_update.cpp` / `system_tray.cpp` | §SELF-UPDATE 常駐圖示兩段式「Check for updates...」→「Install update vX」與 CLI `--check-update`／`--self-update [--force] [package]`：`latest via HEAD: X`（github.com 302 取 tag，免 API 配額）／`falling back to API`／`up to date`／`update available: A → B`／`downloaded … bytes`／`sha256 verified`（GitHub asset digest，拿不到就拒裝）／`updater launched (pid N) … service=1`（Windows：PowerShell 腳本在 `<install>/config/update`，停 service→rename .old→覆蓋→啟 service，失敗回滾；細節在 `config/self_update.log`）／`sudo path rc=` `pkexec path rc=`（Linux：apt-get install .deb；細節在 `~/.config/sunshine/self_update.log`）／`self-update completed: now running X` 或 `last self-update failed: …`（重啟後讀 result 檔） |
| `[VIPLE-HID]` | `backend/hidprobe.cpp` + 四個閘控點（`gamepad.cpp` getUnmappedGamepads / `sdlgamepadkeynavigation.cpp` enable / `input.cpp` SdlInputHandler ctor / `sc_hid.cpp` prepare） | 啟動前與每場開始前的 HID 探測（§HID-PROBE，2026-09-19 Puck 卡死事故）。`probe (startup\|refresh) OK: N HID interface(s) responded in X ms (slowest VID:PID Y ms, limit 1000 ms)` = 正常；`UNRESPONSIVE <name> vid= pid= stuck_at=<CreateFile\|HidD_GetAttributes\|HidD_GetProductString\|enumeration> path=` = 該裝置無回應 → 手把偵測、手把 UI 導覽、串流手把與 SC-HID 停用，各閘控點各印一行 `skipped/refused`；重插裝置後下一次 refresh（開串流／回 UI）自動恢復，不必重啟。**刻意比 SDL 保守**：對所有 HID 介面查字串，卡死的普通鍵鼠也會觸發。`(simulated via VIPLE_HID_PROBE_SIMULATE_HANG, dev-only)` = 測試注入（`VID:PID` 直接注入；`VID:PID:stall` 對實體介面真的卡在字串查詢；`enumerate:stall` 整個列舉逾時），不是真故障 |
| `[VIPLE-CTRL]` | `ControlStream.c`（common-c，Q／A 兩份相同） | 控制通道的防衛，四種行都代表防衛生效（拒送、改送 IDR 或忽略），不是崩潰。**§F2**（`sendMessageEnet` 加密分支）：`§F2 payload too large (ptype=0x%04x len=%d) — limit 251, dropped (count=N)` ＝ 明文超過 256 B stack buffer（payload 上限 251 B）被拒送、回傳 false；前 10 次都印、之後每 1000 次一筆。現有呼叫端都是固定長度（最大是 input 132 B），**1.5.x 正常 log 不該出現**；出現代表有新呼叫端（2.0 VR 送出 API）組出超長封包。**§F1-DBG-ASSERT**（幀號狀態倒帶，三行都改送 IDR）：`Invalid frame loss range (%u to %u) — requesting IDR frame instead`（`queueFrameInvalidationTuple`，每秒最多 1 筆）、`Non-monotonic RFI ranges (start %u, last end %u) — requesting IDR frame instead`（`referenceFrameControlFunc` 聚合時）、`connectionSawFrame: stale frame=%u (lastSeen=%u) — ignored`（倒退幀號不計入統計，每秒最多 1 筆）。這三行與 `[VIPLE-DEPACK] notifyFrameLost: stale` 同時出現＝§FRZ-WATCHDOG 哨兵重置後採納了 failback 殘留的 stale 幀；舊版 Android（debug native、assert 生效）會在這裡 SIGABRT 閃退 |
| `[VIPLE-MPQUIC] §M01-C …` | `ControlStream.c`（common-c，Q／A 兩份相同） | §M01-C ENet 斷線後的重連。以前 §Q-ENET-RECONNECT 只在找到有線網卡時才重連，只有 Wi-Fi 的裝置（手機、筆電）斷線後永遠不重連、120 秒後放棄輸入。現在改看「到 host 的路由是否可用」：`route to host available (local=…)` / `route to host lost` / `route to host now via local=… (was …) — backoff reset`（換了出口介面就重設退避）；`host rejected ENet reconnect (stale peer?)` 是 host 端舊 peer 還在，會繼續重試；重連後約 5 秒印 `ENet healthy rtt=…`（或 `NOT healthy`）。`ENet still down after 120 s … input via QUIC datagram (unreliable), still retrying` 代表輸入暫時走 QUIC datagram、不保證送達；`QUIC transport dead while ENet down — terminating` 是兩條都斷、結束連線 |
| `[VIPLE-MPQUIC] §Q.path-recovery: thread …` | `QuicTransport.c`（common-c，Q／A 兩份相同） | §M01-E 路徑復原執行緒的生命週期：`thread started` → `thread exiting` → `thread joined`。**每個用過 QUIC 的 session 收尾時都要有一行 `thread joined`**，包括 QUIC 閒置逾時或 path 全滅走 `§K.13 IO loop bail` 的情況。2026-09-26 以前 bail 路徑會跳過 join：debug 版在 `Platform.c` 的 `activeThreads == 0` assert 中止，release 版洩漏執行緒。有 `thread started` 卻沒有 `thread joined` 就是 regression |
| `[VIPLE-MPQUIC] §MP-ONLINK …`、`§MP-BINDDEV …` | `Connection.c`、`QuicTransport.c`、`PlatformNetIf.c`（common-c，Q／A 兩份相同） | 多路徑配對。`§MP-ONLINK: if <n> '<name>' /<prefix> shares subnet with peer <i> (-2=primary) — pairing only that peer (added\|failed)`：server 有位址和這張網卡同子網路時只配那一個（例：Steam Frame 的 `wlanap` 10.35.78.1/24 只配 host 適配器的 10.35.78.x），不再拿主位址、Tailscale、公網位址去試。以前 Linux（weak host）會形成上行走家用 Wi-Fi、下行走適配器的不對稱路徑，還吃光 picoquic 的 path ID，真正的對稱位址落得 `probe_new_path failed (1088)`（PATH_ID_BLOCKED）。`§MP-BINDDEV: SO_BINDTODEVICE '<name>' failed (errno N)`（僅 Linux／Android）＝ subflow socket 綁網卡失敗、退回舊行為；注意綁定後 Linux 對沒有路由的目的地會假設 on-link，connect() 仍會通過，所以配對正確性靠 §MP-ONLINK |
| `[VIPLE-MPQUIC] §MP-ROUTECHK …` | `QuicTransport.c`（common-c，Q／A 兩份相同；只在非 Android 的 Linux 生效） | `§MP-ROUTECHK: if <n> '<name>' has no route to peer in main table; skipping path`：主路由表（/proc/net/route）裡這張網卡沒有任何路由通往 peer（連預設路由都沒有），不建路徑。例：Steam Frame 的 host 適配器沒連上時，wlanap 只剩 10.35.78.0/24，對 server 的 LAN／Tailscale／公網位址都會被跳過；以前這些組合會變成死路並吃掉 path ID。VPN 介面（Tailscale 用 table 52）與讀不到 /proc/net/route 時不套用 |
| （無專屬 log）§MP-VERIFIED | **host** `quic_server.cpp` | 影像路徑選擇：path 1+ 必須 `first_tuple->challenge_verified`（路徑驗證完成）才能載視訊或被緊急升級；第一次選路（`_lastVideoPath == -2`）直接用健康的 path 0。觀察點是既有的 `§Q-PATH-SWITCH: bestVideoPath -2(...) -> 0(...)`：多路徑 session 的第一次切換應該是到 0；若是到其他 path 就是 regression。起因：Steam Frame 上未驗證的新路徑（初始 cwin 69 KB）贏過剛被 §K.14 重置到 59 KB 的 path 0，3 秒丟 1680 包 |
| `[VIPLE-MPQUIC] §Q-ACCT …` | **host** `quic_server.cpp`、client `QuicTransport.c`（common-c，Q／A 兩份相同） | QUIC 影像逐序號對帳（2026-09-29 用來抓出 §Q-MTU-PIN）。client：`§K.10 path[i] … rxPkts=N`（socket 取得的原始封包）、`§Q-ACCT client: pqRecv= pqSent=`（picoquic 成功處理／送出）、`§Q-ACCT skip seq=S count=N sinceDeliver=… buffered=…`（抖動緩衝逾時跳過的序號，每行程最多 30 行）。host：`§Q-ACCT server <addr>: pqSent= pqRecv= retx= spurious=`（5 秒一行）、每 10 秒 `§Q-ACCT dgram: queued-only= acked= lost= spurious=`＋`§Q-ACCT lost seqs:`＋`§Q-ACCT queued-never-acked seqs:`（picoquic datagram_acked／lost／spurious 回呼逐序號記帳）。判讀：client 的 skip 序號若出現在 host 的 queued-never-acked＝資料報排入 picoquic 後沒被送出；出現在 lost＝真的網路遺失；host 標 acked＝client 內部丟掉。queued-only 少量（1～2）是統計當下仍在途 |
| （無專屬 log）§Q-MTU-PIN | **host** `quic_server.cpp`＋picoquic fork `loss_recovery.c`、`picoquic_internal.h` | boost 到 1500 的路徑設 `viple_mtu_pinned`，picoquic 因遺失（多為 PTO／RACK 誤判）重置 MTU 時跳過；§K.9 takeover／PAROLE 判定黑洞時解除。以前 MTU 被打回 1232 到下次 drain 重新 boost 之間，佇列開頭 >1232B 的影像資料報會在 `picoquic_format_first_datagram_frame`（§K.11）被無聲刪除。觀察點：`§Q-ACCT queued-never-acked` 應只剩在途的 1～2 個；成段缺號就是 regression |
| `[VIPLE-AUDIO] §M01-A …` | `RtpAudioQueue.c`、`AudioStream.c`（common-c，Q／A 兩份相同） | §M01-A 音訊佇列對網路輸入的檢查。上游用 `LC_ASSERT_VT`，debug 建置（Android `com.piinsta.debug`、Qt Debug 組態）遇到異常封包直接 SIGABRT（2026-09-23 20:06:43 Pixel 5 AudioRecv 在 block size 檢查 abort）；§M01-A 起改成與 release 相同的 runtime 處理，加上這組 log（每個呼叫點每秒最多 1 筆，行為都是「丟包或照常繼續」，不是崩潰）。根因是 QUIC 接收 ring（`quicAudioRing`／`quicVideoRing`）在 ARM64 缺記憶體屏障、讀到 slot 上一輪殘留的 len，同版已修（`Limelight-internal.h` 的 `QUIC_RING_*_FENCE`）。**release 也會印**：`corrupt audio shard length (seq=%u pt=%u len=%u) — dropped, FEC kept` ＝ 音訊有加密（AES-CBC＋PKCS7，密文必為 16 的倍數）但 shard 長度不是 16 的倍數，必定是 client 端毀損，只丟這包、不停用 FEC；`block size mismatch detail: seq=%u pt=%u len=%u blockSize=%u enc=%d block=%u (had data=%u fec=%u)` ＝ 緊接在上游 `Audio block size mismatch (got X, expected Y)` 之後、`Audio FEC has been disabled due to an incompatibility with your host's old software!` 之前，每場最多一次（之後整場停用音訊 FEC）；判讀規則：**enc=1 且 blockSize%16≠0 ⇒ client 端毀損；enc=1 且 blockSize%16==0 ⇒ host 真的送了變長封包**（GFE 3.13、舊 Sunshine）；enc=0 無法只靠長度判斷；`FEC header mismatch in block %u (seq=%u pt=%u len=%u): pt a/b ts a/b ssrc a/b` ＝ 同一 FEC block 的封包 header 不一致，以最先到的封包為準照常收。**只在 debug**：`FEC block %u (ts size pt ssrc) inconsistent with block %u (…)` ＝ 佇列內各 block 的 timestamp／長度／pt／ssrc 不一致；`FEC validation mismatch: block %u shard %u, N byte errors (first at K), total failures M — keeping received packet` ＝ FEC_VALIDATION_MODE 合成丟包後的重建結果與實收封包不同（host parity 與 data 對不上、實收封包毀損，或我們的 RS 重建有 bug），改交付實收封包；`first Opus TOC byte is 0x00 (seq=%u), baseline deferred`／`unexpected Opus TOC byte 0x%02x (expected 0x%02x, seq=%u)` ＝ Opus TOC 一致性檢查（host 是 Sunshine 時只檢查第一包）。上游不帶 tag 的 `Failed to decrypt audio packet (sequence number: %u)` 在 debug 版也不再 abort。**一般串流、failover、outage 都不該出現任何 §M01-A 行**；測試時 grep `[VIPLE-AUDIO]`，連同 `Audio block size mismatch`、`Audio FEC has been disabled`、`Failed to decrypt audio packet` 都應為 0 |
| `[VIPLE-CTRL-TX]` | **host** `stream.cpp`（`control_server_t::send`） | §F4 server 控制訊息可指定 channel／flags：`§F4: channel N >= peer channelCount M, falling back to channel 0` ＝ 要求的 ENet channel 超過該 client 協商的 channel 數（舊 client），改走 channel 0、flags 不變；整個程序只記前 5 次。1.5.x 沒有呼叫端送非 0 channel（M1a 的 VR 訊息才會），**目前不該出現** |
| `[VIPLE-MPQUIC] §F3 …` | **host** `stream.cpp` | §F3 QUIC recv handler 提早註冊（只在開 mpquic 的 session）：`§F3 recv handler registered at session start (via=session-start\|quic-ready, peer=…)` 每條 session 一次，一般順序（RTSP 握手後才連 QUIC）是 `via=quic-ready`；同一條 session 因 QUIC 重連換了 QuicSession 時印 `§F3 recv handler re-registered on new QUIC session (via=…)`。`§Q-IDR-VIA-QUIC: recv handler registered on QUIC session via video loop (§F3 safety net — early registration missed, peer=…)` ＝ 兩個提早註冊點都漏接、由 video 迴圈補上，**不該出現**，出現就是 F3 有漏洞。開 mpquic 的 session 若三行都沒有，client 在 QUIC fallback 期間送的 IDR／FEC／input 會被丟掉 |
| `[VIPLE-MULTI] §M01-D …` / `[VIPLE-MPQUIC] §M01-D …` | **host** `rtsp.cpp`（`clear_for_client`、`session_raise`）/ `stream.cpp`（`recv_ping`）/ `quic_server.cpp`（`retireSession`） | §M01-D 同一個 client（同一張 TLS 憑證）被強制關閉後立刻重開：/resume、/launch 先收掉它自己殘留的 stream session（2026-09-23 事故：殭屍 session 的 P-frame 經同 IP 的新 QUIC 連線灌給新 client → `Network dropped 69411 frames` → -101）。`superseded N stale session(s) of uuid=… before /resume\|/launch (active before=M)` ＝ 收掉 N 條；單一 client 時之後應依序出現 `Listener stopped` → `Listener started` → `New streaming session started [active sessions: 1]` → `Session stored` → `§F3 … via=quic-ready`，**不該**再有 `active sessions: 2`、`§F3 safety net`、重連後的 `ping timeout suppressed`。`stale session of uuid=… has not started yet — marked superseded, not joined` ＝ 殘留的那條還在 `session::start` 中途，只標記、交給 ping timeout（極少見）。`replaced stale pending launch of same client (old id=… new id=…)` ＝ client 在 /resume 之後、ENet 連上之前被殺又重開，pending launch 換成新的（否則 RTSP 會拿到舊 rikey／ping payload）。`[VIPLE-MPQUIC] §M01-D retired QUIC connection of superseded session (peer=…)` ＝ 被取代的 session 擁有的 QUIC 連線從 listener map 移除（cnx 交給 picoquic 30 s idle 回收）。`recv_ping aborted — session stopping before first video\|audio ping` ＝ 被收掉的 session 還在等第一個 UDP ping，立刻放棄（否則 /resume 會被拖到 ping_timeout）。一般串流、failover、outage 都**不該出現任何 §M01-D 行**。B 線（縱深防禦：`claimed by another session — not extending grace`／`dropping stale-session frames\|audio`／`QUIC owned by older session — falling back`）尚未合入，目前版本不會出現 |
| `[VIPLE-DEVENV]` | `wm.cpp`（`Utils::logDevEnvOverride`）／**host** `config.cpp` | 環境變數覆寫了行為（環境變數只准當 dev-only 偵錯開關）。client：`dev-only override NAME=value`（SDL warn，每個名稱每個行程一次），涵蓋 `PREFER_VULKAN`、`GL_IS_SLOW`、`VULKAN_IS_SLOW`、`MATCH_DISPLAY_MODE_TO_VIDEO`、`SEPARATE_TEST_DECODER`、`FORCE_QT_GLES`、`VIPLE_USE_VK_DECODER`、`VIPLE_VK_FRUC_GENERIC`、`VIPLE_VKFRUC_*`、`*_AVOPTIONS`、`*_DECODER_HINT`。host（sunshine.log，啟動時一次，§F7）：`dev-only override VIPLE_SMOOTH_PACING=<v> （…smooth_pacing=<cfg> 被覆寫，生效值=<eff>…）`，優先序 env > `sunshine.conf` 的 `smooth_pacing` > 預設 false。**分析使用者回報的 log 前先 grep 這個 tag**：有這行就代表行為被環境變數改過，要先排除 |
| `[VIPLE-ABR] NvEnc: pinned …` | **host** `nvenc/nvenc_base.cpp`（`pin_level_for_reconfigure`） | §SF-PARAMSETS：encoder 初始化後讀回 NVENC 自動選的 level（HEVC 另加 tier），固定進 ABR reconfigure 的設定，讓整個 session 的 VPS／SPS 不變。`pinned HEVC level_idc=%u tier=high\|main for mid-stream reconfigure`、`pinned H.264 level_idc=%u …`；失敗時 `NvEncGetSequenceParams() failed, level not pinned`／`SPS not found in sequence header (N bytes), level not pinned`（ABR 照常，只是參數集可能改變）。AV1 尚未處理 |
| `[VIPLE-LNXFE]` | （僅 Linux）`settings/streamingpreferences.cpp`、`wm.cpp`、`ffmpeg.cpp`、`session.cpp` | §F6 Linux renderer 決策。`linuxVideoFrontend=auto\|vulkan\|egl -> frontend=PlVk-first\|legacy-order (reason=aarch64\|zink\|default\|user isGpuSlow=N)`（設定或結果改變時才印）；`DRM driver probe: [<driver>] (source=libdrm\|sysfs)`（決定 isGpuSlow；msm 判為不慢）；`EGL probe: vendor='…' driver='…' zink=N`（x86 AUTO 第一次選 renderer 時探一次 Zink）；G-α 驗收摘要 `decoder=%s fmt=%s frontend=%s backend=%s isGpuSlow=%d`、`windowMode=%d fullscreenFlag=FULLSCREEN\|FULLSCREEN_DESKTOP\|NONE isGpuSlow=%d`、`matchVideo=%d (isGpuSlow=%d videoDriver=%s)`。Steam Frame 預期：`DRM driver probe: [msm]`、`reason=aarch64`、`fmt=DRM_PRIME frontend=PlVk isGpuSlow=0`、`fullscreenFlag=FULLSCREEN_DESKTOP`、`matchVideo=0`。**§SF-DRMSPLIT**（PlVk，每個 renderer 印一次）：`DRM_PRIME layout: layers=%d planes0=%d fourcc=0x%08x objects=%d mod=0x%llx -> split into per-plane layers\|as-is\|unsupported multi-plane fourcc\|split failed (alloc)`；Steam Frame 的 iris 預期 `layers=1 planes0=2 fourcc=0x3231564e`（NV12）`-> split`。**§SF-PARAMSETS**（v4l2m2m）：`parameter sets changed on IDR frame %d (<decoder>, %d -> %d bytes); recreating the decoder instead of feeding it to the running instance` ＝ host 換了 VPS／SPS／PPS（例如 ABR 降碼讓 NVENC 改 tier），client 主動重建 decoder；server 修正（`[VIPLE-ABR] NvEnc: pinned …`）部署後不該再出現。**§SF-DMABUF-CACHE**（PlVk）：`dmabuf cache: entries=%zu imports=%llu hits=%llu`（約每 1800 幀一行）、`dmabuf cache cleared (<reason>): …`（解碼器換 hw_frames_ctx 或 renderer 銷毀）；Steam Frame 每場 imports 應停在 6～8，imports 持續增加代表快取沒命中（又回到每幀匯入，會再踩 msm 核心錯誤）。`dmabuf cache not applicable, using pl_map_avframe_ex` 只印一次＝退回 libplacebo 路徑。**§SF-PITCHCLAMP**：`dmabuf plane %d width %d exceeds pitch %lld; clamped to %d (frame=%dx%d)`（每個 renderer 一次；GUI 啟動探測的 1280x720 測試幀在 iris 上會出現）；`dmabuf plane %d rejected: …` 另同步寫 stderr，出現代表描述子不合法、該幀映射失敗。Windows 不印 |
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

## 5b. VR（2.0 §VR，M1a 起）

VR 相關 tag 只在 VR session 出現：client 帶 `--display-target pcvr`，而且 server 回了
`<VipleStreamVRSession>`。一般 session 一行都不會有；一般 session 出現這些 tag 就是 regression
（不變式 5）。協定細節見 `docs/vr_protocol.md`（M1a 定案在 §4.9）。

### client 端

#### `[VIPLE-VR-SESSION]` —— 協商與 session 狀態

```
[VIPLE-VR-SESSION] requesting PCVR (emulated pose): eye=1728x1728 hz=90 serverCaps=0x23
[VIPLE-VR-SESSION] VR request: stream 3456x1728@90 (FRUC off)
[VIPLE-VR-SESSION] negotiated: proto=1 packed=3456x1728 hz=90 codec=hevc layout=sbs overscan=0 recovery=intra irFrames=8 transport=rtp mode=stub session=…
[VIPLE-VR-SESSION] VR session: vrFlags=0x3 (recovery=intra)                    ← common-c，LiStartConnection
[VIPLE-VR-SESSION] ENet throttle configured (interval=5000 accel=2 decel=0)    ← 首次連線與每次 ENet 重連後
[VIPLE-VR-SESSION] VR session: video over RTP/UDP (QUIC kept for control/tracking)
[VIPLE-VR-SESSION] server state=3 progress=100 code=0                          ← 0x5508 STATE（3＝STUB_ECHO）
```

`serverCaps` 是 `/serverinfo` 的 `<VipleStreamVR>`：0x01 PROTO_V1、0x02 PCVR、0x20 RECOVERY_INTRA。
退回平面時一律印 WARN，原因分別是：
- `needs an XR runtime`：沒帶 `--vr-emulate`；
- `host does not offer PCVR`：server 沒有 PCVR bit，也就是 `vr_pcvr=disabled`、Linux server 或舊 server；
- `server did not confirm a VR session`：舊 server 忽略了 `vr=1`；
- `relay path — VR not requested`；
- `no 8-bit HEVC/H.264 decoder`。

server 拒絕時，launch 會以「Host returned error: VR_BUSY: …」之類的錯誤結束，錯誤碼見 `VipleVr.h`。

#### `[VIPLE-VR-POSE]` —— tracking 上行（0x5506）

```
[VIPLE-VR-POSE] tracking sender started: 180 Hz (display 90 Hz × 2), motion=sine
[VIPLE-VR-POSE] 10s: sent=1800 (180.0/s) fail=0 late=0 lastId=…
[VIPLE-VR-POSE] (final) sent=… (…/s over … s) fail=… late=… lastId=…
[VIPLE-VR-POSE] tracking send failing — backing off 1000 ms (sent=… dropped=…)   ← common-c
```

- `late`：送出執行緒落後超過一個週期、直接跳到下一個格點的次數。舊樣本沒有價值，所以不補發。
- ENet 和 QUIC 都送不出去時，連續 3 次就暫停 1 秒，所以斷線期間每秒最多一行 backoff。

#### `[VIPLE-VR-FRAME]` —— 解出的幀與 0x81 metadata 的配對

```
[VIPLE-VR-FRAME] pts-keyed metadata pairing enabled (injectDropEvery=0 injectLossSec=0)
[VIPLE-VR-FRAME] 10s: decoded=… submitted=… hit=… miss=0 noHeader=0 echoMatched=…/… (100.00%) echoKnown=… fallback=0 refreshDone=… fifoMismatch=… fifoSkipped=… injectedDrops=… injectedLoss=… ptsMissing=0 echoAge p50=…ms p95=…ms max=…ms
[VIPLE-VR-FRAME] (final) …（整場累計，decoder 解構時印；decoder 重建時會提前印一次）
[VIPLE-VR-FRAME] meta-miss: pts=… slot=… used=…
[VIPLE-VR-FRAME] decoder did not carry pts — falling back to FIFO pairing
```

判讀：
- **echo 命中率** = `echoMatched / (hit − noHeader)`，M1a 門檻 ≥ 99%。`echoKnown` 是 echoSampleId 還查得到取樣時間的幀數。
- **`echoAge`** = 解出這一幀的時間 − 它回聲的 tracking 樣本的取樣時間（client 時鐘）。M1a 的 stub
  沒有遊戲 render，這個值約等於上行、server 等下一幀、編碼、下行、解碼的總和，可當作之後 MTP 的下限參考。
- **`miss` 必須是 0**。`fifoMismatch` 是「還用 FIFO 配對的話會配錯幾次」，只在 decoder 吞幀或
  `--vr-inject-drop` 注入時才會大於 0；`fifoSkipped` 是因此從 FIFO 丟掉的項目數。M1a 的驗收條件
  「decoder 丟一幀注入後 0 不一致」＝ `injectedDrops>0`、`fifoMismatch>0`，而且 `miss=0`。
- `ptsMissing>0` 代表這顆 decoder 沒把 pts 帶回來，配對退回 FIFO，PCVR 在這種 decoder 上不可靠。
- `(final)` 在同一場裡印了很多次，代表 decoder 一直被重建，要找 `Resetting decoder` 的原因。

#### `[VIPLE-VR-LOSS]` —— recovery=intra 的恢復狀態機（common-c／decoder）

```
[VIPLE-VR-LOSS] 10s: lossEvents=… sends=… resends=… refreshRx=… stale=… recovered=… idrFallback=… lossToRefresh avg=…ms max=…ms recover avg=…ms max=…ms
[VIPLE-VR-LOSS] (final) lossEvents=… recovered=… idrFallbacks=… refreshRx=… tracking sent=… dropped=…
[VIPLE-VR-LOSS] no refresh response — falling back to IDR (lost a..b)
[VIPLE-VR-LOSS] frame N unrecoverable — reporting LOSS (…)                ← depacketizer（取代 RFI 那一行）
[VIPLE-VR-LOSS] injected LOSS for frame N (test)                            ← --vr-inject-loss
[VIPLE-VR-LOSS] 3 consecutive decode errors — requesting IDR (frame N)       ← Qt decoder
[VIPLE-VR-LOSS] failed to create VrSend thread — LOSS will be sent synchronously
```

- 10 秒統計只在有動靜時印。
- `lossToRefresh` ＝ 送出 LOSS 到收到對應 REFRESH_START 的時間。server 接受 LOSS 當下就回覆，所以
  約等於 1 RTT（M1a 在 LAN 實測 ≤ 1 ms）。
- `recover` ＝ 從掉幀到收到帶 `REFRESH_DONE` 的幀（或 IDR）的時間。主要是 wave 本身的長度：
  8 幀在 64 fps 下約 125 ms。
- `stale` 約等於 `refreshRx`：server 每個 REFRESH_START 都送 2 份（第二份晚一個 control tick），第二份一定被當成過期。
- `resends>0` 代表 REFRESH_START 在 2×RTT+20 ms 內沒到。`idrFallback>0` 代表 server 沒回應 LOSS，
  重送 3 次或等了 500 ms 才退回 IDR，要對照 server 端的 `[VIPLE-VR-LOSS]`。

#### `[VIPLE-VR-STATS]` —— server 統計的 10 秒區間（client 端印）

```
[VIPLE-VR-STATS] server 10.0s (10 msgs): poseRx=… ooo=… gap=… tagged=… fallback=… lossRx=… waves=… | refreshStartRx=…
```

server 的 STATS 計數欄位是累計值，這一行印的是兩筆之間的差。`poseRx` 應約等於同一區間 client
`[VIPLE-VR-POSE]` 的 `sent`，兩者的差距就是上行丟失（M1a 門檻 ≤ 0.5%）。`tagged` 應約等於解出的幀數。

### server 端（sunshine.log）

```
[VIPLE-VR-SESSION] accepted /launch caller=… eye=1728x1728 hz=90 … -> proto=1;packed=…;mode=stub (serverIntra=true safetyMs=2000)
[VIPLE-VR-SESSION] /launch: skipping configure_display (stub captures the current desktop)
[VIPLE-VR-SESSION] rejected /launch: 503 VR_BUSY: another device's VR session is active on this host (caller uuid=…)
[VIPLE-VR-SESSION] RTSP ANNOUNCE session=… vrProfile=1 irFrames=8 irPeriodFrames=180 enableIntraRefresh=1 recovery=intra encode=3456x1728@90
[VIPLE-VR-SESSION] stream session start session=… mode=stub codec=hevc recovery=intra loopTimeout=4ms
[VIPLE-VR-SESSION] control connected, STATE stub-echo queued (session=…)
[VIPLE-VR-SESSION] (final) session=… frames tagged=… fallback=… loss=… waves=… latch=… timing=… otherC2S=… truncatedC2S=… s2cDropped=…
[VIPLE-VR-ENC] intra refresh: codec=hevc cnt=8 period=180 singleSlice=1        ← encoder 建立時一次；不支援時 "intra refresh unsupported"
[VIPLE-VR-ENC] idr reason=request|startup-retry|refresh-fallback|refresh-fallback-retry f=…
[VIPLE-VR-ENC] idr reason=refresh-fallback deferred f=… (VR cooldown active)  ← encoder 沒有 IR、IDR 在冷卻：冷卻一到補送
[VIPLE-VR-ENC] refresh wave f=… cnt=8 reason=loss (+N suppressed)            ← 每秒最多一行
[VIPLE-VR-TX] transport=rtp (VR video forced to RTP/UDP; QUIC session present, kept for control fallback)
[VIPLE-VR-POSE-RX] 10s: rx=1800 (180.0/s) ooo=0 gap=… viaQuic=… bad=0 lastId=… | frames tagged=… fallback=0 | c2s loss=… waves=… latch=… timing=… other=…
[VIPLE-VR-POSE-RX] (final) rx=… ooo=… gap=… viaQuic=… bad=… resets=… lastId=…
[VIPLE-VR-LOSS] rx first=… last=… reason=… -> new_wave|absorbed|duplicate|idr (+N suppressed, total=…)   ← duplicate 只在 50 ms 內、RESEND 不算
```

- 一般情況下 `viaQuic` 應該是 0（tracking 走 ENet VR channel）。持續大於 0 代表 client 的 MP-QUIC 判定
  failover、Fix L 把控制訊息轉去 QUIC，要看 client 的 `§Q-FAILOVER-BYPASS`、`marked INACTIVE`。VR
  session 的視訊走 RTP，QUIC 上平常沒有流量；client 每 500 ms 在 QUIC 送 `'P'` 讓路徑保持活躍，
  少了這個就會被判 stall。
- `gap` 是 sampleId 跳號累計，也就是上行丟失的估計；`ooo` 是倒退或重複。
- VR session 的 control 迴圈每 4 ms 醒一次，只有 VR session 如此。
- **M1b 起的 log 衛生**：VR 相關行的 session GUID 只印前 8 hex（`::vr::log_guid`）；寫給 client 的
  `<VipleStreamVRSession>` 仍是完整值。
- `[VIPLE-VR-CAP] qpc-guard: frame timestamp in the future, replaced with now (future=N stale=M)`（warning）／
  `… older than 1s …`（debug）：擷取的 QPC 時間戳不可信時改用 now 並計數，兩類各自每 10 s 最多一行。
  M1b 修正 `qpc_time_difference`（舊版小 10⁴ 倍）後才有意義；桌面 session 正常情況下不會出現 warning。
- `[VIPLE-VR-CAP] init_device: no adapter selected`：`display_base_t::init_device` 收到空 adapter（DDA／WGC 不會走到）。
- **M1b V3（VR 擷取，`display_vr_t`）**：
  - `[VIPLE-VR-CAP] init adapter=<desc> luid=<match|n/a> size=<W>x<H>@<hz> mode=<probe|live>`：建立時一次。選卡（K22）只收非軟體、VendorId≠0x1414、能建 FL 11_0、而且 `D3DKMT` ADAPTERTYPE 不是間接顯示（IddCx，例：MTT VDD 會在 DXGI 裡冒充成同名實體卡）的adapter；候選多於一張又沒有 `vrcompositor_adapter` 紀錄 → `VR_DISABLED: adapter mismatch …`（fail closed）。
  - `[VIPLE-VR-CAP] opened gen=<n> ring=<W>x<H>` / `first frame gen=<n> …` / `frame source gen=<n> is on a different adapter; reinit` / `ring … != display …; reinit` / `open gen=<n> failed: <open-texture[i]|open-shared-fence|open-consumed-fence>`。driver generation 換了只重開 ring，不重建 encoder。
  - `[VIPLE-VR-CAP] 10s: gen= copied= skipped= fenceTimeout= invalid= stale= black= evtToPush p50/p95 presentToPush p50/p95`：健康時 invalid／stale／fenceTimeout 為 0，evtToPush p95 ≤ 1.5 ms（V3 host 實測 0.33 ms）。`black` 只在 driver 回報 HMD_PRESENTING 前（10 fps 黑幀）。
  - `[VIPLE-VR-CAP] gpu-priority=high (was realtime|high)`：擷取期間行程 GPU 優先權改 HIGH（vrcompositor 與遊戲同卡，REALTIME 會搶遊戲）。
  - `[VIPLE-VR-CAP] capture ctx mixed sources; using newest (source=…)`：前一個桌面擷取執行緒還沒結束就接上 VR ctx，強制重建成 display_vr_t。`sync capture path does not support captureSource=1`＝防呆（Windows 走不到）。
  - `[VIPLE-VR-ENC] desktop probe state saved|restored (source=…)`：VR 探測（`vr_probe_scope`）前後保存／還原桌面探測結果，`/serverinfo` 前後必須逐字相同（selftest T1b）；`probe shape codec=… <W>x<H>@<hz> ir=<n>` 與 `intra refresh: …` 是 VR 形狀的探測結果。
  - `[VIPLE-VR-TX] 10s: frames=… presentToFirstPkt p50/p95 transport=rtp`：VR session 的 Present→首封包（§8.3 量測點）。
  - `[VIPLE-VR-SESSION] rtsp override field=<f> client=<v> negotiated=<v>`：pcvr 模式以協商值覆寫 client 帶來的 HDR／解析度／fps／slices（V5 才會出現）。
  - selftest：`T1.init|probe|vr-intra-refresh|restore …`、`T1b.serverinfo result=PASS identical bytes=N`（`T1b.launch-exits` 到 V5 才跑）、`T2.live … consumption= evtToPushMs p50/p95/max`（門檻 ≥ 99%、p95 ≤ 1.5 ms）。

### 5b-1. 機密與權限（M1b 起，Windows server）

- `config: 'relay_psk' = <redacted len=N>`：`config::is_secret_key()` 為真的鍵（`relay_psk`、`relay_url`、結尾
  `_psk`／`_password`／`_secret`／`_token`／`_key`）在 config dump 一律遮蔽；兩個輸出點（`apply_config` 與
  `main.cpp` 重印）都遮。
- `[RELAY] Target: wss://<host#xxxxxxxx>:443`：relay 連線 log 不印 host 原文，改印 SHA-256 前 8 hex。
- `[VIPLE-SEC] config-acl tightened files=N skipped=M`：server（SYSTEM）啟動時把 `config\` 頂層的
  `sunshine.conf`、conf 備份、`sunshine*.log*`、`viplestream-svc.log`、`sunshine-cli.log`、`sunshine_state.json`
  與 `credentials\` 底下的檔案設成 protected DACL `O:SYG:SYD:P(A;;FA;;;SY)(A;;FA;;;BA)`（一般使用者不可讀）；
  目錄本身的 DACL 不動。`[VIPLE-SEC] webui-port publish failed step=… err=…`：`config\webui_port`（給
  `--shortcut` 用的非機密 port 號）寫入失敗。
- CLI 模式（`--version`、`--creds`、`--vr-selftest` 等任何 `--<command>`）的 log 寫到同目錄的 `sunshine-cli.log`，
  **不再截斷** service 使用中的 `sunshine.log`。service 自己的 stdout log 由 `%SystemRoot%\Temp\viplestream.log`
  搬到 `<install>\config\viplestream-svc.log`（protected DACL）。

---

### 5b-2. VR IPC 與 selftest（M1b V2 起，Windows server）

```
[VIPLE-VR-IPC] pipe ready session=<N> dacl=system-only                    ← service 啟動；使用者 ACE 另外補
[VIPLE-VR-IPC] user-ace set sid=*<RID>                                    ← 只印 RID，不印完整 SID
[VIPLE-VR-IPC] user-ace reapplied | user-ace reapply failed               ← 核對時發現 pipe 少了使用者 ACE
[VIPLE-VR-IPC] handshake gen=<G> driver=<ver> abi=<N> vrserver-pid=<pid> iface=IVRDriverDirectModeComponent_009 ring=<W>x<H> luid=set identity=<身分> caps=0x…
[VIPLE-VR-IPC] rejected reason=<原因> …                                    ← 映像路徑經 loggable_path 過濾（空白、`=`、`[` 會變成 `?`）
[VIPLE-VR-ADMIN] pipe ready
[VIPLE-VR-ADMIN] selftest accepted run=<UTC 時間戳> (detached; …)
[VIPLE-VR-ADMIN] result rc=<N> state=done|running|interrupted run=<…> pass=<N> fail=<N> notRun=<N> source=memory|file
[VIPLE-VR-ADMIN] summary written | summary-skip <原因> | status from saved summary | saved summary unreadable
[VIPLE-VR-SELFTEST] T<n>.<情境> result=PASS|FAIL|INFO|NOT-RUN <細節>
[VIPLE-VR-SELFTEST] T<n> end pass=<N> fail=<N> notRun=<N> ms=<N>
[VIPLE-VR-SELFTEST] (final) pass=<N> fail=<N> notRun=<N> rc=<N> ms=<N>
```

- 執行：以管理員身分跑 `viplestream-server.exe --vr-selftest [--detach] --only T0,T2,T6`（由執行中的 service 以 SYSTEM 代跑），
  `--vr-status` 讀進度或結果，`--vr-abort` 中止。結果另存 `config\steamvr\selftest\<run>\summary.json`（service 重啟後
  `--vr-status` 以 `source=file` 讀回；跑到一半重啟會回 `state=interrupted`、rc=6）。
- `result=NOT-RUN`：這一版還不能跑的情境（例如 V2 的 `T2.gpu-hold`、`T2.live` 要到 V3），計入 `notRun=`，不影響 rc。
  rc=0 不代表所有情境都驗過，要看 `notRun`。
- 2026-09-28 `<host>` 實測：T0＋T2 `pass=29 fail=0 notRun=2`、T6 `pass=30`；`T2.second-instance` 的 SYSTEM 探測在實例已滿時回
  ERROR_PIPE_BUSY（231），和 ERROR_ACCESS_DENIED（5）同樣算通過。

## 5c. Steam Frame 探測與執行環境（2.0 §SF，M2a 起）

探測動作（`xr-probe`、`v4l2-probe`、`decode-bench`）的用法、選項、結束碼與 JSON 見
`docs/steam_frame_client.md` §6；這裡只列 log 行格式。探測輸出的每一行同時印到 stdout（不帶 tag）。

**log 位置**：Linux release 建置寫 `~/.cache/VipleStream/VipleStream/logs/`；Flatpak 是
`~/.var/app/io.github.finaltwinsen.VipleStream/cache/VipleStream/VipleStream/logs/`；Windows 同本文件開頭。
探測動作另寫 `probe-<action>-<epochMs>-<pid>.log`，只把 `probe-*.log` 修到最新 50 份，**不動**
`VipleStream-*.log`（連跑幾十次探測也不會把串流 log 擠掉）。

### 5b-3. SteamVR driver（M1b V4 起；寫在 SteamVR 的 `vrserver.txt`，不在 sunshine.log）

```
Loaded server driver viplestream … \config\steamvr\<ver>\viplestream\bin\win64\driver_viplestream.dll   ← SteamVR 自己的行
driver viplestream implements interfaces … IVRDriverDirectModeComponent_009 …                  ← 必須有 _009
[VIPLE-VR-DRV] init ver=<ver> ms=<N> …                                                          ← Init 一律回 None（不讓 SteamVR 進 safe mode）
[VIPLE-VR-DRV] ipc handshake gen=<G> … armed=<0|1>  →  hmd leave-standby
[VIPLE-VR-DRV] pipe lost gen=<G> reason=<…> -> standby (hmd stays connected)                   ← server 消失；pose 約 33 ms 內失效
[VIPLE-VR-DRV] swapset create pid=<pid> fmt=<dxgi> size=<W>x<H> samples=<n> …                  ← P-B3：app 每眼一組、vrcompositor 系統層另有
[VIPLE-VR-DRV] timing 10s: … acqTimeout=<n> … keyedMutex=yes syncOk=<n> releaseFailed=<n>      ← U13：acqTimeout 應為 0
```

- 選卡（K22）：`<install>\config\steamvr\state.json` 的 `vrcompositor_adapter` 以 **LUID** 記錄 vrcompositor 實際使用的卡（`<host>` 上 IddCx 虛擬卡與實體卡同名，不能用名稱比對）。
- selftest T4（只在 `--vr-selftest --manual-steamvr`）：`T4.readback result=PASS|FAIL compared=<n> mismatch=<n> eyeDisagree=<n> … echoMatched=<n> maxAngleDeg=…`、`T4.consume … consumption=<比例> …`。已知限制：vrcompositor 自己重畫第 0 層（系統面板顯示時）的 session 圖案解不出來，`compared`≈0、`decodeFail` 高，不代表傳錯畫面（見 TODO V4）。

### 探測的開始與結束行

`cli/probeutil.cpp`（`beginProbe`／`finish`）。tag 依動作而定：`xr-probe` → `[VIPLE-XR-PROBE]`、`v4l2-probe` →
`[VIPLE-V4L2-PROBE]`、`decode-bench` → `[VIPLE-V4L2] bench:`。

```
[VIPLE-XR-PROBE] xr-probe: VipleStream <版號> <平台> pid=<pid>            ← 平台例：linux-arm64 flatpak、winnt-x86_64 plain、
                                                                          linux-x86_64 plain (running on arm64)
[VIPLE-V4L2] bench: decode-bench: VipleStream <版號> <平台> pid=<pid>
…
[VIPLE-V4L2-PROBE] v4l2-probe: done rc=<rc> json=<JSON 路徑>
[VIPLE-XR-PROBE] xr-probe: done rc=15 json=(none)                         ← JSON 寫不出來：只有原本 rc=0 才改成 15
```

每個探測一定有一對開始／結束行；只有開始行代表行程在探測中途被殺或崩潰（v4l2 ioctl、`xrCreateInstance` 卡死時看
最後一行停在哪）。stdout 的第一行是 `<action>: VipleStream <版號> <平台>`，stderr 的最後一行是 `json: <路徑>`。

### `[VIPLE-SF-ENV]` —— 執行環境摘要

`backend/sfenv.cpp`（`logOnce`）、`streaming/session.cpp`。**只在 Linux 印**，其他平台是 no-op。

```
[VIPLE-SF-ENV] pkg=flatpak id=<app-id> runtime=<runtime ref> flatpak=<ver> [commit=<app commit 前 12 碼>] app=<版號> arch=<build>/<current>[(emulated)] host-os=<ID>/<VERSION_ID>[,variant=…][,build=…] runtime-os=<…> kernel=<…> devices=<…> fs=<…>
[VIPLE-SF-ENV] pkg=appimage|plain app=<版號> arch=<build>/<current>[(emulated)] os=<ID>/<VERSION_ID>[,…] kernel=<…>      ← 非 Flatpak 的第一行
[VIPLE-SF-ENV] session=<XDG_SESSION_TYPE> desktop=<XDG_CURRENT_DESKTOP> wayland=<WAYLAND_DISPLAY> display=<DISPLAY> qpa=<原始>[-><目前>] sdl=<原始>[-><目前>] gamescope=0|1(env=…,gamescope-0) steam-env=<N> video=[<node>:<sysfs name>,…] drm=[<driver>,…] xr-env=<XR_RUNTIME_JSON> xr-json=<路徑>(<source>,loader=0|1)|none
[VIPLE-SF-ENV] session: pkg=<…> arch=<…> os=<…> kernel=<…> gamescope=0|1 video=<N> drm=[<driver>,…] xr=<source>|none sdl-driver=<SDL 實際 driver>|none sdl-app-id=<SDL_APP_ID>|unset
```

- **何時印**：前兩行在啟動時印一次（SDL video driver 決定、DRM hooks 之後；`list` 動作不印）；探測動作在派發時印。
  `session:` 行接在 `[VIPLE-SESSION]` 之後，每場一次。
- **只收便宜的資訊**：讀 `/.flatpak-info`、os-release、sysfs、環境變數白名單、DRM driver 名稱、OpenXR runtime JSON
  路徑；不 dlopen Vulkan、不對 `/dev/video*` 做 ioctl。完整版（Vulkan 裝置、V4L2 QUERYCAP）只在探測 JSON 的 `env`。
- 未設的環境變數印 `unset`、空字串印 `""`；`qpa`、`sdl` 被 `main.cpp` 改寫過時印 `原始->目前`（例如 SSH 下
  `--help` 的 `unset->offscreen`）。
- `pkg`：`flatpak`／`appimage`／`plain`。`arch` 是 build／current，`(emulated)`＝在 qemu 之類的模擬下執行。
- `host-os`／`runtime-os`：Flatpak 內 host 的 `/run/host/os-release` 與 runtime 的 `/etc/os-release`。
- `devices`、`fs`：`/.flatpak-info` 的 `[Context]`（`fs` 截到 240 字）。
- `gamescope=1`：`GAMESCOPE_WAYLAND_DISPLAY` 有值，或 `$XDG_RUNTIME_DIR/gamescope-0` 存在（Flatpak 要有
  `xdg-run/gamescope-0` 權限才看得到）。
- `steam-env`：`STEAM*` 環境變數的**個數**，名稱與值都不印（可能含 token）。
- `video`：sysfs 的 video4linux 節點（最多 16 個），不開裝置；`drm`：`WMUtils::getDrmDriverNames()`，Frame 預期 `msm`。
- `xr-json`：`XrRuntimeJson::resolveActive()` 的結果；`loader=0`＝OpenXR loader 自己找不到（Flatpak 內 host 的
  `~/.config/openxr`），`xr-probe` 會在行程內設 `XR_RUNTIME_JSON`。
- `sdl-driver`、`sdl-app-id` 在 session 開場時才現取（SDL 自己的值，不是 env）。Wayland 下 `sdl-app-id` 應等於 desktop id
  （Flatpak 是 app-id，其他是 `viplestream`），xcb 下是 `viplestream`；其他 Qt 平台（eglfs 等）`main.cpp` 不設，通常是 `unset`。
- **連帶變化**：Linux 啟動時 SfEnv 就會呼叫 `WMUtils::getDrmDriverNames()`（只開 render node，不牽涉 DRM master），
  所以 `[VIPLE-LNXFE] DRM driver probe` 從 M2a 起提前在啟動時出現，判讀 F6 時不要當成 session 內的探測。

### `[VIPLE-V4L2-PROBE]` —— `v4l2-probe`

`cli/v4l2probe.cpp`、`streaming/video/v4l2/v4l2caps.cpp`

```
[VIPLE-V4L2-PROBE] sysfs: N video node(s): video0="<name>" …
[VIPLE-V4L2-PROBE] nodes: N /dev/video* node(s), M /dev/media* node(s)[ (inside Flatpak)]
[VIPLE-V4L2-PROBE] device: <path> (--device)
[VIPLE-V4L2-PROBE] dev=<path> open=ok driver=<…> card="<…>" bus=<…> version=<x.y.z> caps=0x<…> m2m=1 mplane=0|1 legacyCaps=0|1 role=decoder|non-video-decoder|encoder|converter[ kind=stateful|stateless|mixed|unknown] coded=[<fourcc>,…][ otherCoded=[<fourcc>,…]]
[VIPLE-V4L2-PROBE] dev=<path> open=ok driver=<…> … m2m=0 role=not-m2m
[VIPLE-V4L2-PROBE] dev=<path> open=failed <errno> <stat> groups=[…][ hint=…]
[VIPLE-V4L2-PROBE] dev=<path> open=ok querycap=<errno> (not a V4L2 device?) <stat>
[VIPLE-V4L2-PROBE] dev=<path> coded=<fourcc>(<名稱>) flags=<…> size=<…> capture=[<fourcc>,…] profiles=[…] levelMax=<…> vr90=ok|no(need <level>) vr72=ok|no(need <level>) tiers=[…] minBufCapture(static)=N|?
[VIPLE-V4L2-PROBE] dev=<path> coded=<fourcc>(<名稱>) … s_fmt=<errno>|substituted(<fourcc>)
[VIPLE-V4L2-PROBE] dev=<path> decoderCmd stop=<…> start=<…> g_parm output=<…> capture=<…> queueCaps output=[…]|skipped(legacy-caps) capture=[…]|skipped(legacy-caps)
[VIPLE-V4L2-PROBE] dev=<path> header-test codec=<名稱> 1280x720 sourceChange=<ms>[(nextAu)]|none(<ms>ms) capture=<fourcc> WxH visible=<…> minBufCapture=N|? [expbuf=<…>] firstFrame=<ms> out=immediate|nextAu|drain errFlag=0|1 result=ok|frame-with-error-flag|<失敗原因>
[VIPLE-V4L2-PROBE] dev=<path> header-test codec=<名稱> 1280x720 … firstFrame=none[(<原因>)] result=no-frame|source-change-during-capture
[VIPLE-V4L2-PROBE] dev=<path> header-test codec=h264|hevc skipped: <原因>
[VIPLE-V4L2-PROBE] m2m decoders: N: <path> driver=<…> kind=<…>[(legacy-caps)] coded=[…] statefulVideo=[<codec>,…]; …
[VIPLE-V4L2-PROBE] stateful video decoder: yes (<path>=[<codec>,…]; …)                     ← 有 decoder 時才印這行或下一行
[VIPLE-V4L2-PROBE] stateful video decoder: no (no H264/HEVC/AV1/VP9 stateful decoder; the app's v4l2m2m path cannot use these decoders)
[VIPLE-V4L2-PROBE] no m2m decoder visible (<原因>)                        ← rc=13
[VIPLE-V4L2-PROBE] v4l2-probe: not available in this build (V4L2 needs Linux with <linux/videodev2.h>)   ← rc=10
```

- **decoder**＝m2m，且 OUTPUT 至少有一個 compressed 的**視訊**格式（`role=decoder`）。JPEG、MJPEG、PJPG、JPGL、DV、MPEG
  多工容器這類非視訊格式（kernel 會自動加 COMPRESSED 旗標）不算，列在 `otherCoded=[…]`；只有這類格式的節點是
  `role=non-video-decoder`（例如 mtk-jpeg、mxc-jpeg），不算 decoder。`legacyCaps=1`＝只宣告舊式 CAPTURE＋OUTPUT 旗標的 driver。
- `kind` 只看已知 codec 表內的格式：`stateful`、`stateless`（Request API）、`mixed`；只有表外格式（例如還沒查證的 AV1
  fourcc）時是 `unknown`，不預設成 stateful。`kind=stateless` 也算 decoder，但 app 的 v4l2m2m 路徑只能用 stateful。
- **app 能不能用，看 `stateful video decoder:` 那一行**（JSON 的 `summary.hasStatefulVideoDecoder`）：只有 H.264／HEVC／
  AV1／VP9 的 stateful decoder 算 `yes`；`statefulVideo=[…]` 是每個 decoder 的清單。stateless decoder 與 vicodec 的 FWHT
  讓結束碼是 0，這行卻是 `no`。G-α 的替代路徑條件與 PoC-0 看這一行，**不看結束碼**。rc=13 時沒有這行。
- `queueCaps`：`REQBUFS(0)` 只對 decoder 做；舊式旗標判定的節點印 `skipped(legacy-caps)`（`exclusive_caps=0` 的
  v4l2loopback 會被舊式旗標判成 m2m，REQBUFS 會打斷正在讀它的程式）。
- `vr90`／`vr72`：和 3456×1728 所需 level 比對（HEVC 90 Hz 要 5.2、72 Hz 要 5.1；H.264 90 Hz 要 6.0、72 Hz 要 5.2）。
- header-test 的 `sourceChange`：STREAMON 前只入列一份 AU。`<ms>(nextAu)`＝1 秒內沒等到 `SOURCE_CHANGE`，補送第二份 AU
  之後才等到；`none(<ms>ms)` 的數字是總共等了多久（1000 或 2000），之後照舊式流程繼續設定 CAPTURE（JSON
  `captureSetupWithoutSourceChange`）。
- header-test 的 `out=`（JSON `firstFrame.outputAfter`）：
  - `immediate`＝只有一份 AU、不 drain 就吐幀。**只有這個代表沒有額外的幀延遲**。
  - `nextAu`＝補送第二份 AU 才吐幀，串流時每幀至少多等一個幀間隔（90 Hz 約 11 ms）。第二份和第一份是同一張 IRAP
    （IDR／CRA），不是 P 幀，所以這是**樂觀的下限**。等 `SOURCE_CHANGE` 時已經補送過第二份的話，第一段出幀也記這個。
  - `drain`＝送 `DEC_CMD_STOP` 之後才吐幀；串流路徑從不 drain，照現況不能用。
  - `firstFrame=<ms>` 從 CAPTURE STREAMON 起算，不是單幀解碼延遲。
- `firstFrame=none(…)` 的括號：`(drained, no frame)`＝`DEC_CMD_STOP` 成功送出後仍沒有幀；`(source change, capture needs
  reconfig)`＝CAPTURE 串流中途收到 `SOURCE_CHANGE` 又拿到空的 LAST buffer，是解析度變更、CAPTURE 要重新配置；
  `(source change during capture)`＝收到 `SOURCE_CHANGE`、沒有 LAST；`(LAST without drain)`＝沒送 `DEC_CMD_STOP` 卻拿到
  LAST；沒有括號＝三段都逾時。沒有幀而且中途收到 `SOURCE_CHANGE` 時 `result=source-change-during-capture`，否則 `no-frame`。
- **真正的每幀延遲不看 header-test**（測試幀的 SPS 也不是 Sunshine 的串流參數）：以 `decode-bench <Sunshine 錄的樣本>
  --fps 90` 的 `lat p99` 與 `eagainMax` 為準（下一節）。每個 header-test 最壞約 4 秒。**header-test 失敗不改結束碼。**
- `open=failed`：看 errno、`<stat>`（mode／owner）與 `groups=`，SSH 下沒有 active seat 時可能拿不到 uaccess；
  `no m2m decoder visible` 的括號內會說明是沒有節點、開檔失敗、節點都不是 decoder，還是只有非視訊格式（JPEG 等）。

### `[VIPLE-V4L2] bench:` —— `decode-bench`

`cli/decodebench.cpp`。tag 是 `[VIPLE-V4L2]`，每一行都以 `bench: ` 開頭。

```
[VIPLE-V4L2] bench: input file=<檔名> codec=h264|hevc|av1 container=annexb|ivf aus=N keyframes=N WxH bytes=N[ truncatedTail=1]
[VIPLE-V4L2] bench: compare-sw reference=<decoder> frames=N grid=WxH step=N              ← --compare-sw
[VIPLE-V4L2] bench: candidate <label> (sw|v4l2m2m|hwaccel|hw): ok|<失敗原因>              ← auto 時逐一列；指定 decoder 只在失敗時印
[VIPLE-V4L2] bench: decoder=<label> kind=<kind> threads=N open=X.Xms[ outBufs=N capBufs=N]
[VIPLE-V4L2] bench: map-vulkan device="<GPU>"                                            ← --map-vulkan
[VIPLE-V4L2] bench: progress sent=N/M out=N errFlag=N stall=N                            ← 每 10 秒
[VIPLE-V4L2] bench: first-frame WxH fmt=<pix_fmt> sw=<sw_format>|- drm=<layer>[+<layer>…]/mod=0x<…>/objs=N/pitch=<p>[,<p>…]|- afterMs=X.XX
[VIPLE-V4L2] bench: decoder=<label> out=<pix_fmt> drm=<同上> WxH fps=N n=N lat p50=… p95=… p99=… max=… errFlag=N stall=N eagainMax=N
[VIPLE-V4L2] bench: sendBlock p50=… p95=… max=… sendEagain=N sendErr=N recvErr=N ptsMismatch=N skipped=N flushes=N throughput=X.Xfps wall=Nms
[VIPLE-V4L2] bench: drop|corrupt|error=<k> errFrames=N missing=N stallMs=X.X healFrames=N|n/a (psnr>40dB) idrNeeded=0|1|inconclusive|n/a[ merged=N]
[VIPLE-V4L2] bench: ... N more event(s) in the JSON                                       ← 事件行最多 20 行
[VIPLE-V4L2] bench: events=N selfHealed=N needIdr=N healMax=N healMean=X.X inconclusive=N merged=N   ← 有事件且帶 --compare-sw
[VIPLE-V4L2] bench: compare-sw ref=<decoder> compared=N psnr min=… mean=… below40=N failures=N[ firstFailure=…]
[VIPLE-V4L2] bench: map-vulkan device="<GPU>" mapped=N map p50=… p95=… max=… fail=N unsupported=N
[VIPLE-V4L2] bench: ABORTED <原因>                                                       ← rc=14
[VIPLE-V4L2] bench: error: <訊息>
[VIPLE-V4L2] bench: unavailable (this build has no FFmpeg)                               ← rc=10
```

- 延遲（ms）＝送出到收到同一幀；`sendBlock`＝`send_packet` 本身的阻塞時間。`eagainMax`＝送出後、下一張幀輸出之前
  累積的最長封包數（約等於管線深度）；`stall`＝有 `--fps` 時超過 3 個幀間隔、否則超過 100 ms 沒有輸出。
- **量每幀延遲一律帶 `--fps`**（Frame 用 90）：連發模式下一個封包立刻就送，看不出「要等下一個 AU 才出幀」的延遲。
  header-test 的 `out=` 判讀要拿這一輪的 `lat p99`、`eagainMax` 佐證。
- `drm=`：每個 layer 的格式都列（VAAPI 匯出的 NV12 是 `R8+GR88` 兩個 layer，只看第一個會誤判成單平面），`mod` 是第一個
  object 的 modifier，`pitch` 依序列出每個 plane。硬體幀不是 DRM_PRIME 時（例如 VAAPI）另外 `av_hwframe_map` 一次只為描述；
  拿不到描述（SW 幀、匯出失敗）時印 `-`。完整描述在 JSON 的 `firstFrame.drm.layers[]`（`drmVia` 記來源）。
- 事件行的 `<k>` 是**檔內幀號**（0 起算）。`healFrames`＝事件後到 PSNR 穩定回到 40 dB 以上的幀數（要 `--compare-sw`，
  否則 `n/a`）；`idrNeeded=1`＝視窗內沒恢復，或是靠 keyframe 才恢復。PoC-3b 看這組。
- **事件視窗**：注入事件（drop／corrupt）的視窗到下一個注入事件（或結尾）為止，不被中間的 error 事件切斷。視窗內、
  PSNR 癒合點之前（沒癒合就是整個視窗）的 error 事件是同一次注入的後果，併進注入事件：不另成一行、不計入
  `selfHealed`／`needIdr`，事件行尾的 `merged=N` 是併進來幾個。癒合之後才出現的 error 事件自己成一行。error 事件的
  視窗到下一個事件（任何種類）為止。
- `idrNeeded=inconclusive`：視窗被下一個事件截斷時 PSNR 還沒回到 40 dB，判斷不出再等下去會不會自己好。彙總行另計
  `inconclusive`，不算進 `needIdr`。`--drop-every` 很密、decoder 又不會自己好時大多是這個，判讀時一起看 JSON 的
  `windowFrames`。
- 彙總行：`events` 是全部事件數（含被併的），`merged` 是被併的個數；`selfHealed`、`needIdr`、`inconclusive` 只算沒被併的
  事件。
- JSON：`events[]` 依全域幀號 `g` 排序（最多 2000 筆，超過時 `eventSummary.jsonTruncated=true`）。被併的 error 事件緊接在
  所屬的注入事件之後，只帶 `mergedInto`（注入事件的 `g`），沒有視窗欄位；注入事件帶 `mergedErrors`。視窗被截斷時
  `idrNeeded` 是 `null`、`truncated` 是 `true`；沒有 PSNR 時 `healFrames`、`idrNeeded` 也是 `null`，但沒有 `truncated`。
  `eventSummary` 另有 `merged`，`--compare-sw` 生效時另有 `inconclusive`。
- `--compare-sw`、`--map-vulkan` 會擾動延遲與吞吐，量延遲的那一輪不要帶。

### `[VIPLE-XR-PROBE]` —— `xr-probe`

`cli/xrprobe.cpp`、`streaming/xr/xrprobe_instance.cpp`（只在 `CONFIG+=openxr` 建置）

```
[VIPLE-XR-PROBE] env: <SfEnv 兩行合併的摘要>
[VIPLE-XR-PROBE] env: vulkan[i] "<GPU>" type=<…> driver=<…> <driverInfo> api=<x.y.z>      ← 或 env: vulkan unavailable (<原因>)
[VIPLE-XR-PROBE] override: XR_RUNTIME_JSON=<path> (--xr-runtime-json, in-process only)
[VIPLE-XR-PROBE] override: XR_LOADER_DEBUG=all (--loader-debug; loader output goes to stderr)
[VIPLE-XR-PROBE] xr-probe: this build has no OpenXR (CONFIG+=openxr)                     ← rc=10
[VIPLE-XR-PROBE] xr-probe: --session (B stage: …) requires M3a (XrContext); …              ← rc=12
[VIPLE-XR-PROBE] runtime-json candidate: <path> source=<…> exists=0|1 readable=0|1[ symlink-><target>[ (dangling)]]
[VIPLE-XR-PROBE] runtime-json: N candidate path(s) checked, M present
[VIPLE-XR-PROBE] runtime-json: active=<path> source=<…> name="<…>" library=<path> libExists=0|1 loaderWouldFind=0|1[ parseError=…]
[VIPLE-XR-PROBE] runtime-json: none found (no active_runtime json in any searched path)
[VIPLE-XR-PROBE] runtime-json: set XR_RUNTIME_JSON=<path> in-process (host-config (sandbox XDG_CONFIG_HOME hides host ~/.config/openxr))
[VIPLE-XR-PROBE] loader: OpenXR headers <x.y.z>
[VIPLE-XR-PROBE] api-layer: <name> spec=<x.y.z>
[VIPLE-XR-PROBE] ext: <name> v<N>[ [enabled]]
[VIPLE-XR-PROBE] valve-ext: <name>
[VIPLE-XR-PROBE] required: vulkan_enable2=0|1 vulkan_enable=0|1 convert_timespec=0|1 -> tracking=thread|frameloop graphics=enable2|enable|none
[VIPLE-XR-PROBE] instance: runtime="<name>" ver=<x.y.z> api=<1.1|1.0> exts=N enabled=N     ← 或 instance: xrCreateInstance failed (<XrResult>)
[VIPLE-XR-PROBE] timespec: roundtrip=<ns>ns xr-mono=<ns>ns toXr=<us>us toTs=<us>us
[VIPLE-XR-PROBE] system: name="<…>" vendor=0x<…> maxSwapchain=WxH layers=N orient=0|1 pos=0|1 eyeGaze=<…>   ← 或 system: HMD unavailable (<XrResult>); …
[VIPLE-XR-PROBE] viewconfig: types=[…]
[VIPLE-XR-PROBE] viewconfig PRIMARY_STEREO: views=N <每眼建議尺寸> fovMutable=0|1|? blend=[…]
[VIPLE-XR-PROBE] vulkan: path=enable2|enable minApi=<…> maxApi=<…> device="<GPU>" vendor=0x<…> id=0x<…> api=<…> (index i of N)
[VIPLE-XR-PROBE] note: refresh rate, FOV and reference spaces need a session (xr-probe --session, M3a)
[VIPLE-XR-PROBE] instance: destroyed (<XrResult>)
[VIPLE-XR-PROBE] runtime-lib: dlopen(<path>) failed: <dlerror>[ elf=<machine>]           ← rc=13 時的診斷
[VIPLE-XR-PROBE] runtime-lib: dlopen(<path>) ok, xrNegotiateLoaderRuntimeInterface=0|1 (library loads; the failure is inside the runtime or its IPC)
[VIPLE-XR-PROBE] result: no usable OpenXR runtime (rc=13; …)
[VIPLE-XR-PROBE] result: runtime reported INSTANCE_LOST/RUNTIME_FAILURE during the probe (rc=14)
```

- `required:` 行是 PoC-2 的核心：`convert_timespec=0` 時 tracking 要改用 frameloop 模式；`graphics=none` 表示 runtime 兩種
  Vulkan 擴充都沒有（G-β 的替代路徑）。
- `runtime-json: set XR_RUNTIME_JSON=… in-process` 只在 loader 自己找不到、而 host 路徑找得到時出現；使用者自己設了
  `XR_RUNTIME_JSON` 就不會改。
- rc=13 時先看 `runtime-lib:`：`dlopen … failed` 是 runtime 的 `.so` 在沙箱內載不起來（缺函式庫或架構不符，PoC-F 的答案）；
  `dlopen … ok` 代表失敗在 runtime 內部或它的 IPC。要 loader 的細節時加 `--loader-debug`（輸出在 stderr）。

### `[VIPLE-BSDUMP]` —— `stream --dump-bitstream`（dev-only）

`streaming/video/bitstreamdump.cpp`

```
[VIPLE-BSDUMP] armed base=<路徑，不含副檔名> ext=auto|<ext> (dev-only, not persisted)
[VIPLE-BSDUMP] open <path> codec=h264|hevc|av1(0x<videoFormat>) WxH
[VIPLE-BSDUMP] progress frames=N bytes=N                                                 ← 每 10 秒
[VIPLE-BSDUMP] closed frames=N bytes=N path=<path> reason=decoder-destroyed|format-change|set-path|write-error
[VIPLE-BSDUMP] extension .<ext> does not match codec <codec>; writing .<ext> instead
[VIPLE-BSDUMP] cannot open <path>: <原因>
[VIPLE-BSDUMP] cannot write IVF header to <path>: <原因>
[VIPLE-BSDUMP] write failed on <path>: <原因>
[VIPLE-BSDUMP] dump disabled for the rest of this process
[VIPLE-BSDUMP] unsupported videoFormat 0x<…>
```

- `reason=format-change` 之後會再有一行 `open <base>-<n>.<ext>`（decoder 重建、格式或解析度改變）。
- 開檔或寫檔失敗就整個停用（`dump disabled …`），不會每幀刷錯誤。一般 session 沒帶 `--dump-bitstream` 時一行都不會有。

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
