# VipleStream 2.0 架構：Steam Frame 桌面模式與 PCVR 串流

> 狀態：**設計定稿（2026-09-23）**，實作從 M0 開始。協定細節見 [`vr_protocol.md`](vr_protocol.md)。
> 這份設計依 9 份平台／程式碼盤點，以及 3 位審查者（code-facts、vr-pipeline、ops-compliance）共 73 項意見修訂完成，處理紀錄見附錄 A。文中行號是撰寫當下的位置，實作時以原始碼為準。
>
> 使用者決策：Steam Frame 實機待出貨（先做不依賴實機的工作）；自建完整 PCVR 串流；PCVR server 只做 Windows；頭顯只做 Steam Frame（Android 不做 OpenXR，但 common-c 要對齊）。
> 版號沿革：原規劃為 3.0，2026-09-23 使用者更正為 **2.0**（2.0.0）；本文件已全面改用 2.0。
> 主機代號：`<host>` 是測試 server，`<dev-client>` 是開發機。

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

## 一頁摘要

1. **同一個 Frame client**：moonlight-qt 原生 Linux arm64 版，首選 Flatpak 打包，Qt6 由 KDE runtime 提供。桌面模式和 PCVR 都用它。
2. **桌面模式分兩步。**
   - α：平面 app 由 `gamescope --backend openvr` 變成 SteamVR dashboard overlay。client 不寫 XR 程式碼，server 不用改。
   - β：OpenXR quad 或 cylinder 虛擬螢幕，控制器射線當滑鼠。
   - β 和 PCVR 的啟動形態（overlay 內子行程、同行程切換、Steam 直接啟動 XR）由 **PoC-2b** 決定。Session 內的 XR 程式碼三種形態共用。
   - α、β 對 vanilla Sunshine 都能用。
3. **PCVR：server 端流程。**
   - Windows server 自寫 SteamVR driver，DirectMode_009 的部分移植 ALVR（MIT）。
   - driver 把遊戲 render 結果合成成 SBS，經 NT shared texture 加 fence 交給 `display_vr_t`。
   - **跨行程一律在 CPU 端確認 fence 已完成，絕不在 GPU 上等對方行程的 fence。**
   - 之後沿用 NVENC、FEC、RTP 送出。rc 的 VR 視訊強制走 RTP/UDP，QUIC VR 政策到 GA 才開。
4. **PCVR：client 端呈現。**
   - Frame 用 OpenXR projection layer 顯示。projection pose 是遊戲實際的 render pose（`SubmitLayer.mHmdPose`），由 driver 換回 client 空間後回傳。頭部轉動交給機上 SteamVR 的 ATW。
   - 定義了 stale 政策：超過 100 ms 沒有新幀就淡出，超過 250 ms 切到本地 loading 環境。
5. **追蹤與時序。**
   - 追蹤上行：2×顯示 Hz 送 232 B 的 `0x5506`，走 ENet ch 0x07 UNSEQUENCED。
   - server 端做輕量時鐘對映，把樣本的目標時間換成 server QPC，由此設定 `poseTimeOffset`。
   - rc 就做**頻率鎖加 latch 相位回授**（0x5507/08，10 Hz）。GA 再加 VsyncEvent 和 late-latch。
6. **恢復策略。**
   - VR session 協商 `recovery=intra`。client depacketizer 不再死等 IDR，改送 `LOSS`。
   - server 收到後以 `forceIntraRefreshWithFrameCnt=N` 做一次隨選 intra-refresh。
   - IDR 只在 decoder reset 時送，VR 的 IDR cooldown 另設 300 ms；桌面維持 1500 ms。
7. **協定表面積**：
   - `/serverinfo` 多 2 個元素；`/launch` 多一組參數，回應多 1 個元素；`/applist?vr=1` 是 VipleStream 專用查詢。
   - 3 個 ptype（其中兩個是 TLV 容器）、1 條 ENet channel、0x81 header；不動 RTSP 和 SDP。
   - 雙方都明確同意才啟用。
8. **分期**：程式碼依序做 M0 → M1a → M2a → M1b → M3a → M4a → M5a。實機到手後，關卡依序以批次執行：G-α（M2b）、G-β（M3b）、G-rc（M4b）、GA（M5b）。
9. **版號與發佈**：
   - G-α 通過時切 2.0.0。從 2.0.0 起**六件 asset 為必備**。
   - 每次 push 和 GitHub release 都要使用者明確下令；release 流程在 `gh release` 前一律停下等使用者核准。
   - 測試產物只用 scp 在機器之間直傳，不上雲。
10. **實機到手前能做的**：
    - M0、M1a、M1b 全部，包含在 <host> 上做 G-DRV、PoC-5a、PoC-10；
    - M2a、M3a、M4a、M5a 的程式碼，在 Monado、<host>、x64 Flatpak 上跑通；
    - 約占八成程式碼。
11. **實機到手後**：
    - Day 0：唯讀探測加安裝一個 Flathub app（PoC-0）。Flathub 上游 Moonlight 當天就能當使用者的過渡方案。
    - Day 1：用 α 套件跑 probe。
    - Day 2：vrlink 共存與眼動 PoC。
12. **工期**：
    - 單人約 37–48 週：程式 33–42 週，實機關卡批次 4–6 週（可和程式工作部分重疊）。
    - 雙人時，關鍵路徑 M0 → M1a → M1b → M4a → M5a 約 24–31 週。

---

## 0. 原始碼查證與更正

### 0.1 草案原有更正（依審查修訂）

| # | 原說法（出處） | 查證結果 | 對設計的影響 |
|---|---|---|---|
| C1 | nvvideoparser 在 aarch64 上擋住建置的是 cpudetect，或者可以直接排除 nvvideoparser | `VulkanVideoDecoder.cpp:340-347` 分派到 `ParseByteStreamSVE/NEON`，但 `src/` 裡沒有實作；`cpudetect.cpp:95-111` 本來就有 aarch64 分支。`NextStartCode{AVX2,AVX512,SSSE3}.cpp` 本身已經用 `#if defined(__x86_64__)` 包起來（`NextStartCodeAVX2.cpp:1`），aarch64 真正會失敗的只有 `nvvideoparser.pro:73-74` 的 `-m*` 旗標，以及 SVE/NEON 未定義符號。app 仍然需要這個函式庫（`MQ/app.pro:457`、`:693`） | 非 x86 時加 `DEFINES+=DISABLE_VK_VIDEO_PARSER_SIMD_OPTIMIZATIONS`；`-m*` 旗標依 `QT_ARCH` 條件加入；**SOURCES 不動**；`:36` 的 `libs/windows/include/x64` 只給 win32 |
| C2 | 只要 gate `plvk.h` 加上 `CONFIG+=disable-ncnn` | 無條件引用 ncnn 的地方有：`plvk.h:20`、`plvk.cpp:7-13`、`vkfruc.cpp:57`、`vkfruc.cpp:8348`、**`rife_native_vk.cpp:28-31`**（在 libplacebo SOURCES 內，`app.pro:460`）。`app.pro:216` 只檢查 `/usr/local` | arm64 在 deps 裡自建 ncnn（Vulkan＋aarch64），原始碼不動；`app.pro:216` 改讀 `NCNN_PREFIX`；`stb_image_write.h` 由 ncnn 模組裝進 `<prefix>/include/ncnn/` |
| C3 | 「ABR 降碼一律 forceIDR」 | 只有 NVENC 會這樣（`SS/video.cpp:2272-2276` → `nvenc_base.cpp:706-707`），而且繞過 1500 ms cooldown | VR profile 傳 `force_idr=false`（F13） |
| C4 | FEC 單幀上限是主要風險 | `vbvBufferSize = bitrate/fps`、`lowDelayKeyFrameScale=1`，約 860 Mbps@90 才會碰到上限 | 風險降級。真正的問題是恢復策略（見 C16） |
| C5 | tracking「QUIC 健康時走 flow 0x04」 | `sendMessageEnet` 是 ENet 優先，只在三種情況改走 QUIC（`Q/ControlStream.c:852-880,894-912,988-997`），而且都在 `#ifdef VIPLE_MPQUIC` 裡 | Frame 建置和 S0 的 Windows 建置都必須定義 `VIPLE_MPQUIC`（F18） |
| C6 | `lastPayloadLen` 用 8 B 算是現存的 bug | 目前計算正確；client 只在非 H.264/HEVC 時讀這個欄位 | 改送 24 B 時，`SS/stream.cpp:2193`、`:2249` 要一起改 |
| C7 | S3 和 Q 相同的檔「只有 4 個」 | 18 檔相同、18 檔不同；`SS/stream.cpp:18` 會間接 include 已漂移的標頭 | VR 定義放在自給自足的 `VipleVr.h` |
| C8 | headless 主機 `display_names[-1]` 越界 | 不成立（`SS/video.cpp:1238-1243`）。真正的問題是 reinit 時會重新列舉 DDA output，以及探測需要 DDA 顯示器 | VR 模式跳過列舉；探測改法見 C9 |
| C9 | `video::config_t` 可以任意加欄位（草案只提到 `:2893-2894`） | aggregate 初始化點有 **4 處**：`:2893`、`:2894`、`:2989`（`config_h264_yuv444`）、`:2996`（**`const config_t generic_hdr_config`**）；另有衍生複本 `:3005 auto config = generic_hdr_config`。`:2999` 會 `reset_display` 建一個 DDA 顯示器，失敗就 `return false`（`:3000-3002`），整個 encoder 判定失敗 | 新欄位用 **default member initializer**（`int captureSource = 0; int vrProfile = 0;`，C++14 以後仍是 aggregate）；`captureSource` 當成 `validate_encoder`、`reset_display` 的額外參數往下傳；VR 探測模式略過 HDR 和 YUV444 段（§3.3） |
| C10 | client 和 server 的 IDX 值「兩邊不一致」 | **誤讀**：IDX 是各自陣列的索引，client 的 `packetTypesGen7Enc[13]` 和 server 的 `packetTypes[16]` 都是 0x5504（`Q/ControlStream.c:266-281`、`SS/stream.cpp:61,87`）。另外，**0x5502 已經是雙向共用**：upstream 拿它當 S→C 的 RGB LED，VipleStream 拿來當 C→S 的 FEC status（`SS/stream.cpp:64-68`） | 理由改成：IDX 表按世代分陣列，新增一個欄位要同時改 5 個 client 陣列和 1 個 server 陣列，成本高，所以改用常數。VR ptype 從 0x5506 起用常數；0x5502 列為已存在的雙向例外，不再仿效 |
| C11 | ENet throttle 要兩端各自設定 | `enet_peer_throttle_configure` 會送 `THROTTLE_CONFIGURE` 給對端 | client 呼叫就兩向生效；**重連時要重新呼叫**（見 C29 附帶的 F5） |
| C12 | SDDL 給使用者 `GRGW` 就夠了 | `WaitForSingleObject` 需要 SYNCHRONIZE | 使用者 ACE 改成 `GRGWGX` |
| C13 | G-rc 門檻「MTP p95 ≤ 50 ms」 | 和延遲預算矛盾 | 暫定 G-rc p50 ≤ 55、p95 ≤ 65；GA p50 ≤ 45、p95 ≤ 55。**M1b 結束時依實測重定**（§8.2） |
| C14 | 引用位置的小誤差 | `vkfruc.cpp:57`、`plvk.cpp:6348`；`main.cpp:1215-1227` 是依 Qt platformName 設 `SDL_VIDEODRIVER` | 無 |
| C15 | 補充事實 | ① appversion `7.1.431` 必須留在 [7.1.415, 7.1.446)。② §SELF-UPDATE 還沒 commit。③ Q 和 A 的 `src/` 有 7 檔不同，**`enet/` 也有 5 檔不同**（`host.c`、`protocol.c`、`enet.h`、`unix.c`、`win32.c`）。④ payload 上限 251 B。⑤ ch 0x07 可用 | ①列入不變式；②列為 F0；③列為 F1 |

### 0.2 本次審查新增的更正

| # | 事實（證據） | 影響 |
|---|---|---|
| C16 | V4L2 路徑 RFI=0（`ffmpeg.cpp:282,289`），所以 `strictIdrFrameWait=true`（`Q/VideoDepacketizer.c:95`）。任何掉幀都會設 `waitingForIdrFrame`（`:129-133`），之後的 P 幀全部丟掉並請求 IDR（`:855-859`、`:1120-1124`）。RFI 關閉時，`connectionDetectedFrameLoss` 也直接請求 IDR（`Q/ControlStream.c:476-478`）。server 的 intraRefresh 預設是 `"0"`，由 client SDP 決定（`SS/rtsp.cpp:1006,1045`），但三份 common-c 都沒有送這個屬性。另外有 1500 ms cooldown（`SS/video.cpp:2196`） | 草案「intra-refresh 優先」在 client 端走不通，改成 §3.4 的 VR 恢復協定 |
| C17 | 解碼幀和 DU 的對應是 FIFO：`m_Pkt->pts` 從來沒有設定（`ffmpeg.cpp:3470-3478`），`frame->pts` 取自依序 dequeue 的 DU（`:2869-2898`），receive 失敗時不 pop（`:2960` FIXME） | 只要 decoder 丟一幀，之後每一幀的 renderPose 都會錯一格。改成 pts 查表（§2.3） |
| C18 | `display_base_t::is_hdr()`、`get_hdr_metadata()` 直接使用 `output`（`display_base.cpp:762-790`），而 `SS/video.cpp:2030,2412,2476,2749` 都會呼叫它們。width/height/capture_format 由 `display_base_t::init` 在找到 output 之後才設定 | `display_vr_t` 必須覆寫這些函式並自訂 init（§3.3） |
| C19 | `WMUtils::isGpuSlow()` 在非 x86 上預設回 true（`MQ/wm.cpp:208-221`），會影響 7 個決策點：`ffmpeg.cpp:515,523,1944,1952,2104-2108,2119,596-611`、`session.cpp:1349-1351,1958`、`streamingpreferences.cpp:141` | F6 擴大成「Linux renderer 決策」 |
| C20 | v4l2m2m 帶 `AV_CODEC_CAP_HARDWARE`，一定會走 separate test decoder（`ffmpeg.cpp:1752-1764`）。params 整份複製（`:1789`，連 `xr` 一起），接著 testRenderFrame（`:1023`）；每次 RENDER_DEVICE_RESET 都會重來（`session.cpp:3324`） | XrRenderer 必須明確處理測試實例（§2.4） |
| C21 | `LiStopConnection` 要到 exec 結束後，在 `DeferredSessionCleanupTask` 才執行（`session.cpp:1711-1781`）；common-c 的 async callback thread（`Q/ControlStream.c:1094`）在這之前都會持續派送回呼 | 拆除順序和回呼派送方式要改（§2.4） |
| C22 | `/launch` 在 `startConnectionAsync` 裡送出。這個函式在非主執行緒的 `AsyncConnectionStartThread` 執行（`session.cpp:2120-2139`、`:2270`；relay 路徑 `:2177-2227`），早於 `exec()` 建立視窗（`:2872`）。decoder 只在 `SDL_WINDOWEVENT_SHOWN` 或 `SIZE_CHANGED` 時建立（`:3157-3161`） | XrContext 必須在 `/launch` 之前建立；XR 模式的 decoder 由程式主動觸發建立 |
| C23 | `D3DKMTSetProcessSchedulingPriorityClass` 作用在整個行程（`display_base.cpp:~678-694`） | GPU 優先權改由 vr_session 在行程層級管理 |
| C24 | `IVRDriverDirectModeComponent_009` 只有 SwapTextureSet 系列、`SubmitLayer`、`Present`、`PostPresent(const Throttling_t*)`、`GetFrameTiming`。`WaitForPresent`、`GetTimeSinceLastVsync` 屬於 `IVRVirtualDisplay`。`GetFrameTiming` 如果沒有實作，必須清掉 `m_nReprojectionFlags` | virtual_vsync 重寫（§3.1） |
| C25 | `C:\ProgramData` 的預設 ACL 允許 Users 建立子目錄，建立者會以 CREATOR OWNER 取得完全控制。self-update 腳本會跳過 `config`、`credentials`（`self_update.cpp:482`） | driver 部署改放 `<install>\config\steamvr\` 並做完整性檢查（§3.10） |
| C26 | `SUNSHINE_TARGET_FILES` 是全平台共用（`common.cmake:177-181,214,222,224`）；非 Windows 的 self-update 固定找 `-linux-x64.deb`（`self_update.cpp:67-73`） | VR 程式碼在 Linux 要提供 stub（§3.11） |
| C27 | 3456×1728@90：H.264 是 23,328 MB × 90 = 2.10 M MB/s，超過 level 5.2 的 2,073,600，需要 level 6.x；HEVC 是 537.5 M samples/s，超過 level 5.1 的 534.7 M，需要 5.2 | H.264 保底改成 72 Hz，或每眼 1440²（§3.4） |
| C28 | `build_*.cmd`、`build-tools/`、`CLAUDE.md` 都不進 git。`build_sunshine.cmd:18-19` 預設會 bump，自動化呼叫時必須明帶 `--no-bump`（CoworkMCP 已於 2026-09-23 停用，原本的任務圖問題隨之取消） | F10 |
| C29 | ENet 重連路徑（`Q/ControlStream.c:1732-1756`）只重設 `enet_peer_timeout`，throttle 設定會回到預設值 | F5 抽成 helper，首次連線和重連都呼叫 |
| C30 | Flathub manifest 裡的 0004/0005 是套用在 **moonlight-qt** 模組上的補丁，不是 FFmpeg 補丁。finish-args 中的 `--env=IGNORE_RFI_LATENCY_BUG=1`，我們 fork 裡完全沒有讀取（grep 0 筆） | §2.2 措辭修正；這個 env 不帶 |
| C31 | NVENC 的 `forceIntraRefreshWithFrameCnt` 存在於 H.264、HEVC、AV1 的 pic params（`nvEncodeAPI.h:2115,2160,2206`）。前提是 init 時開了 `enableIntraRefresh`；不能和 B 幀並用；優先權高於 slice mode | 隨選 intra-refresh 可行（§3.4） |

---

## 1. 架構總覽

### 1.1 桌面模式

**2.0-α：平面 app，不寫 XR 程式碼**

```
[Steam Frame / SteamOS aarch64]
 SteamVR（機上 OpenXR runtime 加 compositor）
  └ dashboard overlay ← gamescope --backend openvr（nested，預設 1280×720）
     └ VipleStream client（arm64 Flatpak，app-id 見 U6）
        ├ Qt main thread：QML 主機列表、配對、設定
        ├ Session::exec SDL 主迴圈：gamescope 模擬的滑鼠鍵盤、Steam Input 手把、（F14，僅 gamescope）去重後的 SDL_TEXTINPUT
        ├ common-c 執行緒：RTP recv、depacketizer、ENet control、input、audio、QUIC IO
        ├ decoder thread：cgutman FFmpeg hevc/h264_v4l2m2m → AVFrame(DRM_PRIME) → DrmRenderer backend
        └ Pacer render thread：PlVk frontend（pl_map_avframe_ex）→ Wayland（gamescope-0）
      ↕ 家用 Wi-Fi / WAN / relay / MP-QUIC（dongle 不放在關鍵路徑上）
[Windows host] viplestream-svc（LocalSystem）→ viplestream-server（SYSTEM token，主控台 session）
   nvhttp → display_device（primary 或 VDD）→ DDA/WGC → NVENC → FEC → RTP 或 QUIC
```

**2.0-β：OpenXR 虛擬螢幕**

呈現端改成下面這樣，其餘和 α 相同：

```
startConnectionAsync 一開始（非主執行緒、/launch 之前）：XrContext 啟動 → READY → xrBeginSession
  → XR frame thread 先畫本地 loading 環境
decoder thread → XrRenderer::renderFrame：只把 av_frame_ref 放進 mailbox（latest wins），不碰 GPU
XR frame thread（XrContext 擁有，唯一使用 pl_gpu 的執行緒）：
  xrWaitFrame → xrBeginFrame → latch（取 mailbox 最新幀）→ xrAcquire/WaitSwapchainImage
  → pl_map_avframe_ex(DRM_PRIME) → pl_render_image → pl_vulkan_wrap 包好的 swapchain image
  → pl_vulkan_release_ex → xrReleaseSwapchainImage → xrEndFrame（quad 或 cylinder，加上本地指標 quad）
  → 這一幀的 GPU 工作完成後，才 unref 對應的 AVFrame（§2.6-9）
XR input（同一條執行緒）：aim ray 和 quad 求交 → UV → SDL_USEREVENT → main thread → LiSendMousePositionEvent
SDL 視窗：有 Wayland 時照常建立；沒有時用 SDL offscreen driver（程式內設 OVERRIDE 優先權的 hint，不靠使用者的環境變數）
  decoder 由 XR READY 推送的 SDL_USEREVENT 觸發建立（不等 SHOWN）
  XR FOCUSED 期間忽略 gamescope 注入的 SDL 滑鼠事件
```

**β 和 PCVR 的啟動形態**（由 PoC-2b 決定，Session 內的 XR 程式碼共用）：

- (a) 平面 launcher 另外開一個 XR 子行程：`VipleStream stream <host> <app> --display-target xr-desktop|pcvr`。
- (b) 同一個行程直接切換。
- (c) 從 Steam 直接以 OpenXR app 身分啟動（非 overlay）。使用者在文件說明下自行建立 Steam 捷徑，參數走 CLI 或上次使用的設定。

### 1.2 PCVR 模式（2.0-rc）

```
[Frame client]
 startConnectionAsync：XrContext 啟動 → session running → xrLocateViews 取得 FOV、eyeToHead；
   量平均 predictedDisplayPeriod、列舉 refresh → 才送 /launch vr=1（形態 a/b/c 都一樣）
 VrTrackingSender（2×Hz）：
   thread 模式（需要 XR_KHR_convert_timespec_time）：xrLocateSpace(HMD/L/R, T = now + L̂)
   frameloop 模式（缺這個擴充時）：每次 xrWaitFrame 後以 predictedDisplayTime 推出兩個目標時間，第二個延後 period/2 送出
   → 0x5506（232 B）→ ENet ch 0x07 UNSEQUENCED（ENet 失效時沿用 QUIC flow 0x04 fallback）
   → sample ring：{sampleId → sampleTime, pose}，保留 1 秒
 common-c：VR recovery=intra → 掉幀時不等 IDR，改送 0x5507/09 LOSS
 decoder thread（V4L2）：m_Pkt->pts = frameNumber → receive 後用 frame->pts 查 VrFrameMetaMap；查不到就丟幀
 XR frame thread：最新幀 → libplacebo 一個 pass 寫進 2W×H swapchain
   → XrCompositionLayerProjection：views[i].pose = renderPose(0x81，client 空間) ∘ eyeToHead[epoch][i]；fov = 協商值 + overscan
   → 沒有新幀：重送上一幀並保留原 pose（stale 政策設上限）；poseValid=0 的幀不進 projection
 control：vrMessage(HAPTIC) → SDL_USEREVENT → main loop → XrContext haptic 佇列 → XR thread xrApplyHapticFeedback
      ↕ 網路（rc：VR 視訊只走 RTP/UDP）
[Windows host：兩個行程、兩種身分]
 viplestream-server.exe（SYSTEM，使用者 session）
   ENet / QUIC IO → vr::Bridge::onTracking（seqlock、時鐘對映：target_server = sampleTime + predictNs + offset）→ SetEvent(evtTrk)
   vr_bridge thread：hardened pipe 握手、haptic ring → 0x5508、log ring（過濾後）→ server log
   display_vr_t capture：WaitForSingleObject(evtFrm) → 驗證 descriptor → CPU 端確認 sharedFence ≥ n（≤ 2 ms，逾時丟幀）
       → CopyResource → Signal(capture_fence)、GPU Signal(consumedFence) → push(img + vrMeta)
   encode thread（NVENC VR profile；收到 LOSS 時 forceIntraRefreshWithFrameCnt=N）→ 0x81 24 B header
   vr_session orchestrator：以使用者 token 執行 vrpathreg、settings guard（D8b）、啟動 SteamVR、steam://launch/<id>/VR
 vrserver.exe（使用者，medium IL）載入 <install>\config\steamvr\<ver>\driver_viplestream.dll
   tracking：讀 shm → TrackedDevicePoseUpdated（poseTimeOffset = target_server − now；stale 時速度歸零）→ PoseHistory
   DirectMode_009：SubmitLayer(mHmdPose…) → Present → PostPresent（依虛擬 vsync 節流）；GetFrameTiming 清 m_nReprojectionFlags
     → FrameCompositor：以 scene layer 的 pose 為準合成 SBS（其他 layer 做旋轉補償、依格式處理色彩）
     → 只用 consumed 值已經到達的 slot；GPU Signal(sharedFence) → descriptor → SetEvent(evtFrm)
   controller：模擬 profile、raw_from_grip 校正、skeleton、haptic → shm
```

### 1.3 執行緒、行程、權限邊界

| 邊界 | 兩側 | 處理方式 |
|---|---|---|
| SYSTEM ↔ 使用者 medium IL | server ↔ vrserver | **pipe** 由 server 以 `FILE_FLAG_FIRST_PIPE_INSTANCE`、`PIPE_REJECT_REMOTE_CLIENTS` 建立，建立失敗就 fail closed。**shm 和 event** 用每個 session 隨機產生的 GUID 名稱，經 pipe 交給 driver；`CreateFileMapping`、`CreateEvent` 回 `ERROR_ALREADY_EXISTS` 就中止。SDDL 為 `D:P(A;;GA;;;SY)(A;;GRGWGX;;;<console user SID>)S:(ML;;NW;;;ME)`。**雙向驗證**：server 驗 client 的完整映像路徑必須在 `steam_scanner` 找到的 SteamVR 目錄之下，而且 token SID 等於主控台使用者；driver 用 `GetNamedPipeServerProcessId` 驗 server 是 SYSTEM，而且映像位在 install 目錄。貼圖和 fence 由 driver 建立，server 用 `DuplicateHandle` 取得 |
| driver 寫入的資料 | frame descriptor、貼圖表、log ring | 全部做執行期驗證：`texIdx<3`、尺寸等於協商值、fmt 在白名單內、fenceVal 單調遞增；log 行限長、過濾字元，一律加 `[VIPLE-VR-DRV]` 前綴（driver 無法偽造其他 tag） |
| 兩端各自的生命週期 | server 重啟、SteamVR 重啟、ABI 版本不一致 | generation 加一，兩端都重建 fence 和貼圖。driver 發現 pipe 斷線就讓 HMD 進入 standby、送 `poseIsValid=false`，每 500 ms 重連。ABI 不一致時回報 STATE `ABI_MISMATCH_RESTART_STEAMVR` |
| 跨行程同步 | fence | **絕不 GPU-Wait 對方行程的 fence**。一律先用 `GetCompletedValue()`，或 `SetEventOnCompletion` 加上 ≤ 2 ms 的 wait，確認完成後才在本端下 GPU 工作 |
| 網路執行緒 | ENet control、picoquic IO | VR handler 只做 lock-free 的 seqlock 寫入加 SetEvent |
| 單執行緒 task_pool | VR 輸入 | 不經過 task_pool |
| GPU | server、driver、vrcompositor | 同一張 adapter（握手時比對 LUID）。GPU 優先權是**行程層級**的：vr_session 在 session 開始、以及每次 probe 之後設為 HIGH，結束時還原，並記 `[VIPLE-VR-CAP] gpu-priority=` |
| client 沙箱 | Flatpak | PoC-F；不通就改 P2 |
| client main thread | `LiSendMousePositionEvent` 不可並行呼叫 | XR 指標和 haptic 都用 SDL_USEREVENT 送回 main thread |
| common-c 回呼 → XR | `vrMessage` 在 async thread 上執行 | 一律包成 SDL_USEREVENT 送回 main loop；迴圈結束後的事件自然丟棄（和既有 rumble 做法一樣） |
| XR 與 Vulkan queue | runtime 和 libplacebo 共用 queue | `pl_vulkan_import` 傳入 `lock_queue`/`unlock_queue`，和 `xrBeginFrame`、`xrEndFrame`、`xrAcquire/ReleaseSwapchainImage` 共用 XrContext 的 mutex |

### 1.4 不變式（違反任何一條都視為 regression）

1. **VR 啟用閘門**：client 在 `/launch` 明確送 `vr=1`，**而且** server 在回應裡回了 `<VipleStreamVRSession>`，兩者都成立才啟用。不看 `IS_SUNSHINE()`，也不看 `isVipleStreamPeer`。
2. host appversion 固定是 `7.1.431.-1`（0x81 = 24 B 的前提）。
3. 新 ptype 直接用常數，不進 packetTypes/IDX 表；一個 ptype 只用在一個方向。0x5502 是已存在的例外，不再仿效。
4. `video::config_t` 的新欄位加在尾端，並使用 default member initializer。`STREAM_CONFIGURATION`、`DECODE_UNIT`、`CONNECTION_LISTENER_CALLBACKS` 的新欄位只加在尾端。
5. **`vrFlags==0` 時，Windows D3D11 和所有桌面路徑的行為與效能都不變**，以 stream-quality baseline 驗證。
   - XR 在 `/launch` **之前**失敗：退回平面模式，記 log `[VIPLE-XR] fallback stage=prelaunch`。
   - VR session 建立**之後**失敗：送 `/cancel`，在平面 UI 顯示錯誤，**不做**平面串流 fallback。
   - 任何情況都不能 crash。
6. vrserver 載入的 driver DLL 只能放在 `<install>\config\steamvr\<ver>\`：這是 self-update 不會碰的子樹，而且部署前要通過 owner、DACL、reparse point、hash 檢查。install 目錄如果一般使用者可寫，VR 一律停用（fail closed）。
7. 所有設定走 Settings UI、CLI 或 Sunshine config，不用環境變數。既有的環境變數覆寫一律標成 dev-only，使用時在 log 警告。
8. 跨行程同步不在 GPU 上等對方的 fence（§1.3）。
9. **任何特權身分（SYSTEM，或提升的管理員）**絕不寫入、也不執行使用者可寫路徑裡的東西（例如 Steam 整棵樹是 `BUILTIN\Users:(F)`，裡面的 `vrpathreg.exe`、`vrserver.exe` 都不可由特權身分執行）。使用者設定檔（`steamvr.vrsettings`、`openvrpaths.vrpath`）一律**以主控台使用者身分**修改：使用者 token 經 `run_command` 啟動的行程，或行程內在單一執行緒以 `ImpersonateLoggedOnUser` 模擬、寫完 `RevertToSelf`（使用者 2026-09-28 同意改寫；M1b 設計 K21）。
10. 非 Windows server：VR 程式碼編成 stub，`/serverinfo` 的 b1 固定為 0，linux-server `.deb` 必須能建置，而且 KMS 擷取的行為不變。
11. 從 2.0.0 起，六件 asset 同一個版號。每一次 push 和 GitHub release 都要使用者明確下令。

---

## 2. Client（Steam Frame，Linux arm64）

### 2.1 aarch64 移植（M2a 的 F8，不需實機）

| 阻斷點 | 修法 | 檔案 |
|---|---|---|
| nvvideoparser SIMD | C1：非 x86 加 DISABLE define；SOURCES 不動；`libs/windows/include` 只給 win32。M2a 實作：GCC 不再加全域 `-m*`，改在三個 SIMD TU 檔頭（x86 條件內）用 `#pragma GCC target`；clang 維持全域旗標 | `moonlight-qt/3rdparty/nvvideoparser/nvvideoparser.pro`、`src/NextStartCode{SSSE3,AVX2,AVX512}.cpp` |
| ncnn 無條件引用 | C2（包含 `rife_native_vk.cpp`）；Frame 上 FRUC 預設關閉。M2a：`app.pro` 改讀 `NCNN_PREFIX`（預設 `/usr/local`），有 libplacebo 而找不到 ncnn 時 qmake `error()` | `MQ/app.pro:216`；ncnn 的 Flatpak 模組 |
| picoquic / picotls 路徑 | `app.pro:672-683` 的 `PICOQUIC_BUILD`（另加 `PICOTLS_LIBDIR`），以及 **`moonlight-common-c.pro:104-120` 的 `PICOQUIC_DIR`**，都改成 `isEmpty()` 時才用預設值；`libpicotls-fusion.a`（或 `.lib`）存在時才連結 fusion。client 不 include picotls 標頭，不需要 picotls include 變數 | `MQ/app.pro`、`moonlight-qt/moonlight-common-c/moonlight-common-c.pro` |
| picoquic 是 submodule | **不另立 Flatpak 模組**：picoquic 隨 viplestream 模組的來源進沙箱——dev 是工作樹快照，release 是 git 來源（`finaltwinsen/picoquic`，pin 在 gitlink SHA，必須已在 GitHub）。picotls 是 viplestream 模組內的另一個 git 來源（pin 與 picoquic `CMakeLists.txt` 的預設 tag 比對）。`build-viplestream.sh` 在沙箱內離線建靜態庫，再把 `PICOQUIC_BUILD`／`PICOQUIC_DIR` 傳給 qmake | Flatpak viplestream 模組、`MQS/steamframe/flatpak/build-viplestream.sh` |
| MP-QUIC | Frame 建置一律 `DEFINES+=VIPLE_MPQUIC` | `build-steamframe.sh` |
| OpenXR | `CONFIG+=openxr` 區塊（`unix:!macx:openxr`，opt-in、不自動偵測，只有 `build-steamframe.sh` 帶）：pkg-config `openxr`、定義 `HAVE_OPENXR`、編 `xr-probe` 的 A 段 | `MQ/app.pro` |
| 建置腳本 | （M2a 已新增）`MQS/build-steamframe.sh --arch x86_64|aarch64 --flavor dev|release`，用法見 [`steam_frame_client.md`](steam_frame_client.md)。**x86_64 dev** 開 openxr、libdrm、wayland，產出 S1 和 PoC-F-pre 用的 Flatpak。x64 的 AppImage 腳本不動（`build-appimage-native.sh:64` 仍然關閉 wayland 和 libdrm）。本機 `build_moonlight.cmd` 加 `--openxr`（OpenXR loader 走 vcpkg），給 S2 用（延到 M3a）。**沒有腳本路徑的模擬環境，不能當成關卡證據** | `MQS/`、本機 `build_moonlight.cmd`、`docs/building.md` |
| desktop id | 見 §2.4 | `MQ/main.cpp`、`MQ/app.pro` |
| FFmpeg 沒開 V4L2 | cgutman `moonlight_9.0_r1`，pin 在 `d17de7e3`。旗標＝Flathub 清單去掉 `--enable-lto`（qemu 省時；解碼走硬體），含 `--enable-libdrm`、h264/hevc_v4l2m2m decoder、libdav1d；v4l2-request（h264/hevc/av1 hwaccel）一併開；另開 h264/hevc/av1 parser（decode-bench 用 `av_parser_parse2`，`--disable-all` 會把 parser 全關）。**v4l2m2m 不寫 `--enable-v4l2-m2m`**：它在 autodetect 清單內，加上 `--fatal-warnings` 後，明寫的 `*_v4l2m2m` decoder 依賴不滿足時 configure 直接失敗，建得出來就代表有編進去。**R11 真正要評估的是 FFmpeg 9.0 移除了哪些 API**，例如 `plvk.cpp:558-559` 的 `lock_queue` 欄位。0004/0005 是 moonlight-qt v6.1.0 的補丁，不適用我們的 fork | Flatpak FFmpeg 模組 |

### 2.2 依賴與打包（決策 D12，關卡 G-PKG、G-BUILD）

| 選項 | 內容 | 定位 |
|---|---|---|
| **P1 Flatpak aarch64** | 以 Flathub Moonlight 配方為範本。runtime 是 `org.kde.Platform 6.11`（退路 6.10：同為 freedesktop 25.08 基底、仍在維護）。模組（依序，最常改的放後面）：ncnn、SDL3、sdl2-compat、SDL2_ttf、dav1d、OpenXR-SDK loader、cgutman FFmpeg、libplacebo（pin `v7.360.1`＝選 B；和 FFmpeg 9 編不過就退回 A：Flathub 的 `4d82c689`＋補丁）、gamescope WSI layer（**兩種 arch 都裝**，aarch64 版 UNVERIFIED）、VipleStream（picoquic＋picotls 在模組內建置，見 §2.1）。**不建 libdecor**（25.08 runtime 內建）；`appstream-compose: false`（非 Flathub 發佈，上架時再開）。**finish-args 逐條列在 `MQS/steamframe/flatpak/finish-args.md`，每條註明取捨。** 已知的決定：`--device=all`、`--filesystem=xdg-run/gamescope-0`、`host-os:ro`、`xdg-config/openxr:ro`、SteamVR runtime 路徑唯讀；**libdrm 兩種 arch 都開**（x86_64 Flatpak 只給開發用，`build-steamframe.sh` 拒絕 x86_64 release；日後要給 NVIDIA 使用者再照 Flathub 加 `disable-libdrm`）；**`--env=IGNORE_RFI_LATENCY_BUG=1` 不帶**（C30）。每個模組的授權放進 `/app/share/licenses/`，manifest 保留 pin 住的來源，以符合 GPL/LGPL 的原始碼提供義務 | **α 確定採用**；β/rc 在 PoC-F 通過時沿用 |
| P2 sniper arm64 zip | `steamrt/sniper/sdk/arm64` 容器，依賴全部自帶 | M3a 期間只做 spike；G-PKG 判定需要時才全面投入 |
| P3 舊 glibc，全部自帶 | — | 最後的備援 |

- **為什麼 α 用 Flatpak**：上游 Flathub 在 aarch64 開了同一組建置旗標（v4l2m2m 加 libdrm），**實效待 PoC-0 驗證**；而且不必自建 Qt6。
- **β/rc 的風險**：SteamVR 的 OpenXR runtime `.so` 能不能在 KDE runtime 沙箱內載入。實機前先做 PoC-F-pre（S3）。
- **OpenXR runtime 探測**：自動探測多個候選路徑（host 的 `~/.config/openxr/1` 經 xdg-config 權限、SteamVR 安裝目錄的 runtime json）。沙箱把 `XDG_CONFIG_HOME` 改到 `~/.var/app/<id>/config`，loader 自己看不到 host 的 `~/.config/openxr`：由 `XrRuntimeJson::resolveActive()` 解析 host 路徑，只在 loader 找不到而 host 路徑找得到時，於行程內設 `XR_RUNTIME_JSON` 再 `xrCreateInstance`（M2a 的 `xr-probe` 已這樣做）。探測失敗時，Settings 的 XR 區段提供路徑欄位和「重新探測」按鈕；CLI `--xr-runtime-json` 只給開發用。
- **nested 解析度**：PoC-7 如果發現可以調整，就由 app 或 Flatpak 的啟動包裝依 Settings 值決定，**不得要求使用者改環境變數或啟動參數**。
- **log 路徑**：`~/.var/app/<app-id>/cache/VipleStream/VipleStream/logs/`。
- **G-BUILD（M2a 開頭；原列 M0）**：量測 linux-builder 上 qemu-user 的 aarch64 乾淨建置和增量建置時間。策略：`flatpak-builder --ccache`、持久化 state-dir（兩種 arch 共用），依賴模組靠 flatpak-builder 逐模組的快取留在 builder 本機。viplestream 模組的來源一變就整個重建，所以「增量」＝依賴全部 cache hit、app 模組在 ccache 已暖時完整重建一次；以 release flavor（開 LTO）判定。增量建置超過 45 分鐘，就停下交使用者決定 U7（原生 ARM builder）採購。量法與結果見 [`steam_frame_client.md`](steam_frame_client.md) §4。

### 2.3 解碼路徑、fallback 鏈、恢復與配對

| 層 | 路徑 | 引入時點 | 失敗時 |
|---|---|---|---|
| L2（主路徑） | cgutman `hevc/h264_v4l2m2m` → DRM_PRIME → `DrmRenderer` 當 backend → PlVk（α）或 XrRenderer（β 以後），兩者都用 `pl_map_avframe_ex` | α | 退 L3 |
| L3 | 同一顆 decoder 不走 DRM_PRIME（mmap 軟體幀）→ 上傳 | α | 退 L4 |
| L4 | 軟體解碼 | 永遠保留 | 只保證桌面 1080p60；PCVR 直接拒絕 |
| L1（視 PoC） | 自寫 `V4l2Decoder`：EXPBUF 後輸出 AVFrame(DRM_PRIME)，可以做 AV1、可以控制 buffer 數、`S_PARM` 拉高時脈、**錯誤處理可控**，pts 直接用 V4L2 timestamp | 觸發條件：需要 AV1、PoC-3/4 延遲超標，或 PoC-3b 顯示 iris 對破損參考幀的行為無法經 FFmpeg 處理 | 退 L2 |

- **L1 的掛點**：`Session::chooseDecoder`（`session.cpp:515-576`）。
- **DmabufVkImporter 不預先寫**，只在 PoC-4 失敗時才泛化 `vaapi_vk_bridge.cpp`。
- **延遲前提**：
  - VR profile 沒有 B-frame；
  - V4L2 capture buffer 數 ≥ `min_buffers` + mailbox（2）+ 還在 GPU 上的幀數（§2.6-9）；
  - RFI 能力維持 0。
- **VR 恢復（client 端，只在協商 `recovery=intra` 時生效）**：
  - common-c：`strictIdrFrameWait = !RFI && !(VrFlags & VR_RECOVERY_INTRA)`。第一個 IDR 處理之後，`dropFrameState` **不設** `waitingForIdrFrame`，後續幀照常送進 decoder；`connectionDetectedFrameLoss` 改送 0x5507/09 `LOSS{firstLost,lastLost}`（ch 0x07 unseq，連送 2 次）。起播時仍然等第一個 IDR。
  - `ffmpeg.cpp:2982`（receive 失敗就要 IDR）在 VR 模式改成回報 LOSS。只有連續 3 次 decode error、或 decoder reset 時才 `LiRequestIdrFrame`。
  - degraded 視窗：從掉幀開始，到收到 `REFRESH_START.startFrame > lastLost` 的那一波，並且看到帶 `vrFlags.b7 refreshDone` 的那一幀為止。超過 `2×RTT+20 ms` 還沒收到 REFRESH_START 就重送 LOSS；3 次或 500 ms 後請求 IDR。
  - degraded 幀的顯示政策 `vrDegradedPolicy`：`show`（預設，破圖局部而且會自癒）或 `hold`（最多 N 幀維持上一張乾淨幀加 ATW）。最終預設值由 G-rc 的 A/B 決定。
- **幀與 metadata 的配對（取代 FIFO，只在 VR session）**：
  - `submitDecodeUnit` 設 `m_Pkt->pts = du->frameNumber`；VR session 建 decoder 時設 `avctx->pkt_timebase = {1,1000000}`，v4l2m2m 經 `v4l2_buffer.timestamp` 帶回時就是恆等換算。
  - vrMeta 存進以 frameNumber 為 key 的小型 map（64 格）。`avcodec_receive_frame` 之後用 `frame->pts` 查表；查不到就丟掉這一幀、不顯示，記 `[VIPLE-VR-FRAME] meta-miss`。同時把 `m_FrameInfoQueue` 中 frameNumber ≤ 命中值的項目清掉，讓統計佇列維持有界。
  - `vrFlags==0` 時沿用原本的 FIFO（不變式 5）。桌面路徑是否也改成查表，列為 2.0 之後的獨立項目，要有 baseline。
- **Linux renderer 決策（F6 擴充）**：
  - `linuxVideoFrontend`（auto/vulkan/egl）**只決定 frontend**；`rendererSelection` **只決定 decoder cascade**。
  - auto 規則：aarch64，或 EGL vendor 是 Zink 時選 PlVk。同時決定 `isGpuSlow`：DRM driver 是 `msm`（Adreno/Turnip）就視為不慢。
  - C19 列的 7 個呼叫點，逐一確認在 Frame 上的結果：PlVk 優先、不先試 DrmRenderer frontend、不強制 `SDL_WINDOW_FULLSCREEN` 和 matchVideo、偏好設定不推薦 WM_FULLSCREEN。
  - `rendererSelection=RS_VULKAN` 在沒有 Vulkan Video 的裝置上，要快速略過 Vulkan hwaccel，記 log 後退回 cascade。
  - Frame 的預設組合 `RS_AUTO + frontend auto(PlVk)` 寫進 G-α。

### 2.4 XR 模組與生命週期（D4：Session 層 XrContext 加 XrRenderer frontend）

**`MQ/streaming/vr/`（新增，所有平台都編，不依賴 OpenXR）**

| 檔案（新增） | 職責 |
|---|---|
| `vrtracking.{h,cpp}` | `VrTrackingSender`：thread 和 frameloop 兩種模式，記 `[VIPLE-VR-POSE] mode=thread|frameloop`；sample ring；`ITrackingSource`；`LiSendVrTracking` 送不出去就丟 |
| `vrsynthetic.{h,cpp}` | 決定性 pose（`sine|still|yaw30`） |
| `vrframemeta.{h,cpp}` | `VrFrameMetaMap`（frameNumber → vrMeta）、MTP 統計、degraded 視窗狀態 |
| `vrprobedecode.{h,cpp}` | 解碼 `vr_probe` 在畫面角落的位元圖案並和 0x81 比對（只在 debug 設定開啟時） |
| `vrlaunchparams.{h,cpp}` | **VR launch 參數的組裝和回應解析**，由 `NvHTTP::startApp` 和 `Session::tryRelayLaunch` 共用 |

**`MQ/streaming/xr/`（新增，只在 `CONFIG+=openxr` 時編）**

| 檔案（新增） | 職責 |
|---|---|
| `xrcontext.{h,cpp}` | `xrCreateInstance`；擴充一律列舉後才啟用。`XR_KHR_vulkan_enable2` 必要（備援 `vulkan_enable`）。`KHR_convert_timespec_time` **是 PoC-2 必查項**，缺少時 tracking 改用 frameloop 模式。其餘選用：`composition_layer_cylinder`、`FB_display_refresh_rate`、`EXT_eye_gaze_interaction`、`VALVE_frame_controller_interaction`、`EXT_local_floor`。用 `xrCreateVulkan*` 建 Vulkan 和 `pl_gpu`（帶 queue lock）。另外負責：session 狀態機、XR frame thread、**本地 loading 環境**（地板格線加 head-locked 狀態 quad）、最後一幀的自有 `pl_tex` 複本（RGBA、SBS 尺寸，不 ref 舊 decoder 的 AVFrame）、haptic 佇列、`bringUp(timeout=5s)`：READY → xrBeginSession → 量 FOV、eyeToHead、period |
| `xrswapchain.{h,cpp}` | 格式優先 SRGB，其次 UNORM；`pl_vulkan_wrap` 的 color repr 和 swapchain 格式一致 |
| `xrdesktopscreen.{h,cpp}` | quad 或 cylinder 擺放、本地指標、重新置中 |
| `xrprojection.{h,cpp}` | projection 組裝、stale 政策、epoch 對應的 eyeToHead/FOV 表、space-mismatch 偵測 |
| `xrinput.{h,cpp}` | action set、射線、One-Euro 濾波、手把對映、FOCUSED 與 isActive 處理 |
| `xrtrackingsource.{h,cpp}` | `ITrackingSource` 實作 |

**XrRenderer 與共用碼**
- （新增）`MQ/streaming/video/ffmpeg-renderers/xrrenderer.{h,cpp}`：
  - `renderFrame` 只把幀放進 mailbox，`notifyWindowChanged` 回 true。
  - **TestFrameOnly 實例**：`DECODER_PARAMETERS` 尾端加 `testFrameOnly`，在 `ffmpeg.cpp:1789` 的複本上設定。測試實例不註冊 mailbox、不碰 swapchain。
  - **`testRenderFrame`** 要在 XrContext 的 `pl_gpu` 上實際跑一次 `pl_map_avframe_ex(DRM_PRIME)` 再 unmap；失敗就回 false，讓 cascade 退到 PlVk 平面或 L3。
  - 軟體和 unknown-decoder 路徑（`ffmpeg.cpp:2102-2209`）的偏好格式清單也要加上 XrRenderer。
- （新增）`plvk_common.{h,cpp}`：PlVk 和 XrRenderer 共用的純函式；PlVk 的行為用 baseline 驗證。**不繼承 PlVkRenderer。**

**修改的既有檔案**

| 檔案 | 改動 |
|---|---|
| `MQ/streaming/session.cpp` | ① **`startConnectionAsync` 一開始（`/launch` 和 relay 路徑之前）建立 XrContext 並 `bringUp`**；失敗依不變式 5 處理。② XR 模式在 video subsystem 初始化前用 SDL hint 選 offscreen driver（沒有 Wayland 時）；要用 `SDL_HINT_OVERRIDE`：`main.cpp` 可能已依 Qt 平台設了 `SDL_VIDEODRIVER`／`SDL_VIDEO_DRIVER` env 與 OVERRIDE hint（例如 eglfs 時是 `kmsdrm`），NORMAL 的 hint 碰到已存在的 env 或 OVERRIDE hint 會被拒；兩個 env 最好也一併改成同值，理由同 `main.cpp` 的 `setSdlVideoDriver`（見本表 desktop id 那一列）。③ XR READY 推送 `SDL_CODE_XR_READY`，走和 SHOWN 相同的 decoder 建立流程。④ **拆除順序**：停 VrTrackingSender 並 join → 摘掉 haptic sink（atomic 指標換成 null）→ `:3486` 刪 input → `:3493` 刪 decoder → 刪 XrContext（`xrRequestExitSession` → `xrEndSession`）→ `:3524` `SDL_DestroyWindow`。`exec()` 的連線失敗分支也要刪 XrContext。⑤ 在 `:3089-3126` 的 switch 加 `SDL_CODE_XR_POINTER/XR_STATE/XR_EXIT/XR_READY/VR_MESSAGE`（避免踩到 `:3125` 的 `SDL_assert(false)`）。⑥ XR 模式 Pacer 的 `enableFramePacing=false`；XR FOCUSED 期間忽略滑鼠事件 |
| `MQ/streaming/video/decoder.h` | `DECODER_PARAMETERS` 加 `XrContext* xr`、`bool testFrameOnly`（尾端） |
| `MQ/streaming/video/ffmpeg-renderers/renderer.h` | `RendererType::XR` |
| `MQ/streaming/video/ffmpeg.cpp` | `createFrontendRenderer` 在 `params.xr` 存在時先試 XrRenderer；F6 決策；**VR 專用**的 pts 設定與查表；`opaque_ref` 掛載包在 `if (du.vrMeta.valid)` 內 |
| `MQ/streaming/video/ffmpeg-renderers/pacer/pacer.{h,cpp}` | XR 模式只做統計 |
| `MQ/main.cpp`、`MQ/app.pro` | `setDesktopFileName` 的參數改由 qmake 產生的 `desktop_id.h` 提供 `VIPLE_DESKTOP_ID`（`QMAKE_SUBSTITUTES`，同 `version_string.h` 的做法：命令列 `-D` 巨集不會觸發 nmake 重編；qmake 變數 `VIPLE_DESKTOP_ID`，Flatpak 傳 app-id，其餘預設 `viplestream`，只允許 `[A-Za-z0-9._-]`），**仍然在 `QGuiApplication`（`:1057`）之前呼叫**。SDL 視窗 id **依 Qt 平台決定一個值、三處設成同值**：Wayland 用 desktop id（Flatpak 是 app-id，其餘 `viplestream`），xcb 用 `viplestream`，其他平台不設；同一個分支裡 `SDL_VIDEO_WAYLAND_WMCLASS`、`SDL_VIDEO_X11_WMCLASS` 與 SDL3 的字面名稱 `SDL_APP_ID` 三個 env 一律設成這個值，另以 `SDL_SetHintWithPriority("SDL_APP_ID", …, SDL_HINT_OVERRIDE)` 設 hint。原因：sdl2-compat 把兩個 WMCLASS 都對應到 SDL3 的 `SDL_APP_ID`，而且它的 constructor 在 `main()` 之前就把繼承來的 env 抄成 SDL3 的名稱，之後 `SDL_InitSubSystem` 同步 env 時誰最後寫入取決於 SDL3 環境 hash 表的迭代順序（不確定），三個同值才與順序無關；SDL3 又把 env 視同 override 優先權，NORMAL 的 hint 壓不過繼承來的 env，所以 hint 一定要用 `SDL_HINT_OVERRIDE`。video driver 同理：`SDL_VIDEODRIVER` 與 SDL3 的字面名稱 `SDL_VIDEO_DRIVER` 兩個 env 設成同值，hint `SDL_VIDEODRIVER` 也用 `SDL_HINT_OVERRIDE`（所以使用者 export 的 `SDL_VIDEODRIVER` 無法讓 SDL 用和 Qt 平台不同的 driver，和 classic SDL2 原本被 `qputenv` 蓋掉的行為相同）。classic SDL2（AppImage、`.deb`、Windows）讀的是舊名稱，行為不變。驗收：Flatpak 上 `WAYLAND_DEBUG=1` 看 `set_app_id`，以及 `[VIPLE-SF-ENV] session:` 的 `sdl-driver=`／`sdl-app-id=`（SDL 實際採用的值）。這項明確列為早期 init 變更：Windows、x64 AppImage 各跑兩次 `--help`；arm64 在 linux-builder 上用 qemu 跑 `flatpak run … --help` 兩次，納入 M2a 完成條件 |

**生命週期不變式**
- `RENDER_DEVICE_RESET` 只重建 decoder 和 XrRenderer，XrContext 不動。這段期間 XR thread 用自有的最後一幀複本重送，適用 stale 政策。
- XR 進入 `LOSS_PENDING` 時重建 XrContext，最多 3 次。還失敗時：VR session 送 `/cancel` 並顯示錯誤；β 退回平面。
- XR 進入 `EXITING`：視同使用者退出。
- 新 CLI 動作放在 `main.cpp:1472`。

### 2.5 桌面模式呈現

- **α**：交給 gamescope overlay。建議請求 1920×1080 串流，fps 等於 HMD Hz。nested 解析度看 PoC-7，只能經 Settings 調整。
- **β**：world-locked quad，預設距離 1.5 m、寬 60°；runtime 有 cylinder 就改用 cylinder。
  - 本地指標 quad 每個 display frame 更新；統計 overlay 合成進桌面 quad。
  - OpenXR 不可用時，備援是 OpenVR `IVROverlay`。
- **PoC-2b**（實機，Day 1）：從 gamescope openvr overlay 內的行程呼叫 `xrCreateSession`，進入 FOCUSED。觀察 overlay、dashboard 的行為，以及 gamescope 雷射滑鼠會不會和 XR 射線同時送出事件。依結果在 (a)、(b)、(c) 三種啟動形態中選一個預設值。
- **UI**：2.0 的主機和 App 選擇留在平面 UI。

### 2.6 PCVR 呈現：reprojection 正確性清單

1. `views[i].pose = renderPose ∘ eyeToHead[epoch][i]`。renderPose 取自 0x81，由 driver 換回 client 空間（§3.1 pose_history）。client 持續比對 renderPose 和 echo 樣本的 pose，偏差持續超過 0.5°（1 秒）就記 `[VIPLE-VR-REPROJ] space-mismatch`，改用 echo pose。
2. `fov` = 協商的 tangent 加 overscan。`vrOverscanDeg` 預設每側 2°，可調 0–6°，GA 依量到的預測誤差 p99 自動調整。server 在協商時驗證 `(eye+overscan)×2` 不超過 codec 寬度與 level 吞吐上限，超過就縮每眼解析度，或拒絕 H.264。
3. **參考空間**：
   - 上行 pose 和 projection layer 一律用 **STAGE**；
   - 沒有 STAGE 時用 **LOCAL_FLOOR**（OpenXR 1.1 或 `XR_EXT_local_floor`）；
   - 兩者都沒有才用 LOCAL，並依 `vrUserHeightCm` 補 y 偏移。
   - 「坐姿」只影響 SteamVR 端的 seated zero pose，不換 client 空間。
   - driver 提供 standing = raw 的 chaperone 和專屬 `Prop_CurrentUniverseId_Uint64`；**`Prop_DriverProvidedChaperoneVisibility_Bool=false`**，邊界完全交給 Frame 機上處理。
4. **recenter**：收到 `ReferenceSpaceChangePending` 後送 0x5507/02。在 SteamVR 端對映成 seated zero reset 的語意，不換 client 空間；layoutEpoch 加一。
5. **IPD**：Frame 用實體滾輪調 IPD。變動時 client 送 0x5507/01 CONFIG，driver 呼叫 `SetDisplayEyeToHead`，layoutEpoch 加一；0x5508/05 LAYOUT 帶上 eyeToHead。client 依每一幀的 epoch 取用對應版本，避免立體錯位。
6. 單一 `2W×H` swapchain，用 `imageRect` 分眼。**PoC-2 加測左右眼不同色**；不支援時改用 `arraySize=2`，或每眼一個 swapchain。預設每眼 1728²（HEVC level 5.2）。
7. **stale 政策**：
   - 超過 100 ms 沒有新幀就開始淡出；超過 250 ms 切到本地 loading 環境（完全由頭顯本機追蹤驅動）；恢復時淡入。
   - `poseValid=0` 的幀（包含起播 dummy 黑幀）不進 projection layer。
   - `displayTime` 一律用 `predictedDisplayTime`。
8. 2.0 不做 depth 和位置 reprojection，也不做 FRUC；XR 模式強制 SDR。
9. **DRM_PRIME 幀的生命週期**：每張 AVFrame 綁一個 timeline semaphore 值；確認 `pl_render_image` 對應的 GPU 工作完成之後才 unref，避免 v4l2m2m 把 buffer 重新交給 iris 後被覆寫。
10. **latch**：rc 在 `xrBeginFrame` 之後立即取最新幀，並回報 latch slack（0x5507/08）；GA 改 late-latch。
11. **色彩**：vr_probe 加灰階色帶比對，確認 client swapchain（SRGB）和 driver 合成器（依 DXGI 格式處理）之間沒有雙重 gamma。

### 2.7 輸入

| 情境 | 路徑 |
|---|---|
| α | gamescope 滑鼠鍵盤加 Steam Input 手把。**F14 只在 gamescope 下啟用**（偵測 `GAMESCOPE_WAYLAND_DISPLAY`，或設定項），而且只轉送同一批事件中沒有對應 KEYDOWN 的 `SDL_TEXTINPUT`。**等 PoC-7 確認虛擬鍵盤是送按鍵還是文字之後才合併，不隨 1.5.x 全平台發佈** |
| β 射線滑鼠 | trigger 對應左鍵，grip 或 B 對應右鍵，搖桿 Y 捲動；One-Euro 濾波；`SdlInputHandler::handleXrPointer`；XR FOCUSED 期間忽略 gamescope 的滑鼠事件 |
| β 手把模式 | 按住雙 grip 切換成虛擬手把 |
| β 鍵盤 | 藍牙鍵盤；自繪 XR 鍵盤 quad；`IVROverlay::ShowKeyboard` 做 spike |
| PCVR | 全部經 0x5506，由 driver 呈現（§3.1 controller）。session 不是 FOCUSED、或 action 的 `isActive=false` 時，一律送「全部放開」，並設 flags.active=0。指定一個組合鍵（按住 menu 加 trigger 1 秒）送 `/input/system/click`，因為 Frame 的 system 鍵會被機上 SteamVR 攔走 |

### 2.8 設定、UI、CLI

- `MQ/settings/streamingpreferences.{h,cpp}`（列舉只能往後加）：
  - `DisplayTarget { DT_WINDOW=0, DT_XR_DESKTOP=1, DT_PCVR=2 }`；
  - `xrScreenDistanceCm/WidthDeg/Curve`、`xrRuntimeJsonPath`（空值表示自動探測）；
  - `vrRenderScale`、`vrRefreshHz`、`vrTrackingRateMul`、`vrControllerProfile`、`vrOverscanDeg`、`vrCodec`、`vrBitrateMax`、`vrMaxPredictionMs`（HMD，預設 80）、`vrMaxCtrlPredictionMs`（控制器，預設 40）、`vrAllowHighLatency`、`vrDegradedPolicy`、`vrUserHeightCm`；
  - `linuxVideoFrontend`（只影響 frontend，§2.3）。
  - 啟動 `IsVr` 的 app：server 有 PCVR 能力而且 XR 可用時走 PCVR。否則**跳出警告，使用者仍可選擇平面啟動**（不擋既有用法）。
- `MQ/gui/SettingsView.qml`：「XR / VR（Steam Frame）」區段，只在 `HAVE_OPENXR` 時顯示，含 runtime 路徑欄位和「重新探測」。`AppView.qml` 顯示 VR 標記。
- `MQ/cli/commandlineparser.cpp`：
  - 選項：`--display-target`、`--vr-emulate`、`--vr-synthetic-motion`、`--xr-runtime-json`（dev）；
  - 動作：`xr-probe`、`v4l2-probe`、`decode-bench`；
  - 改完跑兩次 `--help`。
- `MQ/backend/nvcomputer.cpp`：解析 `<VipleStreamVR>`、`<VipleStreamVRProto>`，並加 `ASSIGN_IF_CHANGED`。
- `MQ/backend/nvhttp.cpp`、`MQ/streaming/session.cpp`（`tryRelayLaunch`）：兩者都呼叫 `vrlaunchparams`。server 的 b1=1 時，改用 `/applist?vr=1`。
- `MQ/backend/nvapp.{h,cpp}`：解析 `IsVr`。

---

## 3. Server（Windows；非 Windows 為 stub）

### 3.1 SteamVR driver（新增 `SW/steamvr_driver/`）

- **工具鏈**：MSVC，**`/MT` 靜態連結 CRT**。（新增）`Build-SteamVRDriver.ps1 [-Target driver|probe]` 比照 SC-HID 的建置腳本。Stage 階段用 `dumpbin /dependents` 確認不依賴 VCRUNTIME。
- **OpenVR SDK 2.15.6（BSD-3）**：vendor `openvr_driver.h`，另外 vendor `openvr.h` 和 `openvr_api.{lib,dll}`（只給 vr_probe 用），放在（新增）`Sunshine/third-party/openvr/`，附 `LICENSE-OpenVR.txt`。升級必須是單獨一個 commit，並跑 `vr-smoke`。
- **ALVR 移植**：保留 MIT notice，每個移植檔的檔頭記錄來源 commit SHA。

| 元件（新增） | 內容 |
|---|---|
| `driver_main.cpp` | `Init()` 同步讀一次 shm，**armed 時才加入 HMD、L、R** |
| `hmd_device.cpp` | `IVRDisplayComponent` 加 `IVRDriverDirectModeComponent_009`。屬性：`DisplayFrequency`、`GraphicsAdapterLuid`、`Audio_DefaultPlaybackDeviceId`、`IsDisplayRealDisplay=false`、`SecondsFromVsyncToPhotons`（由 virtual_vsync 依實測設定）、`CurrentUniverseId_Uint64`（VipleStream 專屬）、chaperone JSON（standing = raw）、**`DriverProvidedChaperoneVisibility=false`**。presence（b4）對映到 `/proximity`。`VsyncEvents` 只在 GA 開啟 |
| `direct_mode.cpp` | ALVR `OvrDirectModeComponent` 改寫成 _009 簽章：`SubmitLayer`（記錄每個 layer 的 `mHmdPose` 和 `t_target`）、`Present`、`PostPresent(const Throttling_t*)`（依虛擬 vsync 節流）、**`GetFrameTiming`（明確清 `m_nReprojectionFlags`，回報 present/dropped）** |
| `frame_compositor.cpp`＋`shaders/*.hlsl` | 以 **scene layer 的 pose** 為準。dashboard 和 overlay layer 依 Δ(layer pose → scene pose) 做旋轉補償後再疊上去。依 DXGI 格式選 shader 路徑（sRGB 解碼；float 做 tonemap 到 SDR）。輸出到 NT-shared ring（3 張）。**只用 `consumedFence` 已經到達的 slot**，沒有可用 slot 就丟掉這次合成並計數。全程不在 CPU 等 GPU，也不 GPU-wait 對方的 fence |
| `pose_history.cpp` | 存 1024 筆 `{sampleId, 回報的 pose, T_target}`，先比時間，再比角距離（< 1°）。命中後計算 Δ = mHmdPose·reported⁻¹；Δ 如果是穩定的常數偏移（例如 seated reset、PC 上既有的 chaperone），就把 renderPose 修正回 client 空間再送 0x81，記 `[VIPLE-VR-DRV] space-delta=` |
| `controller_device.cpp` | 模擬 Touch（預設）或 Index。每個 profile 有一組 `raw_from_grip` 校正：先用 ALVR 的 offset 當初值，再用 vr_probe 和實機校準，存進 `default.vrsettings`。**`CreateSkeletonComponent`**：由電容觸碰和 trigger/grip 值合成 curl。boolean、scalar、haptic 各元件。active=0 時標 `TrackingResult_Running_OutOfRange` 並放開全部按鍵。每顆鍵的 2 bit 按壓計數用來還原遺失封包中的短按 |
| `virtual_vsync.cpp` | 週期取 `/launch vrPeriodNs`。依 0x5507/08 的 latch slack 在 ±200 ppm 範圍內 slew，讓 slack 穩在目標 margin。明確算出 SteamVR 的目標時間 T_sv。`PostPresent` 睡到下一個虛擬 vsync（rc）。GA 再加 `VsyncEvent` 相位（視 PoC-10）。**不使用 IVRVirtualDisplay 的 API** |
| `tracking.cpp` | `poseTimeOffset = target_server − now`，target_server 由 server 端換算後寫進 shm。HMD 和控制器的外插上限分開設。**stale**：超過 2T 沒有新樣本就把速度和加速度歸零；超過 100 ms 標 `Running_OutOfRange`；只有 pipe 斷線才設 `poseIsValid=false`；記 `[VIPLE-VR-DRV] stale` 計數 |
| `frame_source.h` | DirectMode（主路徑）或 `IVRVirtualDisplay`（G-DRV 失敗時用）。VirtualDisplay 需要另外註冊 `TrackedDeviceClass_DisplayRedirect` 裝置；pose 用 `PresentInfo_t.flVSyncTimeInSeconds + SecondsFromVsyncToPhotons` 搭配 driver 自己回報的樣本，以 SteamVR 相同的外插公式重算，標 **derivedPose**（不標 poseFallback） |
| `ipc_client.cpp` | hardened pipe 握手（驗證 server 身分）、GUID 命名的 shm/event、generation、重連、log ring |
| manifest、settings、input json | `alwaysActivate`、`loadPriority` 由 PoC-5a 決定 |
| `Install-VipleSteamVR.ps1` | **只用於診斷和手動移除**；部署的權威路徑只有 server 編排（§3.10） |
| `check_steamvr_driver_fresh.ps1`、`Stage-VipleSteamVR.ps1`、`THIRD_PARTY_NOTICES.md`、`LICENSE-ALVR.txt`、`LICENSE-OpenVR.txt` | §6 |

- 所有 driver 回呼都包 SEH/try，driver 不能讓 vrserver crash。
- **G-DRV** 在 <host> 上做，不需要實機，條件見 §7。

### 3.2 driver ↔ server IPC

- **ABI 單一來源**：（新增）`SS/vr/vr_ipc_abi.h`，純 C POD，MinGW 和 MSVC 兩邊都用 `static_assert` 鎖住 `sizeof`/`offsetof`。
- **pipe** `\\.\pipe\VipleStreamVR-<consoleSessionId>`：由 server 以 FIRST_PIPE_INSTANCE、REJECT_REMOTE_CLIENTS 建立。
  - 握手內容：abiVersion、driverVer（取自 `driver_version.h`）、PID、SteamVR 介面版本、LUID、每眼參數、Hz、period、控制器 profile、audio endpoint。
  - server 回傳本 session 的 shm/event GUID 名稱。
- **shm** `Local\VipleStreamVR.<guid>.shm`：
  - header：magic、abi、generation、armed、state、心跳；
  - session 設定；
  - tracking seqlock（8 格，含 `target_server_qpc`）；
  - frame descriptor ring（8 格）：`{frameId, texIdx, fenceVal, echoSampleId, echoMatched, renderPose(client 空間), tTarget, presentQpc, flags(repeated, derived, fallback)}`；
  - 貼圖表：3 × `{handle, w, h, fmt}`，加 `sharedFence`、`consumedFence`；
  - haptic ring、log ring、stats。
- **events**：`Local\VipleStreamVR.<guid>.evtTrk`、`.evtFrm`。
- **fence 交接**：
  - driver：GPU Signal(sharedFence, n) → 寫 descriptor → SetEvent。
  - server：CPU 端確認 `sharedFence ≥ n`（最多等 2 ms，逾時丟幀並計數）→ CopyResource → GPU Signal(consumedFence)。
  - server 用 latest-wins 跳過某些 descriptor 時，一次把 consumedFence Signal 到最新值。
  - driver 重用 slot 前用 `GetCompletedValue()` 確認。
  - 既有的同行程跨 device Wait 模型（`display_vram.cpp:430-436,1761-1859`）**只用在同一個行程內**。
- **重連**：generation 加一，兩端都重建 fence 和貼圖。
- **注入測試**：在 VR session 中 kill server，SteamVR 必須在 1 秒內恢復到 standby，不能卡死。
- **新增檔案**：`SS/vr/vr_bridge.{h,cpp}`（平台無關）、`SW/vr_bridge_win.cpp`、（新增）`SS/platform/linux/vr_stub.cpp`。

### 3.3 display_vr_t 擷取來源

- **新增** `SW/display_vr.cpp`；在 `SW/display.h` 宣告 `display_vr_t : display_vram_t`。
- **行為不變的前置重構**：把 `display_base.cpp:555-733` 抽成 `init_device(adapter_luid)`，DDA/WGC 也改呼叫它。GPU 優先權不在這裡設（C23）。用 baseline 驗證沒有回歸。
- **必須覆寫**：
  - `is_hdr()` 回 false，`get_hdr_metadata()` 回 false（C18；XR 模式強制 SDR）；
  - **自訂 `init()`**：依握手參數設定 `width/height/env_width/env_height/width_before_rotation`、`capture_format=DXGI_FORMAT_B8G8R8A8_UNORM`、`display_refresh_rate`、`client_frame_rate`；
  - `capture()`：
    1. `WaitForSingleObject(evtFrm, 100 ms)`，逾時推 false；
    2. 驗證 descriptor（§1.3）；
    3. `pull_free_image`；
    4. CPU 端確認 fence；
    5. `CopyResource`；
    6. Signal `capture_fence`、`consumedFence`；
    7. 寫入 `img->vr`，`frame_timestamp = presentQpc`；
    8. push。
    - driver 回報 HMD_ACTIVE 之前，以 10 fps 推 `poseValid=0` 的黑幀。
- **沿用**：`alloc_img`、`complete_img`、`dummy_img`、`make_*_encode_device`、`is_codec_supported`。
- **單元測試**：VR display 建好後依序呼叫 `is_hdr`、`get_hdr_metadata`、`alloc_img`、`dummy_img`，都不可 crash（也涵蓋無 IPC 的探測模式）。
- `is_event_driven()=true`；重送的幀標 `repeated`。
- **工廠**：`platf::display()` 依 `captureSource` 分支。
- **`video::config_t`**：尾端加 `int captureSource = 0; int vrProfile = 0;`（default member initializer）。
- **captureThread 與 captureThreadSync**：VR 模式跳過列舉，直接重建 `display_vr_t`。
- **probe**：`probe_encoders(captureSource)` → `validate_encoder(encoder, expect_failure, captureSource)` → `reset_display(..., captureSource)`。4 個 aggregate 初始化點都不改；`generic_hdr_config` 和它的衍生複本自動帶預設值 0。**VR 探測模式略過 HDR 段（`:2994-3030` 一帶）和 YUV444 段**，headless 主機不會因為 DDA 失敗而回 503。
- **per-frame metadata**：`platf::img_t` 和 `packet_raw_t` 各加 `std::optional<vr_frame_meta_t>`。

### 3.4 編碼 VR profile（以 `config.vrProfile` 為閘門，桌面路徑不變）

| 項目 | 設計 | 位置 |
|---|---|---|
| codec | HEVC 優先（level 5.2）。H.264 保底只接受「72 Hz」或「每眼 1440²」這類在 level 5.2 內的組合（C27），server 協商時驗證寬度、MaxMBPS、MaxLumaSr。AV1 要等 L1 | `/launch` 協商 |
| **恢復協定（取代草案的「intra-refresh 優先」）** | vrProfile 下**強制** `enableIntraRefresh=1`（需要 `NV_ENC_CAPS_SUPPORT_INTRA_REFRESH`，不依賴 client 的 SDP），H.264/HEVC/AV1 都接上。`intraRefreshCnt = N`（Sunshine config `vr_intra_refresh_frames`，預設 8）。`intraRefreshPeriod` 當作慢速安全網（`vr_intra_refresh_safety_ms`，預設 2000 ms）。收到 0x5507/09 LOSS 時，下一幀設 `forceIntraRefreshWithFrameCnt=N`，同時送 0x5508/06 `REFRESH_START{startFrame, N}`（ch 0x07 unseq ×2），這一波的最後一幀標 `vrFlags.b7 refreshDone`。wave 進行中，`lastLost < waveStart` 的 LOSS 直接吸收；更晚的 LOSS 會重啟 wave。encoder 不支援 IR 時，協商回 `recovery=idr`，G-rc 改用 IDR 門檻。在 <host> 上驗證 forced IR 和 4 slices 並存時的實際 slice 行為 | `SS/nvenc/nvenc_base.cpp`、`SS/video.cpp`、`SS/stream.cpp` |
| IDR | 只在起播、decoder reset、改解析度時送。**VR 另設 `kVrIdrCooldownMs=300`**；桌面的 1500 ms（§S.19）不動，改成具名常數。每次 VR IDR 記 `[VIPLE-VR-ENC] idr reason=` | `SS/video.cpp:2196` 一帶 |
| ABR | 傳 `isDecrease && !vrProfile`，VR 降碼不 reset、不 IDR | `SS/video.cpp:2276` |
| FEC 單幀上限 | 風險低；記 `fec-skip` 計數 | — |
| slices | 固定 4（IR wave 期間由 NVENC 覆寫） | — |
| fps | 一個 session 固定；換 Hz 要重開 session | — |
| HDR | 固定 8-bit SDR | — |
| GPU 優先權 | 由 vr_session 在行程層級設為 HIGH，每次 probe 之後重設，結束時還原（C23） | `SS/vr/vr_session.cpp` |
| foveation（GA） | QP map 的中心 = 依協商 FOV tangent 算出**光軸在影像中的位置**，不是影像中心；PoC-8 通過後才用 gaze | — |
| sub-frame、split-encode | GA 之後的選配 | — |

### 3.5 傳輸與控制迴圈

- **0x81 header**：只在 VR session 寫 24 B 版本；`SS/stream.cpp:2193`、`:2249` 改用實際長度。
- **rc：VR 視訊強制走 RTP/UDP。**
  - VR session 下 `use_quic = quicVideoSession && vrQuicVideoAllowed`，rc 固定 false（改 `SS/stream.cpp:2371-2390`）。QUIC 只用在 control 和 tracking 的 fallback。
  - 如果這個 client 只能走 QUIC（relay 或 tunnel），rc 的 launch 回非 200 `VR_TRANSPORT_UNSUPPORTED`。
  - `[VIPLE-VR-TX] transport=` 必須出現在每一輪 log。
- **GA 的 WAN profile：QUIC VR 政策。**
  - 單一 active path：視訊略過 jitter buffer；
  - 多路徑：`maxWait = min(2 ms + 路徑 RTT 差, 0.3T)`，FEC defer 不延長；
  - VR session 的視訊 flow buffer 調到 **≥ 1024 格**（或依碼率和 fps 動態配置；原本是 `Q/QuicTransport.c:317-319` 的 256 格），並加 `[VIPLE-VR-FEC] jb-overflow` 計數。
  - 改在 Q/A 的 `QuicTransport.c`。
- **送出 pacing**：`VIPLE_SMOOTH_PACING` 改成 config（F7）；VR 送出窗 ≤ 0.3T。
- **FEC**：VR 下限 Wi-Fi 10%、有線 5%。
- **S→C 即時訊息**：`control_server_t::send(payload, channel, flags)`（F4）；VR session 的 iterate 逾時改 4 ms。
- **時鐘對映（server 端）**：
  - `vr::Bridge` 記錄每筆 0x5506 的 arrival QPC。
  - 以 ENet `roundTripTime`（QUIC fallback 時用 QUIC RTT）估 offset：`offset ≈ min_{2s}(arrival − sampleTime) − RTT_min/2`。
  - 每筆樣本換算成 `target_server = sampleTime + predictNs + offset` 後寫進 shm。
  - 記 `[VIPLE-VR-CLK] offset= jitter=`。

### 3.6 App 啟動與 SteamVR 編排

- **新增**：`SS/vr/vr_session.{h,cpp}`、`SW/steamvr.cpp`。
- **`/launch` 帶 `vr=1` 時**：
  - 必須 `session_count()==0`；
  - 不呼叫 `configure_display`；
  - 用 VR 探測模式跑 `probe_encoders`；
  - 解碼 VR 參數，失敗就回非 200 附明確訊息；
  - 立刻回 `<VipleStreamVRSession>`，**編排在背景執行**。
- **`/resume`**：VR 參數不一致時回 503。
- **`vr` 類 app**（`SS/process.cpp:313-488`）：execute 時設 `placebo=true`。`running()` 由 vr_session 狀態機決定：`ORCH_*` 或 `HMD_ACTIVE` 時回 app_id；只有 disarm 或使用者結束才回 0。這樣可以避開 `SS/stream.cpp:1963-1966` 在遊戲還沒起來時就結束 session。
- **編排狀態機**（每一步記 `[VIPLE-VR-ORCH]`，並用 0x5508/02 STATE 推給 client）：
  1. 部署 driver（§3.10），以使用者身分 `vrpathreg adddriver`，檢查 safe mode 封鎖。
  2. **偵測其他 HMD 是否正在使用**（vrlink 已連線、其他 PC-VR 串流程式正在服務頭顯（例如 Virtual Desktop Streamer，測試 host 上就有裝），或有 VR 遊戲在跑）。有的話**預設拒絕**，回 STATE `VRLINK_ACTIVE`；由 Frame client 顯示確認，使用者同意後帶 `force=1` 重試。
  3. D8b settings guard（§3.8），以使用者 token 執行。
  4. 把 shm 設為 armed。
  5. 需要時重啟 SteamVR。「自動重啟」只在沒有任何 HMD 在用時才允許。
  6. 以使用者身分開 `steam://rungameid/250820`。
  7. 等 HMD_ACTIVE，最多 45 s；ABI 不一致時回 `ABI_MISMATCH_RESTART_STEAMVR`。
  8. `steam://launch/<id>/VR`，不行就退回 `rungameid`。
  9. watchdog 改盯 VR session。
- **結束**：disarm，HMD 進入 standby，不強制關 SteamVR；settings guard 的還原見 §3.8。
- **`/applist`**：
  - **`vr` 類 app（例如「SteamVR Home」）只出現在 `/applist?vr=1`**（只有 Qt client 會在 b1=1 時送），標準 `/applist` 不注入。
  - 一般 app 如果在 `steamapps.vrmanifest` 裡有對應，就加 `IsVr` 提示，兩種清單都有；沒帶 `vr=1` 時照舊以平面啟動，行為不變。
  - 即使有人直接以一般 `/launch` 啟動 `vr` 類 app，server 也回非 200「此 app 需要 VR client」。

### 3.7 音訊

- 沿用 WASAPI loopback。driver 指定同一個 endpoint，避免和 `SW/audio.cpp:777-784` 互搶。
- VR session 的 Opus 封包長度用 5 ms。
- 麥克風不在 2.0 範圍（U8）。

### 3.8 vrlink 共存、safe mode、升版、註銷

- **預設策略改為 D8b**：VR session 期間，以**使用者 token** 寫入 `forcedDriver=viplestream`、`driver_vrlink.enable=false`，寫之前先備份並留 marker。編排器本來就要（重）啟 SteamVR，順序上正好。
- **還原**：
  - 在 SteamVR **沒有執行**時才寫回，因為 vrserver 結束時可能把記憶體中的設定寫回檔案；如果 session 結束時 SteamVR 還在跑，就等 vrserver 結束後再還原。
  - server 啟動或使用者登入時，看到 marker 就以使用者 token 還原。
  - UI 提示「需重啟 SteamVR 才能用原生 Steam Link」。
- **降級成 D8a**（armed 閘門加 loadPriority，不動 vrlink）：只有 PoC-5a/5b 證明 vrlink 不會劫持 Frame、也不會影響 Tailscale 路徑時才降級。
- **其他 PC-VR 串流程式**：Virtual Desktop Streamer（測試 host 上有裝，另帶 Virtual Desktop Audio／Gamepad driver）也是一套 PC-VR 串流，列入 §3.6 第 2 步的衝突偵測。它是否也需要類似 D8b 的 settings guard，M1b 盤點後決定。
- **safe mode**：每個 driver 版本只自動解除一次；再被封鎖就回 `SAFE_MODE_BLOCKED`。
- **升版**：記 `[VIPLE-VR-DRV] steamvr=<ver> iface=<...>`；版本變動時自動 self-check。
- **註銷路徑**（新增）。下列三種情況都以使用者身分執行 `removedriver`、還原 marker、刪除版本目錄：
  1. `vr_pcvr` 被設為 disabled 時；
  2. server CLI `--steamvr-driver uninstall`；
  3. server 啟動時偵測到 `vr_pcvr=disabled`，但 driver 仍在註冊中。
  - 移除 VipleStream 的文件步驟也要涵蓋這些動作。

### 3.9 桌面模式的虛擬顯示器（D11）

- α、β 直接串 primary 或 `output_name` 指定的顯示器。
- MTT VDD 模式範本放在 `Sunshine/src_assets/windows/misc/vdd_settings.frame.xml`（M2a 已新增），用法見 [`docs/setup_guide.md`](setup_guide.md#vdd-frame-template)。範本不隨 server zip 出貨（`build_sunshine.cmd` 只收固定清單；日後要出貨時放新的頂層 `vdd\`，**不可**放 `scripts\` 或 `config\`，前者會被 self-update 整個替換、後者被跳過）。server 以 SYSTEM 執行，不自動寫入 `C:\VirtualDisplayDriver`（該目錄一般使用者可寫，不變式 9）。
- 動態 VDD 放到 2.0 之後。

### 3.10 driver 部署（權限邊界）

- server zip 內的 `steamvr\viplestream\` 只是來源；§SELF-UPDATE 契約不變。
- **目的地**：`<install>\config\steamvr\<ver>\`。標準安裝在 `C:\Program Files\VipleStream-Server\` 下，只有 Admin/SYSTEM 能寫。self-update 腳本跳過 `config`（`self_update.cpp:482`），不會 rename 或 copy，所以 DLL 被載入時也不會卡住更新。
- **部署前檢查**（fail closed，失敗就記 `[VIPLE-VR-ORCH] deploy-refused reason=`，VR 停用）：
  1. 路徑上每一層的擁有者都是 SYSTEM 或 Administrators，DACL 不允許 BU 寫入；
  2. 用 `FILE_FLAG_OPEN_REPARSE_POINT` 確認整條路徑沒有 reparse point；
  3. 以 protected DACL 建立版本目錄：SY/BA 完全控制、BU 只有 RX、不帶 CREATOR OWNER；
  4. 複製後做 hash 比對，才交給 `vrpathreg`。
  - 新程式**不可**沿用 `nvprefs_interface.cpp:146` 那種「`ERROR_ALREADY_EXISTS` 就直接接受」的慣例。
- 以使用者身分 `adddriver` 新版、`removedriver` 舊版；刪除舊版目錄，失敗就忽略。
- **部署的權威路徑只有 server 編排這一條**；`Install-VipleSteamVR.ps1` 只拿來診斷或手動移除，在 `docs/steamvr_driver.md` 寫清楚。

### 3.11 Server 檔案清單

- **修改**：
  - `SS/stream.cpp`、`SS/video.{h,cpp}`、`SS/nvenc/nvenc_base.cpp`、`SS/nvhttp.cpp`、`SS/rtsp.{h,cpp}`、`SS/process.cpp`、`SS/config.{h,cpp}`、`SS/platform/common.h`
  - `SW/display.h`、`SW/display_base.cpp`、`SW/audio.cpp`、`SW/steam_scanner.cpp`
  - **`Sunshine/cmake/compile_definitions/common.cmake`**：`SS/vr/vr_bridge.cpp`、`vr_session.cpp` 放在這裡。平台相關部分放在 `platf::vr_*` 介面後面
  - `windows.cmake`：`SW/display_vr.cpp`、`vr_bridge_win.cpp`、`steamvr.cpp`
  - Linux cmake：`SS/platform/linux/vr_stub.cpp`，回報不支援，b1=0
- **新增**：
  - `SW/display_vr.cpp`、`SW/vr_bridge_win.cpp`、`SW/steamvr.cpp`、`SS/platform/linux/vr_stub.cpp`
  - `SS/vr/{vr_ipc_abi.h, vr_bridge.*, vr_session.*}`
  - `SW/steamvr_driver/**`（含 `driver_version.h`，由 propagate 產生）
  - `Sunshine/third-party/openvr/`
  - `Sunshine/tools/vr_probe/`（MSVC，由 `Build-SteamVRDriver.ps1 -Target probe` 建置，不出貨）
  - `S3/VipleVr.h`
- **M1 完成條件**：linux-server `.deb` 能建置，Linux KMS 的 stream-quality 不比 baseline 差，相容矩陣在 Linux server 上跑過。

---

## 4. 協定擴充

完整內容見 [`vr_protocol.md`](vr_protocol.md)。

---

## 5. 既有問題修正（前置工作，都不需要實機）

| # | 問題 | 位置 | 為什麼是 2.0 前置 | 修法 |
|---|---|---|---|---|
| F0 | §SELF-UPDATE 還沒 commit | `SS/self_update.{h,cpp}` 等 | 2.0 要改 updater | 先驗證再 commit，**備妥 1.5.276 release 候選，交使用者決定是否發佈**。完成條件包含「**使用者協助完成 service 模式 UAC 實測**」 |
| F1 | Android common-c 漂移：`src/` 7 檔、**`enet/` 5 檔**；`isBefore16` 凍結 bug | `A/` | VR 依賴 ENet 的 unsequenced 和 throttle 行為 | Q 覆寫 A（`src/` 加 `enet/`）；`PlatformNetIf.h` 和確有平台差異的 enet 檔手動合併，並列入白名單；Pixel 5 回歸 15 分鐘；接上同步檢查 |
| F2 | 256 B buffer 只有 `LC_ASSERT` 守 | `Q/ControlStream.c:798,817` | VR 封包 236 B | 改成 runtime 檢查 |
| F3 | QUIC recv handler 註冊太晚 | `SS/stream.cpp:2407-2409` | fallback 期間訊息會被丟掉 | QUIC session 建立時就註冊 |
| F4 | server 控制訊息固定 ch 0，迴圈 150 ms | `SS/stream.cpp:369-378,1968` | haptics 需要 | 新增 `send(payload, channel, flags)`；VR 期間 iterate 改 4 ms |
| F5 | 從未呼叫 throttle；重連會回到預設值 | `Q/ControlStream.c:1732-1756,2477-2480` | tracking 會被機率性丟棄 | helper 放在首次連線和重連兩處；M1 驗證「ENet 重連後的 pose 丟失率」 |
| F6 | **Linux renderer 決策**（範圍擴大） | `ffmpeg.cpp:579` 等 7 處（C19）、`MQ/wm.cpp:208-221` | Frame 要走 PlVk；違反「不用環境變數設定」 | `linuxVideoFrontend` 加 isGpuSlow 規則（§2.3）。以下環境變數覆寫全部標成 dev-only，使用時在 log 警告：`PREFER_VULKAN`、`GL_IS_SLOW`、`VULKAN_IS_SLOW`、`MATCH_DISPLAY_MODE_TO_VIDEO`、`SEPARATE_TEST_DECODER`、`%s_AVOPTIONS`、`*_DECODER_HINT`、`VIPLE_USE_VK_DECODER`、`VIPLE_VKFRUC_*` |
| F7 | `VIPLE_SMOOTH_PACING` 環境變數 | `SS/stream.cpp:2323-2326` | 違反規則 | 改成 config `smooth_pacing` |
| F8 | aarch64 建置阻斷 | §2.1 | α 的前提 | §2.1（只放在 M2a） |
| F9 | updater 不看架構與打包型態 | `MQ/backend/updater.cpp:118-148` | arm64 會下載 x64 AppImage | 精確比對後綴加架構；Flatpak 和 zip 先做「只通知」 |
| F10 | `version.ps1` 沒有 `set`、沒有防護 | 本機 `build-tools/version.ps1:13-19,57-63` | 2.0.0 切版需要 | `set -Version`：純數字、單調遞增、冪等（已經是目標版號就只 propagate）。**防護寫成共用函式，bump 和 set 都要呼叫**：patch ≤ 999、minor ≤ 9，patch > 900 時先印警告。新增 propagate 目標 `SW/steamvr_driver/driver_version.h`（§6）。`docs/versioning.md`（進 git）記錄 set 的語意、防護條件、切版 SOP，本機 CLAUDE.md 的「跳過 bump」規則加上 set 的例外。單元測試腳本和 version.ps1 一起放在本機，並明確寫成本機限定 |
| F11 | `build_all.cmd` 漂移 | 本機 | 會無聲缺 driver | 改成呼叫單支腳本並帶 `--no-bump` |
| F12 | Linux 依賴與 FFmpeg configure 不在 git | 本機腳本 | arm64 builder 拿不到 | 放進 `MQS/steamframe/`（只放在 M2a） |
| F13 | ABR 降碼 forceIDR | `SS/video.cpp:2276` | VR 會卡頓 | 依 vrProfile 決定 |
| F14 | TEXTINPUT | `session.cpp:2950-2954` | α 虛擬鍵盤 | **只在 gamescope 下，去重後轉送；PoC-7 之後才合併，只隨 Frame 版發佈**（§2.7） |
| F15 | 探測綁定桌面顯示器 | `SS/nvhttp.cpp:1200-1219`；`SS/video.cpp:1371,1555-1573,2523-2531,2893,2894,2989,2996,2999-3005` | headless 主機不能回 503 | §3.3（參數往下傳、default member initializer、VR 略過 HDR/YUV444） |
| F16 | intra-refresh 只接 HEVC，而且要靠 SDP 開 | `nvenc_base.cpp:357-370`、`SS/rtsp.cpp:1006,1045` | VR 恢復協定 | §3.4（強制 IR、三種 codec、隨選 forced IR） |
| F17 | 文件過期 | 文件 | 同步規則 | §6 |
| F18 | `VIPLE_MPQUIC` 預設不一致 | `build_moonlight.cmd:19,83-87` | Frame 必須開；**S0 要用 `--mpquic` 建置** | T9 另外決定 Windows 的預設值 |
| F19 | **（已取消）** CoworkMCP 任務圖會造成版號漂移。CoworkMCP 於 2026-09-23 停用，release 改走 §6 的手動流程，每次建置前核對 `version.ps1 get` 等於目標版號；以下為原始記錄 | `<CoworkMCP repo>/packages/worker/dispatch/{win,host,linux}.json`、`examples\viplestream-release.ts:53-67` | 2.0 發佈流程 | ① `bump-version` op 改成 `pwsh build-tools\version.ps1 set -Version ${version}`。② `win-client`、`win-server` 一律帶 `--no-bump`。③ 每個 build 任務開頭先斷言 `version.ps1 get == spec.version`，不相等就 fail。④ **finalize 前插入人工核准閘門**：coordinator 停下並回報 bb 摘要，等使用者說「發 release」。⑤ linux-arm64 op、host 的 `run-script`/`fetch-artifact`（非特權）寫進**真實 worker 設定**，不能只改範本（本次 coworkmcp 連線失敗，無法確認真實設定，列為待查）。⑥ 在 dry-run（`COWORK_DRYRUN=1`）和一次 1.5.x 實跑中驗證四件版號一致 |

- **α 需要**：F0、F6、F8、F9、F12（F14 看 PoC-7）。
- **M1a 需要**：F2、F3、F4、F5。**M1b 需要**：F7、F13、F15、F16。
- **2.0.0 切版前需要**：F10、F19。
- **發佈方式**：F 項從 1.5.276 起分批以「release 候選」形式備妥；每一次發佈都由使用者決定。

---

## 6. 建置、版號、發佈

- **2.0.0 切版 SOP**
  1. G-α 用 1.5.x 建置通過之後，執行 `pwsh build-tools\version.ps1 set -Version 2.0.0`，propagate，commit `v2.0.0: …`。所有 build 都帶 `--no-bump`。
  2. **六件 asset 全部重建之後，重跑一次縮短版驗證**：Frame 上的縮短版 G-α（decoder 那一行、5 分鐘 fps、兩次 `--help`）；Windows client 兩次 `--help`；Windows stream-quality 對照 baseline。
  3. 備妥 release 候選，交使用者決定是否 push 和發佈。
  - **α-lite（只有軟體解碼）不切 2.0.0**，繼續出 1.5.x。
  - versionCode 為 20000+x，由 F10 防護。
  - `vr_pcvr`：rc 以前預設 `disabled`；GA 改 `auto`。
- **asset**
  - **從 2.0.0 起六件為強制**：Win Client zip、Win Server zip、Android apk、`-linux-x64.AppImage`、`-linux-x64.deb`、`VipleStream-Client-X.Y.Z-linux-arm64.flatpak`（G-PKG 改 P2 時是 `-linux-arm64.zip`）。
  - 本機 CLAUDE.md 的 Release 規範、必備 asset 表、「同版號」條款同步改成六件。
  - **builder 失效規則**：任何一件建置失敗，整個 release 暫停，不得用舊版 asset 頂替；例外必須由使用者明確同意，並記在 release notes。
  - Windows updater 只選不含 linux 的 `.zip`，x64 Linux 只選 `.AppImage`。
- **arm64 builder**
  - 第一階段：linux-builder 用 qemu-user 跑 `flatpak-builder --arch=aarch64`，一律在 flatpak sandbox 內建置。**Monado 和 SteamVR 用 Flatpak 或容器隔離，不裝進 `/usr/local`**，避免污染 x64 AppImage 的 ldd 收集。
  - source 同步：已 push 時 `git pull`。未 push 時：dev 在 <dev-client> 用 `git add -N`（新檔）＋`git diff --binary HEAD` 出 patch、scp 過去 `git apply`；**release 要 commit SHA 完全一致，用 `git bundle`**（<dev-client> `git bundle create` → scp → builder `git fetch <bundle>` 後 `git merge --ff-only`）。寫在 `build-steamframe.sh --help` 與 [`steam_frame_client.md`](steam_frame_client.md) §3.5。
  - 長建置用 `nohup setsid … & disown` 加輪詢。不要包 `systemd-inhibit`：經 SSH 呼叫時 polkit 回 `interactive authentication required`；builder 已關掉 AC 下的休眠（`sleep.target` masked），直接用 `nohup setsid`。
  - 第二階段：`linux-arm64-builder`（U7）；G-BUILD 不過就提前。
  - 不用 GitHub Actions。
- **進 git 的新腳本**
  - `MQS/build-steamframe.sh`（`--arch`、`--flavor`）
  - `MQS/steamframe/flatpak/<app-id>.yml` 加各模組、`finish-args.md`
  - `MQS/steamframe/sniper/*`（spike）
  - **`MQS/frame-poc-collect.sh`**：**只在 Frame 本機產生 tarball，不碰網路**。改由 <dev-client> 經 Developer Mode SSH（scp）拉回。腳本參數化，不寫死任何 IP 或 token。這條流程同時是 G-β、G-rc 的 log 回收 SOP，寫進 `docs/steam_frame_client.md`。
- **driver 打包**
  - 本機 `build_sunshine.cmd` 在 SC-HID 區段之後呼叫 `Stage-VipleSteamVR.ps1`（進 git）。
  - 版號做法擇一改成兩者都做：
    - (a) `version.ps1` 的 Propagate 新增第四個目標，產生 `SW/steamvr_driver/driver_version.h`（進 git），防呆的來源集合包含它，`docs/versioning.md` §2 的表格同步更新；
    - (b) Stage 讀出 DLL 的 FileVersion，和 `version.ps1 get` 比對，不一致就強制重建，重建後仍不一致就 `exit 1`。
  - 必要檔：`driver.vrdrivermanifest`、`bin\win64\driver_viplestream.dll`（`/MT`，用 `dumpbin /dependents` 確認不依賴 VCRUNTIME）、`resources\settings\default.vrsettings`、`Install-VipleSteamVR.ps1`、`LICENSE-ALVR.txt`、**`LICENSE-OpenVR.txt`**、**`THIRD_PARTY_NOTICES.md`**。缺任何一個就 `exit 1`。
  - vr_probe 另外打包成 `VipleStream-VrProbe-dev.zip`，只在開發機與 host 之間 scp，不進 release。
- **release 流程（手動依序；CoworkMCP 已於 2026-09-23 停用）**

  ```
  version.ps1 set/bump → <dev-client> build_all + build_android（--no-bump）
  → linux-builder（SSH）建 x64 AppImage/.deb 與 arm64 Flatpak（qemu → 之後原生 ARM builder），scp 拉回、核對 sha256 與版號
  → deploy_server_to_host.ps1 部署 host → stream-quality ＋ vr-emulate 驗測
  → 【停下回報，等使用者說「發 release」】→ gh release
  vr-smoke：SteamVR 版本變動時或每週一次（server CLI --vr-selftest）
  ```

  **VR 的 server 端取證一律用 SSH 對完整 log 做 grep**（sunshine.log 與 SteamVR 的 vrserver.txt／vrcompositor.txt，脫敏後帶回），不只看尾段（§8.3）。
- **文件**
  - 新增：`docs/vr_architecture.md`、`docs/vr_protocol.md`（含不變式 2、參數格式）、`docs/steam_frame_client.md`（Day 0 過渡指引、log 回收 SOP、安裝範圍 `--user` 或 `--system` 等 PoC-1 確認後寫入）、`docs/steamvr_driver.md`（部署、註銷、手動移除）、`docs/releases/v2.0.0.md`。
  - 更新：`docs/building.md`、`docs/versioning.md`、`docs/log_tags.md`、`docs/rendering_paths.md`、`docs/setup_guide.md`、`docs/troubleshooting.md`、`docs/TODO.md`、README。
  - **docs 一律用 `<host>` 佔位，commit 前跑洩漏 grep。**

---

## 7. 分期里程碑

**工期前提**：以單人全職、依序執行為主，雙人版本另外列出。每個 milestone 拆成「程式碼與模擬（a，不需實機）」和「實機關卡（b）」。實機關卡不放在程式碼的依賴鏈上。

| 里程碑 | 範圍 | 依賴 | 需要實機 | 完成條件 | 工時 |
|---|---|---|---|---|---|
| **M0 前置**（1.5.x 候選） | F0–F4、F6、F7、F9–F11、F17、F18（F5、F13 依賴 VR 旗標，併入 M1a；F19 已取消）；baseline：Windows D3D11（Linux runtime baseline 移到 M1a）；G-BUILD 移到 M2a 開頭 | — | 否 | 同步檢查 PASS（`src/` 加 `enet/`）；Pixel 5 回歸 15 分鐘；`version.ps1 set`/bump 防護測試；updater 選對 asset；baseline 寫進 `scripts/benchmark/results/` | 4–5 週 |
| **M1a 協定骨架** | `VipleVr.h`、協商（含 relay 共用函式）、0x5506–0x5508、0x81、S→C async、VR 恢復協定（common-c 加 NVENC forced IR）、pts 查表、server stub 回聲、Windows `--vr-emulate`、Linux stub | M0 的 F2–F5 | 否 | vr-emulate 在 <host> 上：echo 命中 ≥ 99%；decoder 丟一幀注入後 0 不一致；LOSS → REFRESH_START 往返；**linux-server 建置加相容矩陣（Linux server）** | 4–5 週 |
| **M2a α 建置** | F8、F12、P1 Flatpak、desktop id、`build-steamframe.sh`（兩種 arch）、probes、F6 在 x64 上的檢查、VDD 範本 | M0 的 F0、F6、F9 | 否 | arm64 Flatpak 產出；qemu 跑 `flatpak run --help` 兩次；Windows 和 AppImage 各跑 `--help` 兩次；PoC-F-pre 在 S3 上的前置（**PoC-F-pre 本身延到 M3a 開頭**：使用者 2026-09-28 決定，需要 Steam 登入；M2a 只備妥 `MQS/steamframe/s3/` 的腳本與 SOP） | 3–4 週 |
| **M1b server VR 本體** | driver（API 修正、compositor、pose 空間、控制器、stale）、hardened IPC、`display_vr_t`、VR profile、時鐘對映與頻率鎖、編排（D8b、衝突偵測、註銷）、部署安全、`vr_probe`、`--vr-selftest` | M1a、F7、F13、F15、F16 | 否 | §8.3 M1；**G-DRV、PoC-5a、PoC-10** 完成；依實測重定 G-rc 延遲門檻 | 8–10 週 |
| **M3a β 程式碼** | XrContext（`/launch` 前 bring-up、loading 環境）、XrRenderer（測試實例規則）、quad/cylinder、射線、鍵盤、fallback、SDL offscreen、P2 spike、S2（`--openxr`） | M2a | 否 | S1 和 S2 自動化；XR 失敗注入：`/launch` 前退回平面、`/launch` 後送 cancel | 5–6 週 |
| **M4a rc 程式碼** | projection、tracking 兩種模式、LATCH 回授、控制器 skeleton 與 system 組合鍵、haptic 走 SDL 事件、stale 政策、色彩一致 | M1b、M3a | 否 | S1 projection 對 <host> 跑通；500 ms 斷線注入；kill server 注入 | 5–6 週 |
| **M5a GA 程式碼** | WAN QUIC VR 政策與 buffer、pacer（VsyncEvent，視 PoC-10）、late-latch、QP foveation、L1（視 PoC） | M4a | 部分 | GA 門檻的非實機部分 | 4–6 週 |
| **實機關卡批次** | G-α（M2b）→ G-PKG → G-β（M3b）→ G-rc（M4b）→ GA（M5b） | 對應的 a 段 | 是 | 下表 | 合計 4–6 週 |
| 2.0 之後 | 動態 VDD、麥克風、XR 內 Qt UI、sub-frame、幾何 foveation、Linux PCVR server、桌面 pts 查表 | — | — | — | — |

- **單人順序**：M0 → M1a → M2a → M1b → M3a → M4a → M5a。實機到手時 G-α 所需的程式碼大致已完成，關卡依批次插入。總計約 **37–48 週**。比草案多出的部分來自這次審查新增的範圍：恢復協定、時鐘與頻率鎖、IPC 強化、註銷、Linux stub、控制器 skeleton 等。
- **雙人**：
  - server 線：M0（分攤）→ M1a → M1b → M4a（server 部分）；
  - client 線：M2a → M3a → M4a（client 部分）→ M5a。
  - **關鍵路徑是 M0 → M1a → M1b → M4a → M5a，約 24–31 週**，另加關卡批次。
- **實機到手後**（Developer Mode 下 SSH）：
  - **Day 0（唯讀探測加安裝一個 Flathub app）**：`frame-poc-collect.sh`（本機 tarball，由 <dev-client> 拉回），涵蓋 PoC-0、PoC-1、PoC-6（含 `iw dev <if> get power_save`，並在 180 pps 上行時量 RTT 分佈）、PoC-7。
  - **Day 1**：α 套件跑 `xr-probe`（PoC-2：擴充清單、`convert_timespec_time`、imageRect 左右眼異色測試；PoC-2b；PoC-9；PoC-F），以及 `v4l2-probe`、`decode-bench`（PoC-3：`v4l2-ctl` 列出的 H.264/HEVC 最高 level；PoC-3b：缺參考幀時 iris 是輸出帶 `V4L2_BUF_FLAG_ERROR` 的幀、卡住、還是輸出空白，以及 FFmpeg 如何處理；PoC-3c；PoC-4）。
  - **Day 2**：PoC-5b（含劫持情境）、PoC-8。
- **新增的 PoC 定義**：
  - **PoC-2b**：見 §2.5。
  - **PoC-10**（<host>，不需實機）：SteamVR 是否依 `VsyncEvent` 相位排程，以及 `PostPresent` 節流的效果。取代草案中未定義的 PoC-S2。

**關卡與替代路徑**

| 關卡 | 通過條件 | 失敗時 |
|---|---|---|
| **G-BUILD**（M2a） | 量到 qemu 的乾淨建置和增量建置時間；增量 ≤ 45 分鐘（release flavor，開 LTO） | 停下交使用者決定 U7 採購 |
| **G-α** | log 顯示 `decoder=hevc_v4l2m2m fmt=DRM_PRIME frontend=PlVk isGpuSlow=0 fullscreenFlag=<預期> matchVideo=<預期>`；1080p60、1440p60（以及 HMD Hz）各跑 15 分鐘：≥ 目標 fps×0.99、解碼 p95 ≤ 10 ms、stutter < 5%；overlay 內 UI、滑鼠、鍵盤可用（F14 視 PoC-7）；`--help` 兩次 rc=0 | 沒有 `/dev/video*`：先換 P2 → 再不行出 α-lite（不切 2.0.0）。v4l2m2m 異常：L1 提前。overlay 太糊：把 β 提前 |
| **G-PKG** | P1 沙箱內能拿到 `/dev/video*`，而且 OpenXR runtime 能載入 | β/rc 改 P2 |
| **G-β** | PoC-2b 定出啟動形態；XR session 進入 FOCUSED；frame loop miss < 1%；沒有 Wayland 時走 offscreen，decoder 經由 XR_READY 建立；XR 失敗注入依不變式 5 處理；1440p 文字可讀；指標延遲 ≤ 1 個 display frame | 沒有 `vulkan_enable2`：改 `enable`。dmabuf 匯入失敗：泛化 importer，或走 L3。OpenXR 不可用：IVROverlay |
| **G-DRV**（<host>） | DirectMode 取幀正確；1 小時無 Desync；poseFallback ≤ 1%；`GetFrameTiming` 下沒有半速；**空間驗證**：vr_probe 分別用 Seated、Standing、RawAndUncalibrated 取 pose，涵蓋 Reset seated position 和 PC 上已有 lighthouse chaperone 的情境，space-delta 修正後偏差 < 0.5°；**kill-server 注入**：SteamVR 在 1 秒內回到 standby；重啟 20 次 | 改 VirtualDisplay 的 FrameSource：derivedPose 門檻另訂，poseFallback 不計入 |
| **G-rc** | SBS 3456×1728@90 HEVC（PoC-3 確認 level 5.2）：解碼 p99 < 11 ms。內容 MTP：暫定 p50 ≤ 55、p95 ≤ 65（M1b 依實測重定）。`transport=rtp`；pose 丟失 ≤ 0.5%；poseFallback ≤ 1%（DirectMode）；`vr_probe` 0 不一致（含 decoder 丟幀注入）；latch dup/skip < 0.5%。**恢復**依 PoC-3b 的結果：(i) iris 容錯 → 1% 注入丟包 30 s 內，凍結 ≤ 2 幀、完全癒合 ≤ N+2 幀、0 次 IDR；(ii) iris 卡住但 flush 後可以從 recovery point 恢復 → 恢復 ≤ 1 個 IR 週期；(iii) 必須靠 IDR → 凍結 ≤ 300 ms + 1 RTT，由 stale 政策遮蔽，並把 L1 提前。**500 ms 斷線注入**：沒有凍結的 projection（淡出或 loading 環境），恢復時淡入。1 小時 soak；PoC-5b 通過 | 解碼跟不上：每眼降到 1440² 或改 72 Hz，再不行上 AADT。延遲超標：L1、pacer 提前。vrlink 衝突：D8b 已是預設 |
| **GA** | 90 Hz MTP p50 ≤ 45、p95 ≤ 55（M1b 重定）；120 Hz p50 ≤ 40；latch miss ≤ 0.5%；WAN（Tailscale，RTT 20–30 ms）15 分鐘：p95 ≤ LAN p95 + RTT + 10 ms、沒有 IDR 造成的 >100 ms 卡頓、`jb-overflow=0`；relay 加 PCVR 可用；tc 注入時 ABR 5 s 內收斂、零 IDR；photon 校準誤差 ≤ 5 ms | pacer 沒有收益（PoC-10 或實測）：預設關閉，GA 門檻退回 G-rc 值 |

- **WAN 閘門**：RTT ≤ 25 ms 直接允許；25–60 ms 允許並提示；> 60 ms 時 PCVR 預設停用。

---

## 8. 驗證計畫

### 8.1 無實機時的模擬環境（每一個都有腳本路徑）

| 代號 | 環境與建置 | 驗證內容 |
|---|---|---|
| S0 `vr-emulate` | <dev-client> Windows client，用 **`build_moonlight.cmd --no-bump --mpquic`** 建置；<host> 跑 SteamVR、driver、`vr_probe`（`VipleStream-VrProbe-dev.zip`，由 <dev-client> scp 到 host，登記成 `apps.json` 的 vr 類 app，由編排以使用者 token 啟動） | server、driver、協定全路徑；echo；pose-tag；**人為讓 ENet 失效，確認 0x5506 改走 QUIC flow 0x04，F3 生效**；decoder 丟幀注入；kill server 注入 |
| S1 Monado | linux-builder 跑 `monado-service`（容器或 Flatpak 隔離）；client 用 **`build-steamframe.sh --arch x86_64 --flavor dev`** | 和 Frame 相同的 XR 程式碼路徑；VAAPI DRM_PRIME 走 `pl_map_avframe_ex`；stale 和 loading 環境 |
| S2 SteamVR null driver | <dev-client>，本機 **`build_moonlight.cmd --openxr`** | XR 桌面的輸入和 layer（軟體解碼） |
| S3 gamescope openvr | linux-builder 上 SteamVR Linux null driver（隔離）加 `gamescope --backend openvr` | α overlay、PoC-F-pre、PoC-2b 的前置 |
| S4 vicodec | `modprobe vicodec` | 只在 L1 觸發時使用 |
| S5 ARM | qemu 或 arm64 builder | arm64 runtime 依賴 |
| 相容主機 | **vanilla Sunshine 放在 <dev-client> 的 VM，或 linux-builder 的容器**（M1a 前置） | 相容矩陣 |

- 需要啟動 SteamVR 的測試（重啟 20 次、vr-smoke），做成 server CLI `--vr-selftest`：CLI 經本機 admin API 請執行中的 server 代為執行，由 server 以使用者 token 啟動。**不新增需要特權的 host op**，也不從 SSH 的 Session 0 啟動 SteamVR。

### 8.2 延遲預算（90 Hz、LAN Wi-Fi 6E、HEVC SBS 3456×1728、約 200 Mbps）

| # | 階段 | rc p50 | GA p50 | p95 | 實測（M1b 填） | 量測（log tag） |
|---|---|---|---|---|---|---|
| 1 | 取樣、預測、送出 | 0.2 | 0.2 | 0.5 | | `[VIPLE-VR-POSE]` |
| 2 | 上行 | 1.5 | 1.5 | 4 | | `[VIPLE-VR-POSE-RX]`、`[VIPLE-VR-CLK]` |
| 3 | server 收到 → `PoseUpdated` | 0.1 | 0.1 | 0.3 | 3＋4＋5 合計 p50 8.5／p95 24.1（`echoAge`，SteamVR Home、vr-emulate） | `[VIPLE-VR-DRV]` |
| 4 | 等遊戲取 pose | 1.0（b5 已在 rc） | 1.0 | — | | echo 年齡 |
| 5 | 遊戲 render 加 vrcompositor | 11.1（**ALVR 實務 `steamvr_pipeline_frames=2.1`，4+5 可能約 23 ms，多約 5 ms**） | 11.1 | — | | `vr_probe`：WaitGetPoses→Present |
| 6 | driver 合成 SBS | 0.6 | 0.6 | 1.0 | 6＋7＋8＋首封包 p50 4.41／p95 4.70（`presentToFirstPkt`） | GPU timestamp |
| 7 | IPC、fence 確認、copy | 0.4 | 0.4 | 0.8 | evtToPush p95 1.04（presentToPush p50 1.05） | `[VIPLE-VR-CAP]` |
| 8 | NVENC | 3.5 | 3.5 | 5.0 | | `[VIPLE-NVENC-PROF]` |
| 9 | packetize、FEC、送出窗 | 2.5 | 2.0 | 4.0 | | `[VIPLE-VR-TX]` |
| 10 | 下行 | 1.5 | 1.5 | 4.0 | | CLIENT_TIMING |
| 11 | FEC 重組 → 佇列 | 0.3 | 0.3 | 1.0 | | — |
| 12 | V4L2 解碼 | 4.0 | 4.0 | 6.0 | | `[VIPLE-V4L2]` |
| 13 | 等 latch | 3.0（頻率加相位回授的 margin） | 1.0（late-latch） | — | | `[VIPLE-VR-PACER]` slack |
| 14 | client 轉色 | 0.8 | 0.8 | 1.2 | | GPU timestamp |
| 15 | runtime 合成、scanout、面板 | ≈13 | ≈13 | ≈14 | | 平台決定 |
| | **內容 MTP** | **≈43（+5 待測）** | **≈41** | 門檻見 §7 | | `[VIPLE-VR-MTP10]` |

- 旋轉延遲由機上 ATW 決定。**G-rc 和 GA 的門檻在 M1b 結束時，依第 4、5 項的實測值加上 client 段的估計重定**，避免誤觸 fallback。
- **M1b V6 實測與重定（2026-09-29，`<host>`、S0 60 分鐘、SteamVR Home、vr-emulate sine，待使用者確認後生效）**：
  server 段＝`echoAge`（樣本發布 → app Present，第 3＋4＋5 項）p50 8.46 ms ＋ `presentToFirstPkt`（第 6–8 項＋首封包）p50 4.41 ms
  ＝ 12.9 ms，第 9 項剩餘部分沒有量測、沿用預算 2.5 ms → 15.4 ms。套 §F.9 公式（client 段 24.3 ms、餘裕 3 ms）：
  **G-rc p50 ≤ 43 ms、p95 ≤ 53 ms；GA p50 ≤ 33 ms、p95 ≤ 43 ms**；**Present→首封包 p50 ≤ 5.5 ms、p95 ≤ 6 ms**（實測＋1 ms）。
  限制：第 4＋5 項是輕負載的 SteamVR Home（p95 24 ms 來自 app 取 pose 到 Present 之間的一幀延遲），重負載遊戲
  （ALVR 實務 `steamvr_pipeline_frames=2.1`）會再多約 5–11 ms；G-rc 前要以實際遊戲重量一次 `echoAge`，超過就回到暫定值 55／65。
- **頻寬**：追蹤上行約 0.42 Mbps；視訊 150–300 Mbps 加 FEC。

### 8.3 各里程碑的驗法

| M | harness | log tag | 取證 | 門檻 |
|---|---|---|---|---|
| M0 | `stream-quality`、`network`；Pixel 5；Linux server KMS | 既有 | SSH grep | 不比 baseline 差 |
| M1a/b | `vr-emulate`：`VipleStream.exe stream <host> "SteamVR Home" --display-target pcvr --vr-emulate --vr-synthetic-motion sine`；`--vr-selftest`（SteamVR 重啟 20 次）；server 服務重啟；ENet 重連；相容矩陣（含 Linux server、vanilla VM） | `[VIPLE-VR-SESSION]`、`-POSE`、`-POSE-RX`、`-CLK`、`-IPC`、`-DRV`、`-ORCH`、`-CAP`、`-ENC`、`-TX`、`-FRAME` | **SSH 對完整 log 做 grep**（ORCH 狀態轉換、heartbeat、`(final)`、`idr reason`、`deploy-refused`、`meta-miss` 這些稀疏 tag 必須全檔 grep），加 SteamVR log | 15 分鐘：到達間隔 p99 ≤ 2T、丟失 ≤ 0.5%（**含 ENet 重連後**）、亂序 ≤ 0.1%；echo ≥ 99%；vr_probe 0 不一致；Present→首封包 p95 ≤ 6 ms；copy p95 ≤ 1.5 ms；1 小時不 crash；重連 ≤ 3 s；不進 safe mode；kill server 後 SteamVR 1 秒內回到 standby；LOSS→REFRESH 往返 ≤ 2 RTT；vanilla、舊版、Linux server 全數正常 |
| M2 | `frame-poc-collect.sh`、`v4l2-probe`、`decode-bench`、`analyze_client_log.ps1` | `[VIPLE-V4L2]`、`-PROBE`、`[VIPLE-XR-PROBE]`、`[VIPLE-SF-ENV]` | <dev-client> 經 SSH 拉回 | G-α |
| M3 | S1、S2 自動化；XR 失敗注入 | `[VIPLE-XR]` | — | G-β |
| M4 | S1 對 <host>；實機 `vr-hmd`；本機 `analyze_vr_log.ps1` | `[VIPLE-VR-MTP10]`、`-FRAME`、`-REPROJ`、`-HAPTIC`、`-TX transport=` | 全檔 grep 加 `steamvr` | G-rc |
| M5 | Tailscale；clumsy；tc netem；240 fps 攝影 | `[VIPLE-ABR]`、`-PACER`、`-FEC`（含 `jb-overflow`）、`[VIPLE-MPQUIC]` | network 加全檔 grep | GA |

- 每一段開工前先量 baseline。
- 10 秒彙總行保持短格式，健康路徑也要有 heartbeat，兩端都輸出 `(final)`。
- 每輪結果寫進 `scripts/benchmark/results/vr-round-<N>/`（本機，`summary.json` + `report.md`）。

---

## 9. 風險、緩解、待決事項

### 9.1 風險

| # | 風險 | 緩解 |
|---|---|---|
| R1 | VPU 不開放，或能力、延遲、level 不足 | PoC-0/1/3（含 level）；L1–L4；α-lite；72 Hz 或 1440² |
| R2 | 沙箱內 OpenXR runtime 載入失敗 | PoC-F-pre；P2 spike；runtime 自動探測加 Settings 欄位；IVROverlay |
| R3 | SteamVR 升版打斷 driver；safe mode | pin header、vr-smoke、FrameSource 可切換、每版只自動解除一次、自動 self-check、註銷路徑 |
| R4 | vrlink 共存或劫持 | **D8b 預設**、衝突偵測（`VRLINK_ACTIVE`）、PoC-5a/5b |
| R5 | SYSTEM ↔ medium IL 的 IPC 被搶名或冒充；GPU 資源互搶 | FIRST_PIPE_INSTANCE、GUID 命名、雙向身分驗證、欄位驗證、log 過濾、使用者 token 寫入、行程層級優先權 |
| R6 | 建置基底 | P1 由 runtime 提供；G-BUILD |
| R7 | 三份 common-c 漂移 | F1（含 enet）、`VipleVr.h` 單一來源、同步檢查 |
| R8 | α 的 720p 畫質 | PoC-7；β 提前 |
| R9 | 和 Valve 產品重疊 | 差異化放在 WAN 和統一 client |
| R10 | 音訊預設裝置互搶 | driver 指定同一個 endpoint |
| R11 | FFmpeg 9.0 的 API 移除 | 在 S1 先編一輪（例如 `lock_queue`）；不行就 cherry-pick 到 n8.0.1 |
| R12 | render pose 和空間不一致 | pose 空間換算加 space-delta、STAGE/LOCAL_FLOOR、epoch 帶上 eyeToHead、space-mismatch 偵測、G-DRV 空間矩陣 |
| R13 | SteamVR 不照 VsyncEvent 相位排程 | rc 先用 PostPresent 節流加頻率鎖；GA 的相位控制以 PoC-10 為關卡 |
| R14 | pacer 和 ABR 振盪 | 時間常數分開；每步調整有上限 |
| R15 | ENet throttle 丟 tracking | F5（含重連） |
| R16 | 眼動資料 | PoC-8；QP 中心取光軸 |
| R17 | 上游佔用 ptype 或改 appversion | 只在協商成功時才使用 |
| R18 | 自我更新被 driver 卡住 | driver 放在 `config\steamvr\`（self-update 跳過）加完整性檢查 |
| **R19** | iris 對破損參考幀的行為讓 intra 恢復無效 | PoC-3b；G-rc 三種恢復門檻形式；L1 可控錯誤處理 |
| **R20** | qemu 建置太慢，拖累 release | G-BUILD；U7 提前；builder 失效規則 |
| **R21** | VR 改動讓 Linux server 或桌面路徑回歸 | stub 加不變式 10；`vrFlags==0` baseline；default member initializer |
| **R22** | 發佈流程版號漂移或自動發佈 | F19：set 冪等、`--no-bump`、版號斷言、人工閘門 |
| **R23** | settings guard 還原被 SteamVR 寫回覆蓋 | 只在 vrserver 沒有執行時還原；啟動或登入時補還原 |

### 9.2 待決事項

| 項目 | 目前狀態 |
|---|---|
| D2 桌面呈現 | α overlay，β quad/cylinder；啟動形態看 PoC-2b |
| D3 解碼 | L2 先上；L1 的觸發條件見 §2.3 |
| D5 每眼解析度、overscan、foveation | 等 PoC-3（含 level） |
| D8 vrlink | **預設 b**；PoC-5a/5b 通過才降級成 a |
| D12 打包 | α 用 P1；β/rc 等 G-PKG |
| U3 WAN 目標 RTT | ≤ 25 允許；> 60 預設停用 |
| **U6 app-id 與發佈管道** | **已定案（2026-09-27）：`io.github.finaltwinsen.VipleStream`**。之後不可再改（`~/.var/app/<id>` 的設定和 log 會斷掉）。發佈管道：GitHub release 附 `.flatpak` bundle（§6）；目前不走 Flathub（manifest 關閉 `appstream-compose`，要上架時再開） |
| U7 ARM builder | 依 G-BUILD 的結果 |
| U8 麥克風 | 0x5507/7F 保留 |
| U9 | Valve 方案能不能在 Frame 看完整 Windows 桌面 |
| C15（critic） | Frame 帳號（steamos 或 steamvr），影響 `/dev/video*` 權限和 Flatpak 安裝範圍 |
| `vrDegradedPolicy` 預設值 | G-rc A/B |
| 其他 | 控制器預設 Touch 或 Index；`steam://launch/<id>/VR` 和 vrmanifest 的行為；`loadPriority`；Frame 能不能對我們的 app 關閉 motion smoothing；β 是否隱藏 server 游標；T9；arm64 自我更新；VR 設定放在 Web UI 哪一頁；sniper SDK；Frame 桌面模式（非 VR）是否也改用 intra 恢復（2.0 之後） |

---

## 附錄 A：審查意見處理紀錄

> 註：CoworkMCP 已於 2026-09-23 停用。下表提到的 relay（產物中轉）、run-script、collect、黑板、任務圖等做法，一律改為 SSH／scp，見 §6、§8；第 55 項（F19）取消。

| # | critic | severity | 處理方式 |
|---|---|---|---|
| 1 | code-facts | blocker | 採納，並和第 26 項合併成 VR 恢復協定（§2.3、§3.4、§4.5、§4.8、G-rc）。**部分不採納**：`SdpGenerator.c` 不改，因為 server 在 vrProfile 下強制 IR，不依賴 SDP（審查意見本身也要求「不要依賴 client 的 SDP」），RTSP/SDP 維持不動 |
| 2 | code-facts | major | 採納：覆寫 is_hdr、get_hdr_metadata、自訂 init，加單元測試（§3.3，C18） |
| 3 | code-facts | major | 採納：default member initializer、參數往下傳、VR 略過 HDR/YUV444（C9、§3.3、F15） |
| 4 | code-facts | major | 採納：common.cmake 加 platf 介面、Linux stub、M1a 完成條件（§3.11、不變式 10） |
| 5 | code-facts | major | 採納：F6 擴大成 Linux renderer 決策、G-α log 條件、env 標成 dev-only（§2.3、F6） |
| 6 | code-facts | major | 採納：testFrameOnly 欄位、實際跑 `pl_map` 的 testRenderFrame、自有 texture 複本（§2.4） |
| 7 | code-facts | major | 採納：新的拆除順序、vrMessage 包成 SDL_USEREVENT（§2.4、§1.3） |
| 8 | code-facts | major | 採納：只在 gamescope 下啟用、去重、PoC-7 之後才合併、不隨 1.5.x 全平台發佈（§2.7、F14） |
| 9 | code-facts | major | 採納：單人序列化加雙人版本、重排為 M1a → M2a → M1b。工時依新增範圍重算為 37–48 週（§7） |
| 10 | code-facts | major | 採納：PoC-2b、三種啟動形態、XR 模式忽略 gamescope 滑鼠事件（§1.1、§2.5） |
| 11 | code-facts | minor | 採納：throttle helper 放在首次連線和重連（§4.3、F5） |
| 12 | code-facts | minor | 採納：偽 typeIndex、union buffer、assert 修改（§4.5） |
| 13 | code-facts | minor | 採納：C10 改寫，並記錄 0x5502 例外（C10、不變式 3） |
| 14 | code-facts | minor | 採納：C1 簡化成只改旗標，C2 補上 `rife_native_vk.cpp` |
| 15 | code-facts | minor | 採納：`common-c.pro` 的 `PICOQUIC_DIR`、git source pin SHA（§2.1） |
| 16 | code-facts | minor | 採納：行程層級優先權由 vr_session 管理（C23、§3.4） |
| 17 | code-facts | minor | 採納：PoC-3 加 level、H.264 改 72 Hz 或 1440²（C27、§3.4） |
| 18 | code-facts | minor | 採納：改成十進位定點數，和 vrFov 一致，加 round-trip 測試（§4.2） |
| 19 | code-facts | minor | 採納：措辭修正、列出 finish-args、0004/0005 歸屬、R11 聚焦 API 移除；另查證我們 fork 不讀 `IGNORE_RFI_LATENCY_BUG`，所以不帶（C30、§2.2） |
| 20 | code-facts | minor | 採納：F1 和同步檢查加入 `enet/`（F1、§4.8） |
| 21 | code-facts | minor | 採納：placebo 加上由狀態機決定的 `running()`（§3.6） |
| 22 | code-facts | minor | 採納第二個選項：兩個設定分工（frontend 對 decoder cascade），Frame 預設組合寫進 G-α。不擴充 rendererSelection 的語意，因為 RS_VULKAN 已經代表 Vulkan 解碼，混用會讓語意模糊（§2.3） |
| 23 | code-facts | minor | 採納：`Limelight-internal.h` include `VipleVr.h`（§4.8） |
| 24 | code-facts | minor | 採納：GA 時 buffer ≥ 1024 格，加 `jb-overflow` 計數（§3.5） |
| 25 | code-facts | minor | 採納：不變式 5 改寫、`opaque_ref` 掛載加 `if` 閘門、baseline（§1.4、§2.4） |
| 26 | vr-pipeline | blocker | 採納（和第 1 項合併）：LOSS、forced IR、REFRESH_START 加 b7、VR cooldown 300 ms、PoC-3b、G-rc 三種門檻形式 |
| 27 | vr-pipeline | blocker | 採納：`m_Pkt->pts=frameNumber`、`pkt_timebase`、查表、meta-miss 時丟幀、注入測試。**第 5 點（桌面也改）不在 2.0 採納**：不變式 5 要求 `vrFlags==0` 時行為不變，列為 2.0 之後的項目（§2.3） |
| 28 | vr-pipeline | major | 採納：`startConnectionAsync` 前段 bring-up、fallback 分兩段、寫進不變式 5（C22、§2.4） |
| 29 | vr-pipeline | major | 採納：server 端 offset 對映、poseTimeOffset、T_sv、HMD 和控制器分開設上限、b5 提前到 rc（§3.1、§3.5、§4.3） |
| 30 | vr-pipeline | major | 採納：G-DRV 空間矩陣、UniverseId、space-delta 修正、client space-mismatch 偵測（§2.6、§3.1） |
| 31 | vr-pipeline | major | 採納：API 更正、rc 做頻率鎖加 LATCH 10 Hz、GetFrameTiming 清旗標、GA 做相位（C24、§3.1） |
| 32 | vr-pipeline | major | 採納：raw_from_grip、skeleton、system 組合鍵、非 FOCUSED 時全放開加 OutOfRange、每鍵 2 bit 計數、proximity（§2.7、§3.1、§4.3） |
| 33 | vr-pipeline | major | 採納：CPU 端 fence 檢查、只用可用 slot、跳格時一次 Signal、重建、kill 注入（§1.3、§3.2、不變式 8） |
| 34 | vr-pipeline | major | 採納：D8b 為預設，D8a 要 PoC 證明才降級；另外補上「只在 vrserver 沒有執行時才還原」（§3.8、R23） |
| 35 | vr-pipeline | major | 採納第一個選項：rc 的 VR 視訊強制走 RTP，relay 回明確錯誤，log 記錄 transport。不選「M4 提前做 jitter bypass」，因為強制 RTP 的改動較少，QuicTransport 留給 GA 一次處理（§3.5） |
| 36 | vr-pipeline | major | 採納：STAGE → LOCAL_FLOOR → LOCAL 加身高；seated 只在 SteamVR 端處理（§2.6） |
| 37 | vr-pipeline | major | 採納：XR_READY 觸發建 decoder、SDL offscreen 用 SetHint、PoC 測有沒有 WAYLAND_DISPLAY（§1.1、§2.4） |
| 38 | vr-pipeline | major | 採納：PoC-2 必查、frameloop 模式、mode log（§2.4、§4.6） |
| 39 | vr-pipeline | major | 採納：100/250 ms stale 政策、`poseValid=0` 不進 projection、淡入、500 ms 注入（§2.6、G-rc） |
| 40 | vr-pipeline | minor | 採納：預算表加實測欄和 ALVR 註記，M1b 依實測重定門檻（§8.2） |
| 41 | vr-pipeline | minor | 採納：driver stale 行為（§3.1 tracking） |
| 42 | vr-pipeline | minor | 採納：補上 `xrBeginFrame` 和它的 lock（§1.1、§1.3） |
| 43 | vr-pipeline | minor | 採納：timeline semaphore、buffer 數公式（§2.6-9、§2.3） |
| 44 | vr-pipeline | minor | 採納：協商時驗證寬度與 level、QP 中心取光軸、overscan 自動調整（§2.6、§3.4） |
| 45 | vr-pipeline | minor | 採納：以 scene pose 為準加旋轉補償、依格式處理色彩、灰階色帶比對（§3.1、§2.6-11） |
| 46 | vr-pipeline | minor | 採納：`ChaperoneVisibility=false`（§2.6、§3.1） |
| 47 | vr-pipeline | minor | 採納：derivedPose 旗標（佔用原本 SBS 的 b5）、依 FrameSource 分開門檻、DisplayRedirect（§3.1、§4.4、G-DRV） |
| 48 | vr-pipeline | minor | 採納：eyeToHead 納入 layoutEpoch 和 LAYOUT（§2.6-5、§4.5） |
| 49 | vr-pipeline | minor | 採納：`vrlaunchparams` 共用、相容矩陣加 relay 那一列（§2.4、§4.7） |
| 50 | vr-pipeline | minor | 採納：power_save 併入 PoC-6、imageRect 併入 PoC-2；PoC-S2 以 PoC-10 定義取代，RL1 的引用刪除（§7、R13） |
| 51 | ops-compliance | blocker | 採納首選：改放 `<install>\config\steamvr\<ver>\`，加上 owner/DACL/reparse/hash 檢查、fail closed，寫進不變式 6（C25、§3.10） |
| 52 | ops-compliance | major | 採納：a/b 拆段、關卡不放在依賴鏈上、單人和雙人工期、F8/F12 只留在 M2a（§7） |
| 53 | ops-compliance | major | 採納（和第 4 項合併）：stub、b1=0、Linux baseline 和 `.deb` 納入完成條件（§3.11、不變式 10） |
| 54 | ops-compliance | major | 採納：2.0.0 起六件強制、刪掉 M5 的重複說法、builder 失效規則、同步修改本機 CLAUDE.md（§6） |
| 55 | ops-compliance | major | 採納：F19（set 冪等、`--no-bump`、版號斷言、真實 worker 設定、dry-run）。真實設定因 coworkmcp 連線失敗未能確認，列為待查（C28、F19） |
| 56 | ops-compliance | major | 兩個做法都採納：propagate 產生 `driver_version.h`，Stage 再比對 FileVersion（§6、F10） |
| 57 | ops-compliance | major | 採納：`build-steamframe.sh --arch/--flavor`、`build_moonlight --openxr`、「沒有腳本路徑不能當證據」（§2.1、§8.1） |
| 58 | ops-compliance | major | 採納：qmake 注入 desktop id、smoke test（含 qemu）、U6 提前拍板（§2.4、§9.2） |
| 59 | ops-compliance | major | 採納：FIRST_PIPE_INSTANCE、GUID 命名、雙向驗證、欄位驗證、log 過濾、使用者 token 還原（§1.3、§3.2、不變式 9） |
| 60 | ops-compliance | major | 採納：三條註銷路徑、ABI 不一致狀態、單一權威部署路徑（§3.8、§3.10） |
| 61 | ops-compliance | major | 採納：`vr` 類 app 只出現在 `/applist?vr=1`、未帶 `vr=1` 時明確報錯、IsVr 只是提示並可平面啟動（§3.6、§4.7、§2.8） |
| 62 | ops-compliance | major | 採納大部分：vendor `openvr.h`/`openvr_api`、`VrProbe-dev.zip` 走 relay、apps.json 登記、`--vr-selftest`、vanilla VM。**部分不採納「CMake 獨立 target」**：vr_probe 改由 `Build-SteamVRDriver.ps1 -Target probe`（MSVC）建置，因為 `openvr_api` 是 MSVC 產物，而且和 driver 的工具鏈一致（D7）；server 的 MinGW/CMake 不受影響（§3.1、§8.1） |
| 63 | ops-compliance | major | 採納：M0 的 G-BUILD、ccache、lockhash 快取、隔離安裝、relay 加 git apply（§2.2、§6） |
| 64 | ops-compliance | minor | 採納：共用防護函式、>900 警告、`versioning.md`、測試腳本標成本機限定（F10） |
| 65 | ops-compliance | minor | 採納：切版後重跑縮短版 G-α、α-lite 不切 2.0.0、安裝範圍等 PoC-1（§6） |
| 66 | ops-compliance | minor | 採納：腳本不碰網路、<dev-client> 拉回、`<host>` 佔位、Day 0 措辭（§6、§7） |
| 67 | ops-compliance | minor | 採納：人工核准閘門、「備妥候選交使用者決定」、F0 的 UAC 條件（F0、F19、不變式 11） |
| 68 | ops-compliance | minor | 採納：LICENSE-OpenVR、THIRD_PARTY_NOTICES、`/MT` 加 dumpbin、Flatpak licenses（§6、§2.2） |
| 69 | ops-compliance | minor | 採納：自動探測加 Settings 欄位、CLI 只給開發用、nested 解析度由 Settings 控制（§2.2） |
| 70 | ops-compliance | minor | 採納：預設拒絕、`VRLINK_ACTIVE`、client 確認後帶 force、只在沒有 HMD 使用時才自動重啟（§3.6） |
| 71 | ops-compliance | minor | 採納：run-script 全檔 grep、標明哪些 tag 必須全檔 grep（§6、§8.3） |
| 72 | ops-compliance | minor | 採納：參數格式同第 18 項；摘要改成「2 個元素」（摘要 7、§4.1） |
| 73 | ops-compliance | minor | 採納：S0 用 `--mpquic` 建置，並測 ENet 失效時改走 QUIC 加 F3（§8.1、F18） |

---
