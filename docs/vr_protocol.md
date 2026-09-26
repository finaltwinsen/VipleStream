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
         i16 sx, i16 sy, u8 battery, u8 flags(b0 active,b1 focused), u16 rsv                      = 48
gaze：f16 yaw, f16 pitch, u8 conf, u8 flags, u16 rsv                                            = 8
```

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
| | 06 CLIENT_TIMING | 1 Hz 統計 | ch 0x07 unseq |
| | 07 FRAME_FEEDBACK（GA） | 每 8 幀一批 | ch 0x07 unseq |
| | **08 LATCH** | 10 Hz：`{frameId u32, slackUs i32, displayPeriodNs u32}` | ch 0x07 unseq |
| | **09 LOSS** | `{firstLost u32, lastLost u32, reason u8}`，送 2 次 | ch 0x07 unseq |
| | 7F | 保留：麥克風 | — |
| 0x5508 S→C | 01 HAPTIC | `{device, durationUs, freqHz, amp, eventId}` | ch 0x07 unseq |
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
HAPTIC 20 B、STATS 40 B。**STATS 的計數欄位一律是 session 累計值**：STATS 走 UNSEQUENCED，掉一筆也不會
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
- `--display-target window|pcvr`、`--vr-emulate`、`--vr-synthetic-motion still|sine|yaw30`、
  `--vr-eye WxH`、`--vr-hz N`；
- dev 用：`--vr-inject-drop N`（每 N 個解出的幀丟一幀）、`--vr-inject-loss N`（每 N 秒注入一次 LOSS）。

---

