# VipleStream 3.0 VR 協定擴充

> 狀態：**設計定稿（2026-09-23）**，架構背景見 [`vr_architecture.md`](vr_architecture.md)。
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
| 3.0 Frame ↔ vanilla Sunshine 或 1.5.x VipleStream | 沒有 `<VipleStreamVR>` → 只有平面或 XR 桌面模式 | client 閘門 |
| vanilla Moonlight（含 Flathub 版）或 Android ↔ 3.0 server | 一般 session；標準 `/applist` **不含** `vr` 類 app；帶 `IsVr` 的一般 app 照舊以平面啟動；直接 launch `vr` 類 app 會收到明確錯誤 | `/applist?vr=1`、launch 檢查 |
| 1.5.x client ↔ 3.0 server | 同上 | 閘門不看 `isVipleStreamPeer` |
| Android 3.0（common-c 已對齊）↔ 3.0 server | 一般 session，`vrFlags=0` | — |
| 任何 client ↔ 3.0 **Linux** server | b1=0，不會啟用 VR | stub |
| 3.0 ↔ 3.0，主機沒有 SteamVR 或 `vr_pcvr=disabled` | b1=0 | serverinfo |
| VR session 進行中，其他裝置要 launch 或 resume | 503 busy | VR 獨佔 |
| 3.0 ↔ 3.0，PCVR，直連 | 啟用（rc：RTP） | 兩段協商 |
| 3.0 ↔ 3.0，PCVR，**經 relay** | rc：`VR_TRANSPORT_UNSUPPORTED`；GA：啟用（QUIC VR 政策） | `vrlaunchparams` 共用 |
| Windows 3.0 client ↔ 3.0，啟動 `IsVr` app | 警告後可平面啟動 | §2.8 |

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

---

