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

- `[VIPLE-RES] Stream config requested: WxH; host advertises max: WxH (switchable max|current mode)`
  （`session.cpp` 的 §H.4 clamp）。`switchable max`＝host 回了 `<DisplayModeSwitchable>1`，max 是 host
  串流前可切到的最高模式；`current mode`＝舊 host 或沒開 dd，max 是 host 目前模式。
- `[VIPLE-RES] auto display mode: resolution=auto|manual fps=auto|manual local=WxH@Hz (...) host=... -> stream WxH@N, bitrate K kbps (...)`
  —— §H.4-AUTO：「自動（本機螢幕最佳）」解析結果（本機串流視窗所在螢幕的桌面模式；XR 桌面固定 2560x1440）
  與 host 可切換最高模式取小，以及位元率是否跟著重算（`default, recomputed`／`user setting kept`）。
- `[VIPLE-RES] auto fps (XR desktop): runtime X Hz, host max N Hz -> stream WxH@N (was M)` —— XR 桌面自動
  更新率在 XrContext bring-up 後（/launch 之前）換成 runtime 實際顯示更新率。
- server 端：`[VIPLE-RES] /serverinfo advertising switchable max of \\.\DISPLAYn: WxH @ NHz (K modes[, cached])`
  （開了 dd、`output_name` 對得到已啟用裝置；列舉結果快取 10 秒）或
  `[VIPLE-RES] /serverinfo advertising primary display current mode: WxH @ NHz`（原本行為）；目標裝置未啟用時先印
  `dd target '...' not active/enumerable; falling back ...`。
- server 端：`[VIPLE-RES] dd refresh snap: WxH @ XHz not in \\.\DISPLAYn mode list; setting display to NHz (encode fps unchanged)`
  —— §H.4-SNAP：要求的更新率不在目標顯示器該解析度的模式表裡（XR runtime 72／20 Hz 等），改把顯示器設成
  ≥ 要求值的最小可用 Hz（沒有就最高），否則 Windows 設模式失敗（1610）連解析度都不切；編碼 fps 不變。
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
| `[VIPLE-UI-NAV]` | `gui/sdlgamepadkeynavigation.cpp` | 介面（不是串流中）的手把導覽，§UI-SCROLL（2026-10-11）。每次啟用手把導覽（啟動、串流或 VR 子行程結束回到介面）各印一行：`enabled: N gamepad(s): <名稱…>` = 介面行程看到的手把；`polling started (window has focus)` = 視窗拿到焦點、開始輪詢（沒有這一行＝視窗沒有焦點，手把在介面裡不會有反應）；`first gamepad input: button N (uiNav=0\|1)` = 第一個按鍵進來了（`uiNav=1`＝設定頁的 Tab 導覽模式）；`right stick scroll in use (axis=N)` = 第一次用右搖桿捲設定頁。另有 `gamepad added: <名稱> (now N)` = 啟用之後才接上的手把。使用者回報「手把在介面裡沒反應」時，先看有沒有前兩行、手把的名稱是什麼 |
| `[VIPLE-HID]` | `backend/hidprobe.cpp` + 四個閘控點（`gamepad.cpp` getUnmappedGamepads / `sdlgamepadkeynavigation.cpp` enable / `input.cpp` SdlInputHandler ctor / `sc_hid.cpp` prepare） | 啟動前與每場開始前的 HID 探測（§HID-PROBE，2026-09-19 Puck 卡死事故）。`probe (startup\|refresh) OK: N HID interface(s) responded in X ms (slowest VID:PID Y ms, limit 1000 ms)` = 正常；`UNRESPONSIVE <name> vid= pid= stuck_at=<CreateFile\|HidD_GetAttributes\|HidD_GetProductString\|enumeration> path=` = 該裝置無回應 → 手把偵測、手把 UI 導覽、串流手把與 SC-HID 停用，各閘控點各印一行 `skipped/refused`；重插裝置後下一次 refresh（開串流／回 UI）自動恢復，不必重啟。**刻意比 SDL 保守**：對所有 HID 介面查字串，卡死的普通鍵鼠也會觸發。`(simulated via VIPLE_HID_PROBE_SIMULATE_HANG, dev-only)` = 測試注入（`VID:PID` 直接注入；`VID:PID:stall` 對實體介面真的卡在字串查詢；`enumerate:stall` 整個列舉逾時），不是真故障 |
| `[VIPLE-CTRL]` | `ControlStream.c`（common-c，Q／A 兩份相同） | 控制通道的防衛，四種行都代表防衛生效（拒送、改送 IDR 或忽略），不是崩潰。**§F2**（`sendMessageEnet` 加密分支）：`§F2 payload too large (ptype=0x%04x len=%d) — limit 251, dropped (count=N)` ＝ 明文超過 256 B stack buffer（payload 上限 251 B）被拒送、回傳 false；前 10 次都印、之後每 1000 次一筆。現有呼叫端都是固定長度（最大是 input 132 B），**1.5.x 正常 log 不該出現**；出現代表有新呼叫端（2.0 VR 送出 API）組出超長封包。**§F1-DBG-ASSERT**（幀號狀態倒帶，三行都改送 IDR）：`Invalid frame loss range (%u to %u) — requesting IDR frame instead`（`queueFrameInvalidationTuple`，每秒最多 1 筆）、`Non-monotonic RFI ranges (start %u, last end %u) — requesting IDR frame instead`（`referenceFrameControlFunc` 聚合時）、`connectionSawFrame: stale frame=%u (lastSeen=%u) — ignored`（倒退幀號不計入統計，每秒最多 1 筆）。這三行與 `[VIPLE-DEPACK] notifyFrameLost: stale` 同時出現＝§FRZ-WATCHDOG 哨兵重置後採納了 failback 殘留的 stale 幀；舊版 Android（debug native、assert 生效）會在這裡 SIGABRT 閃退 |
| `[VIPLE-MPQUIC] §M01-C …` | `ControlStream.c`（common-c，Q／A 兩份相同） | §M01-C ENet 斷線後的重連。以前 §Q-ENET-RECONNECT 只在找到有線網卡時才重連，只有 Wi-Fi 的裝置（手機、筆電）斷線後永遠不重連、120 秒後放棄輸入。現在改看「到 host 的路由是否可用」：`route to host available (local=…)` / `route to host lost` / `route to host now via local=… (was …) — backoff reset`（換了出口介面就重設退避）；`host rejected ENet reconnect (stale peer?)` 是 host 端舊 peer 還在，會繼續重試；重連後約 5 秒印 `ENet healthy rtt=…`（或 `NOT healthy`）。`ENet still down after 120 s … input via QUIC datagram (unreliable), still retrying` 代表輸入暫時走 QUIC datagram、不保證送達；`QUIC transport dead while ENet down — terminating` 是兩條都斷、結束連線 |
| `[VIPLE-MPQUIC] §Q.path-recovery: thread …` | `QuicTransport.c`（common-c，Q／A 兩份相同） | §M01-E 路徑復原執行緒的生命週期：`thread started` → `thread exiting` → `thread joined`。**每個用過 QUIC 的 session 收尾時都要有一行 `thread joined`**，包括 QUIC 閒置逾時或 path 全滅走 `§K.13 IO loop bail` 的情況。2026-09-26 以前 bail 路徑會跳過 join：debug 版在 `Platform.c` 的 `activeThreads == 0` assert 中止，release 版洩漏執行緒。有 `thread started` 卻沒有 `thread joined` 就是 regression |
| `[VIPLE-MPQUIC] §MP-ONLINK …`、`§MP-BINDDEV …` | `Connection.c`、`QuicTransport.c`、`PlatformNetIf.c`（common-c，Q／A 兩份相同） | 多路徑配對。`§MP-ONLINK: if <n> '<name>' /<prefix> shares subnet with peer <i> (-2=primary) — pairing only that peer (added\|failed)`：server 有位址和這張網卡同子網路時只配那一個（例：Steam Frame 的 `wlanap` 10.35.78.1/24 只配 host 適配器的 10.35.78.x），不再拿主位址、Tailscale、公網位址去試。以前 Linux（weak host）會形成上行走家用 Wi-Fi、下行走適配器的不對稱路徑，還吃光 picoquic 的 path ID，真正的對稱位址落得 `probe_new_path failed (1088)`（PATH_ID_BLOCKED）。`§MP-BINDDEV: SO_BINDTODEVICE '<name>' failed (errno N)`（僅 Linux／Android）＝ subflow socket 綁網卡失敗、退回舊行為；注意綁定後 Linux 對沒有路由的目的地會假設 on-link，connect() 仍會通過，所以配對正確性靠 §MP-ONLINK |
| `[VIPLE-MPQUIC] §MP-ROUTECHK …` | `QuicTransport.c`（common-c，Q／A 兩份相同；只在非 Android 的 Linux 生效） | `§MP-ROUTECHK: if <n> '<name>' has no route to peer in main table; skipping path`：主路由表（/proc/net/route）裡這張網卡沒有任何路由通往 peer（連預設路由都沒有），不建路徑。例：Steam Frame 的 host 適配器沒連上時，wlanap 只剩 10.35.78.0/24，對 server 的 LAN／Tailscale／公網位址都會被跳過；以前這些組合會變成死路並吃掉 path ID。VPN 介面（Tailscale 用 table 52）與讀不到 /proc/net/route 時不套用 |
| （無專屬 log）§MP-VERIFIED | **host** `quic_server.cpp` | 影像路徑選擇：path 1+ 必須 `first_tuple->challenge_verified`（路徑驗證完成）才能載視訊或被緊急升級；第一次選路（`_lastVideoPath == -2`）直接用健康的 path 0。觀察點是既有的 `§Q-PATH-SWITCH: bestVideoPath -2(...) -> 0(...)`：多路徑 session 的第一次切換應該是到 0；若是到其他 path 就是 regression。起因：Steam Frame 上未驗證的新路徑（初始 cwin 69 KB）贏過剛被 §K.14 重置到 59 KB 的 path 0，3 秒丟 1680 包 |
| `[VIPLE-MPQUIC] §Q-ACCT …` | **host** `quic_server.cpp`、client `QuicTransport.c`（common-c，Q／A 兩份相同） | QUIC 影像逐序號對帳（2026-09-29 用來抓出 §Q-MTU-PIN）。client：`§K.10 path[i] … rxPkts=N`（socket 取得的原始封包）、`§Q-ACCT client: pqRecv= pqSent=`（picoquic 成功處理／送出）、`§Q-ACCT skip seq=S count=N sinceDeliver=… buffered=…`（抖動緩衝逾時跳過的序號，每行程最多 30 行）。host：`§Q-ACCT server <addr>: pqSent= pqRecv= retx= spurious=`（5 秒一行）、每 10 秒 `§Q-ACCT dgram: queued-only= acked= lost= spurious=`＋`§Q-ACCT lost seqs:`＋`§Q-ACCT queued-never-acked seqs:`（picoquic datagram_acked／lost／spurious 回呼逐序號記帳）。判讀：client 的 skip 序號若出現在 host 的 queued-never-acked＝資料報排入 picoquic 後沒被送出；出現在 lost＝真的網路遺失；host 標 acked＝client 內部丟掉。queued-only 少量（1～2）是統計當下仍在途 |
| `[VIPLE-MPQUIC] §MP-SINGLE-AVAIL: path id=… became available while video is on path N — set back to backup` | **host** `quic_server.cpp` | server 端只讓影像路徑是 available（根因已由 picoquic fork §MP-PERPATH-WAKE／§MP-PERPATH-DG 修掉，此守衛保留為保守設計）。**原缺陷**：兩條以上 available 時，影像所在（非最低 RTT）路徑的 per-path queue 會被餓死——路徑有被選中、pacing／cwin 都放行，卻組不出資料封包（inFlight=0、佇列一路漲；client 16 s 無幀後自行斷線）。新路徑本來就設 backup，但 client 之後的 PATH_AVAILABLE 會蓋掉，這裡設回去。寬限：該路徑持續 available ≥ 0.5 s、影像路徑一直非 backup、距上次非初次換路 ≥ 3 s（client §Q-LOSS-DEMOTE 同時送 AVAILABLE＋BACKUP，先到的不可被立刻打回）。曾試過 server 主動把 backup 改 available 做探測（§MP-PROBE）與尊重 client 狀態（§MP-PEER-STATUS），都會觸發此缺陷，已移除 |
| `[VIPLE-MPQUIC] §MP-SCHED-DIAG video path N queue>500 with nothing in flight \| i<k> id= bk= cwin= inTr= poll= paced= cong= sel= …` | **host** `quic_server.cpp` | 上述餓死特徵（影像路徑佇列 > 500 且 inFlight=0）出現時才印各路徑 picoquic 排程計數增量，10 s 節流。picoquic fork 修正後不應再出現；出現就是 §MP-PERPATH-WAKE／DG regression |
| （無專屬 log）§MP-PERPATH-WAKE／§MP-PERPATH-DG | picoquic fork `sender.c`、`paths.c` | 多 available 路徑時 per-path datagram 佇列的排程修正：`picoquic_prepare_packet_ex` 設定喚醒時間前，若有可用、已驗證、未壅塞路徑的 per-path 佇列還有資料，就在 pacing 允許時立即喚醒；`picoquic_sort_available_paths` 優先選這種路徑，ACK 搭在任一路徑的封包上。修正前排程器每次挑最低 RTT 路徑回 ACK、沒東西送就按那條路徑的計時器長睡（約 70 次 prepare/s），影像路徑佇列餓死 |
| `[VIPLE-MPQUIC] §MP-FAST-STALL: subflow N (if M) carried X Mbps, nothing received for Ys while subflow K is alive` | client `QuicTransport.c`（common-c，Q／A 兩份相同） | 正在載影像（上個統計週期 ≥ 1 Mbps）的路徑完全收不到 max(5×RTT, 1 s) 即判停滯（原 3 s），條件：有替代路徑（3 s 內有收的活躍路徑，或 ICMP 探測 25 s 內新鮮、且 10 s 內沒被標 INACTIVE 的 standby）、本路徑升級已滿 3 s。Frame 斷路接手 1.07～2.0 s（基準 3.87 s）；替代路徑的活性判斷不到時退回 3 s 規則（實測 3.1～3.5 s） |
| （無專屬 log）§MP-RTO2 | **host** `quic_server.cpp` | sticky 判定「目前影像路徑已死」由 `nb_retransmit > 0` 改為 `≥ 2`：Wi-Fi 上單次 RTO 常見，舊規則讓健康路徑換路＋IDR。真的斷線由 client §MP-FAST-STALL＋§MP-BACKUP-FLIP 處理 |
| （無專屬 log）§MP-MEASURE | **host** `quic_server.cpp` | §Q-STICKY-ESCAPE 的量測改為比例式：每 500 ms 視窗以 `bytes_sent/send_mtu` 估封包數、`nb_losses_found − nb_spurious` 算真實 loss；< 30 包的視窗記為「未量測」，≥ 2% 才算 lossy。候選須 6 視窗中 ≥ 4 個有樣本、0 個 lossy、loss 比例 ≤ 目前路徑一半、RTT 不差超過 1 ms、非 backup（§MP-SINGLE-AVAIL 下備援路徑沒有樣本，escape 目前等同停用；品質換路改由 client §Q-LOSS-DEMOTE／§MP-FAST-STALL 觸發）。`§Q-STICKY-ESCAPE: leaving path …` 行尾會印 `loss X‰ vs Y‰`。起因：閒置路徑沒樣本卻被當成「6 視窗零 loss」，M7 把影像切到 −73 dBm 的家用 Wi-Fi 後 ABR 從 17 Mbps 砍到 1.8 Mbps |
| `[VIPLE-MPQUIC] §MP-PURGE: dropped N stale video datagrams queued on old path id=…` | **host** `quic_server.cpp` | 影像換路時清掉舊路徑 per-path queue 裡的影像資料報並扣回 approxVQ。舊路徑被 client 標成 backup 後 picoquic 不再從它送 datagram，裡面的幾千個影像會一直讓 §Q-STALE 誤判而每 ~6 s 觸發安全閥 |
| `[VIPLE-MPQUIC] §MP-BACKUP-FLIP: video path id=… flipped to backup by peer — back to cnx-level queue, purged N video datagrams` | **host** `quic_server.cpp` | per-path 模式下目前影像路徑被 peer 的 PATH_BACKUP 翻成 backup：退回 cnx-level queue（3 s 後才可 resume）並清掉卡住的影像。client 的 REGAIN 駐留期間常見 |
| （無專屬 log）§MP-REJOIN | **host** `quic_server.cpp` | 單路徑期間 `_lastVideoPath = -1`（走 cnx-level）；第二條路徑加入時比照初次選路：path 0 未 demoted／backup 就留著，它暫時 RTO 時只有其他路徑已暖（cwin ≥ 64 KB）才讓出。觀察點：`§Q-PATH-SWITCH: bestVideoPath -1(...) -> <idx>` 若切到 cwin 很小的新路徑就是 regression。起因：M9 path 0 載了 30 s，家用 Wi-Fi 子流重新加入瞬間被選中（cwin 15 KB），凍結 16 s |
| `[VIPLE-MPQUIC] Connection closed (<transport\|application>) local=0x… remote=0x… app=0x… paths=N` | client `QuicTransport.c`（common-c，Q／A 兩份相同） | §MP-CLOSE-DIAG：QUIC 連線關閉原因（picoquic local／remote／application error）。三個都 0 且是 session 結束時＝正常關閉 |
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
[VIPLE-VR-FRAME] meta-miss: frame pts=… not in the meta map|has no 0x81 VR header — dropped (total N)   ← M4a R1：PCVR 接 XR 時才丟幀
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
- M4a R1：PCVR 走 XR projection 時（decoder 有 XrContext），查不到 meta 或沒有 0x81 header 的幀直接丟（沒有 render pose
  無法正確投影）；查到的 meta 以最終 pts（rtpTimestamp）放進 `VrRenderMetaRing` 交給 XrVideo。`--vr-emulate`（平面）不丟。

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
[VIPLE-VR-SESSION] accepted /launch caller=… eye=1728x1728 hz=90 … -> proto=1;packed=…;mode=stub (serverIntra=true safetyMs=2000)[ dispHz=120]
                                                                                    ← 行尾的 dispHz=<n>：client 帶了 vrDispHz 才有（半速串流：頭盔的顯示更新率和 hz 不同）；沒帶時這一行和以前一樣
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
[VIPLE-VR-UPLINK] 10s: arrivalMaxMs=… gt2T=N | ctrl loop bodyMaxUs=… cbMaxUs=… cbMaxType=0x<type> slowCb=N events=N
                                                                              ← 2026-10-05：server 收到 tracking 樣本的最大間隔與 > 2 個顯示週期的次數；控制執行緒
                                                                                 迴圈本體與單一 callback 的最長耗時（≥ 2 ms 算 slowCb）。和 driver 的 tracking 10s
                                                                                 arrivalMaxMs 對照：server 端就大＝網路／client；只有 driver 端大＝server 轉送
[VIPLE-VR-LINK] multi-link ready: video=auto|all|primary maxAgeMs=…               ← §VR-MULTILINK（2026-10-06）：session 協商了多連線
[VIPLE-VR-LINK] link N created: server <addr> video:<port> audio:<port> <-> client <addr> video:<port> audio:<port>
[VIPLE-VR-LINK] link N confirmed (both directions pass)                       ← 第一次收到帶 CONFIRMED 的 PING
[VIPLE-VR-LINK] link N cannot keep up - video paused on it for [at least ]S s (…) ← 飽和偵測：1 s 視窗內至少 24 個批次、其中 40% 以上送不完；
                                                                                 auto 時退回探測（at least）
[VIPLE-VR-LINK] link N measured X Mbps (need Y) - video starts on it          ← auto：探測中的連線連續 2 次量到夠快
[VIPLE-VR-LINK] link N measured faster than measurable (need Y Mbps) - video starts on it
                                                                              ← auto：頭盔回報 65535（整個探測幀同一瞬間到）連續 2 次
[VIPLE-VR-LINK] hold 10s: waits=N recovered=N expired=N heldPkts=N maxWaitMs=… limitMs=… | nack=N repaired=N gone=N
                                                                              ← client（§VR-MULTILINK-HOLD）：下一幀先到時，等落後那條連線補齊這一幀的次數、
                                                                                 等到／等不到的次數、排過隊的封包數、最長等待與上限；沒有等待也沒有回報就不印。
                                                                                 至少兩條連線實際在送影像、或 server 會補包時才會等。
                                                                                 nack／repaired／gone（§VR-LINK-REPAIR）＝送出的缺包回報數、回報之後收齊的
                                                                                 block 數、server 回「補不了」的次數。可以補包時 maxWaitMs 可能大於 limitMs：
                                                                                 等待期間前面的 block 收齊過，期限會從那一刻起再延一個幀週期（上限兩個）。
                                                                                 照 §VR-REPAIR-RTT 的規則跑過的視窗行尾多一段 ` | rtt …`（見 5b-5）
[VIPLE-VR-LINK] link features: ctrl=on|off repair=on|off repairRtt=on|off (client offered 0x.., server ctrl=… repair=… repairRtt=enabled|disabled)
                                                                              ← server（2026-10-07）：這個 session 協商出來的連線層功能
                                                                                 （§VR-LINK-CTRL／§VR-LINK-REPAIR／§VR-REPAIR-RTT；`vr_multilink_ctrl`、
                                                                                 `vr_multilink_repair`、`vr_multilink_repair_rtt` 開著、而且 client 支援）
[VIPLE-VR-LINK] link features agreed by the server: ctrl=… repair=… repairRtt=…   ← client：同上（LINK_READY 尾端的 hubCaps）
[VIPLE-VR-REPAIR] nack frame=F block=B have=N/T (data D) attempt=K (…)[ SEND FAILED][ eta=… idle=…]
                                                                              ← client：回報正在等的 FEC block 缺 shard。have＝手上有幾個／這個 block 共幾個；
                                                                                 0/0＝一個封包都沒收到。括號是觸發原因：tail seen（最後一個 shard 到了還不夠）、
                                                                                 idle（3 ms 沒有新封包）、next frame waiting（下一幀到了）、every link passed
                                                                                 （兩條以上在送：每條都送過這個 block 了）、slower link overdue（較慢那條超過
                                                                                 平常的時間差還沒送到）、retry。每場前 12 筆照印，之後每 50 筆一筆。
                                                                                 照 §VR-REPAIR-RTT 的規則送出的行尾多 ` eta=… idle=…`（毫秒；見 5b-5）
[VIPLE-VR-REPAIR] gave up frame=F block=B (wait expired|hold full): nacks=N refunds=N, … ms since the last one (rto … ms), have=N/D, first packet … ms ago, gone=0|1
                                                                              ← client：回報過、還是等不到，放棄這個 block（接著會報 LOSS）。refunds＝鏈路空檔後
                                                                                 退還的回報次數；first packet＝這個 block 第一個封包多久以前到的（−1＝一個都
                                                                                 沒到）；gone＝server 回了「補不了」。自己的節流：每場前 30 筆，之後每 20 筆一筆
[VIPLE-VR-POSE] dev: --vr-synthetic-hmd - the head pose sent to the host is synthetic (…)   ← client（warning）：dev 選項，真的 XR 連線上
                                                                                 頭的姿態改送合成運動（無人配戴的測試用）；tracking sender 的 source 會印
                                                                                 `xr+synthetic-hmd`。2026-10-10 起送的是「取樣時間＋predictNs 那一刻」的合成姿態
                                                                                 （和真的樣本同一個語意）；之前送的是取樣當下的姿態卻帶著 predictNs，host 少外插
                                                                                 這麼多，算繪姿態在合成軌跡上固定落後一個 predictNs（頭盔上約 26 ms）
[VIPLE-VR-POSEERR] dev: pose error and lag are measured against the synthetic head pose (--vr-synthetic-hmd)
                                                                              ← client（warning，2026-10-10）：`--vr-synthetic-hmd` 開著時，`[VIPLE-VR-POSEERR]` 與 CLIENT_TIMING
                                                                                 的 poseLag 兩個欄位改拿「合成軌跡在該幀顯示時刻的頭部姿態」當基準（10 s 行的行尾多
                                                                                 ` ref=synthetic`）。原本比的是頭盔真的姿態——沒有人戴、放著不動，位置誤差一兩公尺、
                                                                                 落後 0 筆，host 的 §VR-PREDICT 整場不動。顯示出去的東西（projection 的姿態、重投影、
                                                                                 LATCH、MTP）不受影響。樣本和基準走的是同一條合成軌跡、同一個時鐘：host 預測得剛好時
                                                                                 量到的落後是 0（host 會照它調 §VR-PREDICT，學到的值留給之後的場次，和配戴的場次一樣）
[VIPLE-VR-POSEERR] dev: --vr-synthetic-hmd without an XrTime conversion (frameloop tracking) - …
                                                                              ← client（warning）：runtime 沒有時間換算擴充，顯示時刻只有估計值，對不準合成軌跡上的時刻：
                                                                                 基準不換，照舊拿頭盔真的姿態比（量不到落後）
[VIPLE-VR-INPUT] dev: --vr-synthetic-menu-at - the left MENU button is pressed N s after tracking starts (toggles the host's SteamVR dashboard)[; on a real XR runtime …]
                                                                              ← client（warning）：dev 選項，追蹤開始後第 N 秒在左手按一下 MENU（150 ms；host 當成
                                                                                 系統鍵）。只是「切換」儀表板，關掉之後的時序和配戴不同，不要用來做直通的驗測。
                                                                                 沒有帶 --vr-synthetic-hands 時行尾會提醒：真的 XR 連線上左手沒被追蹤，host 會忽略這一下
[VIPLE-VR-REPAIR] nack frame=F block=B age=…ms have=N/T (data D) attempt=K round=R[ eta=N] -> resend N pkts|GONE reason=R|nothing to send
                                                                              ← server：收到一則回報與處置。age＝這個 block 第一批送出到現在；GONE reason
                                                                                 1＝太舊（超過三個幀週期；帶 §VR-REPAIR-RTT 旗標的回報是超過 100 ms，
                                                                                 這種回報另印 client 的 eta，毫秒）、2＝補包額度用完、3＝封包倉裡沒有這個 block；
                                                                                 nothing to send＝client 其實已經夠還原，或缺的剛補過還在路上。
                                                                                 「太舊」與「額度用完」另外節流（每場前 30 筆、之後每 20 筆一筆），格式是
                                                                                 `age=…ms attempt=K rounds=R[ wanted=N pkts, budget left M] -> GONE reason=1|2`
[VIPLE-VR-REPAIR] link N sent K/M repair pkts, queued … ms, sending took … ms  ← server：一批補包在這條連線送出的情況（queued＝在補包佇列等了多久；
                                                                                 K < M＝等過頭或送到一半過期，沒送的算進下一行的 late）
[VIPLE-VR-REPAIR] 10s: nack=N rounds=N pkts=N gone=N (expired=N budget=N unknown=N) nothingToSend=N exhausted=N | L1 tx=N late=N queuedMaxMs=… | L2 …[ | rtt=N ageMaxMs=…]
                                                                              ← server：10 秒彙總（有回報才印）。rounds／pkts＝補了幾輪、共幾個封包（每條連線
                                                                                 各送一份只算一次）；exhausted＝同一個 block 補滿 5 輪還缺。
                                                                                 協商了 §VR-REPAIR-RTT 時行尾多：rtt＝帶旗標的回報份數、ageMaxMs＝這 10 秒
                                                                                 年齡最大的一份回報（block 第一批送出到收到回報）
[VIPLE-ABR] ramp held: N shards repaired in M ms (P% of the video packets; src=…)
                                                                              ← server：補包救回來的缺包佔 3% 以上，這一輪不回升位元率（也不降；補得回來的
                                                                                 不算丟包）。每 5 秒最多一行
[VIPLE-VR-LINK] skipping '<if>' (USB network link is not used for multi-link)  ← client：USB 網路 gadget 不當連線
[VIPLE-VR-LINK] 10s: trkFirst enet=N | L1 up|down[(probing)][(paused Ns)] tx=N (… Mbps) dropAge=N dropBlock=N dropFull=N paused=N standby=N fault=N err=N
                     sendMaxMs=… audio=N/N mutes=N probes=N starts=N forced=N deliv=P%|- rx ping=N trk=N dup=N bad=N trkFirst=N ctl=N s2c=N/N client rx=N used=N rttMs=… gapMaxMs=N burstMbps=N | L2 …
                                                                              ← 每條連線一段。dropAge＝出列時已超過兩個幀週期；dropBlock＝送到一半
                                                                                 期限到了；paused／standby＝暫停期間或 video=primary 時沒送的；
                                                                                 trkFirst＝這條連線先送到的追蹤樣本；client …＝頭盔端在 PING 回報的
                                                                                 收到／被採用封包數、往返時間、最大到達間隔與最近一串封包的到達速率；
                                                                                 probes／starts＝累計送出的探測批數與「量到夠快而開始送影像」的次數；
                                                                                 forced＝沒有任何連線在送影像時，保底在這條連線送出的封包數（累計）；
                                                                                 deliv＝送達率（頭盔在 PING 回報的累計收到數 ÷ server 的累計送出數，約每秒結算；還不知道時印 -）；
                                                                                 ctl＝這條連線先送到的 0x5507（LOSS／LATCH／NACK）則數、s2c＝送出／送不出去的
                                                                                 T_S2C（§VR-LINK-CTRL）。
                                                                                 client 端的同名 10s 行另有每條連線的 lagMs（這條比最先送到的那條平常晚多久，
                                                                                 §VR-LINK-REPAIR 決定回報時機用）與結尾的 ctl tx／drop、s2c rx／dup／bad
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
  - `[VIPLE-VR-CAP] opened gen=<n> ring=<W>x<H> released=<k>` / `first frame gen=<n> …` / `frame source gen=<n> is on a different adapter; reinit` / `ring … != display …; reinit` / `open gen=<n> failed: <open-texture[i]|open-shared-fence|open-consumed-fence>`。driver generation 換了只重開 ring，不重建 encoder。`released`（§VR-RING-RELEASE，2.0.0）＝開 ring 時直接交還 driver、沒複製的幀數：同一個 generation 換消費者（例：VR 斷線後 `/resume`），上一個消費者停掉後 driver 發布的最後幾幀（最多 3 個 slot）不論是否過期都放回；舊版用 read_latest() 排空，停過 1 s 以上那一筆會被 §B.8 時間窗判不合格而不交還，driver 沒有 slot 可用、只剩 10 fps 重送幀。
  - `[VIPLE-VR-CAP] 10s: gen= copied= skipped= fenceTimeout= invalid= stale= black= released= evtToPush p50/p95 presentToPush p50/p95 echoAge p50/p95`：健康時 invalid／stale／fenceTimeout 為 0，`released` 只在換消費者或等幀逾時 100 ms 時才可能非 0（交還曾被判不合格、沒複製的幀）。`echoAge`（V6 起）＝echo 樣本發布到 tracking ring → app Present，涵蓋延遲預算第 3＋4＋5 項，只算 ECHO_MATCHED 的幀（S3-09 用）。`black` 只在 driver 回報 HMD_PRESENTING 前（10 fps 黑幀）。
    - **evtToPush 的健康門檻是有條件的**（2026-10-09 更正）：p95 ≤ 1.5 ms 只在**沒有遊戲負載**時成立（V3 host 實測 0.33 ms）；SteamVR 儀表板開著（第 0 層是合成器自己的貼圖）時也會隨遊戲負載升高（每眼 4328 時 p50 1.4／p95 3.1 ms）。遊戲貼圖直接當第 0 層（直通）時，driver 的合成排在遊戲這一幀的 GPU 工作之後，等 fence 就是在等遊戲算完：**直通加上有負載的遊戲，p50 2～3 ms 是正常的**（2026-10-09 那一場事件前 20 個視窗 p50 1.70～2.44、p95 2.07～2.72 ms），而且跟著遊戲每一幀的 GPU 負載走（2026-10-10：解析度倍率 300%、遊戲仍是每秒 120 幀時 p50 5.8 ms）。沒有人配戴的場次（模擬頭盔、Monado、頭盔放著）host 的儀表板多半整場開著，走不到直通。要量和配戴相同的直通，場次前在 `<host>` 的 `steamvr.vrsettings` 加 `dashboard.enableDashboard=false`、結束後還原（`steam_frame_client.md`「無人驗測和實際遊玩不一樣的地方」）；不要用 `--vr-synthetic-menu-at`——它只是切換，關掉之後是另一種時序（evtToPush 約 0.2 ms、echoAge 多一格），量到的不是這裡說的 2～3 ms。
    - **§VR-FENCE-LATE（2026-10-09）起行尾多的欄位**（前面的欄位名稱、順序、格式都沒有動；開關開或關都會印）：`late=<n> fenceWait p50=<ms>ms p95=<ms>ms max=<ms>ms teardownReq=<n> overload=<0|1> budgetMs=<n> presentToFence p50=<ms>ms p95=<ms>ms max=<ms>ms overPeriod=<n>`。`late`＝第一段等待逾時、之後在等待預算內到值而照樣複製的張數（已含在 `copied` 裡；`vr_capture_late = disabled` 時恆為 0）；`fenceWait`＝複製成功的每一張從被叫醒到 CPU 端確認 fence 到值的時間（含準時的）；`presentToFence`＝同一批影像從 driver 的 Present 時刻（descriptor 的 `present_qpc`）到 fence 到值的時間；`overPeriod`＝複製成功的張數裡 `fenceWait` 達到一個顯示週期的（已含在 `copied` 裡）；`teardownReq`＝擷取迴圈這 10 秒要求拆線幾次（下面 `teardown request` 的各種 why 合計；descriptor 連續不合格那一種是 reader 自己拆的，不算在這裡）；`overload`＝發布給 LATCH／PREDICT 看的過載旗標；`budgetMs`＝目前的等待預算（3 個發布間隔，夾在 3 個顯示週期與 `vr_capture_late_max_ms` 之間；開關關著印 0）。開關開著時 `fenceTimeout` 只算「等到預算用完而放棄」的張數。健康時（Present 之後的 fence 等待短於第一段等待）late／teardownReq／overload／overPeriod 都是 0。fence 等待超過第一段等待（120 Hz 是 5 ms）時 late 就不是 0，即使遊戲仍然跑滿更新率（2026-10-10 實測解析度倍率 300%：copied 1200、late 600～1010）；重到掉出更新率時 late 接近 copied、copied 跟著遊戲實際的幀率走，這是預期的（變慢但不斷）。
    - **第一段等待的界線不是剛好 `firstWaitMs`**：等待以 1 ms 為單位醒來、醒來先看有沒有到值再看期限，實際落在 `firstWaitMs` 到 `firstWaitMs`＋約 1.8 ms 之間（舊規則 74 次第一次逾時的 `waitedMs` 是 5.0～6.8 ms），`fenceWait` 另外含讀 descriptor 的時間；所以準時（不算 `late`）的影像 `fenceWait` 可以略大於 `firstWaitMs`。
    - **這幾個數字怎麼讀**：「發布後實際晚多久」看 `presentToFence`（`presentToPush` 是它再加約 0.1～0.3 ms 的複製；編碼端回堵、影像池滿的時候還會多出等影像池的時間）。`fenceWait` 只在晚到不超過一個發布間隔時才等於它：晚到超過一個發布間隔時 server 還在等上一張，下一張被取用的時刻就是上一張做完的時刻，`fenceWait` 會貼在發布間隔上（模型：發布間隔 10.4 ms，發布後晚 12 ms 與晚 20 ms，`fenceWait` p50 都是 10.4 ms）——**`fenceWait` 接近發布間隔表示已經飽和，不代表只晚那麼多**。有沒有跳過已完成的影像看 `skipped` 與 driver 的 `noslot`。`budgetMs` 用的發布間隔是以實際發布的張數算的：server 佔著 slot 等、driver 走 `noslot` 時，間隔會被 server 自己拉長（模型裡晚 40 ms 時是 50 幾而不是 31），fence 完全不動時頂到上限，它不等於 3 個遊戲幀。要評估 `vr_capture_late_max_ms` 的預設或取用順序，以 `presentToFence` 為準。（`overPeriod`＋`fenceTimeout`）÷（`copied`＋`fenceTimeout`）是「過載的條件收窄成等超過一個顯示週期才算」時會看到的比例（見下面 `overload` 的已知限制）。
    - **§VR-CADENCE（2026-10-10）起行尾再多的欄位**（前面的欄位一個字都沒有動；兩個開關怎麼設都會印）：`ovlMode=<late|cadence> cadence=<unknown|locked|slip|unsure> slips10=<n> abandonOvl=<0|1> latchHold=<0|1> predictHold=<0|1> cadHold=<0|1>`。印的都是**已發布**的值：`ovlMode`＝`vr_capture_overload`；`cadence`＝已發布的快照裡擷取端用的 Present 節拍（bridge 回報的狀態；確定的值在報告中斷後沿用 2 s，所以和 `[VIPLE-VR-DRV] timing 10s` 的 `cadence` 可能差那 2 s；快照每秒發布一次，旗標沒有跟著變的時候最多再慢 1 s。和下面 `cadence` 行的限速無關：那一行來不及印的時候，這裡已經是新的值）；`slips10`＝最近一次讀到的摘要的「最近 10 s 滑動數」（含卡頓後 3 s 內的，封頂 32）；`abandonOvl`＝放棄的影像 ≥ 30% 的旗標（和 `overload` 同一套 30%／5%／2 s 的規則，只算放棄的）；`latchHold`／`predictHold`＝發布給 stream.cpp 的兩個決定（LATCH 走不走 hold、§VR-PREDICT 取不取樣）；`cadHold`＝`cadence` 模式會怎麼決定 LATCH；`overload`（前面既有的欄位）仍是晚到的旗標。**怎麼讀**：預設（`ovlMode=late`）下 `latchHold`、`predictHold` 永遠等於同一行的 `overload`，`cadHold` 只是影子——拿它和 `latchHold` 對照就是「開了 `cadence` 會差在哪」（跑滿更新率、每一張都晚的遊戲是 `overload=1 latchHold=1 cadHold=0`）。`ovlMode=cadence` 時 `cadHold` 等於 `latchHold`，那種遊戲是 `overload=1 latchHold=0 predictHold=1`。`vr_capture_late = disabled`，或目前發布的是預設快照（沒有 frame source、generation 還沒開起來）時，新欄位是 `cadence=unknown slips10=0 abandonOvl=0 latchHold=0 predictHold=0 cadHold=0`。
  - `[VIPLE-VR-CAP] capture late=<on|off> firstWaitMs=<ms> maxWaitMs=<ms> backoffStep=<k> overloadMode=<late|cadence>`（§VR-FENCE-LATE）：每次進擷取迴圈印一行實際生效的設定（每個 display 物件一次，`capture_e::reinit` 重建之後也會再印）：開關、第一段等待（`fence_wait_ms`）、`vr_capture_late_max_ms`、接手的退避階梯（0＝從頭開始）。行尾的 `overloadMode`（§VR-CADENCE，2026-10-10）＝`vr_capture_overload` 的設定值；只在 `late=on` 時有作用。
  - `[VIPLE-VR-CAP] first fence timeout gen=<n> frame=<frame_id> expected=<descriptor 的 fence 值> completed=<當下的完成值> waitedMs=<ms> mode=<legacy|late>[ result=error]`（§VR-FENCE-LATE）：每個 generation 第一次第一段等待（`fence_wait_ms`，120 Hz 是 5 ms）逾時時印一次（數字是逾時當下的；這一行等那一張處理完、slot 交還之後才印）。`mode=late`＝之後會繼續等（`vr_capture_late = enabled`）；`legacy`＝這一張直接丟。`expected − completed` 是那一刻還有幾張沒做完。行尾多了 `result=error` 表示不是逾時，而是 CPU 端的 fence waiter 本身出錯（建不出 event）：那時沒有真的等、也沒有讀完成值（`completed=0` 沒有意義），這一張照逾時處理、不會繼續等。
  - `[VIPLE-VR-CAP] teardown request gen=<n> why=<legacy-1s|stall|starve|device-lost> expected=… completed=… sinceAdvanceMs=… sinceCopyMs=… published=… copied=… nextAllowedMs=…`（warning，§VR-FENCE-LATE）：擷取端每次要求拆線印一行（緊接著會有 `[VIPLE-VR-IPC] teardown gen=<n> reason=requested bye=4`，device-lost 是 `bye=5`）。`why`：`legacy-1s`＝舊規則（開關關著：連續逾時、距第一次逾時滿 1 s）；`stall`＝完成值連續 3 s 沒有增加（從沒前進的第一張起算，之後中間有沒有影像的空檔也照算）；`starve`＝連續沒有一張複製成功、累計滿 5 s（完成值在動但追不上 descriptor 寫的值；只累計有影像在來的時間——相鄰兩張之間最多算 0.5 s，沒有影像的空檔不算）；這兩種都要連續失敗的期間 driver 發布了 ≥ 30 張（第一張之前發布的不算）。`device-lost`＝完成值讀到 UINT64_MAX 而自己的 device 沒事。`sinceAdvanceMs`＝完成值上一次前進（讀到的值創新高，或有一張複製成功）到現在；`sinceCopyMs`＝上一次複製成功到現在（牆上時間，含沒有影像的空檔，所以 `why=starve` 時可能遠大於 5000）；這個 generation 還沒有過就從開 ring 算起。`published`／`copied`＝這個 generation 累計發布（依 frame_id 的差，含被 latest-wins 跳過的）／複製成功的張數；`nextAllowedMs`＝下一次最早多久以後才會再拆（5000／15000／30000；legacy-1s 與 device-lost 沒有退避，印 0）。**分辨「晚到」與「不動」**：`sinceAdvanceMs` 很小而 `sinceCopyMs` 很大＝fence 一直在前進、只是每一張都比等待上限晚（2026-10-09 的事故，開關開著就不會拆）；兩個都大＝fence 真的不動。
  - `[VIPLE-VR-CAP] teardown deferred gen=<n> why=<stall|starve> allowedInMs=<ms> backoffStep=<k>`：卡死／餓死的判定成立，但距上一次拆線還在退避期內（每個 generation 只印一次；時間到了才印上面的 `teardown request`）。退避期內每一張都放棄、沒有影像可交，擷取迴圈仍然每 100 ms 讓擷取執行緒檢查一次停止，session 可以照常結束。`[VIPLE-VR-CAP] teardown backoff reset gen=<n> (10 s healthy)`：拆過線之後連續 10 s 健康，退避的階梯歸零。健康＝每秒複製成功的張數 ≥ 發布張數的一半，以對齊整秒的一秒格逐格判、開頭不滿一秒的那一格不算，所以實際是 10～11 s；這一行可能緊接在一張放棄的影像之後（一半放棄、一半複製的串流也算健康）。階梯另外在行程裡留一份備份，跨得過 display 的重建與 60 s 內接著開的下一場；隔更久的下一場從頭開始（看 `capture late=` 那一行的 `backoffStep`）。
  - `[VIPLE-VR-CAP] overload <on|off> gen=<n> latePermille=<‰> fps=<n> budgetMs=<n> waitMaxMs=<ms> why=<late-frames|recovered|no-frame-source|new-generation|capture-exit>`（§VR-FENCE-LATE，只有 `vr_capture_late = enabled` 會出現）：過載旗標變化各一行。`on`＝連續看滿 1 s 的影像後（generation 剛開、或視窗空了——整整 1 s 沒有影像——之後都要重新看滿），最近 1 s「晚到＋放棄」≥ 被取用張數的 30%（而且至少 3 張）；`off`＝降到 5% 以下持續 2 s（`recovered`），或 frame source 沒了、換 generation（新的還沒開起來也算）、離開擷取迴圈。`latePermille`／`fps`＝最近 1 s 晚到＋放棄的千分比與推給編碼器的張數（視窗實際涵蓋 0.9～1.0 s，`fps` 會比真正的每秒張數少最多約一成；要精確的張數看 10 s 行的 `copied`）；`waitMaxMs`＝最近 1 s 單張等 fence 最久多久。**旗標設著的期間**（預設的 `vr_capture_overload = late`；`cadence` 見下面），server 不把 LATCH 樣本餵給頻率鎖（見下方 `[VIPLE-VR-LATCH] 10s` 的 `overloadHeld`），§VR-PREDICT 也不取樣（那段時間沒有 `[VIPLE-VR-PREDICT] 10s` 行、也不會更新下一場的起始值）。**已知的限制**：「晚到」的定義是第一段等待逾時（120 Hz 是 5 ms）。一幀的 GPU 時間穩定落在第一段等待與一個顯示週期之間、仍然跑滿更新率的遊戲，每一張都算晚到，旗標會在開場約 1 s 後設起來、整場不清——LATCH 與 §VR-PREDICT 整場不追蹤（那種遊戲的樣本其實可用；LATCH 那時只照已經學到的時鐘差走）。看得出來的樣子：`late` 佔 `copied` 的五成以上、`overPeriod` 接近 0、`copied` 仍然跟著更新率（120 Hz 是每 10 秒約 1200）、`overload=1`。2026-10-10 量到了（解析度倍率 300%：每秒 120 幀、每 10 秒 1200 張裡 600～1010 張算晚到、`overPeriod` 0～1、`overload=1`）。**§VR-CADENCE（2026-10-10）**：這個旗標本身的定義沒有改；`vr_capture_overload = cadence` 收窄了 LATCH 的條件（預設仍是 `late`，也就是上面說的行為）——LATCH 只在合成器的 Present 鎖不住虛擬 vsync（`cadence=slip`）或放棄的影像 ≥ 30%（`abandonOvl=1`）時不追；§VR-PREDICT 仍看這個旗標，另外加上 `slip`（它要的是絕對延遲穩定，每一張都晚的時候送出時刻是差一格的雙峰）。三個詞不要混：`overload`＝晚到（這個旗標，下文稱 late 旗標）；`cadence=slip`＝Present 鎖不住虛擬 vsync（看 driver 回報的「每次 Present 跨了幾個虛擬 vsync 格」）；`cadence=unsure`／`unknown`＝說不準，一律照 late 旗標。門檻還沒有逐幀的實測，`cadence` 要等影子欄位（10 s 行的 `cadHold`、`[VIPLE-VR-DRV] timing 10s` 的 `slips` 等）確認之後才開（`docs/TODO.md`、`steam_frame_client.md`「已知的限制」）。
  - `[VIPLE-VR-CAP] cadence <unknown|locked|slip|unsure> gen=<n> why=<none|marginal|heavy|recovering|recent|fraction|suspect|proving|no-report|no-frame-source|new-generation|capture-exit> div=<d> slips10=<n> applied=<0|1>[ skipped=<n>]`（§VR-CADENCE，只有 `vr_capture_late = enabled` 會出現）：擷取端用的 Present 節拍狀態變化各一行。狀態：`locked`＝確定鎖得住（每 `div` 格一次 Present，而且已經連續觀察滿 10 s）；`slip`＝確定鎖不住；`unsure`＝有紀錄但下不了結論；`unknown`＝沒有報告。`why`：`slip` 的原因是 `marginal`（10 s 內有 4 個不屬於卡頓的滑動；之後 12 s 沒有才清。很快就結束的短過載也是這一種——120 Hz 下湊不滿 24 個滑動、大約不到 1.7 s 的——所以每一次都要 12 s 才清，不是 3 s）、`heavy`（3 s 內的滑動達格數的 1/15：120 Hz 24 個、90 Hz 18 個、60 Hz 12 個）、`recovering`（重度剛結束，3 s 沒有滑動就清）；`unsure` 的原因是 `recent`（最近 12 s 內有「卡頓後 3 s 內的滑動」——分不出是卡頓的餘波還是被卡頓蓋住的持續滑動）、`fraction`（穩定在更新率的整數分之一，但 driver 回報的節流值 nFramesToThrottle 解釋不了：連續 24 張都跨同樣的格數而節流值對不上；或是已經採用的節拍（`div` ≥ 2）之後累計 24 張節流值對不上——SteamVR 不再節流，而遊戲剛好還留在那個速度附近自由跑）、`suspect`（timing 紀錄裡有不可能的值，另見 `[VIPLE-VR-DRV] cadence input suspect`）、`proving`（連續觀察還不滿 10 s：連線剛開起來，或紀錄中斷超過 2 s、連續 12 筆沒有輸出目標而從頭來過之後）；`no-report`＝沒有夠新的摘要（bridge 超過 1 s 沒更新，或確定的狀態在報告中斷後沿用的 2 s 過了；這時 `div`／`slips10` 印 0）；`no-frame-source`／`new-generation`／`capture-exit`＝快照被清成預設的原因。卡頓＝一次 Present 比節拍多跨至少 2 格、而且至少多 16 ms；多跨 1 格是滑動。`div`＝目前的節拍（每幾格一次 Present；SteamVR 把遊戲固定在半速而且紀錄裡的節流值對得上時是 2）、`slips10`＝最近 10 s 的滑動數（封頂 32）。`applied`＝這個狀態有沒有拿來做決定：1＝`vr_capture_overload = cadence`，0＝只是影子（決定仍照 late 旗標）。同一秒內最多一行：來不及印的，下一次發布時狀態仍和上一行印的不同才補印（只是晚印）。行尾的 `skipped=<n>`（不是 0 才印）＝上一行之後還有幾次狀態變化完全沒有印出來——例如同一秒內 locked → unsure → slip，印出來的是 `cadence slip … skipped=1`；變了又變回上一行印的狀態時沒有東西可印，那兩次算在之後的下一行。清除的那一行（`why` 是後三種）與它之後的第一行不受限速，沒印出來的變化也不帶到下一個 generation。這一行只看得到快照發布當下的狀態：旗標沒有跟著變的時候最多比實際的變化晚 1 s（快照每秒發布一次），兩次發布之間變了又變回來的看不到。連線開起來的頭 10 s 是 `unsure why=proving`；遊戲載入的卡頓與之後 12 s 內多半是 `unsure why=recent`。
  - `[VIPLE-VR-CAP] hold latch=<0|1> predict=<0|1> gen=<n> cadence=<unknown|locked|slip|unsure> overload=<0|1> abandonOvl=<0|1> why=<update|no-frame-source|new-generation|capture-exit>[ skipped=<n>]`（§VR-CADENCE，只有 `vr_capture_overload = cadence` 會出現）：發布給 stream.cpp 的兩個決定（`latch`＝LATCH 走 hold、`predict`＝§VR-PREDICT 不取樣）變了就一行，附上當下決定它們的三個量。`late` 模式下兩者就是 `overload on／off`，不另外印。和 `cadence` 行一樣同一秒內最多一行、同樣的 `skipped`（上一行之後還有幾次變化完全沒有印出來）：決定本身是一變就發布的，被限速的只有這一行——要看每 10 秒實際擋了幾則 LATCH，看 `[VIPLE-VR-LATCH] 10s` 的 `overloadHeld`。`skipped` 常常不是 0 表示決定來回得很快（例如節拍在 `locked` 與 `unsure why=fraction` 之間換，而每一張都晚到）。怎麼來的：`cadence=slip` → 兩個都是 1；`cadence=locked` → `latch`＝`abandonOvl`、`predict`＝`overload`；`unsure`／`unknown` → 兩個都等於 `overload`。所以 `latch=1` 時 `predict` 一定是 1。跑滿更新率、每一張都晚的遊戲，節拍被判定鎖住之後是 `hold latch=0 predict=1 … cadence=locked overload=1`。`why=update` 以外的三種是快照被清成預設（`latch=0 predict=0`）。
  - `[VIPLE-VR-CAP] gpu-priority=high|realtime (was realtime|high)[ vr_gpu_priority=realtime]`：擷取期間行程的 GPU 優先權。
    預設改成 HIGH（原本的顧慮是 REALTIME 會搶遊戲）；`vr_gpu_priority = realtime` 時不降，行尾會多那一段
    （§VR-GPU-PRIO：HIGH 和遊戲同一個排程層級，遊戲把 GPU 佔滿時每一幀晚一個遊戲幀才送出）。括號裡是擷取結束後還原的值；
    硬體加速 GPU 排程開著而且 `nvenc_realtime_hags` 關著時兩個都是 high。
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
[VIPLE-VR-DRV] space-delta=<yaw>/<pitch>/<roll>deg pos=<x>,<y>,<z>mm trigger=<still|event:<name>>  ← S2-06：採用新的 SteamVR↔client 空間差（V6 起）
[VIPLE-VR-DRV] controller activate hand=<left|right> index=<n>  /  controller deactivate hand=…   ← S2-09 最小 Touch 控制器（V6 起）
[VIPLE-VR-DRV] controller hand=<left|right> render model=<name> (client profile <n>)   ← M4a 收尾：依 0x5506 input.profile 選外觀（Touch／Index／Frame）
[VIPLE-VR-DRV] controller hand=<…> render model <name> not installed - using Touch
[VIPLE-VR-DRV] controller hand=<left|right> <out-of-range|tracking> age_ms=<n>                   ← 控制器狀態切換（沒資料、active=0、> 100 ms）
[VIPLE-VR-DRV] tracking 10s: new=N staleSends=N stale=N oor=N invalid=N torn=N offsetUs min=… max=… p50=… ctrlOffUs p50=… p95=…
               arrivalMaxMs=… gt2T=N hold=N holdExpired=N poseFlags=0x<n>
                                                                                    ← 2026-10-05 起尾端加：HMD offset 中位數、控制器 offset（§VR-CTRL-OFFSET 開才有值）、
                                                                                       driver 收到新樣本的最大間隔與 > 2T 次數、§VR-STALE-HOLD 次數與超過 100 ms 的次數、
                                                                                       pose_flags（bit0 控制器 offset、bit1 stale hold、bit2 角速度轉機體座標）
[VIPLE-VR-DRV] tracking recovered gap_ms=… total_ms=… hold=<0|1>                  ← stale 之後收到新樣本；gap_ms＝距上次 stale 重送、total_ms＝整段空窗（2026-10-05 起）
```

- 選卡（K22）：`<install>\config\steamvr\state.json` 的 `vrcompositor_adapter` 以 **LUID** 記錄 vrcompositor 實際使用的卡（`<host>` 上 IddCx 虛擬卡與實體卡同名，不能用名稱比對）。
- selftest T4（只在 `--vr-selftest --manual-steamvr`）：`T4.readback result=PASS|FAIL compared=<n> mismatch=<n> eyeDisagree=<n> … echoMatched=<n> maxAngleDeg=…`、`T4.consume … consumption=<比例> …`。已知限制：vrcompositor 自己重畫第 0 層（系統面板顯示時）的 session 圖案解不出來，`compared`≈0、`decodeFail` 高，不代表傳錯畫面（見 TODO V4）。

### 5b-4. SteamVR 編排器（M1b V5 起；`vr_pcvr = enabled`，Windows server）

```
[VIPLE-VR-ORCH] start app="<name>" …                         ← /launch mode=pcvr 後背景啟動
[VIPLE-VR-ORCH] env installed=<0|1> …                          ← Steam／SteamVR 探索（維護 tick 每 60 s 也會做一次）
[VIPLE-VR-ORCH] step=<deploy|register|conflict|guard|arm|launch-steamvr|wait-hmd|launch-app|disarm|prune|quit-vrmonitor|unregister-other> result=…
[VIPLE-VR-ORCH] state <ORCHESTRATING|HMD_ACTIVE|ERROR…> code=<n> …   ← 同一份 STATE 推給 client（0x5508 STATE）
[VIPLE-VR-ORCH] error code=<n> …                               ← code 見 VipleVr.h（4–21，例 7=HMD_TIMEOUT、16=OPENXR_RUNTIME_OTHER 為警告）
[VIPLE-VR-ORCH] deploy-refused reason=…                        ← secure_fs 檢查不過（owner／DACL／reparse／hash），fail closed
[VIPLE-VR-ORCH] guard-restore skipped key=…                    ← vrserver 還在跑時不還原設定，下次 tick／啟動再補
[VIPLE-VR-ORCH] safe-mode unblocked ver=…                      ← guard 套用時清掉 blocked_by_safe_mode（vrserver 不在跑時）
[VIPLE-VR-ORCH] step=quit-settle waitedMs=<N> …                ← §QUIT-SETTLE：vrserver 起來未滿 20 s，先等滿才結束 SteamVR
[VIPLE-VR-ORCH] step=wait-driver result=fail reason=safe-mode-blocked retry=1   ← §SAFE-RETRY：SteamVR 以 safe mode 啟動、擋掉 driver
[VIPLE-VR-ORCH] safe-mode: SteamVR blocked our driver at launch … restarting SteamVR once   ← 接著 quit-steamvr、guard、再啟動一次；仍被擋回 code 2
[VIPLE-VR-ORCH] pose flags ctrlOffset=<0|1> stalePolicy=<legacy|hold> angvelLocal=<0|1>
                                                                ← 每次組 session config 一次（vr_ctrl_pose_offset／vr_stale_policy／vr_angvel_local → driver pose_flags）
[VIPLE-VR-LINK] ENet disconnected but a link is still alive - keeping the session, waiting for the control channel to reconnect   ← §VR-LINK-GRACE（server）
[VIPLE-VR-LINK] control ping timeout suppressed - a link is still alive, extending 5 s                                             ← 同上，ENet 還沒重連時每 5 s 一行
[VIPLE-VR-SESSION] half-rate: display D Hz, stream S Hz                              ← §VR-HALF-RATE（client）：串流跑顯示更新率的一半
[VIPLE-VR-SYNTH] texture setup failed|flow pass failed|combine pass failed - frame synthesis off for this session   ← §VR-SYNTH（client）：合成失敗，這個 session 改回重送
[VIPLE-XR] 10s video … synth drawn=N shown=N[ (FAILED: off)]                          ← §VR-SYNTH：這 10 s 合成了幾張、顯示了幾次合成的那一格
[VIPLE-VR-MTP10] … mismatch=N… synthGuard=N                                           ← §VR-SYNTH（2026-10-10，開著補幀才印）：這一格要顯示合成那一半，但 xrEndFrame 之前
                                                                                    又 release 了下一張的第一次發布（它的下半不是合成畫面）→ 改顯示上半的次數；穩態每 10 s 0～1
[VIPLE-VR-ENC] probe: HEVC 10-bit supported|not supported                              ← §VR-10BIT：VR 探測（來源 display_vr）測 HEVC 10-bit 的結果
[VIPLE-VR-SESSION] 10-bit HEVC (Main10, SDR) requested by the client                    ← §VR-10BIT（server）：這個 VR session 用 10-bit 編碼
[VIPLE-VR-SESSION] 10-bit was requested (vr10bit) but this device's hardware decoder path only outputs 8-bit - streaming 8-bit   ← client（Frame）：不要求 10-bit
[VIPLE-VR-LDIAG] client cfg: latchDelay=… ms trackLock=… halfRate=… synth=… fovea=… overscan=… jitter=<spec|off> trackRelock=… latchRelatch=… latchWait=… ms abSec=…
                                                                                    ← client，PCVR 第一次建立 XR 時印一次：這一場 client 端的設定值（2026-10-09 起；之前的 log 查不到延後挑影像是幾毫秒）
[VIPLE-VR-LDIAG] 10s late=N lateShift=N gap=N early=N catchup=N lateMax=N us | held=N heldMiss=N rtyp=N us pubLag p50/p95=… us | wake2latch p50/p95/max=… us latchPhase p50=… us | age p50/p95/p99=… ms | slackPub(250us)=n0,n1,…[ wait=… ms waited=N saved=N timeout=N] | events=N …（2026-10-11 起，見 5b-6）[ ab=<0|1>]
                                                                                    ← client 每 10 s，緊接在 [VIPLE-VR-MTP10] 之後（視窗相同）。量測一律開著，不改變呈現。
                                                                                       late＝有影像但沒趕上自己那一格、被下一張蓋掉（串流幀數）；lateShift＝整條對位晚幾格（顯示格數）；
                                                                                       gap＝那一格根本沒有影像（host 沒出、掉包，或到了 client 沒發布——對照 10s video 的 overwritten／errors）；
                                                                                       early＝影像提早搶了前一格；catchup＝從晚幾格回到原位；lateMax＝晚到最多多久。
                                                                                       全速時「多出來的重複格數」≈ late＋lateShift＋gap＋early；半速時 late 與 gap 要乘 2。
                                                                                       held／heldMiss＝等 GPU 比平常（rtyp）多 1 ms 以上的張數／其中剛錯過的；pubLag＝GPU 做完到真正發布；
                                                                                       wake2latch＝xrWaitFrame 返回到 latch；latchPhase＝latch 落在顯示格點之後多久；
                                                                                       age＝每個顯示格的內容年齡（**含重複的那幾格**，MTP10 不含）；
                                                                                       slackPub＝每張發布的影像到「第一個挑得到它的 latch」還有多久的直方圖（每格 250 µs；0 附近＝險過，接近一個週期＝剛錯過）；
                                                                                       wait…＝§VR-LATCH-WAIT 開著時：實際上限、等了幾格、等到幾張、逾時幾次；ab＝同場 A/B（vrlatchabsec／--vr-latch-ab）是哪一邊。
                                                                                       分類用每張影像的 pts（host 的 Present 時刻）：Present 間隔偏離半格以上的那幾次可能多算 gap 1＋early 1
[VIPLE-VR-LDIAG] 60s gpu-by-phase (250 us bins after the display grid; n/avgUs/maxUs) <相位 ms>:<張數>/<平均 µs>/<最大 µs> …
                                                                                    ← client 每 60 s：影像 render thread 開始處理的時刻落在顯示格點之後多久 → 那一格的平均／最大處理時間。
                                                                                       某一段的平均明顯較長＝頭盔 GPU 的忙碌期；那幾格的最大值＝被卡住的影像最晚多久做完
[VIPLE-VR-LDIAG] wait paused N s (predictAhead … ms, missed N)                         ← §VR-LATCH-WAIT 的 runtime 防護：predictAhead 連 2 s 高過半個週期、或 10 s 內 XR 漏 3 幀 → 暫停等待（60 s 起加倍、上限 480 s，自動恢復）
[VIPLE-VR-LDIAG] latch wait … ms ignored on this platform: …                            ← Windows 的定時等待太粗，守不住「延後＋等待 ≤ 6 ms」：等待不作用（頭盔上不會出現）
[VIPLE-VR-POSE] tracking phase: N% of frames use the previous sample at the locked phase - searching again   ← §VR-TRACK-RELOCK（vrtrackrelock）：鎖相維持中連 5 s 有 15% 以上的幀用到前一筆樣本 → 重新搜尋
[VIPLE-VR-MTP10] … mismatch=N[ relatch=N][ | trk … researches=N[ prev=P%]][ ab=<0|1>]      ← relatch＝§VR-LATCH-RELATCH 救回來的次數（不算進 mismatch）；prev＝用到前一筆樣本的幀（不含晚一格顯示的）
[VIPLE-XR] 10s video … jit n=N p95=… ms                                                 ← dev：--vr-test-jitter 開著時，這個視窗被延後的張數與延後時間
[VIPLE-XR-PROBE] selftest-latch <PASS|FAIL> <案例> …／selftest-latch: N case(s), N failure(s)   ← `VipleStream.exe xr-probe --selftest-latch`（不需要 XR runtime；全過 rc 0、有失敗 14）
[VIPLE-VR-POSE] tracking phase locked: phase=P ms (sample interval T ms, MTP A -> B ms at the edge) #N   ← §VR-TRACK-LOCK（client）：找到邊界，退 2 ms 維持
[VIPLE-VR-POSE] tracking phase: MTP A ms is above the locked level B ms - searching again              ← §VR-TRACK-LOCK：連 3 秒偏高，重新找
[VIPLE-VR-MTP10] … | trk search|hold phase=P ms late=L% researches=N                                 ← §VR-TRACK-LOCK 開著時多的欄位（late＝用到前一筆樣本的幀）
[VIPLE-VR-LINK] control reconnect: no answer|no route after N attempt(s) on A - trying host address B (another link's address|the session address again)   ← §VR-LINK-REHOME（client）：ENet 重連換目標位址
[VIPLE-VR-LINK] control connection re-homed to host address B (local=L)                ← §VR-LINK-REHOME：控制連線連到了不是 session 位址的 host 位址
[VIPLE-VR-ORCH] foveated encoding: centre density P% of uniform (edge E%)              ← §VR-FOVEA：這次編排開了注視點編碼（client 要求的強度）
[VIPLE-VR-SESSION] /resume: foveation stays at P% (running orchestration; client asked for Q%)   ← §VR-FOVEA：/resume 沿用進行中的強度
[VIPLE-VR-SESSION] /resume: display rate for pose prediction stays at D Hz (running orchestration; client reported E Hz)   ← §VR-PREDICT：/resume 帶的 vrDispHz 和進行中的編排不同時沿用進行中的
[VIPLE-VR-FOVEA] unwarp pass failed - showing the stream without undoing the foveated encoding for this session   ← client：還原失敗，這個 session 的畫面是壓縮過的樣子
[VIPLE-VR-ORCH] … step=precheck … console session is locked (sign-in or lock screen)   ← §VR-HOST-LOCKED：host 停在登入／鎖定畫面，VR session 不啟動（STATE ERROR code 22）
[VIPLE-VR-ORCH] render scale P% (recommended render target WxH per eye, stream WxH)   ← §VR-RENDER-SCALE：`vr_render_scale_pct` 不是 100 時印
[VIPLE-VR-ORCH] step=arm result=ok reason=rearm                 ← §VR-REARM：HMD 在 standby、VR 遊戲還在跑，不重啟 SteamVR、直接重新 arm
[VIPLE-VR-ORCH] rearm from standby ok (VR app kept running) ms=<N>   ← 重新 arm 後 vrcompositor 恢復 Present
[VIPLE-VR-ORCH] step=wait-hmd result=fail reason=rearm: no present   ← 10 s 內沒有 Present → error code 19（STEAMVR_RESTART_REQUIRED）
[VIPLE-VR-ORCH] step=launch-steamvr result=fail reason=steam not signed in (no saved login)   ← §STEAM-LOGIN：Steam 沒登入又沒記住帳號，立刻回 code 17（不再白等 60 s）
[VIPLE-VR-ORCH] steam not running (saved login) - started=<0|1>   ← §STEAM-LOGIN：記住了帳號但 Steam 沒在跑，先以使用者身分 `steam.exe -silent`
[VIPLE-VR-ORCH] step=driver-lost result=ok reason=steamvr closed on host - ending session   ← §VR-EXIT：使用者在 host 關掉 SteamVR，正常結束 session（不再自動重開 SteamVR）
[steam-watchdog <appid>] game exited (RunningAppID=…); ending stream   ← §VR-EXIT：PCVR 的 Steam 遊戲結束也會收掉串流（graceful，client 不跳錯誤）
[VIPLE-VR-ORCH] step=idle-quit-steamvr result=ok reason=no VR app, HMD in standby   ← §VR-RESTORE-IDLE：session 結束、沒有遊戲、HMD standby 滿 30 s，結束 SteamVR 以還原 host 設定
[VIPLE-VR-ORCH] stop reason=…
```

- guard 只以使用者 token 修改 `steamvr.vrsettings`（`forcedDriver=viplestream`、`driver_vrlink.enable=false`），留 marker；vrserver 沒在跑時才還原，server 啟動或維護 tick 看到 marker 會補還原。
- 已知限制（V5）：driver 進入 standby 後再 arm，vrcompositor 可能不再 Present（與 V4 的 `AcquireSync` 逾時同源），目前遇到「我們的 HMD 在 standby 且沒有 VR app」時直接重啟 SteamVR 迴避。

### 5b-5. §VR-REPAIR-RTT：補包的量測與「跟著往返時間走」的規則（2026-10-10）

規則本身見 `docs/vr_protocol.md` §4.10 的「§VR-REPAIR-RTT」。這裡分兩種行：**量測**（協商了補包就印，不管 host 的
`vr_multilink_repair_rtt` 開著沒有，也不改變任何行為）與**規則**（照新規則跑的時候才有）。新欄位一律加在既有行的行尾。

```
量測（client）
[VIPLE-VR-LINK] 10s: …原樣… | rtt10s L1 n=40 min=7.9 avg=10.1 max=38.3 win=8.1/9.6/10.9[ L2 n=…]
                                                                              ← 每條連線這 10 秒的往返時間：樣本數、最小／平均／最大，與最近 8 筆的
                                                                                 最小／中位數／第二大（新規則看的是 win 這一組；前面的 rttMs 只是最近一筆）。
                                                                                 這 10 秒沒有樣本的連線只印 `L<id> n=0`
[VIPLE-VR-REPAIR] gate 10s: rules=legacy|rtt|mixed on=NN% flips=N paused=N | lost=N (nacked=N unasked=N whole=N) repaired=N used=N
                  | rtt lo=… mid=… hi=… eta=… try=… cap=… | lateDone 5/10/20/33/+=a/b/c/d/e never=N | stall 4/6/8/12/+=a/b/c/d/e
                                                                              ← 每 10 秒（協商了補包、而且有影像封包在來）。
                                                                                 rules＝這 10 秒照哪一套規則（legacy＝原本的、rtt＝跟著往返時間走的、mixed＝兩種都有）；
                                                                                 on＝補包開著的時間比例（以相鄰兩個影像封包的間隔加權；影像停超過 1 秒的那一段只算 1 秒）；
                                                                                 flips＝補包開／關翻了幾次；paused＝保險觸發了幾次。
                                                                                 lost＝沒收齊就被跳過的幀，括號裡是分類：nacked＝回報過、等到期限還是放棄；
                                                                                 unasked＝收到一部分、沒回報過就放棄；whole＝一個封包都沒到、也沒回報過（含連續掉好幾幀時
                                                                                 佇列沒等過就跳過的）。三類加起來比 lost 少的部分＝回報過、後來因為後面缺太多幀而放棄的。
                                                                                 repaired＝回報之後收齊的 block；used＝其中補包真的用上的（不是它自己晚到的封包剛好湊齊）。
                                                                                 預先報過、補包在輪到之前就先排著、輪到時直接收齊的幀也算在這兩欄裡。
                                                                                 rtt …＝印的這一刻正在送影像的連線的往返（最近 8 筆的最小／中位數／第二大）與照它算出來的
                                                                                 eta／etaTry／等待上限（毫秒）——新規則沒開的時候也印，方便事先看它會怎麼訂；沒有樣本印 `-`。
                                                                                 lateDone＝回報過才放棄的 block，放棄之後多久才到齊（≤5／≤10／≤20／≤33／>33 ms）；never＝60 ms
                                                                                 內沒到齊。放棄前手上已經有的 shard 再到一份（上一輪補包晚到、兩條連線各送一份）不算進度。
                                                                                 server 回了 `REPAIR_GONE`（或回報送不出去）的 block 不追蹤，兩欄都不含它——所以 never 是
                                                                                 「server 補了、60 ms 內卻沒到齊」，補包在路上掉了才會這樣；server 沒補的看它那一側
                                                                                 `[VIPLE-VR-REPAIR] 10s` 的 `gone (expired=… budget=…)`。同時最多追蹤 4 個 block；影像有應用層
                                                                                 加密時晚到的封包在解密前就丟了，只能以幀為單位計數（看不到序號、不去重），這時的數字偏樂觀。
                                                                                 stall＝同一個 block 相鄰兩個封包的到達間隔（3～4／4～6／6～8／8～12／≥12 ms）。不算的兩種：
                                                                                 回報過、隔了一個往返才到的（當成補包）；序號跳號之後才到的（中間掉了一段，那段間隔是
                                                                                 掉的那幾個封包佔的時間，不是鏈路自己的停頓）。
                                                                                 **救回率的分母用 lost＋used**（補包救回來的＋沒救回來的），不要用 repaired：它含自己晚到湊齊的
[VIPLE-VR-MTP10] … | repRun max=N ge3=N ge5=N latchSkip=N rr=0|1|m          ← 行尾新增。repRun＝同一張影像連續重複顯示幾格：這 10 秒的最大值（還沒結束的那一段也算）、
                                                                                 長度達到 3 格與 5 格的段數（一段在下一張新影像出現時結算）。半速＋補幀時每張真的影像平常就
                                                                                 重複 1 格，判讀時扣掉。latchSkip＝被補包拖晚的幀剛放行（60 ms 內）、沒有拿來更新 LATCH 樣本的
                                                                                 新影像張數（「拖晚」＝補包用上才收齊、或跨幀等待結束才放行；報了但自己收齊的不算）。
                                                                                 半速場次驗收時看它佔 `frames new` 的比例；rr＝這 10 秒補包照哪一套規則（0 原本的、1 新的、
                                                                                 m 兩種都有）
量測（server）
[VIPLE-VR-FECSTAT] 10s: recovered=N (shards=N max=N) failed=N wholeFrame=N | fec=P%
                                                                              ← VR session 每 10 秒（這 10 秒有收到 client 的 FEC status 才印）。recovered＝靠 FEC 還原成功的
                                                                                 block 數（括號：一共還原了幾個 data shard、單一 block 最多幾個；補包補到夠還原的也算在
                                                                                 這裡，還原的個數是還原當下還缺的）；failed＝收到的不夠還原；
                                                                                 wholeFrame＝client 依幀號跳號合成的回報**份數**（收到的欄位都是 0）：每次沒收齊就換幀都有
                                                                                 一份——收到一部分才放棄的那一幀也有（它另外算一筆 failed），而連續好幾幀沒到只有一份。
                                                                                 所以它不是「一個封包都沒到的幀數」，那個數看 client `gate 10s` 的 whole；
                                                                                 fec＝目前的 FEC 百分比。只是計數，不影響自動位元率

規則（client；host 開了 vr_multilink_repair_rtt、而且只有一條連線在送影像時）
[VIPLE-VR-SESSION] repair options: rtt=offer|off hold=auto|N ms ab=N s idle=auto|fixed|frame|learned|size ahead=on|off
                                                                              ← 協商了多連線的 VR session 開始時一行：client 這一邊的選項（rtt=offer＝願意用新規則，
                                                                                 用不用由 host 決定；hold＝等待上限；ab／idle／ahead 是開發用的命令列選項）
[VIPLE-VR-REPAIR] nack … (…)[ SEND FAILED] eta=15.6 idle=8.3                 ← 照新規則送出的回報：這份回報用的 eta、這個 block「多久沒有新封包才回報」（毫秒）
[VIPLE-VR-REPAIR] nack frame=F block=0 have=0/0 (data 0) attempt=1 (whole frame missing behind frame N) eta=…
                                                                              ← 預先回報：第 N 幀報過之後，替它後面整幀沒到的第 F 幀送的（total＝0）
[VIPLE-VR-REPAIR] skipped frame=F block=B (too late to ask|after a loss): try x ms, cap y ms, have=N/D
                                                                              ← 沒回報就放棄：too late to ask＝現在報也趕不上期限；after a loss＝剛放棄過前面的 block，
                                                                                 intra refresh 還沒到，這段時間不回報、不等（從那一輪第一次放棄起算「最短往返＋一個幀週期
                                                                                 ＋2 ms」，中間再放棄的不延長）。和 gave up 共用節流（每場前 30 筆、之後每 20 筆一筆）
[VIPLE-VR-REPAIR] skipped frame=F (+N whole frames missing behind it: too many to repair in time): cap … ms, about N pkts/frame, pkt gap N us
                                                                              ← 後面整幀沒到的幀多到補不完：這一段全部不補、不等，立刻放棄
[VIPLE-VR-REPAIR] rtt: paused N ms (level L)                                  ← 保險觸發：回報過卻連續救不回，新規則暫停這麼久（期間不回報、不等）；level＝退避的級數
[VIPLE-VR-REPAIR] ab -> rtt|legacy frame=N                                    ← --vr-repair-ab：換邊（從 legacy 開始）
[VIPLE-VR-REPAIR] ab: arm=rtt|legacy secs=N on=NN% lost=N (nacked=N unasked=N whole=N) repaired=N used=N nack=N spurious=N late=N tooMany=N fixMs avg=… max=…
                                                                              ← --vr-repair-ab：剛結束的那一邊的小結（串流結束時再印最後那一邊）。nack＝送出的回報；
                                                                                 spurious＝報早了的回報：報完、補包還不可能到（距第一份回報不到一個最短往返），這個 block
                                                                                 自己的封包又到了，每個 block 只算一次。之後補包有沒有用上不看——真的缺包、只是報早了的
                                                                                 也算；要數「整份白報」用 repaired − used。late／tooMany 同下一行；
                                                                                 fixMs＝第一份回報到收齊（含補包沒用上、自己收齊的 block）
[VIPLE-VR-LINK] hold 10s: …原樣… | rtt eta=… try=… cap=… idle=… late=N afterLoss=N tooMany=N spurious=N used=N ahead=N fixMs avg=… max=… hist=a/b/c/d/e/f held hist=a/b/c/d/e/f
                                                                              ← 照新規則跑過的視窗才有這一段。eta／try／cap／idle＝最近一次用的值（毫秒）；
                                                                                 late／afterLoss＝沒回報就放棄的兩種（同上面的 skipped）；afterLoss 成串出現＝連續好幾幀都受損
                                                                                 （成串掉包），每放棄一次之後暫停裡的兩三幀都不補；tooMany＝後面缺太多幀而整段不補的次數；
                                                                                 spurious＝報早了的回報（定義同上一行的 ab 小結）；used＝回報之後才到、而且被採用的**封包**數
                                                                                 （gate 10s 的 used 是 block 數）；ahead＝預先回報的份數（已含在 nack 裡）；
                                                                                 fixMs 與 hist＝第一份回報到收齊（≤8／≤12／≤16／≤20／≤25／>25 ms；含自己收齊的 block）；
                                                                                 held hist＝被壓住的幀從它第一個封包到達到放行 ≤4／≤8／≤12／≤17／≤25／>25 ms

規則（server）
[VIPLE-VR-LINK] vr_multilink_repair_rtt needs vr_multilink_repair and vr_multilink_ctrl = enabled - it stays off   ← warning：只設了主開關
[VIPLE-VR-LINK] link features: … repairRtt=on|off (client offered 0x07, server … repairRtt=enabled|disabled)       ← 見上面的 link features
[VIPLE-VR-REPAIR] nack frame=… eta=N -> …／[VIPLE-VR-REPAIR] 10s: … | rtt=N ageMaxMs=…                             ← 見上面兩行的說明
```

- 沒開主開關的 host、舊 host、兩條以上連線在送影像：只有「量測」那幾行，`rules=legacy`、`rr=0`、`latchSkip=0`，
  `hold 10s` 沒有 ` | rtt …` 那一段。
- 開發用的故障注入 `vr_multilink_fault` 的 `d<linkId>:<ms>` 現在也把這條連線收到的回報晚這麼久才處理（之前只有
  PONG 晚兩趟、補包只晚一趟，本機模擬會高估補包趕得上的機率）。延後排在去重之後：同一則控制訊息兩條連線各送
  一份時，被延後的那條若先收到，整則訊息就照它晚處理（另一條的那一份被當成重複）——兩條連線的場次不要拿它量
  「控制訊息走快的那條」的時間。

### 5b-6. 頭盔端管線的量測（2026-10-11）

PCVR 走真 XR 時 client 這一側的量測：一律開著、不改變任何一格的呈現，既有欄位的定義、位置、數值都不變，只在既有行的
行尾加欄位、另加幾種新行。之後要調頭盔端管線的每一步都拿它們當儀表。

```
同場切換（§VR-LATCH A/B）
[VIPLE-VR-LDIAG] client cfg: …原樣… abSec=N                                   ← 既有欄位，現在印的是實際生效的值：`--vr-latch-ab <秒>`（dev，0 或 20～600；只覆寫這一次、
                                                                                 不存設定，0＝這一場不切換）優先，沒帶才用保存的 `vrlatchabsec`
[VIPLE-VR-LDIAG] ab: N switches are armed and will toggle together: latchRelatch, latchWait
                                                                              ← warning，bring-up 時一次：開了同場切換、會跟著切的開關卻有兩個以上——分不出是誰的效果，
                                                                                 要比哪一個就只開那一個
[VIPLE-XR] 10s video …原樣… ab=<0|1|m>                                        ← 行尾新增（開了同場切換才印）：這個視窗 99% 以上的格跑在哪一邊；m＝兩邊混在一起（換邊落在
                                                                                 視窗中間），不拿來比。這一行的 10 秒視窗是它自己的計時，但和 [VIPLE-VR-MTP10]／[VIPLE-VR-LDIAG]
                                                                                 的視窗一樣從第一格起算，整場只差幾格，所以平常和同一輪的 LDIAG 標的是同一邊：每一邊的最後
                                                                                 一個視窗標那一邊，換邊後的第一個視窗標新的那一邊（還沒穩下來的那一個，比的時候照樣丟掉）。
                                                                                 兩種視窗差到 1% 以上（120 Hz 是 12 格）的時候，換邊的那個視窗才會標 m

frame loop（XR 桌面也有）
[VIPLE-XR] 10s frames=… …原樣… stale=N discarded=N                            ← 行尾新增：這 10 秒 xrBeginFrame 回 XR_FRAME_DISCARDED 的次數（正常是 0）
[VIPLE-XR] xrBeginFrame returned XR_FRAME_DISCARDED (N so far)                ← warning，一場最多 10 行：前一格 begin 之後沒有 xrEndFrame 就又 begin，runtime 把前一格作廢。
                                                                                 它是成功碼，流程照常；以前只檢查失敗碼，這個結果看不到
[VIPLE-VR-XRLOOP] 10s order=legacy | wakePhase p50/p95/max=…/…/… us | latchLead p50/p05=…/… ms endLead p50/p05/min=…/…/… ms | span p50/p95/max=…/…/… us | beginCall max=… us endCall p50/p95/max=…/…/… us | ahead p50/min/max=…/…/… ms[ (approx)] | waitShort=N | chain dec/render/slack/lead p50=…/…/…/… ms sum=… ms | tail p50/max=…/… us[ ab=<0|1>]
                                                                              ← PCVR，每 10 秒一行，和 [VIPLE-VR-LDIAG] 10s 同一格印、排在它後面（視窗與 ab 都相同）。
                                                                                 每第 6 個視窗兩行中間隔著一行 [VIPLE-VR-LDIAG] 60s gpu-by-phase，所以要把兩行配成一對就找
                                                                                 「前一行 LDIAG 10s」，不要找「上一行」。相位是 LDIAG 相位。
                                                                                 order＝frame loop 的呼叫順序，目前固定是 legacy；
                                                                                 wakePhase＝xrWaitFrame 返回的相位（預期：120 Hz 約 4840，也就是距 vblank 0.22 ms）。p50 是
                                                                                 環狀的中位數；p95、max 是從 p50 往晚的方向數的，不折回——晚到繞過格點的那一格會讓 max 超過
                                                                                 一個週期（例：p50 4840、max 8540＝有一格晚了 3.7 ms 才返回），而不是折成一個很小的數字。
                                                                                 晚超過半個週期的分不出是晚還是早，那種格看 ahead 的 min；
                                                                                 latchLead／endLead＝挑影像／xrEndFrame 返回的那一刻，到這一格的預測顯示時刻還有多久
                                                                                 （p05、min＝最晚的那幾格；預期延後 6 ms 時 latchLead 約 22.3 ms）；
                                                                                 span＝xrWaitFrame 返回到 xrEndFrame 返回（SteamVR 自己記的 ApplicationTime CPU 最貼近這一段）；
                                                                                 beginCall／endCall＝xrBeginFrame、xrEndFrame 這兩個呼叫本身花多久；
                                                                                 ahead＝這一格的預測顯示時刻 − 返回當下，逐格的值（`10s pcvr` 的 predictAhead 是平滑過的）。
                                                                                 max 多出一個週期＝runtime 把預測往後推了一格；沒有時間換算擴充時是估計值，標 (approx)
                                                                                 （有擴充、某一格卻換不出來時，那一格的 wakePhase／latchLead／endLead／ahead 都不記）；
                                                                                 waitShort＝相鄰兩次 xrWaitFrame 返回的間隔不到半格的次數（正常是 0）；
                                                                                 chain＝延遲鏈：dec（解碼：DU 進佇列到解出）＋render（影像 render thread 開始處理到 GPU 做完）
                                                                                 ＋slack（GPU 做完到挑影像，可以是負的）＋lead（挑影像到預測顯示時刻）各自的 p50 與四項的和。
                                                                                 沒有含的只有解碼幀轉手與在 mailbox 裡等的時間。這個視窗沒有樣本的那一項印 `-`（沒有解碼樣本的
                                                                                 dec；一張新影像都沒挑到時的 render 與 slack；預測顯示時刻都換算不出來時的 lead），少任何一項
                                                                                 sum 也印 `-`——不會印成一條少了幾毫秒、看起來正常的鏈。
                                                                                 **同一場裡比延遲看 chain**，不要看 [VIPLE-VR-MTP10] 的 p50（鎖相開著時它只會一次跳半個週期）；
                                                                                 tail＝上一格的 xrEndFrame 返回到這一格呼叫 xrWaitFrame——frame thread 在兩格之間記帳、印 log、
                                                                                 處理事件花多久（預期 p50 約 0.2 ms）。它吃掉的是「回到 xrWaitFrame 到下一次放行」的餘裕
                                                                                 （頭盔上 p50 約 1.7 ms、最少約 1.1 ms）：max 接近這個數字＝那一格晚放行，量測本身擠到呈現了。
                                                                                 每 10 秒印這幾行的那一格工作最多，它的 tail 算在**下一行**的 max 裡；只算緊鄰的兩圈
                                                                                 （session 沒在跑、xrWaitFrame／xrBeginFrame 失敗的圈之後那一格不算）

逐張的晚到計數（PCVR；不看分類器的基準）
[VIPLE-VR-LDIAG] 10s …原樣…[ wait=…] | events=N stride=N waitNew=N waitBy(us<250,500,750,1000,1500,2500,4000,+)=n,n,n,n,n,n,n,n waitSlots(1,2,3+)=n,n,n resc=N rescBy(us<250,500,750,1000,1500,+)=n,n,n,n,n,n flip=N[ ab=<0|1>]
                                                                              ← 行尾新增（排在 wait=… 之後、ab= 之前）。
                                                                                 events＝多出來的重複顯示格數＝lateShift＋early＋stride×(late＋gap)。**流暢度看它**：late／lateShift／
                                                                                 early 怎麼分要靠分類器的基準，基準要連 120 張正常才更新，每場開頭與事件多的場次可能整場停在
                                                                                 另一邊（晚到的影像全記成 early、lateMax 是 0），這個總和不受影響；
                                                                                 stride＝串流一幀佔幾個顯示格（全速 1、半速 2）；
                                                                                 下面幾欄用「等待格」（已經輪到下一張影像、它卻還沒發布的重複格）與影像的送出時刻，不看基準：
                                                                                 waitNew＝有等待格之後才顯示的新影像張數；waitBy＝它比最後一個等待格的 latch 晚多久才發布
                                                                                 （µs，分八格）；waitSlots＝它等了 1／2／3 格以上；
                                                                                 resc＝其中在那個 latch 當下已經送出的（GPU 工作都送出去了，比 latch 早 150 µs 以上）——那一格就
                                                                                 發布的話顯示得到；rescBy＝它們晚多久（前兩格≈多等一下就等得到的張數）；
                                                                                 flip＝沒有等待格、但上一格的 latch 當下已經送出的新影像——一送出就發布的話會早一格顯示、搶掉
                                                                                 前一張的格（半速＋補幀時搶掉的是合成的那一格）。它數的是張數，不是會多搶的格數：整條對位
                                                                                 晚一格的那段期間，每一張都在上一格的 latch 之前就送出了，所以每一張都算一次（數字會接近
                                                                                 影像的張數）——那種狀態下一送出就發布只是整條早一格、少一個重複格。數字大＝對位停在晚的
                                                                                 那一邊；要不要做提早發布只看 resc。
                                                                                 讀法：waitBy 落在後面幾格（≥2500）的不一定是晚到，前一張提早搶格之後照原本節拍來的下一張也
                                                                                 落在那裡——不拿 waitBy 的總數當晚到張數，看前面幾格的形狀。waitNew − resc 裡有一塊是補包拖晚的幀，
                                                                                 大小看同一輪 [VIPLE-VR-MTP10] 的 latchSkip。frame thread 沒回報的格之後的那一張、開場的第二張
                                                                                 影像不計。自洽：rescBy 各格相加＝resc；waitBy、waitSlots 各格相加＝waitNew；resc ≤ waitNew

影像 render thread（XR 桌面也有；沒有顯示格點時相位與比例印 -。顯示格點只有 PCVR、而且那一格的預測顯示時刻換算得出來才有：
                   沒有時間換算擴充、或那一格換算失敗時，那幾張影像不記相位——不拿「返回後兩個週期」的估計值充數）
[VIPLE-XR] 10s video …原樣… | mbox p50/p95=…/… ms swWait p95/max=…/… ms swNotOk=N rel max=… us | sub/done p50=…/… us coll=P% queued=P%[ synthSub/synthDone p50=…/… us][ ab=<0|1|m>]
                                                                              ← 行尾新增。mbox＝解碼幀在 mailbox 裡等多久（放進去到影像 render thread 取出；延遲鏈沒有含的
                                                                                 那一段。補幀開著時影像 render thread 還在做補幀那一批，新的一幀進不來，p95 會比較長）。
                                                                                 只記畫出來並發布的那幾張：沒有 meta 被丟的、stall 測試丟掉的、acquire／render 失敗的幀不在
                                                                                 裡面（它們算在同一行的 noMetaDropped／stallDropped／errors）；
                                                                                 swWait＝xrWaitSwapchainImage 這個呼叫花多久；swNotOk＝它回的不是 XR_SUCCESS 的次數（等滿
                                                                                 100 ms 回 XR_TIMEOUT_EXPIRED 也算；正常是 0。只量，流程不看它）；
                                                                                 rel＝xrReleaseSwapchainImage 這個呼叫的最大值；
                                                                                 sub／done＝送出（這張影像最後一筆 GPU 工作送出去）與 GPU 做完的相位（LDIAG 相位）的 p50；
                                                                                 coll／queued＝送出的相位換成 vblank 相位後落在 2.0～3.6 ms（做到一半會被頭盔合成器插隊）
                                                                                 ／3.6～5.2 ms（排在合成器後面）的比例——不必擷取 gpu-trace 就看得到的合成器干擾。這兩段是
                                                                                 全速量的（半速＋補幀時被擋的那一段早 0.25～0.5 ms）；顯示不是 120 Hz 時印 -；
                                                                                 synthSub／synthDone＝補幀開著時，補幀那一批送出與做完的相位。
                                                                                 這四個相位的 p50 是**環狀的中位數**（見下面第二點），不是照數值的大小取的

每個 pass 的 GPU 時間（PCVR）
[VIPLE-VR-PASS] 60s convert0:n/avgUs/minUs/maxUs[ convert1:…] unwarp:… down8:… down16:… flow:… combine:…[ other:…]
                                                                              ← 影像 render thread 每 60 秒一行，只列這 60 秒有樣本的名稱（不分同場切換的哪一邊）。每一欄＝
                                                                                 次數／平均／最小／最大（µs）。convert<n>＝解碼出來的影像轉成 RGB（libplacebo 的 pl_render_image；
                                                                                 n 是它在那一次算繪裡的第幾個 pass，3 以上併在 convert3；補幀開著時每張影像做兩次）；
                                                                                 unwarp＝注視點還原；down8／down16＝補幀用的 1/8、1/16 縮圖；flow＝位移估計（粗、細兩層記在
                                                                                 一起）；combine＝補幀的合成；other＝認不得的 pass（不該出現）。
                                                                                 數字是 libplacebo 每個 pass 本來就帶的 GPU 計時，這裡只是接出來：被頭盔合成器插隊的那一次會含
                                                                                 被擋住的時間（看 min 與 avg 差多少）；結果晚幾格才查得到，個別樣本可能重複或漏掉，60 秒的平均
                                                                                 與極值可以用
[VIPLE-VR-PASS] pass timers unavailable on this driver                        ← 開場 60 秒一筆結果都沒有（這顆驅動的佇列不支援時間戳查詢），只印一次

decoder 執行緒（VR 走真 XR 才有）
[VIPLE-VR-DECPOLL] 10s frames=N recvWait p50/p95/max=…/…/… us polls=N pollUs=0 mixed=0
                                                                              ← decoder 執行緒自己每 10 秒印（視窗和別的行不是同一個）。frames＝這 10 秒取到的解碼輸出幀數；
                                                                                 recvWait＝取到輸出的那一刻，距最近一次把輸入交給解碼器多久。沒有輸出、也沒有新輸入時迴圈睡 1 ms
                                                                                 再問，所以這個數字是 1 ms 一格的（預期多數落在 1.1 ms 附近、少數 2.1 ms）；polls＝這樣去睡的次數；
                                                                                 pollUs＝輪詢的間隔（目前沒有另外設，固定印 0）；mixed＝這個視窗裡間隔有沒有變過（目前固定 0）

自測
[VIPLE-XR-PROBE] selftest-latch <PASS|FAIL> <案例> (…)                        ← `xr-probe --selftest-latch` 現在依序跑三個純邏輯 header 的自測、一共 71 個案例。
                                                                                 xrlatchstats.h：原有的 27 個（輸出不變），加上 `latch-wait-…`／`latch-flip-…`／
                                                                                 `latch-half-wait-…`／`latch-half-flip-…` 15 個（不看基準的晚到計數：差一點、真的晚、150 µs 的
                                                                                 邊界、半速、基準過期、兩張擠在同一格、沒有送出時刻、分格的邊界、隨機腳本的自洽）；
                                                                                 xrphase.h 的 19 個：`phase-…` 17 個（LDIAG 相位 ↔ vblank 相位、120 Hz 的判斷、交出相位的
                                                                                 分類、延遲鏈與 events 的算式，以及相位的環狀中位數：橫跨接縫的一群、整群轉任何角度、
                                                                                 散滿整圈的視窗、離群值、不折回的 p95／max、邊界）、`rank-scan-matches-sort`（10 秒行那些
                                                                                 分位數的取法，逐一和排序的結果比對）與 `ab-window-side`（同場切換時一個視窗算哪一邊）；
                                                                                 xrvideostats.h 的 10 個：`video-…` 7 個（`10s video` 行尾那一段：空的視窗、全速、補幀那一批
                                                                                 橫跨接縫、沒有顯示格點、別的更新率、分位數的位置、視窗的輪替）與 `pass-…` 3 個
                                                                                 （[VIPLE-VR-PASS]：pass 的名稱怎麼認、60 秒那一行、沒有計時只說一次）
```

- 相位有兩種座標：這份文件裡 `latchPhase`、`gpu-by-phase` 與這一節新增的相位欄位都是 **LDIAG 相位**（原點是預測顯示時刻的
  格點）；頭盔上 gpu-trace 的時間軸是 **vblank 相位**。在 Steam Frame 的 120 Hz 量到預測顯示時刻落在 vblank 之後 3.716 ms，
  所以 vblank 相位＝(LDIAG 相位＋3716) 對週期取餘（例：latch 的 LDIAG 相位 2750 µs＝距 vblank 6.47 ms）。這個常數只量過
  120 Hz，別的更新率不適用。
- 相位是環，`sub`／`done`／`synthSub`／`synthDone`（與 `[VIPLE-VR-XRLOOP]` 的 `wakePhase`）的 p50 取的是**環狀的中位數**：
  到所有樣本的環上距離總和最小的位置，也就是前半圈、後半圈各有一半樣本的那一點（做法：把環從這個視窗的中心的
  正對面切開，再照順序取中間那一筆）。不可以照數值的大小取——影像做完的相位散滿整圈、補幀那一批做完的時刻又正好
  橫跨 0／週期的接縫，照數值取的中位數看接縫落在哪裡會差一到四毫秒（拿既有場次的資料重建：`done` 差 0.8 ms 上下，
  `synthDone` 多數視窗差 1 ms 以上）。資料散得很開時這個數字每個視窗會差幾百微秒，比兩邊時看多個視窗的中位數。
  `latchPhase` 是既有欄位，仍是照數值取的（它只擠在一小段，沒有差別）。

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

### `[VIPLE-XR]` —— XrContext（M3a β，XR 虛擬螢幕）

`streaming/xr/xrcontext.cpp`、`streaming/session.cpp`（只在 `CONFIG+=openxr` 建置；Windows 本機 `build_moonlight.cmd --openxr`）

```
[VIPLE-XR] instance: runtime="<名稱>" ver=<x.y.z> …                    ← 擴充先列舉後啟用；缺 XR_KHR_vulkan_enable2 即失敗
[VIPLE-XR] system: "<名稱>" …
[VIPLE-XR] vulkan: gpu="<GPU>" …                                        ← Vulkan instance／device 經 xrCreateVulkan*KHR 建立
[VIPLE-XR] session created: spaces=[VIEW,LOCAL,STAGE,LOCAL_FLOOR] refresh=<Hz>
[VIPLE-XR] session state <舊> -> <新>                                   ← 要看到 FOCUSED；null HMD 進 standby 會退回 SYNCHRONIZED
[VIPLE-XR] bring-up OK in <ms> ms: session begun, frame thread started
[VIPLE-XR] bring-up failed: <原因>
[VIPLE-XR] 10s frames=N missed=N (x.xx%) notRendered=N …                ← frame thread 每 10 s；前 2 s 暖機另計 warmupMissed
[VIPLE-XR] 10s video recv=N drawn=N overwritten=N render p50=<ms> p95=<ms> …   ← X2：影像 mailbox 與 XR thread 畫出統計
[VIPLE-XR] 10s d3d11-readback frames=N failed=N p50=<ms> p95=<ms> (sw=<fmt> WxH)   ← Windows XR＋D3D11VA：幀搬到系統記憶體再上傳
[VIPLE-XR] test map via d3d11 readback: ok|failed
[VIPLE-XR] frame thread exit: frames=N missed=N (x.xx%) …
[VIPLE-XR] xr-desktop unavailable (<原因>) - falling back to the flat window   ← 不變式 5：/launch 前失敗退回平面
[VIPLE-XR] xr-desktop requested but this build has no OpenXR - falling back …
[VIPLE-XR] session did not reach STOPPING within 3 s; forcing frame thread stop
[VIPLE-XR] session ended by the runtime: loss|exit                     ← X5：非 shutdown() 的結束（LOSS_PENDING／instance loss／SESSION_LOST，或 runtime 發起的 EXITING）
[VIPLE-XR] XR session lost - rebuilding (up to 3 attempts); video paused
[VIPLE-XR] XR session rebuilt (attempt n/3)                            ← 之後 Recreating renderer by internal request（decoder 重建、要 IDR）
[VIPLE-XR] XR rebuild attempt n/3 failed: <原因>                        ← 退避 0.5／1／2 s
[VIPLE-XR] giving up on XR after 3 attempts - falling back to the flat window
[VIPLE-XR] runtime ended the XR session (EXITING) - continuing in the flat window
[VIPLE-XR] xrWaitFrame: <XrResult> - treating as session loss
[VIPLE-XR] no Wayland/X11 display - using the SDL offscreen video driver   ← Linux 無頭；VAAPI 另印 `VAAPI: offscreen video driver - using a DRM render node display`
[VIPLE-XR] … SDL offscreen video driver (PCVR under gamescope: headless, no flat window)   ← §VR-HEADLESS：Frame 上 PCVR 刻意不開平面視窗（否則 Frame 加回手把模式＋雷射滑鼠）
[VIPLE-XR] test: simulating LOSS_PENDING|a runtime-initiated exit (--xr-test-fail)   ← dev
[VIPLE-XR] test: simulating a GPU wait timeout (--xr-test-fail gpuwedge)              ← dev（M4a 收尾）
[VIPLE-XR] GPU wait timed out (<原因>) - treating the XR context as lost              ← M4a 收尾：render 等 1 s／拆除等 2 s 逾時；之後走 loss 流程
[VIPLE-XR] video: GPU wedged - leaking libplacebo objects and N in-flight frame(s)
[VIPLE-XR] GPU wedged - leaking the Vulkan device instead of waiting for it
[VIPLE-XR] display refresh rates=[72.0,90.0,120.0] current=<Hz> -> requested <Hz> (want 90.0): XR_SUCCESS   ← PCVR（M4a 收尾）
[VIPLE-XR] display refresh (running, try N) rates=[…] measured=<Hz> want=<Hz> -> requested <Hz>: XR_SUCCESS | keeping the measured rate (nothing closer)
                                                                                    ← §XR-REFRESH-LATE：session 跑起來、週期穩定後不是想要的更新率時再要求一次（最多 2 次）
[VIPLE-XR] display refresh rate changed <舊> -> <新> Hz                             ← XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB
[VIPLE-XR] refresh mismatch: runtime reports <Hz> (requested <Hz>) but predictedDisplayPeriod=<ms> (<Hz>) - using the measured period
[VIPLE-XR] predictedDisplayPeriod did not settle within <ms> ms (last <ms>) - using it anyway
[VIPLE-XR] destroyed
```

- M4a 收尾（CLI `stream`，main.cpp）：session 建立後的錯誤印 `Stream error: <訊息>`／`Stream error: starting <stage> failed (error <n>)…` 到 stderr，CLI 以 rc 1 結束（不開對話框）；建立前的錯誤仍是 `Stream failed: …`。

- `--display-target xr-desktop` 在 `startConnectionAsync` 開頭（`/launch` 與 relay 之前）bring-up，逾時 5 s；拆除在 decoder 刪除之後、`SDL_DestroyWindow` 之前。
- 沒有影像時畫 loading quad（前方 1.5 m、寬 60°、深灰）；X2 起影像由 XrRenderer（frontend，只放 mailbox）交給 XR thread，用 `pl_vulkan_import` 共用 XrContext 的 VkDevice（queue lock 同一把）以 libplacebo 畫進影像 quad swapchain。testRenderFrame 在 XR 的 pl_gpu 上實際 map 一次，失敗退回平面。
- dev：`--xr-dump-frame <path>` 把第 300 幀的影像 quad 讀回存 PNG（最長邊 ≤ 1280）。補幀（§VR-SYNTH）開著時改成從第 2400 張起
  每 480 張存一組、一共六組：`<path>` 是第二次發布的那一張（上半真的、下半合成），`<path 去掉副檔名>-first.png` 是第一次
  發布的那一張；第 2 組起檔名在副檔名之前加 `-2`、`-3`…。兩張的上半必須逐像素相同（2026-10-10 的畫面錯亂就是先發布的
  那一張畫錯、只存第二張看不出來）。log：`[VIPLE-XR] frame dump: <檔名> (WxH from WxH, mode=…)`。
- X3：螢幕依頭部水平朝向擺在正前方 1.5 m（第一次 FOCUSED 時；Ctrl+Alt+Shift+R recenter，log `placement(recenter)`）；有 `XR_KHR_composition_layer_cylinder` 送 cylinder，否則 quad。影像在獨立 render thread 畫（XR thread 只送最後 release 的影像＝最後一幀複本），stale 狀態 `no-video/live/stale/lost`：1 s 沒新幀疊橘色狀態條、5 s 改 loading。10s 統計分 XR thread cpu 與 render thread cpu／gpu 等待。dev：`--xr-test-stall-ms`、`--xr-test-recenter-sec`。
- S2（Windows SteamVR null driver）注意：OpenXR app 只會拉起 vrserver，要先完整啟動 SteamVR（否則 `xrGetVulkanGraphicsDevice2KHR` 回 RUNTIME_FAILURE）；null HMD 不動會進 standby、沒有控制器時 dashboard 搶焦點（停在 VISIBLE），驗測前要關 standby 與 dashboard。
- `xr-probe --session [--duration N]`：B 段用 XrContext 實跑 session 與 frame loop，回報最高狀態、幀數、miss%、refresh、每眼 FOV、reference space；rc 0 成功、13 runtime 載不起來、14 runtime 錯誤。

### `[VIPLE-XR]` pcvr —— PCVR 走真 XR（M4a R1）

`streaming/session.cpp`（`setupXrPcvr`）、`streaming/xr/xrcontext.cpp`

```
[VIPLE-VR-SESSION] PCVR XR measured: space=<STAGE|LOCAL_FLOOR|LOCAL> tracking=<thread|frameloop> hz=<x>[ (clamped)] period=<ns>
                   recommended=<w>x<h> ipd=<mm> fov L[l r u d] R[l r u d] deg -> launch eye=<w>x<h> hz=<n>
[VIPLE-VR-SESSION] PCVR: XR runtime unavailable (<原因>) - streaming flat (invariant 5)
[VIPLE-XR] bring-up OK in … (… pcvr space=STAGE tracking=thread)
[VIPLE-XR] pcvr projection: <w>x<h> per eye from <2w>x<h> SBS, space=…, first render pose (x, y, z | qx, qy, qz, qw) echo=<sampleId>
[VIPLE-XR] pcvr video state <no-video|live|fade|loading> -> <…> (age <ms>)   ← §VR-STALE：>500 ms 疊半透明黑、>2000 ms 改 loading quad（2026-10-03 前是 100／250 ms）
[VIPLE-XR] 10s pcvr projection=N noMeta=N state=<0..3> space=… tracking=… predictAhead=<ms> hmd=(x, y, z | qx, qy, qz, qw)
[VIPLE-VR-SESSION] XR session <lost|exited> after the VR session started - ending the stream (invariant 5)   ← M4a 收尾：同時顯示錯誤並以正常退出送 /cancel
[VIPLE-VR-SESSION] bitrate <kbps> kbps (<VR default|--bitrate>; <w>x<h>@<hz>, flat setting <kbps> kbps / flat default <kbps> kbps ignored, ABR on|off)
                                                                                    ← §VR-BITRATE：VR 不再參考平面位元率偏好，只有 CLI --bitrate 才覆寫 VR 預設
[VIPLE-VR-SESSION] PCVR: MP-QUIC off for this session (preference unchanged)       ← §VR-LAUNCHER：PCVR 執行期不開 QUIC，偏好設定不動
[VIPLE-VR-LAUNCHER] mode=<vr|desktop>                                              ← Frame GUI 的模式選單（steam_frame_client.md §8.6）
[VIPLE-VR-LAUNCHER] launching VR stream: app="<name>"[ (takeover)]                 ← GUI 另開 `stream --display-target pcvr [--takeover] -- <位址> <app>` 子行程
[VIPLE-VR-LAUNCHER] stream process finished exit=<rc> status=<0 正常|1 崩潰>[ error=<給使用者看的原因>]
[VIPLE-VR-LAUNCHER] failed to start the stream process                             ← error
[VIPLE-VR-LAUNCHER] could not take the single-instance lock back                   ← warning：子行程結束後 GUI 拿不回單一實例鎖（別的實例趁隙拿走）
[VIPLE-VR-LAUNCHER] waiting for the VR stream process to finish                    ← GUI 結束時子行程還在跑：先等 8 s，再 SIGTERM，最後 SIGKILL
[VIPLE-VR-SESSION] PCVR has no flat window to fall back to: <原因>                  ← error：無頭 PCVR 不退回平面，以啟動錯誤結束
[VIPLE-VR-POSE] tracking sender started: <Hz> …, source=<xr-thread|xr-frameloop|sine|…>
[VIPLE-VR-POSE] 10s: sent=… fail=… late=… noPose=…    ← noPose：XR 還沒有有效 HMD pose 的拍數（不送）
```

- `--display-target pcvr` 不帶 `--vr-emulate` 時，XrContext 在 `initialize()` bring-up（串流尺寸決定之前）並量每眼 FOV、
  eyeToHead（以 VIEW space locate）、period 填 `/launch`；每眼解析度用 `--vr-eye`（預設 2160²，2026-10-03 前是 1728²）。Hz 夾在 60–144（Monado
  null compositor 固定 20 Hz）。tracking：有 XrTime 換算（Linux `XR_KHR_convert_timespec_time`、Windows
  `XR_KHR_win32_convert_performance_counter_time`）時送出執行緒當下 `xrLocateSpace`（thread 模式，`vrCaps` 帶
  TRACK_THREAD），沒有時沿用 frame loop 最近一次 locate（frameloop 模式）。
- projection：單一 2W×H swapchain，左右眼各取一半 imageRect；view pose＝0x81 帶回的 renderPose（追蹤參考空間）∘
  eyeToHead，fov 用量測值（＝協商值，overscan 0）。
- XR 在 VR session 建立之後失效：先停 tracking、拆 XR、結束串流（`/cancel` 與 UI 錯誤訊息待後續切片）。

### `[VIPLE-VR-INPUT]`／`[VIPLE-VR-HAPTIC]` —— PCVR 控制器與震動（M4a R2）

`streaming/xr/xrvrcontrollers.cpp`、`streaming/xr/xrcontext.cpp`、`streaming/session.cpp`（`handleVrMessage`）

```
[VIPLE-VR-INPUT] bindings accepted: <profile> (N)             ← 完整清單被拒時：… full binding list rejected, retrying core set
[VIPLE-VR-INPUT] interaction profile L|R: <profile> (wire profile <n>)   ← M4a 收尾：0x5506 input.profile（1 Touch、2 Index、3 Frame、4 其他）
[VIPLE-VR-INPUT] 10s L active=0|1 edges=N pressCtr=0x… | R active=… | focused=0|1 systemCombos=N focusLoss=N | haptic applied=N failed=N dropped=N
[VIPLE-VR-HAPTIC] rx=N queued=N device=1|2 dur=<us> freq=<Hz> amp=<0-1> id=N   ← 前 3 則與每 50 則印一次
[VIPLE-VR-HAPTIC] xrApplyHapticFeedback(L|R): <XrResult>        ← 前 3 次失敗
```

- PCVR 取代 β 的 XrInput（一個 session 只 attach 一組 action set）：grip pose（含速度）→ 0x5506 `pose[LEFT/RIGHT]`
  （server 自己套 raw_from_grip）；按鍵依 `docs/vr_protocol.md` 的按鍵 bit 填 `buttons／touches`，`pressCtr` 每鍵 2 bit
  按下邊緣計數；trigger／grip 0–65535、搖桿 ±32767、battery 255（未知）；`flags` b0 active（pose 有效）、b1 focused。
- 綁定：simple（select 當 trigger）、Oculus Touch（左 x/y/menu、右 a/b/system，trigger／squeeze 以 0.70 推 click）、
  Valve Index（兩手 a/b、system、trigger click）、Frame（`frame_controller_valve`：右 a/b/menu；左 dpad_down→A/X、
  dpad_up→B/Y、view→MENU，**左手幾何待實機確認**）。
- 沒有 FOCUSED：按鍵與類比值全部放開（pressCtr 不動）。同一手 menu＋trigger 按住 1 s → SYSTEM（期間遮掉 MENU 與
  TRIGGER），`systemCombos` 計數。
- haptic：0x5508/01 在 main thread 解析 → `XrContext::queueHaptic` → XR frame thread `xrApplyHapticFeedback`
  （duration 0＝最短、freq 0＝未指定）。**server 目前沒有送 HAPTIC**：driver 已把 SteamVR 的震動推進 IPC，但
  `vr::set_haptic_sink` 沒有人註冊（待做）。
- dev：`stream --vr-test-input`（8 s 一輪合成按鍵：右 A、B、trigger 漸進、搖桿；左 grip、menu＋trigger 1.4 s →
  SYSTEM）、`--vr-test-haptic`（每 3 s 本地注入一則 HAPTIC，走 clVrMessage 同一路徑）。

### M4a R4 補充（真 driver 端到端）

- `[VIPLE-XR] 10s video … stallDropped=N noMetaDropped=N`：PCVR 時沒有 0x81 render pose 的幀在 acquire 前丟棄的累計數（正常應為 0；非 0 代表 decoder 與 render 之間 meta 查不到）。`10s pcvr … noMeta=N` 暖機後也應為 0。
- `[VIPLE-XR-INPUT] keyboard disabled (no QGuiApplication, e.g. xr-probe)`：xr-probe（QCoreApplication）不畫虛擬鍵盤，屬正常。
- `[VIPLE-VR-CAP] adapter auto-pick: the only one of N hardware adapters driving a display: <GPU>`：server 雙顯卡時自動選卡。
- driver（`Steam\logs\vrserver.txt`）：`[VIPLE-VR-DRV] pipe lost gen=N reason=… -> standby (hmd stays connected)`＝server 消失後進 standby（R4 實測 kill server 後 89 ms）；`ipc handshake gen=N … idle=1 armed=0`＝新 server 重新握手。

### `[VIPLE-VR-MTP10]`／`[VIPLE-VR-LATCH]`／`[VIPLE-VR-TIMING]` —— 延遲與頻率鎖（M4a R3）

client `streaming/xr/xrcontext.cpp`；server `stream.cpp`、`vr/vr_latch.cpp`

```
[VIPLE-VR-MTP10] n=N p50=… p95=… p99=… ms noSample=N | latch sent=N (x/s) lastSlack=… us | timing sent=N   ← client 10 s；沒有時間換算擴充時行尾註明 approx
[VIPLE-VR-POSEERR] 10s n=N pos p50=… p95=… mm rot p50=… p95=… deg | lag n=N p05=… p50=… p95=… ms (+behind/-ahead, head moving)
                                                                                    ← client 10 s（§VR-POSEERR）：0x81 算繪姿態對「該幀實際顯示時的頭部姿態」的差。pos 是頭盔
                                                                                       重投影修不掉的平移偏差；lag 是算繪位置在頭部軌跡上對應的時間差（延後 100 ms 估，正＝落後），
                                                                                       只算頭部 ±50 ms 內移動 ≥ 10 mm、而且是第一次顯示的幀（同一張影像重複顯示時誤差照算、不估 lag）。
                                                                                       行尾有 ` ref=synthetic`（dev `--vr-synthetic-hmd`，2026-10-10）＝這一行是對合成的頭部軌跡量的，
                                                                                       不是頭盔真的姿態：pos／rot 是「host 預測到顯示時刻準不準」，不是重投影要補的量（rot>…、edge
                                                                                       也不代表露邊）。合成運動要有位移才估得出 lag：sine 約 67% 的新影像、fast 約 99%、tilt30yaw
                                                                                       約 41%（集中在某幾秒），yaw30／still 沒有位移＝0 筆
[VIPLE-VR-PREDICT] session start vsync_to_photons_us=N (<來源>) stream=X.XHz display=Y.YHz minSamples=K
                                                                                    ← server（§VR-PREDICT）。<來源>＝fixed by config｜learned｜converted from the value learned at X Hz
                                                                                       ｜default: period + 30 ms，2026-10-10 起多兩種：same display rate: value learned at X Hz stream、
                                                                                       default: display period + 30 ms（右括號之前的格式沒變，新欄位都加在後面）。
                                                                                       換算的來源是半速那一筆時寫成 converted from the value learned at X Hz display, Y Hz stream。
                                                                                       2026-10-05 起學到的值依更新率分開存；這個更新率沒學過時由最接近的更新率換算
                                                                                       （v' = 14 ms + (v − 14 ms)·T'/T）。2026-10-10 起比的是頭盔的顯示更新率（半速串流時 host 的週期
                                                                                       加倍、顯示週期沒變）：學到的值依（串流, 顯示）存；顯示更新率相同、只是串流幀率不同（全速↔半速）
                                                                                       的值直接沿用（same display rate…，X＝那個值是在多少 Hz 的串流學到的），不同才換算（converted…
                                                                                       的 X 是來源的顯示更新率，全速時和以前印的一樣）；沒學過任何值時是顯示週期＋30 ms
                                                                                       （半速印 display period，38.3 ms；之前用 host 的週期是 46.7 ms）。
                                                                                       stream／display＝host 串流與頭盔顯示的更新率（client 沒帶 vrDispHz 時兩個相同）；minSamples＝
                                                                                       這一場一個 1 s 視窗至少要幾筆樣本才採用：串流半秒的幀數，夾在 20～60（120 Hz 60、90 Hz 45、
                                                                                       72 Hz 36、60 Hz 30）。2026-10-10 之前固定 60，半速每秒只有 41～45 筆，整場不調整
[VIPLE-VR-PREDICT] probe: vsync_to_photons_us A -> B (pose lag mean of 3 windows x.x ms)   ← 探測：session 開頭 10 個可靠視窗（≥ minSamples 筆樣本）先丟掉（暖機），之後 3 個視窗
                                                                                       平均 ≥ 1.5 ms、且 3 個視窗最大差 ≤ 6 ms 時往補償方向改 8～10 ms
[VIPLE-VR-PREDICT] SteamVR applies mid-session changes (pose lag x.x -> y.y ms after A -> B us); tracking
                                                                                    ← 探測後 2 s 再取 3 個視窗平均，落後往預期方向移動 ≥ 步長一半：之後改追蹤（同一個行程的下一個 session 直接追蹤）
[VIPLE-VR-PREDICT] SteamVR did not apply the mid-session change (pose lag x.x -> y.y ms); keeping A us, next session starts at B us; <will probe again next session|second time in a row - later sessions only measure>
                                                                                    ← 探測沒效果：退回起始值、本 session 停調，下一個 session 從「起始值＋探測前後平均」開始；
                                                                                       2026-10-05 起連續兩個 session 都這樣才不再探測（舊版判一次就整個行程放棄）
[VIPLE-VR-PREDICT] probe inconclusive (pose lag windows spread x.x ms); back to A us, measuring again
                                                                                    ← 2026-10-05：探測後 3 個視窗最大差 > 6 ms（載入、掉拍），退回起始值重量，不算一次「不採用」
[VIPLE-VR-PREDICT] measured pose lag x.x ms at A us (no probe: SteamVR ignores mid-session changes); next session starts at B us
                                                                                    ← 2026-10-05：已知不採用時只量（舊版借用上面「did not apply」的訊息，看起來像又探測了一次）
[VIPLE-VR-PREDICT] 10s: vsync_to_photons_us=N (min A max B) pose lag ema=x.x ms updates=K windows=W
                                                                                    ← 追蹤階段的 10 s 摘要：落後的移動平均（α 0.25）≥ 1 ms 時補一半、每秒 ≤ 3 ms；session 內 ±40 ms；
                                                                                       每個視窗先夾到 ±20 ms。健康時 min／max 差幾 ms、ema 在 ±1 ms 附近
[VIPLE-VR-PREDICT] driver not connected; vsync_to_photons_us stays A                    ← warning：bridge 沒連線，這次不算調整（探測會重新量）
[VIPLE-VR-LATCH] pacing ppm=N slackEma=…us target=…us (applied|stub: log only)   ← server：前 5 次；之後變化 ≥ 50 ppm 且距上次 ≥ 5 s 才印
[VIPLE-ABR] Bitrate: A -> B kbps (cut|ramp, loss=N staleDrops=N in <ms>ms, src=…, vr lossPct=x.xx)
                                                                                    ← §VR-ABR-RATIO：VR session 的 cut 依丟包比例（1～3% −10%、3～8% −25%、>8% 砍半）；
                                                                                       <1% 當成零丟包（不加 FEC、不降碼、不擋回升）。桌面 session 沒有 vr lossPct、行為不變。
                                                                                       lossPct 的分母是照位元率估的影像封包數（不含 FEC），斷訊時回報的缺包可能超過它：> 100 時加註 `(est)`
[VIPLE-ABR] VR outage (src=…): will restore N kbps once the link is clean again   ← §VR-ABR-OUTAGE：丟包 ≥ 30% 的視窗或 client 回報連掉 3 幀以上（src=client-loss）＝斷訊。
                                                                                       記下斷訊前的位元率；斷訊期間照常降碼
[VIPLE-ABR] VR link clean again: restoring A -> N kbps (pre-outage bitrate)       ← 最後一個斷訊訊號後安靜 1 s、2 個乾淨視窗、client LATCH 幀號 300 ms 內有前進（沒 LATCH 的舊 client
                                                                                       不看）、參考值高於 target → 直接拉回（每次斷訊只拉一次）。修正前約 3 分 40 秒才回到 129 Mbps
[VIPLE-ABR] VR outage again right after restoring (src=…): restore target lowered to N kbps
                                                                                    ← 拉回後 5 s 內又斷、且這次事件在低位元率（≤ 參考值一半）時沒斷過：參考值打 75 折（連線可能已變窄）；
                                                                                       低位元率也斷過＝閃斷，不打折。client 重送的 LOSS 只延長事件；30 s 沒有斷訊訊號就結束
[VIPLE-FEC] VR unrecoverable frame, FEC: A% -> B%                                  ← §VR-FEC-LOSS：client 回報 LOSS、掉的是 1～2 幀 → +10%（上限 40%）
[VIPLE-FEC] VR outage (N frames lost), FEC stays A%                                ← 連掉 3 幀以上＝斷線，FEC 補不回來：不加，只把回降往後延
[VIPLE-FEC] VR no unrecoverable frame for a while, FEC: A% -> B%                   ← 30 s 沒有 LOSS、也沒有缺包 ≥1% 的視窗 → 每 5 s −5%，降回 fec_percentage
                                                                                       VR 的 FEC status（缺包 ≥1%）不再加 FEC，只延後回降；FEC 高於基準時影像位元率上限
                                                                                       ＝設定值 ×(100+基準)/(100+FEC)，上限移動時 [VIPLE-ABR] 立即套用（不受 10% 防抖動帶限制）
[VIPLE-VR-LATCH] 10s: rx=N slackEma=…us target=…us ppm=N haptics=N err=…us integ=… slips=N dropped=N mode=<legacy|v2>
                                                                                    ← server 10 s（haptics＝累計排入的 HAPTIC）。2026-10-05 起尾端加：平滑後的誤差、積分項（ppm）、
                                                                                       累計相位滑移（相鄰兩筆新量測跳 > 0.6 週期）、v2 丟掉的樣本（重複 frameId、|slack| > 1.5 週期）
[VIPLE-VR-LATCH] controller mode=<legacy|v2> targetPct=N hostPeriod=…ms            ← session 開始一次（§VR-LATCH-V2，設定 vr_latch_mode／vr_latch_target_pct）
[VIPLE-VR-TIMING] 10s: presented=N xrMissed=N metaMiss=N period=…ms decode p50/p95=… render p50/p95=… slack p50/p05=… mtp p50/p95=…ms
```

2026-10-09 起（§VR-LATCH-V3、§VR-LATCH-ACQ 與量測；既有欄位的順序不變，新欄位都加在行尾）：

```
[VIPLE-VR-LATCH] controller mode=<legacy|v2|v3> targetPct=N hostPeriod=…ms integSeed=<ppm|none> ab=<off|秒:模式:pct> acquire=<0|1>
                                                                                    ← targetPct 印生效值（v3 且 vr_latch_target_v3_pct > 0 時是它）；integSeed＝v3 從上一個 session
                                                                                       學到的積分（兩端的時鐘差）起步；acquire＝這個 session 會不會用到定時步進
[VIPLE-VR-LATCH] 10s: … mode=v3 st=<wait|measure|burst|track> gate=<0|1> steps=N bad=N
                                                                                    ← v3：st＝wait（節拍閘門關著：黑幀、host 沒在出幀、影像停住，輸出只剩積分）／measure（收 5 筆）／
                                                                                       burst（定時步進與它的盲區）／track（PI 追蹤）；steps＝累計步進次數；bad＝失敗的輪數。
                                                                                       v3 的 slackEma＝目標＋誤差，err＝最近 7 筆的環狀中位數到目標的最短路徑
[VIPLE-VR-LATCH] step why=<acq|trim|reacq> err=…us center=…us target=…us burst=…ppm x …ms base=…ppm
                                                                                    ← §VR-LATCH-ACQ：開始一次定時步進（err 正值＝影像太早到；burst＝這段時間的週期修正與長度；base＝結束後改回的值）
[VIPLE-VR-LATCH] step result: moved=…us of …us err=…us -> <track|trim|backoff>         ← 那一步量完的結果：開始追蹤／再補一步／這一輪失敗（退避 5 s 起加倍、上限 60 s）
[VIPLE-VR-LATCH] step result: moved=n/a of …us err=n/a -> void                          ← 那一步量不到結果（量完之前 host 掉拍、影像停住或 LATCH 停了）：不算成敗
[VIPLE-VR-LATCH] ab -> mode=… targetPct=N                                               ← vr_latch_ab 的同場 A/B 每次切換（切換後第一個 10 s 視窗不要算）
[VIPLE-VR-LATCH] vr_latch_ab could not be parsed (…) - ignored                          ← 格式錯：當成沒設
[VIPLE-VR-KICK] dev: PostPresent free-run for N ms                                      ← server（dev，`vr_dev_kick_every_s`，2026-10-10）：把 driver 的節拍模式暫時改成
                                                                                           「PostPresent 立即返回」N 毫秒的那一刻。host 直通落在多一格的時序（B）時，這一下會把它
                                                                                           推回等遊戲這一格（A）；本來就在 A 時狀態不變、那 10 秒多 3～4 張幀
                                                                                           （`steam_frame_client.md`「host 直通時的兩種時序」）。多出來的那幾張在 timing ring 裡
                                                                                           是「跨 0 格」的紀錄：server 知道這一段是自己開的，節拍偵測器不把它們當成不可能的值，
                                                                                           另外計在下面 `timing 10s` 行尾的 `freeRun`（不影響 `cadence`，也不會有 suspect 警告）
[VIPLE-VR-KICK] one period behind the game (presentPhase p50=… p90=… ms over N frames, echoAge p10/p50=…/… ms over M frames, period … ms): free-run for … ms (#n)
                                                                                    ← server（§VR-DIRECT-KICK，`vr_direct_kick = enabled`，2026-10-10；echoAge 是 2026-10-11 加的）：host 直通
                                                                                       落在多一格的時序（B）連續 3 秒，自動做一次 kick 的那一刻。判定為 B 要最近 1 秒的兩個量都成立
                                                                                       （每 100 ms 評估一次）：presentPhase 至少 24 張、p50 < 1.15 ms、p90 < 2.2 ms（印的 p50／p90／N 就是
                                                                                       那一秒的）；回聲年齡至少 24 筆、p50 ≥ 5.5 ms＋半個週期（60 Hz 是 13.8 ms、72 Hz 12.4、90 Hz 11.1；
                                                                                       印的 echoAge p50／M 就是那一秒的）。echoAge 的 p10＝同一秒的第 10 百分位，只印不判：判定只比中位數
                                                                                       的下限，追蹤樣本一陣一陣到（相鄰兩批隔 22 ms 以上）時沒有多一格的畫面 p50 也會過門檻，那時 p10
                                                                                       還是只有幾毫秒；真的多一格時每一張都舊一格，p10 照理和 p50 差不多（還沒有實際的數據，收到再決定
                                                                                       要不要把 p10 加進判定）。回聲年齡＝[VIPLE-VR-CAP] 10s 行的 echoAge 同一個量（影像 Present
                                                                                       的時刻減掉它用的姿態被發布給 driver 的時刻），擷取端每收下一張對到姿態的影像交一筆。60 Hz 量到
                                                                                       等遊戲這一格（A）4.4～7.7 ms、B 17.7～20.7 ms：B 多 13.0～13.3 ms，不是整整一個週期（B 的 Present
                                                                                       在格內也早 2～3 ms），所以 13.8 ms 的門檻離 A 的上緣 6.1 ms、離 B 的下緣只有 3.9 ms；72／90 Hz
                                                                                       沒有量測，照同樣的差外推，B 的下緣離門檻只剩約 2.4 與 1.0 ms。
                                                                                       只看 presentPhase 會誤判：2026-10-11 遊戲還在載入、第 0 層是合成器自己的畫面的那一兩分鐘連推三次
                                                                                       都沒有變化（當下 presentPhase p50 0.79～1.00 ms）。對照 log，那三次都在 client 不在的時候——斷線後
                                                                                       沒有追蹤樣本、影像都對不到姿態，或沒有串流在擷取——判定的視窗裡一筆回聲年齡都沒有；同一段載入、
                                                                                       追蹤樣本還在到的視窗，回聲年齡 p50 是 2.7～5.2 ms（presentPhase p50 1.17～2.13 ms）。兩種現在都不推：
                                                                                       沒有回聲年齡、或回聲年齡沒有多一格，就不會有這一行；頭部靜止的無人場次回聲年齡不可靠，也可能
                                                                                       不會有。presentPhase 像 B 而回聲年齡不成立的時候，印的是下面的 not kicking 行。
                                                                                       period＝host 的顯示週期；free-run＝PostPresent 立即返回多久（週期 × 1.25，夾在 20～40 ms）；
                                                                                       #n＝這個 generation 的第幾次。只在 host 週期 ≥ 11 ms（約 90 Hz 以下）時會有；握手後前 5 秒不評估；
                                                                                       離上一次 kick（含 dev 的）至少 5 秒。當下做不了的（另一段定時步進還沒結束）不印，B 再連續成立
                                                                                       3 秒才再試
[VIPLE-VR-KICK] result: waits for the game again (presentPhase p50 … -> … ms, echoAge p50 … -> … ms)
                                                                                    ← 那一次的結果（kick 之後 2 秒出來：前 500 ms 的樣本不看，接著 1.5 秒的 presentPhase p50 ≥ 2.0 ms）：
                                                                                       回到等遊戲這一格（A）。兩組數＝kick 之前、之後的 p50。成不成功只看 presentPhase；echoAge 只是印出來
                                                                                       對照（回到 A 應該少將近一個週期：60 Hz 約 13 ms），那 1.5 秒的回聲年齡不到 24 筆時箭頭後面印 n/a
[VIPLE-VR-KICK] result: no change (presentPhase p50=… ms, echoAge p50=… ms); next try in … s
                                                                                    ← 沒有變化（那 1.5 秒的 presentPhase p50 < 2.0 ms：還是 B，或剛好開了儀表板）。下一次最早要等
                                                                                       10／30／90／300 s（之後都是 300；成功一次就歸零），而且那時還要是 B（兩個量都成立）才會做。
                                                                                       presentPhase p50=n/a＝那 1.5 秒不到 24 張（幾乎沒有 Present），量不出來，照沒有變化退避；
                                                                                       echoAge p50＝同一段的回聲年齡（不到 24 筆印 n/a），只是印出來對照
[VIPLE-VR-KICK] the game's frame takes most of the period (presentPhase p90=… of … ms): leaving the timing alone from now on
                                                                                    ← 跟在 result: waits for the game again 後面：回到 A 之後那 1.5 秒的 p90 超過週期的 80%（遊戲一幀
                                                                                       佔掉大半個週期，留在 B 不一定比較差，而我們推不回 B）。這個 generation 之後不再 kick
[VIPLE-VR-KICK] 20 tries on this connection: leaving the timing alone from now on       ← 跟在第 20 次的 result 後面：一個 generation 最多 20 次，用完了，之後不再 kick
                                                                                       （那一行如果是 no change，它的 next try 不會發生）。driver 重連（新的 generation）才重新算
[VIPLE-VR-KICK] not kicking: presentPhase p50=… p90=… ms over N frames looks one period behind, but echoAge p10/p50=…/… ms over M frames does not (needs p50 >= … ms over >= 24 frames)
                                                                                    ← server（2026-10-11）：只有 presentPhase 那一邊像多一格——已經連續 3 秒，只看它的舊規則這時就 kick 了——
                                                                                       回聲年齡卻不成立，所以不推。數字＝這一次評估的那 1 秒視窗；needs＝回聲年齡的門檻（5.5 ms＋半個週期）與
                                                                                       筆數下限。M=0 時 echoAge 印 n/a：一筆回聲年齡都沒有（沒有串流在擷取、client 斷線沒有追蹤樣本、影像
                                                                                       都沒有對到姿態）。0 < M < 24 時數值照印，擋下來的是筆數。怎麼讀：
                                                                                       ・M 接近每秒的張數、p50 只有幾毫秒＝合成器自己的畫面（遊戲還在載入）之類，本來就沒有多一格；
                                                                                       ・M=0 而同一段 [VIPLE-VR-CAP] 10s 的 echoAge 有數值＝回聲年齡沒有交到偵測器（交接壞了）；
                                                                                       ・p50 離門檻很近、或 M 在 24 上下＝可能真的多一格卻推不出去（頭不動、對到姿態的影像不到四成；
                                                                                         72／90 Hz 的門檻是外推的）。
                                                                                       一條連線（generation）最多每 60 秒一行；不看退避與 5 秒的間隔（它說的是判定，不是 kick）；等結果的
                                                                                       那 2 秒與「leaving the timing alone」之後不評估，不會有。這一行不改變任何狀態
[VIPLE-VR-KICK] echo ages are arriving from the capture thread (first: … ms)            ← server（2026-10-11）：這條連線第一次收到擷取端交來的回聲年齡（每個 generation 一行）。整場都在等遊戲
                                                                                       這一格、沒有 kick 也沒有 not kicking 行的時候，靠它知道擷取端 → bridge 的交接是通的；數值應該和
                                                                                       [VIPLE-VR-CAP] 10s 的 echoAge 差不多。只在偵測器會動作的 generation（host 週期 ≥ 11 ms）才收，所以
                                                                                       120 Hz 不會有這一行
[VIPLE-VR-DRV] timing 10s: recs=… posehistMiss=N periodPpm min/max=a/b presentPhase p50/p95/p99/max=… ms bursts=N[ kicks=N]
                                                                                    ← periodPpm＝driver 這個視窗實際採用的週期相對標稱值（v2 在 ±200 內；有步進的視窗約 ±4000）；
                                                                                       presentPhase＝每一幀的 Present 落在虛擬 vsync 格內的位置（遊戲算完的時刻的抖動就在這裡）；
                                                                                       bursts＝走完的定時步進數（kick 結束時也算一次）；kicks＝這個視窗裡開始了幾次 kick（dev 的定時與
                                                                                       自動的都算）。kicks 這一欄只在 `vr_direct_kick = enabled` 或 `vr_dev_kick_every_s` 不是 0 時才有
[VIPLE-VR-TIMING] 10s: … mtp p50/p95=…ms newPer10s=N                                    ← client 這 10 s 顯示了幾張新影像（和 client 的 MTP10 frames new 對照）
[VIPLE-VR-LATCH] 10s: … mode=<legacy|v2|v3>[ st=… gate=… steps=… bad=…] overloadHeld=N   ← §VR-FENCE-LATE（2026-10-09）：行尾再加 overloadHeld＝這 10 s 走 hold（沒有餵進控制器）的 LATCH 筆數
                                                                                       （含在 rx 裡）。走不走 hold 看擷取端發布的決定：預設（vr_capture_overload = late）就是過載旗標（[VIPLE-VR-CAP] overload on），cadence 時見 [VIPLE-VR-CAP] hold 行。
                                                                                       那段期間三種模式的 ppm 都只剩積分（比例項拿掉）、integ 不動，也不會被當成 LATCH 停了而
                                                                                       衰減；legacy／v2 的 err 印 0（v2 的 slackEma 因此等於目標），v3 是 st=wait gate=0。
                                                                                       vr_capture_late = disabled 時恆為 0
```

2026-10-10 起（§VR-CADENCE；既有欄位的順序不變，新欄位都加在行尾）：

```
[VIPLE-VR-DRV] timing 10s: recs=… bursts=N cadence=<unknown|locked|slip|unsure> why=<none|marginal|heavy|recovering|recent|fraction|suspect|proving> div=<d> slips=<n> masked=<n> hitches=<n> skipped=<n> early=<n> slipSecs=<n> burstMax=<n> slipGapMaxMs=<ms> edge=<n> thrMax=<n> noTgt=<n> zeros=<n> negative=<n> restarts=<n> orphans=<n> freeRun=<n>
                                                                                    ← server 那一行（recs= 開頭的）行尾加 Present 節拍偵測器的結果。擷取端的兩個開關怎麼設都印；這個 generation 沒有 session config 時不印。
                                                                                       訊號是 timing ring 的 VRIPC_TE_VSYNC_VIRTUAL.arg1＝這次 PostPresent 跨了幾個虛擬 vsync 格，**準時是 1**（vr_ipc_abi.h 那一行的註解寫的是
                                                                                       「跳過的 vsync 數」，實際的值是跨的格數；那個檔和 driver 共用，等下次動 driver 一起改）。比節拍多跨 1 格＝滑動；多跨 2 格以上而且至少多 16 ms＝卡頓。
                                                                                       cadence／why／div＝印的當下偵測器的狀態（意思同 [VIPLE-VR-CAP] cadence 行；div＝每幾格一次 Present，unknown 時是 0）。
                                                                                       其餘是這 10 s 的計數：slips＝不屬於卡頓的滑動；masked＝卡頓後 3 s 內的滑動；hitches＝卡頓；skipped＝所有可用紀錄多跨的格數合計；
                                                                                       early＝比節拍少跨（div ≥ 2 才有）；slipSecs＝有 slips 的整秒數；burstMax＝單一整秒內最多幾個 slips；slipGapMaxMs＝這個視窗內
                                                                                       相鄰兩個 slips 最久隔多久；edge＝在節拍上、但 Present 落在格內最後 14% 的張數（只印不判）；thrMax＝看到的最大 nFramesToThrottle；
                                                                                       noTgt＝沒有輸出目標的紀錄；zeros／negative＝有輸出目標而跨 0 格／負值；restarts＝一次跨超過 2 s；orphans＝配不到同一幀其他紀錄的
                                                                                       VSYNC_VIRTUAL（紀錄掉了，不用）。欄位名刻意不和 driver 自己那一行（presents=… missed=… noTarget=…）重複。
                                                                                       freeRun＝有輸出目標而跨 0 格、但落在 server 自己把節拍模式寫成「PostPresent 不等虛擬 vsync」的那一段
                                                                                       （含寫回之後的 100 ms）的筆數：dev 的 vr_dev_kick_every_s 每次約 20 ms（[VIPLE-VR-KICK]）與 selftest 的節拍實驗。
                                                                                       那時合成器在同一格裡多跑幾輪，多出來的幾輪就是跨 0 格——合法的，不算進 zeros、不算不可能的值；
                                                                                       每個格點仍然恰好有一筆跨 1 格的紀錄，所以節拍的判定不受影響。沒開那兩種東西時恆為 0。
                                                                                       鎖住時：cadence=locked why=none div=1，slips／masked／hitches／skipped／zeros／negative／restarts／orphans 都是 0。
                                                                                       div=1、沒有卡頓時 skipped 應等於 driver 那一行的 missed（兩行的視窗錯開約 1～4 s）；對不上就是組幀或 arg1 的語意有問題。
[VIPLE-VR-DRV] cadence input suspect gen=<n>: impossible values in the timing records; Present cadence not used (the late-frame flag decides)
                                                                                    ← warning，每條連線一次：10 s 內有 3 筆不可能的值（跨 0 格或負值、一次跨超過 2 s）。這段期間偵測器報 unsure why=suspect，
                                                                                       擷取端照 late 旗標決定；10 s 內不到 3 筆才恢復。看同一段時間 timing 10s 行的 zeros／negative／restarts。
                                                                                       最可能的原因是 driver 與 server 的版本不一致（arg1 的語意不是「準時＝1」）。
                                                                                       dev 的 vr_dev_kick_every_s 不會觸發它（那一段的 0 計在 freeRun）；開著 kick 還看到這一行，照樣是上面的原因。
[VIPLE-VR-LATCH] 10s: … overloadHeld=N lockR=<0.00～1.00|n/a> lockRmin=<…|n/a> lockRmax=<…|n/a> lockN=<n>
                                                                                    ← lockR＝LATCH 樣本的相位一致性：最近 40 筆（8 s 內）的 slack 取模一個週期之後各當成一個單位向量，平均向量的長度。
                                                                                       0＝均勻散開（樣本沒有眾數）、1＝全部同相；不到 20 筆印 n/a。lockR／lockRmin／lockRmax＝這個視窗最後一次、最小、最大的值；
                                                                                       lockN＝這個視窗收的樣本數（重複的 frameId、|slack| 超過 1.5 個週期的不算）。被 hold 的樣本也算在裡面。只是診斷，不參與任何決定。
                                                                                       取模用的週期＝host 虛擬 vsync 週期與顯示週期的較小值（半速串流是顯示的那一個）；被 hold 的 LATCH 用那一則自己帶的顯示週期，
                                                                                       所以從第一則起就被擋、或擋著的時候換更新率，數字也是對的。
                                                                                       參考值（由頭盔 log 的直方圖換算，母體是所有發布的影像，和每 100 ms 一筆的 LATCH 不完全相同）：鎖住的視窗 0.31～0.80（中位數 0.63），
                                                                                       解析度倍率 400% 0.08～0.30。v3 有定時步進的視窗 lockRmin 會偏低（相位是被刻意移動的）；vr_latch_ab 切換之後重新收樣本。
```

- **過載時不採計（§VR-FENCE-LATE）**：host 擷取端的過載旗標設著時（fence 等待超過第一段等待、影像晚到；不一定是 GPU 壓滿），LATCH 量到的 slack 與 CLIENT_TIMING
  回報的姿態落後反映的是 GPU 排隊，不是節拍或預測時間。2026-10-09 的事故裡（舊規則的拆線循環，頭盔每秒只收到約 10 張黑幀）v2 的 ppm 頂到 +200、積分累積到 +118。
  旗標設著的期間：LATCH 樣本不進控制器（`overloadHeld`），輸出只留積分（學到的時鐘差）——旗標要看滿一段時間才設，設起來之前漏進去的
  幾筆已經把比例項帶偏（v2 三筆就頂到 ±200 ppm），所以不照原樣凍結；§VR-PREDICT 把那一秒當成沒有樣本（不探測、不追蹤、不更新下一場的
  起始值）。
  旗標清掉後 legacy／v2 的平滑值從第一筆新樣本重新開始，v3 重新收滿節拍閘門再量相位。
  §VR-CADENCE（2026-10-10）：以上是預設的 `vr_capture_overload = late`。`cadence` 時 LATCH 與 §VR-PREDICT 分開判（`[VIPLE-VR-CAP]` 的 `hold` 行）：
  跑滿更新率、只是每一張都晚的遊戲，LATCH 照常進控制器（它要的是影像到達的相位取模一個週期之後穩定），§VR-PREDICT 仍不取樣（它要的是絕對延遲穩定）；
  Present 鎖不住虛擬 vsync 時兩者都不取樣，即使沒有一張影像晚到。
  還沒處理的：旗標設起來之前的那一個 CLIENT_TIMING 視窗仍會被 §VR-PREDICT 採計；探測走到一半遇到過載時，前後的視窗會隔著過載相比
  （`docs/TODO.md`）。
- client 每 100 ms 送 0x5507/08 LATCH、每 1 s 送 0x5507/06 CLIENT_TIMING（都走 ch 0x07 unsequenced）；定義見 `vr_protocol.md` §4.5。
- `decode p50/p95`：2026-10-05 起是 decoder 執行緒記的單幀解碼延遲（DU 進佇列到解出，與統計浮層同定義；`VrDecodeTiming`），
  之前的 client 一律填 0。
- client 的 `[VIPLE-VR-MTP10]` 尾端（2026-10-05 起）：`frames new=N repeat=N skip=N mismatch=N`——新影像、latch 時沒有新影像
  （重複幀）、兩次 latch 之間到了兩張以上而沒顯示的、xrEndFrame 時 runtime 會用的影像不是 projection 姿態所屬那張（錯配一幀）。
- client 的 `[VIPLE-VR-POSEERR] 10s` 尾端：`rot>1deg=…% >3deg=…% edge(>Xdeg overscan)=…%`——旋轉誤差超過 overscan 的幀
  會在重投影時露出沒畫面的邊（dev `--vr-overscan` 0 時任何誤差都會露）。2026-10-10 起，dev `--vr-synthetic-hmd` 的場次
  在最後面再接 ` ref=synthetic`（整行改對合成的頭部軌跡量，見上面 `[VIPLE-VR-POSEERR]` 的說明）；沒開時這一行不變。
- client 的 `[VIPLE-VR-INPUT] 10s` 尾端：`vel L n=… linValid=…% angValid=…% |v|max=… |w|max=…`（右手同），runtime 回報的
  控制器速度有效率與最大值（無效時送 0，SteamVR 就不外插拍子）。
- pcvr 才把 ppm 寫進 driver pacing；stub 只記錄。

### `[VIPLE-XR-INPUT]` —— XR 控制器射線滑鼠（M3a X4）

`streaming/xr/xrinput.cpp`、`streaming/xr/xrkeyboard.cpp`、`streaming/input/mouse.cpp`（`handleXrPointer/Button/Scroll/Key`）

```
[VIPLE-XR-INPUT] bindings accepted|rejected: <profile> (N)       ← SteamVR 2.17.10 接受 simple、oculus touch、valve index、valve/frame_controller_valve
[VIPLE-XR-INPUT] interaction profile <hand> -> <profile>
[VIPLE-XR-INPUT] 10s frames=N hit=<%> moves=N buttons=N scrolls=N hand=<left|right> filterLatency~<ms> keyboard=<open|closed> keys=N sticky=0x<mods>
[VIPLE-XR-INPUT] keyboard opened|closed (<hotkey|controller|close key|dev test|focus lost>)
[VIPLE-XR-INPUT] keyboard keys and modifiers released (<why>)          ← 關鍵盤或失去 FOCUSED 時放開按住的鍵與黏滯修飾鍵
[VIPLE-XR-INPUT] <profile>: retrying without the keyboard toggle (<path>)   ← 鍵盤開關鍵的路徑不被 runtime 認得時，去掉它重送
[VIPLE-XR] keyboard texture ready in <ms> ms (first render warms up fonts)   ← 背景執行緒畫貼圖；Windows 首次畫字約 0.5～3 s
[VIPLE-XR] keyboard texture upload failed (format N)                      ← swapchain 不是 8-bit RGBA／BGRA，鍵盤不顯示
[VIPLE-XR-INPUT] dev key vk=0xNN down|up mods=0xN                        ← 只有 dev --xr-test-keyboard 才逐鍵記錄
[VIPLE-XR-INPUT] dev test keyboard done: N presses, N keys sent
[VIPLE-XR] dev keyboard dump WxH -> <path> (ok|FAILED)
```

- 射線與 quad／cylinder 求交得 UV，One-Euro 濾波後經 `SDL_CODE_XR_POINTER/BUTTON/SCROLL`（108–110）送到 main thread，以串流解析度送絕對滑鼠座標。trigger＝左鍵、squeeze 或 B＝右鍵、搖桿 Y 捲動；按下只在命中時送、放開隨時送；失去 FOCUSED 放開全部；XR FOCUSED 期間忽略平面視窗滑鼠。
- dev：`stream --xr-test-pointer` 合成射線（每 4 s 一圈、圓頂按一次 trigger）；`xr-probe --selftest-ray` 求交自測（全過 rc 0、有失敗 14）。
- 虛擬鍵盤：US QWERTY 五列（含 Esc、Tab、Bksp、Del、Enter、左右 Shift、Ctrl、Win、Alt、方向鍵、「中/英」、✕）。開關：Touch 左 menu、Index 左 thumbstick click、Frame 左 view（不被接受時自動去掉）、熱鍵 Ctrl+Alt+Shift+K、鍵盤上的 ✕。鍵盤優先於影像螢幕求交，命中時不送滑鼠。trigger 按下／放開＝按鍵按下／放開（host 端自己連發）；Shift／Ctrl／Alt／Win 黏滯一次，送出順序模擬實體鍵盤（修飾鍵先按、一般鍵、修飾鍵後放），經 `SDL_CODE_XR_KEY`（113）送 `LiSendKeyboardEvent`。「中/英」＝Shift 單擊（Windows 注音預設的中／英切換；Win+Space 是切換鍵盤配置，不是同一件事）。一般模式不記錄按了哪些鍵，只記數量。
- dev：`stream --xr-test-keyboard "<文字>"`（鍵盤出現後合成射線逐鍵點擊，大寫與 Shift 字元自動先點 Shift）、`--xr-dump-keyboard <path>`（貼圖 PNG；PNG 編碼在 XR thread，會讓那一刻漏幾幀，量效能時不要帶）。

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
