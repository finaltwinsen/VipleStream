# VipleStream 2.0 VR 協定擴充

> 狀態：**設計定稿（2026-09-23）**；**M1a 協定骨架已實作（2026-09-26）**，實作定案見 §4.9。架構背景見 [`vr_architecture.md`](vr_architecture.md)。
> 所有擴充都在雙方明確協商後才啟用，vanilla Moonlight／Sunshine 與舊版 VipleStream 的行為不變。

**路徑縮寫（repo 相對路徑）**
- `Q/` = `moonlight-qt/moonlight-common-c/moonlight-common-c/src/`
- `A/` = `moonlight-android/app/src/main/jni/moonlight-core/moonlight-common-c/src/`
- `S3/` = `Sunshine/third-party/moonlight-common-c/src/`
- `SS/` = `Sunshine/src/`
- `SW/` = `Sunshine/src/platform/windows/`
- `MQ/` = `moonlight-qt/app/`
- `MQS/` = `moonlight-qt/scripts/`（已進 git）
- 「本機」＝不進 git 的檔案：`build_*.cmd`、`build-tools\`、`CLAUDE.md`（`.gitignore:40-47,172`）

標「（新增）」的是新檔案，其餘路徑都確認過存在。

---

## 協定擴充

### 4.1 新增項目總表（這就是全部）

| 類別 | 新增 | 不做的事 |
|---|---|---|
| HTTP | `/serverinfo` 加 **2 個元素**：`<VipleStreamVR>`、`<VipleStreamVRProto>1`；`/launch` 加 `vr*` 參數；launch 回應加 `<VipleStreamVRSession>`；`/applist?vr=1` | 不擴充 RTSP、SDP |
| Control ptype | 0x5506 TRACKING（C→S）、0x5507 VR_C2S（TLV）、0x5508 VR_S2C（TLV）；0x5509–0x550F 保留 | 不進 IDX 表 |
| ENet | ch 0x07 `CTRL_CHANNEL_VR` | 不走 `IDX_INPUT_DATA` |
| QUIC | 沿用 flow 0x04 | 不新增 flow |
| 視訊 | 0x81 的 24 B header，只在 VR session 送 | 不用 SEI |

### 4.2 能力協商

1. **`/serverinfo`**：`<VipleStreamVR>` 是 bitmask。b0 PROTO_V1、b1 PCVR（只有 Windows）、b2 HAPTICS、b3 GAZE_UPLINK、b4 QP_FOVEATION、b5 RECOVERY_INTRA（encoder 支援 IR）。
2. **`/launch`**：`&vr=1&vrEye=WxH&vrHz=N&vrPeriodNs=<平均週期>&vrFov=<8 個 tan×10000，逗號分隔>&vrIpd=<mm×100>&vrEyeToHead=<14 個逗號分隔整數：每眼 pos mm×100 ×3、quat×10000 ×4>&vrCaps=<hex>&vrCodecs=<mask>&vrCtrl=touch|index&vrOverscan=<deg×10>&vrForce=0|1`。
   選填 `&vrFovea=<101–250>`（§VR-FOVEA，2026-10-08）：注視點編碼，正前方的像素密度是均勻取樣的幾 %；不帶或 100＝關。
   server 接受而且是真的 driver（`mode=pcvr`）時，在 `<VipleStreamVRSession>` 尾端加 `;fovea=<同一個值>`，client 看到才還原；
   `/resume` 回的是進行中那次編排的值。映射式：每軸以 tangent 0 為中心、每一側各自正規化，`t = a·e + (1−a)·e³`，`a = 100/值`。
   - 全部是十進位定點數，不用 base64。
   - 要有 round-trip 單元測試；server 解碼失敗就回非 200。
   - `vrCaps`：b0 RECOVERY_INTRA、b1 TRACK_THREAD、b2 GAZE、b3 SKELETON。
   - 送這些參數之前，XrContext 必須已經 running。
3. **launch 回應**：`<VipleStreamVRSession>` 內容包括 proto、打包尺寸、Hz、codec、layout=sbs、overscan、`recovery=intra|idr`、`irFrames=N`、`transport=rtp`、`universeId`、sessionGuid。
   - server 做不到時回非 200，附明確錯誤碼：`VR_BUSY`、`VR_TRANSPORT_UNSUPPORTED`、`VR_CODEC_LIMIT`、`VRLINK_ACTIVE`。
   - client 只有收到這個元素時才設 `vrFlags`。

### 4.3 Tracking 上行：0x5506（232 B，加 V2 header 共 236 B，上限 251 B）

```
u8 ver=1 | u8 flags(b0 hmd,b1 L,b2 R,b3 gaze,b4 presence,b5 phaseSample) | u16 spaceEpoch       = 4
u32 sampleId | u64 sampleTimeNs(client 時鐘) | u32 predictNs(target − sample)                   = 16
pose×3（HMD、L、R）：f32 pos[3], f32 rot[4], f32 linVel[3], f32 angVel[3]                        = 156
input×2：u32 buttons, u32 touches, u32 pressCtr(16 鍵 × 2 bit), u16 trigger, u16 grip,
         i16 sx, i16 sy, u8 battery, u8 flags(b0 active,b1 focused), u8 profile, u8 rsv           = 48
gaze：f16 yaw, f16 pitch, u8 conf, u8 flags, u16 rsv                                            = 8
```

- **input.profile（M4a 收尾，原 u16 rsv 的低位元組）**：client 目前的 OpenXR interaction profile
  （`VIPLE_VR_CTRL_PROFILE_*`：0 未知、1 Touch、2 Index、3 Frame、4 其他）。server 原樣 memcpy 進 IPC
  `vripc_ctrl_input_t.profile`（同位移，不 bump ABI），driver 依此選控制器 render model。舊 client 送 0，driver
  維持 Touch 外觀。

- **頻率**：2×顯示 Hz。b5 相位對齊取樣從 GA **提前到 rc**：每幀依 latch 回授多送 1 個樣本。
- **通道**：ch 0x07 UNSEQUENCED，沿用 C5 的 fallback。
- **ENet throttle**：抽成 helper `configureVrThrottle(peer)`，參數 `(5000, 2, 0)`。**首次連線（`:2477-2480` 之後）和 §Q-ENET-RECONNECT（`:1756` 之後）都在 `enetMutex` 內呼叫**，只在 VR session 生效。
- **server handler**：
  - 檢查 size、version、seq，套用時鐘對映，寫 seqlock，SetEvent；
  - 沒有協商 VR 的 session 一律丟棄；
  - 每 10 秒輸出 `[VIPLE-VR-POSE-RX]`。

### 4.4 每幀 metadata：0x81 header（24 B，`static_assert == 24`）

| byte | 欄位 |
|---|---|
| 0 | `0x81` |
| 1–2 | frame_processing_latency |
| 3 | frameType |
| 4–5 | lastPayloadLen（用 24 B 計算） |
| 6 | vrFlags：b0 poseValid、b1 echoMatched、b2 poseFallback、b3 repeated、b4 posRelative、**b5 derivedPose**（原本的 SBS 旗標移除，因為 layout 已經協商過）、b6 foveated（GA）、**b7 refreshDone** |
| 7 | layoutEpoch（涵蓋 recenter、overscan、foveation、**eyeToHead**） |
| 8–11 | echoSampleId |
| 12–17 | render orientation（client 空間），smallest-three 48 bit |
| 18–23 | render position 3×i16，單位 0.1 mm；超出範圍時設 posRelative |

- **client**：`Q/VideoDepacketizer.c:1002-1013` 在 `VrFlags != 0` 時解析；`DECODE_UNIT` 尾端加 `vrMeta` 和 `frameNumber` 的對應。
- **相容**：vanilla 和舊版 client 永遠不會協商 VR，所以看不到這個 header。

### 4.5 VR_C2S（0x5507）與 VR_S2C（0x5508）：TLV 容器

- **格式**：`u8 subtype, u8 len, payload[len]`，總長 ≤ 251 B，不認得的 subtype 依 len 跳過。

| ptype | subtype | 內容 | 通道 |
|---|---|---|---|
| 0x5507 C→S | 01 CONFIG | IPD、eyeToHead、FoV、Hz、renderScale | reliable |
| | 02 RECENTER | 新 spaceEpoch | reliable |
| | 03 PRESENCE、04 CONTROLLER_STATE | — | reliable |
| | 05 REFRESH_CHANGED | u16 Hz，觸發重開 session | reliable |
| | 06 CLIENT_TIMING | 1 Hz 統計（M4a R3 定義 32 B；§VR-PREDICT 2026-10-03 尾端加 4 B＝36 B，舊 server 只讀前 32 B）：`{framesPresented u32, xrMissed u32, metaMiss u32（三者累計）, displayPeriodNs u32, decodeP50Us u16, decodeP95Us u16, renderP50Us u16, renderP95Us u16, slackP50Us i16, slackP05Us i16, mtpP50_100us u16, mtpP95_100us u16, poseLagP50_100us i16（遊戲算繪姿態相對實際顯示的時間差，正＝落後、負＝超前；樣本不足＝0x7FFF）, poseLagCount u16}` | ch 0x07 unseq |
| | 07 FRAME_FEEDBACK（GA） | 每 8 幀一批 | ch 0x07 unseq |
| | **08 LATCH** | 10 Hz：`{frameId u32, slackUs i32, displayPeriodNs u32}` | ch 0x07 unseq |
| | **09 LOSS** | `{firstLost u32, lastLost u32, reason u8}`，送 2 次 | ch 0x07 unseq |
| | 7F | 保留：麥克風 | — |
| 0x5508 S→C | 01 HAPTIC | `{device, durationUs, freqHz, amp, eventId}`（M4a R3：pcvr session 由 `bridge::set_haptic_sink` 把 driver 推進 IPC 的震動打包送出） | ch 0x07 unseq |
| | 02 STATE | `{state, code, progress}`：編排進度、HMD 就緒、safe mode、`VRLINK_ACTIVE`、`ABI_MISMATCH_RESTART_STEAMVR` | reliable |
| | 03 STATS | 1 Hz 統計，含 `clkOffsetJitter`、`staleCount` | ch 0x07 unseq |
| | 04 CONFIG_ACK、05 LAYOUT | epoch 對應的 FoV、overscan、foveation、**eyeToHead** | reliable |
| | **06 REFRESH_START** | `{startFrame u32, frameCnt u8, reason u8}`，送 2 次 | ch 0x07 unseq |

- **client 接收（`Q/ControlStream.c`）**：
  - 新增偽 typeIndex `IDX_VIPLE_VR_S2C = 0x100`（不進 packetTypes 表）；
  - `QUEUED_ASYNC_CALLBACK` 的 union 加 `{uint8_t len; uint8_t *buf;}`，payload 另外配置 buffer 複製（最長 251 B）；
  - 改 `needsAsyncCallback()`（`:1220-1228`）和 `queueAsyncCallback` 開頭的 `LC_ASSERT`（`:1236`）；
  - `asyncCallbackThreadFunc` 的 switch（`:1094-1110`）加分支，並在 async thread 釋放 buffer；
  - 分派點在 `:1548-1550`。
  - `vrMessage` 回呼在 Qt 端一律包成 SDL_USEREVENT。

### 4.6 時鐘與 MTP 量測（D10 修訂：不做完整對時，但 server 端做輕量 offset 對映）

- **offset 對映**：見 §3.5。只用來換算 tracking 的目標時間，不當作跨機 timeline。
- **MTP**：`MTP_content = 顯示該幀的 predictedDisplayTime − sampleTime(echoSampleId)`，全部在 client XrTime 內計算。
  - thread 模式用 `convert_timespec_time`；
  - frameloop 模式的時間戳，全部以 predictedDisplayTime 的差分計算。
- **分段**：
  - server 段由 STATS 下發；
  - client 段用 client 時鐘；
  - 網路段用總數減掉其餘各段推估。
- **L̂**：MTP 的 EWMA（α=0.05）。HMD 和控制器分別設上限。
- 每 10 秒輸出 `[VIPLE-VR-MTP10]`。

### 4.7 相容矩陣

| client ↔ server | 結果 | 保證機制 |
|---|---|---|
| 2.0 Frame ↔ vanilla Sunshine 或 1.5.x VipleStream | 沒有 `<VipleStreamVR>` → 只有平面或 XR 桌面模式 | client 閘門 |
| vanilla Moonlight（含 Flathub 版）或 Android ↔ 2.0 server | 一般 session；標準 `/applist` **不含** `vr` 類 app；帶 `IsVr` 的一般 app 照舊以平面啟動；直接 launch `vr` 類 app 會收到明確錯誤 | `/applist?vr=1`、launch 檢查 |
| 1.5.x client ↔ 2.0 server | 同上 | 閘門不看 `isVipleStreamPeer` |
| Android 2.0（common-c 已對齊）↔ 2.0 server | 一般 session，`vrFlags=0` | — |
| 任何 client ↔ 2.0 **Linux** server | b1=0，不會啟用 VR | stub |
| 2.0 ↔ 2.0，主機沒有 SteamVR 或 `vr_pcvr=disabled` | b1=0 | serverinfo |
| VR session 進行中，其他裝置要 launch 或 resume | 503 busy | VR 獨佔 |
| 2.0 ↔ 2.0，PCVR，直連 | 啟用（rc：RTP） | 兩段協商 |
| 2.0 ↔ 2.0，PCVR，**經 relay** | rc：`VR_TRANSPORT_UNSUPPORTED`；GA：啟用（QUIC VR 政策） | `vrlaunchparams` 共用 |
| Windows 2.0 client ↔ 2.0，啟動 `IsVr` app | 警告後可平面啟動 | §2.8 |

### 4.8 三份 common-c 要改的檔案與同步檢查

| 檔案 | Q | A | S3 |
|---|---|---|---|
| （新增）`VipleVr.h`：ptype、**channel**、旗標、tracking struct、0x81 layout、TLV subtype、LOSS 和 REFRESH 結構；**單一定義來源** | ✔ | ✔ | ✔ byte-identical，由 `SS/stream.cpp` include |
| `Limelight.h`：`vrFlags`、`vrMeta`、`vrMessage`、`LiSendVrTracking`、`LiSendVrMessage`（尾端） | ✔ | ✔ | ✘ |
| `Limelight-internal.h`：**`#include "VipleVr.h"`**（不另外定義 `CTRL_CHANNEL_VR`）、`extern int VrFlags` | ✔ | ✔ | ✘ |
| `ControlStream.c`：送出 API、S→C 偽 typeIndex 與 async 修改（§4.5）、runtime 長度檢查（F2）、throttle helper（首次連線與重連）、`connectionDetectedFrameLoss` 的 VR 分支（LOSS） | ✔ | ✔ | — |
| **`VideoDepacketizer.c`**：0x81 VR 欄位；`strictIdrFrameWait` 和 `dropFrameState` 的 VR recovery 分支 | ✔ | ✔ | — |
| `Connection.c`（VrFlags）、`FakeCallbacks.c` | ✔ | ✔ | — |
| `QuicTransport.c`（GA：VR jitter 政策與 buffer 容量） | ✔ | ✔ | — |
| `SdpGenerator.c`、`RtspConnection.c`、`Input.h`、`InputStream.c` | 不改 | 不改 | 不改 |

- **SdpGenerator.c 不改**：server 在 vrProfile 下強制開 IR，不依賴 SDP 的 `intraRefresh`（附錄 A 第 1 項）。
- **同步檢查**：（新增，進 git）`moonlight-qt/moonlight-common-c/check_commonc_sync.ps1`。
  - ① Q 對 A 的 **`src/` 加上 `enet/`** 全部 byte-identical；白名單只有 `PlatformNetIf.h`，以及 F1 評估後列入的 `enet/` 平台檔（寫明理由）；
  - ② Q 對 S3 的指定檔案必須一致；
  - ③ 其他漂移檔只列為警告。
  - 本機 `build_*.cmd` 各加一行呼叫，漂移時 `exit /b 1`。

### 4.9 M1a 實作定案（2026-09-26）

設計稿沒有寫死、實作時才定下來的細節。**線上格式的權威來源是 `Q/VipleVr.h`**（三份 byte-identical）。
本節只做說明；兩者不一致時以 `VipleVr.h` 為準，並回頭修這份文件。

**`<VipleStreamVRSession>` 的格式**：單一元素，內容是 `;` 分隔的 `key=value`，client 用既有的
`getXmlString` 讀出後自己切開（`MQ/streaming/vr/vrlaunchparams.cpp`）。不認得的 key 一律略過，
server 可以往後加欄位。`mode=stub` 表示 M1a 的 stub 回聲（見下），M1b 有 SteamVR driver 之後改為
`mode=pcvr`。

```
<VipleStreamVRSession>proto=1;packed=3456x1728;hz=90;codec=hevc;layout=sbs;overscan=0;recovery=intra;irFrames=8;transport=rtp;universeId=0;session=<guid>;mode=stub</VipleStreamVRSession>
```

**launch 錯誤**：沿用 Sunshine 慣例，放在 XML root 的 `status_code`（非 200）與 `status_message`，
HTTP 狀態本身仍是 200。`status_message` 以錯誤碼開頭，後接 `: 說明`。§4.2 的四個之外，另外定了三個：
- `VR_BAD_PARAMS`：參數解析失敗；
- `VR_DISABLED`：`vr_pcvr=disabled`，或平台不支援；
- `VR_NEEDS_VR_CLIENT`：一般 launch 指到 vr 類 app（M1b）。

server 的檢查順序：relay 路徑、停用、參數解析，這三項在任何副作用之前；之後是 VR 獨佔（在 §M.1
擁有權判斷之前，admin 或 takeover 也不能搶走進行中的 VR session），接著收掉同一 client 的殘留
session（§M01-D）、`session_count()==0`、codec 交集（放在 `probe_encoders` 之後）。VR session 進行中，
**任何**其他 client 的 launch 或 resume，不論有沒有帶 VR，都會拿到 503 `VR_BUSY`。

**`STREAM_CONFIGURATION.vrFlags`（`VIPLE_VR_SF_*`）**：b0 `ENABLED`、b1 `RECOVERY_INTRA`。client 只在
收到 `<VipleStreamVRSession>` 時才設（不變式 1）。common-c 在 `LiStartConnection` 把它存成全域
`VrFlags`，`LiStopConnection` 結束時清零。大端平台一律 0。

**TLV payload 大小**（`static_assert` 鎖定）：LOSS 9 B、LATCH 12 B、REFRESH_START 6 B、STATE 4 B、
HAPTIC 20 B、STATS 40 B、CLIENT_TIMING 36 B。

**LATCH 與頻率鎖（M4a R3）**：client 在 XR frame thread 記下每張新串流影像第一次被 latch 進 projection layer 的時刻，`slackUs`＝latch 時刻 − 影像就緒（render thread release）時刻，正值＝幀比 latch 早到；`frameId` 是 client 的影像發布序號（server 只記錄）；10 Hz 送最近一張。server 以 EMA（α 0.2）平滑 slack，目標＝顯示週期 / 4（夾 1～4 ms），PI 控制（Kp 0.05 ppm/µs、Ki 0.004、積分上限 150）算出週期修正 ppm（夾 ±200；正值＝週期變長），pcvr 模式以 `bridge::set_pacing_ppm` 寫 driver pacing（epoch 不變、不重新對齊）；ppm 變化 ≥ 2 或距上次 ≥ 1 s 才更新；超過 2 s 沒 LATCH 每秒減半回到 0。stub 模式只記錄。**`slackUs` 是取模一個週期的相位**（2026-10-05 Frame 實測確認）：只在新幀第一次被 latch 時量，晚到 δ 的幀會被下一個 latch 接走、量成「週期 − δ」，實際上不會出現負值；沒有新幀時 client 每 100 ms 重送同一個 `frameId` 的舊值。wire 格式不變，server 的 `vr_latch_mode = v2`（§VR-LATCH-V2）依此處理：誤差在上側折返（≥ 半週期就減一個週期）、丟掉重複 `frameId` 與 |slack| > 1.5 週期的樣本、輸出飽和時不積分、目標＝模數（host 虛擬 vsync 週期與顯示週期的較小值）× `vr_latch_target_pct`（預設 40%）、Kp 0.12、Ki 0.008；預設仍是 legacy。**MTP_content**＝顯示該幀的 predictedDisplayTime（換成 client steady 時鐘）− echoSampleId 的 sampleTime；沒有時間換算擴充的 runtime 以 ≈2 週期估 predictedDisplayTime。**STATS 的計數欄位一律是 session 累計值**：STATS 走 UNSEQUENCED，掉一筆也不會
漏算，client 以相鄰兩筆相減得到區間值。欄位只往後加，client 依 len 能讀多少算多少。

**STATE.state**：0 IDLE、1 ORCHESTRATING、2 HMD_ACTIVE、3 **STUB_ECHO**（M1a）、0xFF ERROR。

**控制器按鍵 bit**：buttons 與 touches 共用同一套，pressCtr 每鍵 2 bit、順序相同。b0 SYSTEM、b1 MENU、
b2 A/X、b3 B/Y、b4 THUMBSTICK、b5 TRIGGER、b6 GRIP、b7 TRACKPAD、b8 THUMBREST；b9–b15 保留。

**0x81 header 的 renderRot**：smallest-three 編碼，2 bit 最大分量索引加 3×15 bit，以 48 bit little-endian
存放，最大分量取正號。往返誤差實測最差 0.007°（20 萬組隨機四元數）。

**M1a 的 server stub 回聲**：
- 沒有 SteamVR driver，VR session 直接跑在一般的桌面擷取上；由 server config `vr_pcvr=stub` 開啟
  （預設 `disabled`），這時不呼叫 `configure_display`。
- server 在每幀 encode 前，取**最新收到的 tracking 樣本**，把 sampleId 和 HMD pose 填進 0x81 header，
  旗標設 `POSE_VALID|ECHO_MATCHED`。沒有樣本時設 `POSE_FALLBACK`、echoSampleId=0。
- client 據此量 echo 命中率，以及 echo age（取樣到解出該幀的時間）。

**VR 視訊的傳輸（rc 以前）**：兩端都強制走 RTP/UDP：server `stream.cpp` 的 `use_quic`，client
`VideoStream.c` 的 `useQuicVideo`，兩邊必須一致。client 的接收執行緒在 `useQuicVideo` 時只讀 QUIC ring。
QUIC 連線保留給控制與 tracking 當 fallback。這時 QUIC 上平常沒有流量，而 client 的路徑 stall 偵測
「3 秒沒收到封包」會誤判主路徑死掉，觸發 failover。所以 VR session 期間，client 每 500 ms 在 QUIC
stream #0 送一個 `'P'`，靠它的 ACK 讓路徑保持活躍；server 端只會刷新 pingTimeout，ABR tick 已節流。

**恢復協定**：
- **server**：接受 LOSS 的**當下**就回 `REFRESH_START`（2 份，第二份延到下一個 control tick，
  避免兩份被 ENet 打包進同一個 datagram）。startFrame 用 encoder「下一個會開始 wave 的幀號」：encode
  迴圈檢查完 `vr_refresh` 之後就設成 frame_nr+1，所以估計值幾乎總是精確；偶爾實際晚一幀也安全（見
  client 的 waveEnd 下界）。wave 的最後一幀標 `REFRESH_DONE`，wave 內的幀 frameType=4。encoder 做不到
  IR 時改送 IDR；IDR 若還在 VR cooldown（300 ms）內，就等 cooldown 一到補送（不放棄，client 已經收到
  REFRESH_START）。改送 IDR 時，打包端另外補一則 `reason=IDR` 的 REFRESH_START。
- **server 端 wave 狀態**：wave 還沒開始時收到的 LOSS 一律吸收。wave 進行中，`lastLost < waveStart`
  的吸收；更晚的會重啟 wave。完全相同的第二份 LOSS 只在 50 ms 內視為重複；**RESEND 一律不去重**
  （每次重送的內容都相同，去重會吞掉第 2、3 次重送）。吸收的 RESEND 會觸發重送 REFRESH_START。
- **client**（`Q/ControlStream.c`，只在 recovery=intra）：VideoRecv 與 decoder 執行緒只登記掉幀、
  不碰 `enetMutex`（§M01-C），由專用的 VrSend 執行緒（每 2 ms 一輪）送 LOSS：第一份立刻送，第二份延
  5 ms（UNSEQUENCED，分開兩個 datagram）。2×RTT+20 ms 內沒收到涵蓋 lastLost 的 REFRESH_START 就重送；
  重送 3 次或等了 500 ms 就退回 IDR。收到 REFRESH_START 之後，等帶 `REFRESH_DONE` 而且幀號 ≥
  `startFrame + frameCnt − 1` 的那一幀（下界用 waveEnd：被重啟的舊 wave 的最後一幀也帶 REFRESH_DONE）；
  1 秒沒等到，就視為沒回應，重新要一波。任何晚於 lastLost 的 IDR 都會結束 degraded。
- **depacketizer**：recovery=intra 時不等 RFI 幀；網路掉幀之後不丟棄下一個完整幀，交給 intra refresh 自癒。
  第一個 IDR 之前仍然嚴格等 IDR。
- Qt decoder 的解碼錯誤改報 LOSS（`LiReportVrLoss`），連續 3 次才要 IDR。

**幀與 metadata 配對**（`MQ/streaming/vr/vrframemeta.*`）：
- 設 `m_Pkt->pts = frameNumber`、`pkt_timebase = 1/1000000`。
- decoder 吞幀時，`m_FrameInfoQueue` 依 pts 跳過較早的項目並補計 `m_FramesOut`，維持既有的佇列不變式。
- VR session 關閉 FRUC。renderer 端以 `LiGetVrFlags()` 判斷，不改偏好設定。

**client 退回平面**：relay 路徑、偏好強制 AV1 這兩種在套用 VR 串流形狀之前就判斷（事後才退回，平面
串流會留著 2W×H@Hz 的 VR 形狀）。

**M1a 的 client CLI**（值不寫入 QSettings）：
- `--display-target window|pcvr`、`--vr-emulate`、`--vr-synthetic-motion still|sine|yaw30`（dev 另有 `tilt30yaw`、`fast`）、
  `--vr-eye WxH`、`--vr-hz N`；
- dev 用：`--vr-inject-drop N`（每 N 個解出的幀丟一幀）、`--vr-inject-loss N`（每 N 秒注入一次 LOSS）；
  `--vr-synthetic-hmd`（2026-10-07）：真的 XR 連線（頭盔）上，頭的姿態改送 `--vr-synthetic-motion` 的合成運動
  （另有 `fast`：左右 ±30° @1.2 Hz、上下 ±15° @1.7 Hz，接近實際遊玩的量級），取樣時機、`predictNs`、控制器仍來自
  XR runtime。無人配戴的場次用來跑「頭在動」才走得到的路徑；畫面會在頭盔裡甩動，不可以戴著用。

---


### 4.10 §VR-MULTILINK：多連線（2026-10-06，預設關閉）

client 的每張網卡各開一條連線，影像、音訊、追蹤在每條連線各送一份，接收端去重。背景與實測見
`docs/steam_frame_client.md` §8.7。單一定義來源是 `VipleVr.h` 的「§VR-MULTILINK」段落。

**協商**（任何一邊不支援，行為與單一路徑完全相同）

| 位置 | 內容 |
|---|---|
| `/serverinfo` `<VipleStreamVR>` | bit `0x40` `VIPLE_VR_SERVER_CAP_MULTILINK`：`vr_multilink` 不是 `disabled` |
| `/serverinfo` `<NetworkInterface>` | `mpquic_enabled` 或 `vr_multilink` 開啟時列出 server 的每個介面位址（client 用來替每張網卡找同子網路的 server 位址） |
| `/launch` `vrCaps` | bit `0x10` `VIPLE_VR_CLIENT_CAP_MULTILINK` |
| `<VipleStreamVRSession>` | 多一項 `multilink=1`（雙方都支援才有）；client 據此設 `VIPLE_VR_SF_MULTILINK` |

**連線清單**（走既有的加密控制通道，reliable）

| 方向 | TLV | 本體 |
|---|---|---|
| C→S | `0x5507/0A LINK_HELLO` | `u8 count`＋`VIPLE_VR_LINK_DESC[count]`（14 B：linkId、flags、client 影像埠、client 音訊埠、client IPv4、server IPv4） |
| S→C | `0x5508/07 LINK_READY` | `u8 count`＋`VIPLE_VR_LINK_PORTS[count]`（6 B：linkId、status、server 影像埠、server 音訊埠）＋選用的 `u8 hubCaps` |

- 只支援 IPv4；最多 4 條（`VIPLE_VR_LINK_MAX`）；linkId 從 1 起。
- server 把每條連線的 socket 綁在 client 配對到的 server 位址（不是本機位址就拒絕），埠由系統配。
- client 重送 `LINK_HELLO`（2 s 沒收到 `LINK_READY`，最多 5 次）時，描述相同的連線回原來的埠，不重建。
- **連線層功能的協商**（2026-10-07）：client 在每個 `VIPLE_VR_LINK_DESC.flags` 填它支援的 `VIPLE_VR_LINK_F_*`
  （`0x01 CTRL`、`0x02 REPAIR`；舊 server 不讀這一欄），server 把同意啟用的放在 `LINK_READY` 陣列之後的 `hubCaps`
  （沒有任何功能時不附這個 byte；舊 client 本來就只讀陣列）。`REPAIR` 要同時有 `CTRL`。兩者的內容見本節最後兩段。

**連線上的資料報**

| socket | 方向 | 內容 |
|---|---|---|
| 影像 | S→C | 影像 RTP 封包，位元組與單一路徑時完全相同（含加密前綴）；長度 < 64 B 的是連線控制（PONG、T_S2C） |
| 影像 | C→S | PING（每 250 ms）、DATA（加密的 0x5506；協商了 CTRL 之後也載 0x5507） |
| 音訊 | S→C | 音訊 RTP 封包（資料與 FEC），位元組與單一路徑時完全相同 |
| 音訊 | C→S | PING（只有標頭；維持中間設備的狀態用，server 不看內容） |

連線控制資料報的標頭是 `VIPLE_VR_LINK_HDR`（12 B）：`magic 'V','L'`、`type`、`linkId`、`u32 seq`、`u32 arg`。

| type | 方向 | 說明 |
|---|---|---|
| 1 PING | C→S | `seq`＝ping 序號、`arg`＝client 微秒時戳的低 32 bit；後接 `VIPLE_VR_LINK_PING`（16 B：flags、burstSeq、最近量到的 RTT、這條連線收到／被採用的影像封包累計數、上一個 PING 以來的最大到達間隔、burstMbps） |
| 2 PONG | S→C | 原樣帶回 `seq`、`arg`（client 用來算 RTT） |
| 3 DATA | C→S | `seq`＝每個 session 遞增的訊息序號（同一則訊息在每條連線的 `seq` 相同）；後接 `tag[16]`＋密文，明文＝`u16 ptype`（LE）＋payload。`0x5506`；協商了 CTRL 之後還有 `0x5507`（一則一個 TLV：LOSS、LATCH、NACK） |
| 4 T_S2C | S→C | `seq`＝server 每個 session 遞增的訊息序號（每條連線各送一份、`seq` 相同）；後接 `tag[16]`＋密文，明文＝`u16 0x5508`＋TLV。整個資料報必須 < 64 B（`VIPLE_VR_LINK_CTRL_MAX_LEN`，client 靠長度和影像封包區分）。只在協商了 CTRL 之後出現 |

- **存活**：client 最近 1 s 內在這條連線收過 PONG，才在 PING 帶 `CONFIRMED`；server 最近 2 s 內收過帶 `CONFIRMED` 的
  PING，才把這條連線當成可用（兩個方向都通）。只要有一條可用，server 就不再走原本的單一路徑送影像與音訊；
  全部不可用時自動退回。client 一律同時收原本的 socket 與所有連線的 socket。
- **來源檢查**：server 只接受來源位址與埠等於 `LINK_HELLO` 宣告值的資料報；影像與音訊只送往宣告的端點。
- **DATA 的加密**：AES-GCM，金鑰與控制通道相同；IV＝`seq`（LE32）＋6 個 0＋`'C'`＋`'L'`（控制通道是 `'C','C'`／`'H','C'`、
  影像是 `'V'`，不會重複）。server 以 64 格滑動視窗對 `seq` 去重並防重放：先查視窗、解密成功才登記。
- **追蹤的三個來源**：連線（每條一份）與 ENet。client 在有已確認的連線時只走連線；沒有時走 ENet。server 端
  `on_tracking()` 另外以 `sampleId` 去重，哪一份先到就用哪一份。
- **到達速率量測（`vr_multilink = auto` 用）**：client 把「同一幀在這條連線到達的影像封包」（至少 32 個）當成一次量測，
  速率＝位元組數÷從第一個到最後一個的時間（Linux 用核心的接收時戳），放在 PING 的 `burstMbps`（每量到一次 `burstSeq` 加一）。
  整幀在 300 µs 內到（一個聚合框就送完）時回報 **65535**＝快到量不出來，server 當成合格。
  server 對探測中的連線每秒只送一次探測（6 ms 內的所有批次，約一整幀）；`burstSeq` 有變才算新的量測，連續 2 次
  ≥ max(50, 影像位元率 × 1.5) Mbps 才開始在這條連線送影像。送不動（飽和偵測）的連線退回探測。
- 沒有協商 CTRL 時，LOSS／LATCH／CLIENT_TIMING 與 S→C 的 VR 訊息只走 ENet（session 本身那個位址）。

**§VR-LINK-CTRL：時間敏感的控制訊息走連線**（2026-10-07，server `vr_multilink_ctrl`，預設關閉）

ENet 只走 session 開啟時的那個位址。那條鏈路變差時（實測：使用者走到適配器收訊差的位置），影像已經改由另一條連線
送，LOSS 與 REFRESH_START 卻還困在差的那條上——一筆 LOSS 等了 832 ms 才退回 IDR，另一筆根本沒送到。

- C→S：`LOSS`（0x5507/09）與 `LATCH`（0x5507/08）改放進 DATA，在每條已確認的連線各送一份；連線送得出去時不再走
  ENet。server 以 DATA 的 64 格視窗去重後，交給和 ENet 相同的處理函式。
- S→C：server 一接受 LOSS 就經 T_S2C 回 `REFRESH_START`（0x5508/06），被進行中或已排定的 wave 吸收的 LOSS 也回
  （內容是那一波的描述；已排定、第一幀還沒編時回排定當下記下的估計值，一定不晚於實際起點；排定期間又回報了
  不在估計值之前的掉幀時往後推到 `lastLost＋1`）。
  ENet 那一份照送，client 以 wave 的起點去重。
- T_S2C 的加密：AES-GCM，金鑰與 DATA 相同；IV＝`seq`（LE32）＋6 個 0＋`'H'`＋`'L'`。client 以 64 格視窗對 `seq`
  去重並防重放（先查視窗、解密成功才登記）。
- client 重送 LOSS 的門檻（2×RTT＋20 ms）改用連線量到的最短 RTT。
- 相容：舊 client 的 `flags` 是 0 → `hubCaps` 沒有 CTRL → 兩邊都照舊走 ENet。

**§VR-LINK-REPAIR：補包**（2026-10-07，server `vr_multilink_repair`，要同時開 `vr_multilink_ctrl`，預設關閉）

FEC 補不回來的 block（掉的比 parity 多）原本只能整幀放棄、報 LOSS、等一波 intra refresh。server 留著原封包、
也看得到每條鏈路的狀況，所以改成：頭盔只描述現況，補哪些、走哪條、補不補由 server 決定。

| 方向 | TLV | 本體 |
|---|---|---|
| C→S（DATA） | `0x5507/0B NACK` | `VIPLE_VR_TLV_NACK`（9 B：`u32 frame`、`u8 block`、`u8 flags`（保留）、`u8 need`、`u8 total`、`u8 attempt`）＋`have[(total+7)/8]`（bit i＝第 i 個 shard 已收到）。`total＝0`＝這個 block 一個封包都沒收到，不帶位元圖 |
| S→C（T_S2C） | `0x5508/08 REPAIR_GONE` | `VIPLE_VR_TLV_REPAIR_GONE`（6 B：`u32 frame`、`u8 block`、`u8 reason`）：補不了，不必再等。reason 1＝太舊、2＝額度用完、3＝沒有這個 block 的紀錄 |

- **server**（`vr_multilink.cpp` 的 `on_nack`、純邏輯在 `vr_multilink_logic.h`）
  - 封包倉：最近 24 個 FEC block **已加密**的封包（只存 shard 總數 ≤ 255 的 block）。補包是原樣重送——同一個 IV、
    同一份密文，client 的接收路徑不必知道它是補的。絕不能改任何位元組後沿用原 IV 重新加密。
  - 補多少（`plan_repair`）：「還差幾個才夠還原」＋ max(2, 八分之一) 的餘裕，照 shard 序號挑（data 在前）。
    client 一湊滿 data 個 shard 就做 Reed-Solomon 還原並往下走，多送的都是浪費。只挑倉裡有的（還沒送出的不算缺）。
  - 上一輪補的還在路上（距上一輪不到「送影像的連線裡最短的 RTT＋3 ms」，夾在 4～12 ms）：那些當成 client 會收到。
  - 上限：同一個 block 最多 5 輪；block 第一批送出超過三個幀週期 → `GONE(EXPIRED)`（鏈路斷一小段、連續兩幀
    整個沒到時，client 要等第三幀的封包到了才知道缺，第一幀這時已經兩個週期多一點；再長的空檔交給 refresh）；
    補包額度（每秒＝影像封包率的 15%，最多累積 600 個——兩個最大的 block 各補一次）用完 → `GONE(BUDGET)`。補滿輪數還缺時不回 GONE
    （前幾輪可能還在路上，client 自己有上限）。
  - 走哪條：和影像相同的連線（`primary` 模式下待命的連線不送）；都不在送時（保底中）每條可用的都送。每條連線一個
    補包佇列，tx 執行緒優先處理、送影像批次的途中也插隊；在佇列裡等超過 8 ms 的整批不送。
- **client**（common-c `RtpVideoQueue.c` 的 `nk*`）
  - 回報的對象永遠是佇列正在等的那個（幀, block）；每一份都是完整的狀態，掉了或重複都無妨。
  - 往返時間太長就不回報（`VideoStream.c`）：連線量到的最短 RTT＋2 ms 超過一個半幀週期（90 Hz 時約 RTT > 14 ms）
    時補包來不及，回報只會在已經很擠的鏈路上加碼。（缺一段的幀在尾端到的時候就回報，離期限還有將近兩個週期，
    所以門檻比等待上限寬；頭盔實測適配器那條的 RTT 中位數 3 ms、最大 13 ms。）
  - 第一份回報的時機——只有一條連線在送影像：這個 block 的最後一個 shard 到了還不夠→立刻；3 ms 沒有新封包；
    下一幀的封包到了→立刻。
  - 兩條以上在送：看每條連線各自送到哪裡了。每條都「走過」這個 block（送到它的最後一個 shard 或更後面的封包）
    還是不夠→立刻；只有一部分走過→從第一條走過起算，等「還沒走過的那幾條平常比已經走過的晚多久，再多八分之一
    ＋1 ms」（至少 2 ms、最多一個幀週期；還沒走過的若是平常比較快的那條，只等下限），而且還沒走過的連線正在送
    這個 block（2 ms 內還有它的封包）時不算逾時；都還沒走過→「3 ms＋其餘連線裡最慢那條的時間差」沒有新封包
    （送來最後一個封包的那條不算）。
    每條連線平常晚多久由連線層量：每一幀比各條連線第一個封包的到達時刻，往最近 4 個樣本的第二大靠（比它小就立刻
    升、比它大每次降十六分之一）；只有一條在送的幀不取樣，探測中的連線不當比較基準。印在 10 s 行的 `lagMs`。
    太早回報的代價是 server 白送一輪：S0 實測（一條落後 8 ms、另一條每 100 ms 斷 25 ms）舊規則每 10 秒回報數十次，
    全是多餘的；新規則 30 秒 0 次。
  - 「一段時間沒有新封包」只在接收真的閒下來（等封包逾時）時判定：手上還有封包在處理時，同一批裡排在後面的
    可能就是這個 block 的。
  - 之後每隔 clamp(2×RTT＋3 ms, 4, 10 ms) 還沒補齊就再報一次，最多 3 次。鏈路整個斷一小段時，空檔裡送的那幾次
    不算——恢復後退還（每個 block 最多 2 次）、馬上再報。判斷「落空」：回報送出後一路沒有任何封包、而第一個到的是
    後面的封包（不是補包）；或隔了一個幀週期以上才又有封包。（只看後者不夠：真實鏈路上量到的靜默比空檔本身短。）
  - 等待（§VR-MULTILINK-HOLD）：可以補包時，只有一條在送也會等（等的是補包）。等待期間前面的 block 收齊過
    （有進展）就從那一刻起再給一個幀週期，從後面的封包到達起算不超過兩個幀週期——連續兩幀都沒到時第二幀才趕得上。
    有後面的封包在排隊時，第一份回報最晚在「期限減一個重報間隔」送出（整個 block 都沒到時靠這一條；代價是一幀
    有兩個以上的 block、或落後量接近幀週期時，可能在落後那條送到之前多補一輪——寧可多補，不賭落後那條）。
    收到 `REPAIR_GONE`、或該報的都報了也等過一輪，就不必等到上限（一個幀週期）；兩條以上在送時，要每條都走過這個 block 才適用這條（否則還在等另一條
    的那一份），而且之前補過一輪時 `REPAIR_GONE` 不立刻放行——它是從快的那條先到的，慢的那條上前一輪的補包可能
    還在路上，等到「上一份回報＋最慢那條的時間差」。`REPAIR_GONE` 要等和它同一批收進來的影像封包都處理完才生效；
    收到它（或等待到期）要返回之前，接收端先把各 socket 上已經到的資料報收完。
  - shard 總數 > 255 的 block（不做 FEC 的超大幀）不回報。
- **ABR**（`stream.cpp`）：補包救回來的缺包（「補之前還差幾個」那部分）不算丟包、不降碼；佔影像封包 3% 以上的視窗
  不回升（`kVrAbrRepairHoldPct`）。補完還剩的洞由 client 的 FEC status 照常回報；補不回來的走 LOSS 與原本的降碼。
  會走到補包的是成串掉包（Wi-Fi 的短暫空檔），和位元率高低無關——S0 實測照丟包算會把位元率砍到下限。
  收到 NACK 會延後 FEC 百分比的回降（和 LOSS 相同的 30 秒）。
- LATCH 的相位量測不排除補過包的幀：`frameId` 是 client 的發布序號、不是線上的幀號，server 認不出來；每秒只取
  10 個樣本，補過包的幀佔極少數。
- 相容：任何一邊沒有 REPAIR → 不存封包、不回報，行為與只有多連線時相同。
