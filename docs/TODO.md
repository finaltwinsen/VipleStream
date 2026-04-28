# VipleStream — 待辦 / 技術債清單

這份文件記錄**明確知道之後要做、但目前刻意沒做**的事。每一條都要
寫清楚「為什麼先這樣保留 / 何時該清 / 清的時候要做什麼遷移」，不
要只寫「以後改」。

---

## 已完成（保留紀錄，後續維護用）

落實在 git history。新增條目時請填 commit hash 或版號，方便交叉對照。

| 原條號 | 內容 | 落實版本 | 備註 |
|---|---|---|---|
| A.4 | Windows service `SunshineService` → `VipleStreamServer` | v1.2.93–95 | `entry_handler.cpp` 加 try-new-fallback-old；`deploy_sunshine.ps1` 自動 stop+delete 舊、注冊新；安裝路徑同步移到 `C:\Program Files\VipleStream-Server`。 |
| A.9 | Binary 檔名 `Moonlight.exe` / `sunshine.exe` / `sunshinesvc.exe` → `VipleStream.exe` / `viplestream-server.exe` / `viplestream-svc.exe` | v1.2.94–95 | 用 CMake `OUTPUT_NAME` 改 binary 而不改 target 名（避免 ripple）；`tools/sunshinesvc.cpp` 寫死 spawn `Sunshine.exe` 的字串也改了；svc 必須住在 `tools/` 子目錄（C++ code 從 module path 反推 install dir 用 strip-2 邏輯）。 |
| A.10 | Linux desktop file ID `com.moonlight_stream.Moonlight` → `com.piinsta` | v1.2.93 | `.desktop` / `.appdata.xml` 檔名跟內容、`main.cpp` 的 `setDesktopFileName` + WMCLASS env、`app.pro` 的 install paths 都同步改。 |
| 新 | Android applicationId `com.limelight.unofficial` → `com.piinsta` | v1.2.93 | Java `namespace 'com.limelight'` 保留作內部命名空間（避免每個 .java 檔的 `package` 宣告全改）。是 self-host 部署不上 Play Store 所以 update continuity 不重要。 |
| 新 | mDNS 內部 instance name fallback 從 `"Sunshine"` 改成 `"VipleStream"` | v1.2.93 | Service type `_nvstream._tcp` 仍維持（GameStream 標準，wire-compat）。 |
| 新 | 防火牆 rule 名稱 `Sunshine` → `VipleStream-Server` | v1.2.93 | 連帶 `add-firewall-rule.bat` / `delete-firewall-rule.bat`。 |
| 新 | NVAPI per-app profile 名稱 + binary 路徑 | v1.2.93 | `SunshineStream` / `sunshine.exe` → `VipleStream-Server` / `viplestream-server.exe`（NVAPI 用實際 binary 檔名比對）。 |
| 新 | `<VipleStreamProtocol>` capability marker 加進 `/serverinfo` XML | v1.2.93 | Server 永遠 emit 但**不擋任何請求**；vanilla Moonlight 忽略未知 tag。Qt + Android client 解析後存 `isVipleStreamPeer` 旗標，UI 用來決定是否顯示 VipleStream-only 入口。**badge UI 還沒實作**（見下面 §H.5）。 |
| 新 | Service helper log 檔名 `sunshine.log` → `viplestream.log` | v1.2.95 | `tools/sunshinesvc.cpp:124`。寫到 `%SYSTEMROOT%\Temp\viplestream.log`。 |
| H.Phase 1+2+3 | Steam library 自動匯入 + applist 多送 5 個 metadata XML tags + multi-mode sort | v1.2.67 + v1.2.96 | applist 端 emit `<Source>` / `<SteamAppId>` / `<SteamOwners>` / `<LastPlayed>` / `<Playtime>`（空值省略）；client 端 NvApp 補對應欄位 + comparator 把 `source==""` pin 到頂；Sort 模式 DEFAULT/RECENT/PLAYTIME/NAME 三端（Qt + Android）。 |
| **H.5** | **`<VipleStreamProtocol>` capability marker UI badge** | **v1.2.105** | Qt PcView (`PcView.qml:391-411`) 跟 Android PcGridAdapter (`PcGridAdapter.java:98-101` + `pc_grid_item.xml:44-60` + `vs_viple_badge_bg.xml`) 都掛了 lime 邊 + ink3 底的 `V` chip，`visible: isVipleStreamPeer`。資料層走 QAbstractListModel role (`computermodel` 的 `IsVipleStreamPeerRole`) 不需 Q\_PROPERTY。回頭整理 TODO 才發現是 v1.2.105 落實的，當時沒同步移到完成欄位。 |
| **B** | **Discord Rich Presence App ID 獨立化** | **v1.2.123** | 註冊 VipleStream Discord App (`1497869749244268545`)，replace 上游 Moonlight 的 `594668102021677159`。順便重排 Rich Presence layout：遊戲名搬到 `details`（顯示更顯眼）、`state` 永遠是 `via VipleStream`、加 `largeImageText="VipleStream"` icon hover tooltip。`SettingsView.qml` tooltip 移除「currently shown as 'Moonlight'」caveat。1024×1024 icon 從 `assets/icon/viplestream_icon.svg` 用 `render_icon.mjs` 一次生成（順便為 Discord asset 上傳產出 `assets/icon/dist/discord_icon_1024.png`）。 |
| **G.2** | **FP16 ONNX model variant + cascade rewrite** | **v1.2.124–127** | RIFE 4.25 lite native fp16 export (PyTorch `model.half()` + boundary fp32 I/O 給 C++ binding 0 改動)，改寫 cascade 從「resolution 降解」改成「同 res 試 fp16→fp32→Generic 三層 model 變體」。一路解掉 4 個 sub-bug：(a) v1.2.61 onnxconverter_common 結構性炸 → PyTorch native 重 export；(b) `convert_float_to_float16` 改 op_block_list 留下 Cast value_info 不一致 → 全清重 infer；(c) probe 沒偵測 ORT silently disable → 加 `wantedOrtAtEntry` snapshot；(d) Concat_12 shape mismatch (1080 非 128 倍數) → wrapper 內部 `F.pad` to 128 multiple + crop output。A1000 實測 fp16=132 ms / fp32=107 ms / 都超 14 ms budget → fall back Generic（A1000 launch overhead 25 ms floor 不可繞過）。RTX 30/40+ 預期 fp16 反而會贏 fp32 因為 Tensor Core 加速。`VIPLE_FRUC_MODEL=fp16\|fp32\|auto` env var override。Bundle 兩個 onnx (~33 MB total)。 |
| **G.3** | **嘗試其他架構（IFRNet-S）** | **v1.2.128** | Negative result 紀錄：IFRNet-S 雖然 params 最少（2.4M vs RIFE 9.5M），op count 卻最多（802 vs RIFE 456），A1000 上跑 1080p 680 ms（比 RIFE fp32 慢 3.8×）。原因：IFRNet 的 `embt.repeat(1,1,h,w)` 加上 PyTorch 2.6 trace 把動態 shape ops 展開成 Range×40 / Unsqueeze×62 / Expand×56，加上 PReLU 比 LeakyReLU 慢。**確認 A1000 是 launch-overhead-bound，「params 少 ≠ 快」、op count 才是關鍵指標**，因此 §G.3 走「換小模型贏 RIFE」這條路不通。Export script (`scripts/export_ifrnet_s.py`)、ONNX (`tools/fruc_ifrnet_s.onnx`)、bench harness 都進 repo 當未來其他 GPU tier 的 reference 留著但不 ship 進 cascade。 |
| **H.4** | **Steam 帳號切換 — server async + Qt + Android 全端** | **v1.2.108–122** | Server: `/steamprofiles` + 異步 `/steamswitch` (回 202+task_id) + `/steamswitch/status?id=X`；force-kill straggler steam.exe + Steam Guard 2FA 偵測；`current_user` 走 HKEY_USERS walk（修 v1.2.117 之前 HKCU=.DEFAULT 的 root cause — `CreateProcessAsUserW` 沒呼 `LoadUserProfile`）；async task registry + 60 s GC，徹底解決同步 handler 9 秒 block 造成 /serverinfo starvation 的「假斷線」。Qt client: dropdown + busy overlay 顯示即時進度（`Asking Steam to shut down…` / `Logging in as XXX…`）。Android client: 同等功能（Spinner + SpinnerDialog + rollback on error）。修 OkHttp `addPathSegment` 把 `/` URL-encode 成 `%2F` 的 bug（改用 `addPathSegments` plural）。 |
| Y | DirectML auto-cascade probe + Generic fallback | v1.2.86–91 | init 時 probe 720p→540p→480p→360p 各解析度推論時間，挑能塞進預算的最高；全 miss 就 fall back 到 Generic。同步把 default backend 改成 Generic、DML option 加「需要強 GPU」警語、移除 180fps 警告。 |
| 新 | DirectML reset-race 修正 | v1.2.82–86 | post-unpack + concat allocator 的 fence-gated reset；多 ring slots；submitFrame 從 50 ms → 3 ms。 |
| 新 | Android FRUC bitrate strategy (a) — 文件化 + log invariant | v1.2.133 | 既有行為已是「總 bitrate 不變、per-frame 自動 2x」。`Game.java:450-457` 加 effective per-frame bitrate log 跟 inline comment 鎖死不變式（避免日後重構默默改成 strategy b）；FRUC settings summary + `summary_seekbar_bitrate` (en + zh-rTW) 加說明。 |
| **§I.A** | **Vulkan 路徑 recon — 綠燈** | **v1.2.134–135** | A1 (v1.2.135) `eglPresentationTimeANDROID` spike — negative result，仍維持往 Vulkan 推進；A2+A3 (v1.2.134) `vkEnumerateInstanceExtensionProperties` 在 Pixel 5 / Adreno 620 確認 `VK_GOOGLE_display_timing` + `VK_ANDROID_external_memory_android_hardware_buffer` 都支援，minimal Vulkan init 跑通。Phase A 三個綠燈條件全達成。 |
| **§I.B** | **VkPassthroughBackend — MediaCodec → AImageReader → AHardwareBuffer → VkImage → swapchain（不含 FRUC）** | **v1.2.136–147 + v1.2.150 + v1.2.152** | 主線 11 子 commit：B.0 `IFrucBackend` interface (v1.2.136)；B.1 VkBackend 骨架 + auto-fallback chain (v1.2.137)；B.2a `VkInstance + VkDevice + Queue` (v1.2.139)；B.2b `VkSurfaceKHR + VkSwapchainKHR` on display Surface (v1.2.140)；B.2c.1 acquire+clear+present sanity roundtrip (v1.2.141)；B.2c.2 Java `ImageReader` scaffolding (v1.2.142)；B.2c.3a active mode + per-frame clear render (v1.2.143)；B.2c.3b `AHardwareBuffer→VkImage` import validated on real frames (v1.2.144)；B.2c.3c.1 `VkSamplerYcbcrConversion + VkSampler` from AHB externalFormat (v1.2.145)；B.2c.3c.2 graphics pipeline + UV gradient test pattern (v1.2.146)；B.2c.3c.3 真實視訊 sampling via YCbCr immutable sampler (v1.2.147)。後續修補：B.2c.3c.4 (v1.2.150) — orientation 修正 + decoder time µs 精度；B.2c.3c.5 (v1.2.152) — output FPS 1s sliding window counter（cross-thread volatile，Pixel 5 / Adreno 620 / LineageOS 22.1 live 驗測 frame 2700–3060 區間 39–45 FPS）。Pixel 5 / Adreno 620 / LineageOS 22.1 live 驗證 3360+ frame `render_ahb_frame` 全乾淨。**Opt-in：`adb shell setprop debug.viplestream.vkprobe 1`**（system property，TODO 原計畫的 `VIPLE_VK_PROBE` env var 在 Android 上 toggle 不便）；預設使用者完全沒感（GLES 不變）。 |
| **§I.C.1** | **Vulkan FRUC port spec (recon)** | **v1.2.149** | 規劃文件：`docs/vulkan_fruc_port.md`。把 GLES 路徑 4 program × 3 quality 變體（10 GLSL）的 binding / uniform / size 列表、GLSL 310 ES → 450 差異對照、7 storage / sampled image 規劃、per-frame command buffer 草案、7 風險點全部固化。此 commit 不動執行路徑，後續 C.2–C.7 才開動。 |

---

## 仍在的相容性包袱

理由都一樣：改了會讓既有使用者的安裝、設定、paired hosts、saved
credentials 在升級時無聲無息地壞掉，需要寫 migration code 才該動。

### A.1 QSettings organizationName + Windows registry 主路徑

**目前：** `moonlight-qt/app/main.cpp` 仍是 `setOrganizationName("Moonlight Game Streaming Project")`。Windows 上 paired hosts、偏好設定全部存在 `HKCU\Software\Moonlight Game Streaming Project\VipleStream`。

**現象：** `applicationName` 已是 `VipleStream`（v1.2.43）但 organizationName 沒動，registry 路徑像是「上游品牌名底下的 VipleStream key」。改了 → 路徑換成 `HKCU\Software\VipleStream\VipleStream`，原來的 key 變無主，使用者體感是所有 paired hosts 一夕消失。

**清的時候要做：**
1. 新版啟動時先用舊 org / app name 構造 `QSettings`，檢查 `HKCU\Software\Moonlight Game Streaming Project\VipleStream` 在不在
2. 在 → 讀全部 key/value、寫到新的 `QSettings("VipleStream", "VipleStream")`
3. 砍掉舊 registry 樹（或留 migrated-flag 避免重複搬）
4. macOS / Linux 同樣處理 `~/.config/Moonlight Game Streaming Project/` → `~/.config/VipleStream/`
5. unit test 覆蓋初始 / 已遷移 / 部分遷移三種情境

**併合進 A.11**：v1.2.43 把 `applicationName` 改成 `VipleStream` 已經造成升級時設定看起來「回預設值」（檔名從 `Moonlight.ini` 變 `VipleStream.ini`）。這份 migration 也要處理。

### A.2 WiX installer 的 registry / install paths

**目前：** WiX installer 的 `HKCU\Software\Moonlight Game Streaming Project` 安裝狀態旗標 (`Installed` / `DesktopShortcutInstalled` 等)、`InstallFolder = "Moonlight Game Streaming"`、`APPDATAFOLDER = %LOCALAPPDATA%\Moonlight Game Streaming Project` 全沒動。

**影響檔案：** `moonlight-qt/wix/Moonlight/Product.wxs`、`moonlight-qt/wix/MoonlightSetup/Bundle.wxs`。

**注意：** 我們**目前根本沒在用 WiX MSI installer 出貨**，client 是 zip + 手動解開、server 用 `deploy_sunshine.ps1` 部署。所以 WiX 的相容性債是「如果哪天要改用 MSI 出貨才需要清」，不是現在的痛點。

**清的時候要做：**
1. 新 InstallFolder = `VipleStream`、新 APPDATAFOLDER = `%LOCALAPPDATA%\VipleStream`
2. 新 HKCU 路徑 = `HKCU\Software\VipleStream`
3. WiX 升級邏輯：偵測舊 key / 路徑存在 → copy → 砍舊
4. 配新的 `UpgradeCode` GUID，否則 MSI 嘗試 in-place upgrade 找不到舊路徑

### A.6 HTTP Basic auth realm `"Sunshine Gamestream Host"`

**目前：** `Sunshine/src/httpcommon.cpp:132` 用這個字串當 HTTP Basic auth realm。Web UI 登入時 browser 把 username/password 存在這個 realm 下。

**影響：** realm 一改，browser 已存的憑證對不上新 realm，使用者要重新登入。

**清的時候要做：**
1. 新 realm = `"VipleStream-Server Web UI"`
2. Release notes 寫「升級後 Web UI 會要求重新登入」

### A.7 協定 / wire-format 字串（**不該清**）

**目前：** `"NVIDIA GameStream"`、cert CN `"Sunshine Gamestream Host"`、UA header `Moonlight/...`、mDNS service type `_nvstream._tcp`、serverinfo 的 `state` 字串 `SUNSHINE_SERVER_FREE` / `SUNSHINE_SERVER_BUSY`、HTTP `/serverinfo` `/launch` 等 endpoint 路徑全部維持上游原名。

**理由：** 這是 GameStream wire protocol 的一部分，改了等於跟整個 Moonlight / Sunshine 生態脫鉤——使用者沒辦法用原版 Moonlight 連 VipleStream-Server，也沒辦法用 VipleStream client 連別人家的 vanilla Sunshine。User 已明確要求「混搭互聯」（v1.2.93 討論）。

**保留到永遠。** 這條留著就是要提醒未來想動的人：不要動。

### A.8 內部 class names

**目前：** `NvHTTP` / `NvComputer` / `NvApp` / `SunshineHTTPS` / `SunshineHTTPSServer` / `SunshineSessionMonitorClass`（Windows window class）等內部命名沒改。

**清的時候要做：** 真的想改就在獨立 refactor PR 裡 batch rename，**不要**跟 feature / bugfix 混在一起（diff 會無法 review）。優先度極低。

---

## C. Sunshine Web UI 其他語系檔

**目前：** v1.2.43 只改了 `en.json` / `zh_TW.json` 的 Sunshine / Moonlight → VipleStream-Server / VipleStream。另外 20 個語系檔還是舊字串。

**要做：** 下一次 i18n sync commit 順便對 20 個檔做 bulk replace。**不影響功能，只是品牌一致性。**

---

## D. 上游 wiki 連結

**目前：** `moonlight-android/.../HelpLauncher.java` 的 setup guide / troubleshooting 指 `finaltwinsen/VipleStream#readme`；GameStream EOL FAQ 仍指上游。

**要做：** VipleStream 自己的 docs（setup guide、troubleshooting、錯誤碼表）有實際內容後，HelpLauncher URL 換成 VipleStream 自己的 docs。

---

## F. DirectML FRUC — 架構級優化（Option C+）

DirectML auto-cascade（v1.2.91）已經把「中低階 GPU 跑 DML 會掉幀」的痛點解掉，hot path 已 ship。下面是「如果要推 DML 到 4K120 real-time」才需要的架構改動。

### F.1 FRUC pipeline 整個搬進 D3D12

**Gain：** 消除 `ctx4->Signal`（D3D11 UMD flush）每 frame 60-80 μs。
**Risk：** 高。整個 renderer 的 present / blit / overlay 管線都要重寫成 D3D12 path。Moonlight 原本的 d3d11va / dxva2 / vaapi 多 backend 抽象要重新設計。
**工時估：** 1-2 週。
**觸發條件：** 4K120 real-time DML 需求，目前不急。

### F.2 Command bundles for pre-recorded DML pipeline

**Gain：** 30-40 μs / frame。
**Risk：** 中。Bundle 建立後 PSO / root sig 不能改、binding 限制多。
**工時估：** 2-3 天。

### F.3 Zero-copy D3D11→D3D12 heap alias

**Gain：** 10-20 μs / frame。
**Risk：** 中。Heap alias 的 state 管理比 texture share 嚴格。

---

## G. DirectML FRUC — 更多 ONNX model variants

§G.2（fp16 model + cascade rewrite）跟 §G.3（IFRNet-S 試水）都已 ship，
見「已完成」表的對應列。重要的 negative result：**A1000 launch overhead
25 ms floor 不可繞過，任何 ML 模型 op count > 150 都上不了 14 ms budget**。
這個結論影響下面剩下的 G.1 / G.4：

### G.1 11-channel 輸入 (RIFE v4 v1)

**Gain：** v1 node count 少 50%（216 vs 456），可能比 v2 快 30-50%。
**重新評估（§G.3 之後）：** A1000 仍可能達不到 14 ms（216 ops × 0.1 ms launch
overhead = 21.6 ms 已經超了），但對 RTX 30/40+ 可能值得；要 ship 還是
得搭 §G.4 的「下載管理」避免 release zip 再加肥。
**做法：** `tryLoadOnnxModel` 加 11-channel branch；pack 前加 optical flow CS 或塞 zero flow。

### G.4 模型下載管理

**動機：** `fruc.onnx` 22 MB 跟 release zip 一起出。多數 NVIDIA user 用 NvOF / Generic，DML 是少數派。
**做法：** Moonlight 第一次選 DML 時跳「下載 22 MB」dialog，model 放 `%LOCALAPPDATA%\VipleStream\fruc_models\`。低優先度。

### 已就位的診斷工具（沿用）

- `VIPLE_DIRECTML_DEBUG=1` — D3D12 debug layer + DML validation
- `VIPLE_DML_RES=540|720|1080|native` — tensor 解析度 override
- `[VIPLE-FRUC-DML]` log — 每 120 frame 印 per-stage EMA μs
- `[VIPLE-FRUC-DML-ORT]` log（v1.2.83）— ort_concat / ort_run / ort_post / wait_pack / wait_post / wait_concat 拆解

---

## H. Steam library 自動匯入

全部 ship（見「已完成」表的 H.Phase 1+2+3 / H.4 / H.5）。本章 close — 留下兩條未來真要做才回看的備忘：

- **`isVipleStreamPeer` 條件隱藏 SettingsView**：目前所有 client-side 設定（Relay / FRUC / 影格節奏）都跟 host 類型無關，沒有需要隱藏的；哪天加了 VipleStream-only 的 client 設定再回頭加。
- **Vanilla peer 反向 badge**（「Standard Sunshine / GFE」灰色 chip）：討論過但暫不做，對純 vanilla 使用者會變成常駐視覺雜訊。

---

## E. moonlight-android Icon 補完

**目前：** v1.2.30 已用 lime VipleStream icon，但：
- Splash screen 需確認用新 icon
- 「最近應用程式」列表 icon 在各版本 Android 上的顯示要驗證
- Android 12+ themed icon（Material You 單色化）沒提供 monochrome mask → 會 fallback 到 adaptive-icon。要支援需加 `ic_launcher_monochrome.xml`

---

## I. Android FRUC Vulkan 路徑（取代 GLES）

**動機：** 兩個 roadmap 等級的需求都靠這條路徑解：
1. **HDR streaming 遲早要做** — GLES OES external texture 對 10-bit / P010 / HDR metadata 支援差，Vulkan + AHardwareBuffer 是必經
2. **降低 FRUC 加的 ~17ms 延遲** — 雙 swap V-sync block 是結構性問題，async compute queue + `VK_GOOGLE_display_timing` 是真正解法

**為什麼不直接動：** 整套 GLES → Vulkan 換軌約 2.5–3.5 週，touches every Android device。要分階段 ship + 保留 GLES fallback。

### 設計：backend 抽象 + auto-fallback

```
新增 com.limelight.binding.video.fruc.IFrucBackend interface
  ├── GlesFrucBackend  (現有 FrucRenderer 加 implements)
  └── VkFrucBackend    (Phase B+ 新增)

MediaCodecDecoderRenderer 持有 IFrucBackend，runtime 決定走哪條：
  preferredBackend = "vulkan" + 可用 + init 成功 → VkFrucBackend
  任一步失敗 → fall back GlesFrucBackend
  GLES 也失敗 → frucEnabled = false → MediaCodec 直連 display surface
```

預設 `gles`，使用者可從 settings 勾「Vulkan (experimental)」。**穩定 1 個月 + 6 個月後**才分別翻預設、移除 GLES。

### 階段（每階段獨立 commit、可獨立 ship）

| Phase | 工時估 | 內容 | Ship 樣貌 | 狀態 |
|---|---|---|---|---|
| **A** Recon | 0.5–1 day | A1 `eglPresentationTimeANDROID` spike；A2 `vkEnumerateInstanceExtensionProperties` 確認 Pixel 5 支援 `VK_GOOGLE_display_timing` + `VK_ANDROID_external_memory_android_hardware_buffer`；A3 minimal Vulkan init 驗 build chain | 不 ship 程式碼，只交報告 | ✅ v1.2.134–135 |
| **B** Plumbing | 5–7 day | 抽 `IFrucBackend` interface、現有 GlesFrucBackend 改名實作；新增 `VkPassthroughBackend`（MediaCodec → AImageReader → AHardwareBuffer → VkImage → swapchain，**不含 FRUC**）；system property `debug.viplestream.vkprobe=1` 啟用 | 預設使用者完全沒感（GLES 不變） | ✅ v1.2.136–147 主線 + v1.2.150 (orientation/µs) + v1.2.152 (FPS counter) |
| **C** Vulkan FRUC | 5–7 day | GLSL → SPIR-V (含 v17 / v1.1.136 改動全部 port)；Vulkan compute pipeline；接 B 的 present 鏈；加 `VK_GOOGLE_display_timing`；Settings 加「FRUC 後端：GLES (預設) / Vulkan (實驗)」toggle | 自願者開、出問題自動退回 GLES | ⏳ 進行中 |
| **D** Async compute | 3–4 day | ME 移到專屬 compute queue，跟 present queue 平行；ME 跟前一幀 present 重疊 — **這才是 +17ms 延遲的真正解法**，也解 §I.C.5.b 觀察到的 dual present + waitIdle thermal regression；baseline 對比驗 latency 真的下降 | 同 C toggle，效能升級 | 🟡 v1.2.162 ship in-flight ring（host-side waitIdle 移除）但 perf 平 — 真要 latency gain 還要 mailbox present 或 multi-queue，**Adreno 620 queue family 數量待 probe** |
| ↳ **D.a** In-flight ring + fence-gated AHB cleanup ✅ v1.2.162 | 加 `VK_FRAMES_IN_FLIGHT=2` 環形 buffer：每 slot 1 cmdbuf + 2 acquireSems + 2 renderDoneSems + 1 fence + per-slot pendingImg/Mem/View tracking。`init_in_flight_ring` 從既有 cmdPool 配 2 cmdbuf、`fci.flags=VK_FENCE_CREATE_SIGNALED_BIT` 第一 frame fence wait 立刻通過。`render_ahb_frame` dual-present path 整段重寫：(1) `vkWaitForFences(slot.fence)` (2) free slot's pending AHB 從上次同 slot reuse (3) `vkResetFences` (4) 兩次 `vkAcquireNextImageKHR` 各拿一張 swapchain image (5) 1 cmdbuf 內 record dispatch_fruc + 兩個 render pass (interp pipeline → fb[imgIdxPass1] / graphics pipeline → fb[imgIdxPass2]) (6) 1 vkQueueSubmit 含 2 wait sems / 2 signal sems / fence (7) 2 vkQueuePresentKHR (PTS 同 §I.C.6) (8) 把 imgIn/memIn/viewIn 存進 `slot.pending*` 不立刻 free。`vkQueueWaitIdle` 從 dual-present 路徑完全移除。dispatch_fruc 暫用 `be->cmdBuffer` 指針 swap hack（restore 在 endRecording 後）— 後續 §I.D.b 應 refactor 成參數。Fall-back 單 present 路徑 (`fInitialized=0`) 保留原 single-cmdbuf+waitIdle 不變。**Pixel 5 / Adreno 620 自驗**：`[VKBE-RING] in-flight ring ready: 2 slots × ...`、frame #60–960 outputFps 穩定 45.0±0.3（jitter 比之前 §I.C 段更穩，frame pacing 改善 — driver 看到 ring 在 schedule），no validation warning，no GPU hang。但 input rate 仍 ~45 FPS（跟 §I.C.4.b 同），thermal 看似升 0.5–1.5°C（連續 test 沒冷卻 device，不能下定論）。**Wall-clock 沒改善** 因為 60Hz display × FIFO present × dual present = 33ms/frame 上限是 GPU/display 端 bottleneck，host 端 ring 只移除 spin-wait 沒 unblock vsync gating。 | — |
| ↳ **D.b** Smart-mode dual present ✅ v1.2.163 | (d) 從上面三個 fix 候選裡先做 (d) — input < display rate 時才 dual present。`vk_backend_s` 加 `fInputWindowStartNs/fInputWindowFrames/fInputFpsRecent/fLastSingleMode`；render_ahb_frame 開頭量 input fps（1s sliding window via monotonic_ns）；`displayHz = 1e9 / fRefreshDurationNs`；判斷 `singleMode = (inputFps > 0 && inputFps >= displayHz × 0.92)`，default dual 直到第一次量測完成。Single mode 邏輯：只 acquire 1 swapchain image (slot.acquireSem[0])、render pass 2 (real, graphics pipeline + AHB direct) 用 `fb[imgIdxPass1]`、skip render pass 1 (interp)、submit semCount=1、單 present；compute pipeline 仍跑（維持 prev frame state for re-entry to dual mode）。`fInterpolatedCount` 只 dual mode 才 +1。Mode 切換時印 `[VKBE-RING] mode change: dual → single (input ~57.7 FPS, display 60.0 Hz)`。**Pixel 5 / Adreno 620 自驗（決定性結果）**：stream 開始 1.4s 內 default dual → 自動切 single；frame #60–#660 outputFps 穩定 **58.7–60.2 FPS**（mean ~60），**完整恢復 input fps，解了 §I.C.5.b 觀察到的 60→45 throttle regression**。GPU work 預期減半（1 render pass + 1 vsync wait vs 2+2），thermal 應隨改善（未獨立驗測但邏輯上必降）。Compute pipeline 還在跑（warp 結果不顯示，但 prev frame 鏈不斷裂），切回 dual mode 時無 warm-up 視覺破壞。 | — |
| ↳ **D.c** Pixel 5 90 Hz hint + smart-mode criterion ✅ v1.2.164 | 解 v1.2.163 第二輪測試發現的 stuck-in-dual 問題 + 把 Pixel 5 panel 從 default 60 Hz 升到 90 Hz 讓 FRUC 真正能補幀。3 個改動：(1) `create_surface` 結尾 `ANativeWindow_setFrameRateWithChangeStrategy(window, 90.0, FIXED_SOURCE, CHANGE_FRAME_RATE_ALWAYS)` via `dlsym(libandroid.so)` (API 31+ 動態 load 避免 minSdk 21 的 weak-link SIGSEGV)，hint 接受時 set `fHintedRefreshHz=90.0`；(2) smart-mode 改用 `fHintedRefreshHz` 覆蓋 `vkGetRefreshCycleDurationGOOGLE`（Adreno 620 driver cache swapchain create 時的 60Hz refresh，panel 切 90 Hz 後也不 update — observed bug）；(3) smart-mode criterion 從 `input ≥ display×0.92` 改成 `2×input ≤ display×1.05`（dual present 真正 fits 才 enable，避免「初始 dual → throttle → measure 低 → 卡 dual」迴圈）+ 預設 single mode（量測未滿 1s 走 single，clean baseline）。Pixel 5 自驗（panel 透過 `adb shell settings put system min_refresh_rate 90` 鎖在 90 Hz）：`mode change: dual → single (input ~X FPS, display 90.0 Hz)` log 確認 fHintedRefreshHz 生效；60 fps input 在 90 Hz panel 持續 single mode (60×2=120 > 94.5 → no headroom for interp，正確判斷)；frame #120-480 outputFps 60.0±0.2 stable。**目標 45→90 setup**: server-side stream rate 設 45 (client UI frame rate slider) + Pixel 5 panel 90 Hz mode → smart-mode 自動觸發 dual (45×2=90 ≤ 94.5) → 90 fps displayed。Vulkan client 端 ready，等 user / server-side 配合驗。 | — |
| ↳ **D.c v2** Java 端 max refresh rate ✅ v1.2.165 | v1.2.164 hardcoded hint 90 + fHintedRefreshHz=90 是 Pixel-5-specific bug — Pixel 9 (120 Hz panel) 上 60×2=120 > 90×1.05=94.5 → 誤判 single mode → output 仍 60 (user observed v1.2.164 60→120 fail)。改用 `Display.getSupportedModes()` Java side query device max refresh rate，透過 `nativeInit(.., maxRefreshHz)` 多帶一 jfloat 參數傳進 native；setFrameRate hint 跟 fHintedRefreshHz 都用這個值。Pixel 5 / 9 / Tensor / etc 自動 device-portable。 | — |
| ↳ **20-iter optimization sequence** ✅ v1.2.165–172 | autonomously shipped 8 commits (12 iters batched) 補幀 > 延遲 > 畫質 priorities：1-3 v1.2.165–166 startup measurement / mode diagnostics / faster fps publish；4 v1.2.167 host frame timing p50/p90/p99 ring buffer；5+6 v1.2.168 queue family probe + MAILBOX present mode preference (vsync block off PASS 1)；7 v1.2.169 smart-mode hysteresis (10% deadband，避免 flip-flop)；8 v1.2.170 single mode skip dispatch_fruc (省 5-10ms GPU/frame)；11+12 v1.2.171 inAcquire barrier trim + [VKBE-PERF-SUMMARY] every 600 frames + [VKBE-PERF-FINAL] on destroy；15+16 v1.2.172 past timing aggregate (avg/min/max instead of 8 raw) + HDR pref pipeline (`prefs.enableHdr` → `nativeSetHdrEnabled` → log gate state，§I.E.b/c 才會真開 HDR pipeline)；18 v1.2.173 baseline.sh logcat filter 加 VIPLE-VK-PROBE 標籤 (§I.A.A2 vk_probe.c output)。 | — |
| **E** HDR | 獨立 | 10-bit P010 path、color space management、HDR10 metadata；HDR toggle 自動拉 backend 到 Vulkan | 「啟用 HDR」勾選 | 🟡 §I.E.a recon ship v1.2.165；§I.E.b/c 待動 |
| ↳ **E.a** HDR capability recon ✅ v1.2.165 | 純 read-only probe + log 不改執行路徑：(1) `vkEnumerateInstanceExtensionProperties` 偵測 `VK_EXT_swapchain_colorspace` → 條件 enable instance ext；(2) `vkEnumerateDeviceExtensionProperties` 偵測 `VK_EXT_hdr_metadata` → 條件 enable device ext (跟 §I.C.6 共用 enumeration pass)；(3) `vkGetPhysicalDeviceSurfaceFormatsKHR` 完整 dump 到 logcat with human-readable colorSpace name；(4) `vkGetPhysicalDeviceFormatProperties` 探 3 個 HDR-relevant format。新增 struct fields `fHdrColorspaceExt` / `fHdrMetadataExt` / `fHdrCapableSurface`，現在僅作觀測，§I.E.b/c 才會 act。**Pixel 5 / Adreno 620 / LineageOS 22.1 結果**：兩個 HDR ext 全 available；surface advertises HDR10_ST2084 / HDR10_HLG / BT2020_LINEAR / DISPLAY_P3 / DCI_P3 / ADOBERGB / PASS_THROUGH / EXTENDED_SRGB 完整 HDR colorspace family；67 surface format pairs；最關鍵：**format=64 (A2B10G10R10_UNORM_PACK32) + colorSpace=HDR10_ST2084** 成立 → 10-bit RGB + PQ encoded HDR10 swapchain 可建。`A2B10G10R10` 跟 `R16G16B16A16_SFLOAT` 兩個 format optimalTiling=0x1fd83（SAMPLED+STORAGE+COLOR_ATTACHMENT 全有），可作 swapchain image / interp storage image。**Adreno 620 不直接支援 `G10X6_B10X6R10X6_2PLANE_420` (P010 ycbcr) — optimalTiling=0x0**；要走 AHB external format YCbCr conversion（driver 自行 negotiate decoded P010 buffer → internal format）。 | — |
| ↳ **E.b/c** Plan after recon | (b) Swapchain HDR enable: HDR toggle ON 時改用 format=64 + HDR10_ST2084_EXT colorSpace 重建 swapchain；render pass attachment format 跟著改；fragment shader 處理 PQ encoding（不要做 sRGB linearize）。(c) Decoded P010 import path: MediaCodec set `KEY_COLOR_FORMAT=COLOR_FormatYUVP010` (Android 11+)；AHB 進來 externalFormat 變化（之前是 8-bit YCbCr, HDR 是 10-bit），ensure_ycbcr_sampler 要重建 sampler 鏈；descriptor set immutable sampler 要 invalidate + 重新 bake pipeline (HDR toggle 是 non-trivial state change，需要 destroy + 重建 graphics + interp pipelines)。風險：interp pipeline 跟 compute pipeline 對 R16G16B16A16_SFLOAT storage image 的 read-modify-write 行為要驗 (motion field 還是 r32i 不變；prev/curr/interp frame 改 16-bit FP)。 | — |

### Phase C 細部子計畫（2026-04-27 規劃）

GLES 路徑現在用 4 個 program × 3 quality 變體（共 10 個 GLSL）+ 827 行 `FrucRenderer.java` orchestration。整段 port 到 Vulkan 不可一個 commit 收。仿 §I.B 拆 7 個獨立 ship 的子 phase：

| Sub-phase | 內容 | 風險 / 失敗點 |
|---|---|---|
| ~~**C.1** Recon — shader port spec~~ ✅ v1.2.149 | 已 ship 為 `docs/vulkan_fruc_port.md`：5 個 shader 的 binding / uniform / size 全部列；GLSL 310 ES → 450 差異對照；7 個 storage / sampled image 規劃；per-frame command buffer 草案；7 個風險點。 | 低；不動執行路徑 |
| ~~**C.2** Shader port — compile only~~ ✅ v1.2.153 | 4 個 GLSL 翻成 GLSL 450 compute（motionest / mv_median / warp + 新增 ycbcr_to_rgba 取代 oes_to_rgba.frag），`glslc --target-env=vulkan1.1 -O` 編成 SPIR-V 1.3 + `xxd -i` 包成 `.spv.h`。Quality preset 用 `-DQUALITY_LEVEL=N` 編 motionest × 3 + warp × 3 = 6 份；mv_median × 1 + ycbcr_to_rgba × 1 = 2 份；總 8 個 SPV/header。建置 script `shaders/build_shaders.cmd` 自動 resolve `%ANDROID_HOME%\ndk\<latest>\shader-tools\windows-x86_64\glslc.exe`。**只進 build、未接 pipeline**：8 個 `.spv.h` 還沒被 `vk_backend.c` `#include`，所以 ndk-build 結果跟 §I.B 完全一致（compile + link 都 pass，APK size 不變）。 | 中；GLES `imageStore` / `bitCount` 行為差異可能要重寫 |
| ~~**C.3** Compute pipeline scaffold~~ ✅ v1.2.154–155 | 在 VkBackend 加 4 個 `VkPipeline` (compute) + descriptor set layouts；分配 storage images（prev/curr RGBA、motionField、filteredMotionField、interpFrame）；**dispatch 但不 sample 結果到 swapchain**。拆兩個子 commit ship。 | 中；storage image format / access flags |
| ↳ **C.3.a** Init/destroy scaffold ✅ v1.2.154 | `vk_backend.c` 加：6 storage images (currFrameRgba / prevFrameRgba / motionField / prevMotionField / filteredMotionField / interpFrame，rgba8 或 r32i)、4 個 compute pipelines (ycbcr / motionest_q1 / mv_median / warp_q1)、4 個 desc set layout (set=0, ycbcr 用 immutable YCbCr sampler、其餘用 immutable linear sampler)、push constant range 8/12/8/16 bytes、descriptor pool + 4 個 desc sets、persistence linear sampler。Lazy init（first AHB frame after ycbcr 就緒），fail soft（compute init 失敗不影響 graphics pipeline）。**未 dispatch、未 update desc sets** — 純 scaffolding。Pixel 5 / Adreno 620 自驗：`[VKBE-COMPUTE] init done: 6 storage images (W=1920 H=1080, mvW=30 mvH=17), 4 pipelines (ycbcr/motionest_q1/mv_median/warp_q1), 4 descsets`，stream FPS 不退（frame 60–240 outputFps 56–60）。 | — |
| ↳ **C.3.b** Per-frame dispatch ✅ v1.2.155 | init 結尾追加：static descriptor writes (11 binding，全 layout=GENERAL) + 一發 init clear command buffer（6 image UNDEFINED→GENERAL + clear prev_rgba/prev_mv 為 0）。新增 `dispatch_fruc(VkImageView ahbView)`：每 frame 重 bind ycbcr binding 0 ← AHB view，然後依序 bind+push+dispatch 4 條 pipeline（ycbcr (W/8,H/8,1) → motionest (mvW/8,mvH/8,1) → mv_median (mvW/8,mvH/8,1) → warp (W/8,H/8,1)），中間 `vkCmdPipelineBarrier` memory barrier (COMPUTE→COMPUTE, SHADER_WRITE→SHADER_READ\|WRITE) 串連讀寫；最後 prev rotate `vkCmdCopyImage` (currFrameRgba→prev / motionField→prev_mv) + transfer barrier 給下一 frame。`render_ahb_frame` 的 inAcquire barrier dstStage 從 `FRAGMENT_SHADER_BIT` 擴成 `COMPUTE_SHADER_BIT \| FRAGMENT_SHADER_BIT`（兩條都會讀 imgIn）。**仍不接 swapchain**：interpFrame 寫好但沒被 graphics 採樣，使用者看到的是原 AHB passthrough。Pixel 5 / Adreno 620 自驗：4 個 dispatch 每 frame 跑（log #0–#5/120/240/360 都印出），frame #120/240/420 outputFps 61.2/59.8/60.2，**stream 60 FPS 完全不退**，no validation warning，no GPU hang。 | — |
| ~~**C.4** Wire compute → present~~ ✅ v1.2.156–157 | `render_ahb_frame` 改成 `import AHB → blit YCbCr→curr_rgba → ME → median → warp → 雙 blit (interp + real) → present`；保持每 frame `vkQueueWaitIdle`（暫時）。拆兩個子 commit ship。 | 高；FRUC algo 視覺正確性 |
| ↳ **C.4.a** Single present interp ✅ v1.2.156 | 新增第二個 graphics pipeline `fInterpPipeline`：與既有 graphicsPipeline 共用 `renderPass + fullscreen.vert + video_sample.frag`，唯一差別是 desc set layout binding 0 immutable sampler 換成 `fLinearSampler` (sample interpFrame rgba8) 而非 `ycbcrSampler`。`fDescPool` maxSets 4→5、sampler descriptor 9→10 容下新 desc set；init 時把 binding 0 ← interpFrame view (GENERAL) 寫好。`render_ahb_frame` 在 dispatch_fruc 後加 `COMPUTE_SHADER → FRAGMENT_SHADER` 的 memory barrier 讓 warp write 對 fragment read visible，render pass 內條件 bind：`fInitialized` → 用 fInterpPipeline + uvScale=(1,1)；fail → 退回原 graphicsPipeline + AHB ycbcr 路徑。Pixel 5 / Adreno 620 自驗：stream 拉通 1920×1080@60 FPS HEVC，frame #120/240/360/480/600/720/840/960 outputFps 55.87/59.97/58.23/59.94/59.80/58.20/57.83/56.03（平均 ~58 FPS，跟 input 60 差 ~3% 在誤差內）；**畫面正確顯示 warp_q1 的輸出**（靜態 desktop 場景下 mv≈0 時 warp 退化成 pass-through，視覺與 AHB 直 sample 等價，無撕裂無鬼影）；no validation warning，no GPU hang。 | — |
| ↳ **C.4.b** Dual present (interp + real) ✅ v1.2.157 | 真正的 spec 「雙 blit」semantics：在 PASS 1 (interp) `vkQueueWaitIdle` 之後新增 PASS 2 (real / AHB)：`vkAcquireNextImageKHR` 拿第二張 swapchain image → reset+begin cmdbuf → render pass `framebuffers[imgIdx2]` + bind `graphicsPipeline + descSet + AHB uvScale` → draw → end → submit + present + waitIdle。第 2 個 PASS 重用 `acquireSem` / `renderDoneSem`（Vulkan wait 機制保證每 submit/present cycle 自動 unsignal）。`imgIn` PASS 1 的 inAcquire barrier 已轉到 `SHADER_READ_ONLY_OPTIMAL`，PASS 2 不需 layout transition。新增 `vk_backend_s.fInterpolatedCount` 每完成一輪 dual present +1，透過新 JNI `nativeGetInterpolatedCount` 暴露給 `VkBackend.java getInterpolatedCount()`，perf overlay 顯示。Pixel 5 / Adreno 620 自驗：input rate 從 60 → **45 FPS** throttle (predicted：90 Hz display × 2 present × waitIdle ≈ 22 ms/input → 45 FPS 上限；§I.D async compute 才會解)，但 stream 沒掛、網路丟失 0%、延遲 1.7/2.7/2.0 ms 正常；**Interpolated counter 增到 1962 frames** 持續遞增；Remnant 遊戲畫面正確（dual present 視覺合成無撕裂無鬼影）。 | — |
| ~~**C.5** Quality preset wiring + GLES parity~~ ✅ v1.2.158–159 | 拉 `setQualityLevel(0/1/2)` 切換 ME pipeline；對 GLES baseline 跑 `scripts/benchmark/android/`，verify fps / latency 不退步。拆 C.5.a (wiring) → C.5.b (baseline 對比)。 | 中；perf 可能輸 GLES，要分析 |
| ↳ **C.5.a** Quality preset wiring ✅ v1.2.158 | `vk_backend.c` 加 `fMeShaderQ[3]` / `fWarpShaderQ[3]` / `fMePipelineQ[3]` / `fWarpPipelineQ[3]` + `fQualityLevel` int (default 1)；init_compute_pipelines 在原本 4 pipeline 之上多 build 4 個（Q0/Q2 × ME/warp），Q1 entry alias 到既有 `fShader[FRUC_PIPE_ME]`/`fPipeline[FRUC_PIPE_ME]` 不重 build。Layout 三個 Q 共用（per `docs/vulkan_fruc_port.md` §4 設計）— 不重 alloc descset/layout。dispatch_fruc 取 `qIdx = clamp(fQualityLevel, 0, 2)` 選 `fMePipelineQ[qIdx]` + `fWarpPipelineQ[qIdx]`。新 JNI `nativeSetQualityLevel(handle, level)` 暴露給 Java；`VkBackend.setQualityLevel` 從只 cache `this.qualityLevel` 改成 + call native。Pixel 5 / Adreno 620 自驗：init log 含 8 pipeline（vkCreateComputePipelines × 2 都過），default Q1 dispatch log `ycbcr+motionest_q1+mv_median+warp_q1` 跟 §I.C.4.b 等價，stream 不退步、no validation warning、no GPU hang。**Runtime quality switch** 留給 §I.C.7 settings UI toggle 一併驗（一個 stream 内 setQualityLevel 一般只被叫一次，從 Java settings 讀 prefs）。 | — |
| ↳ **C.5.b** Baseline 對比 (GLES vs Vulkan FRUC) ✅ v1.2.159 | 跑 `scripts/benchmark/android/android_baseline.sh` 兩次（vkprobe=0 GLES / vkprobe=1 Vulkan FRUC），各 60s，stream Steam Desktop 1920×1080@60 HEVC 至 <host>。順便 patch baseline.sh 加 `VKBE:V` / `VKBE-NAT:V` logcat tag（之前 GLES-era filter 沒涵蓋 Vulkan path）。**Finding：Vulkan path 在這 device + 這 input rate 的 case `regression`，不通過 §I 鐵律 #2「不能退步」**：thermal 60s window 內 GPU mean 38.5C → 42.9C (+4.4C)、CPU mean 43.1C → 47.4C (+4.3C)、且 trend 仍向上 (delta GPU +2.3C / CPU +3.0C 在 60s 內)；GLES 同期 trend 平 (delta 0~0.2C)。gfxinfo post-stream（限制：抓 Activity 主 surface UI overlay 不是 SurfaceView stream）顯示 Vulkan jank 56.67% / p50=24ms / p90=28ms — 90Hz display 11.11ms budget 的 ~2×。**Root cause** 在 §I.C.4.b 自驗已識別：dual present + per-frame `vkQueueWaitIdle` 強迫 CPU+GPU serialize、無 overlap 空間，導致 GPU 一直在等同步 → CPU usage 高 → thermal 升。GLES path 也是 dual swap 但內部排程更省（dual-buffer + eglSwapBuffers 隱含 vsync block 而非 explicit waitIdle）。**Fix path：§I.D async compute** — ME / warp 移到 dedicated compute queue，跟 graphics queue 平行，移除 waitIdle，預期 thermal 跟 fps 都回來。Raw data 留在 `temp/baseline_gles_v158/` 跟 `temp/baseline_vulkan_v158/` 給 §I.D 驗證對比用。 | — |
| ~~**C.6** `VK_GOOGLE_display_timing`~~ ✅ v1.2.160 | 加 extension probe + `vkGetPastPresentationTimingGOOGLE` / `vkGetRefreshCycleDurationGOOGLE`；FRUC interp frame 帶 PTS 給驅動程式排程。 | 中；驅動行為文件少 |
| ↳ Implementation | `vk_backend.c`: create_device 用 `vkEnumerateDeviceExtensionProperties` probe `VK_GOOGLE_display_timing`，enabled 列表動態長度 (2 base + 1 conditional)；vkCreateDevice 後 load `vkGetRefreshCycleDurationGOOGLE` / `vkGetPastPresentationTimingGOOGLE`；create_swapchain 結尾 query refresh duration 並 cache `fRefreshDurationNs`；render_ahb_frame 兩個 PASS 都 chain `VkPresentTimesInfoGOOGLE` (PASS 1 desired = monotonic_ns()，PASS 2 desired = PASS 1 + refreshDuration/2)；每 120 frame query 過去 8 個 present 的 actual vs desired delta + margin 印 `[VK-DISPLAY-TIMING]` log。`<time.h>` 加進去用 `clock_gettime(CLOCK_MONOTONIC)`，跟 driver PTS 同 clock domain。 | — |
| ↳ Findings | Pixel 5 / Adreno 620: **swapchain refresh = 16.67 ms = 60 Hz**（修正 §I.C.4.b 90 Hz 假設 — 這個 device LineageOS 22.1 default mode 是 60 Hz；真要 90 Hz 要改 prefer presentation mode 或加 Android frame rate hint，未做）；refreshDurationGOOGLE 一次 query OK；**past presentation timing 持續回傳數據**：actual - desired delta 集中在 +14~+27ms（≈1-2 個 60Hz cycle），margin 變動大（部分為 uint64 wrap 表示 driver 排到比 desired 還早結束）。**Driver behaviour 觀察**：FIFO present mode 一定等下個 vsync，desired=now 沒辦法繞過去 — VK_GOOGLE_display_timing 給 visibility（看 actual vs desired）但沒給 control（PTS 是 hint 不是 deadline）。真改 latency 還是要 §I.D async compute 移除 vkQueueWaitIdle。 | — |
| ~~**C.7** Settings UI toggle~~ ✅ v1.2.161 | XML preference + summary 加「FRUC 後端 GLES / Vulkan (experimental)」；移除 `debug.viplestream.vkprobe` 強制需求。 | 低；純 UI |
| ↳ Implementation | `preferences.xml` 加 `CheckBoxPreference key="checkbox_fruc_backend_vulkan"` (default false / dependency `checkbox_enable_fruc`)；繁中 title「FRUC 後端：Vulkan (實驗)」+ summary 標明 §I.C 階段 throttle/thermal 預警，建議真要試的勾、不對就取消。`PreferenceConfiguration` 加 `frucBackendVulkan` boolean field + `getPreferences` 讀 prefs。`MediaCodecDecoderRenderer.configure()` 邏輯改成 `vkOptIn = prefs.frucBackendVulkan \|\| VkBackend.isOptedIn()`：UI 勾選 OR `debug.viplestream.vkprobe=1` 任一觸發 Vulkan，default 兩者都 false → 走 GLES。Log 從原本只印 isOptedIn 改成印 `prefs.frucBackendVulkan=X vkprobe-override=Y → Vulkan=Z`，event source 一目了然。Pixel 5 / Adreno 620 自驗：(a) vkprobe=0/prefs default → `Vulkan=false`，走 GLES；(b) vkprobe=1/prefs default → `Vulkan=true`，走 Vulkan；(c) prefs=true wiring 是標準 Android `prefs.getBoolean` pattern，未透過 UI tap 實測但 high confidence。 | — |

每個子 phase 的 commit message 用 `vX.Y.Z: §I.C.N — <短摘要>` 格式，跟 §I.B 一致。

### 不可動的鐵律 (§I)

1. ~~**Phase A 結果決定整個計畫是否繼續**~~ — A 已綠燈通過 (v1.2.134–135)，extension 都支援。
2. **每 Phase 都要有 baseline 對比**。沿用 `scripts/benchmark/android/`，量出來不如預期就停。
3. ~~**MediaCodec → AHardwareBuffer 在 LineageOS 22.1 / Pixel 5 上的行為是最大未知**~~ — B 已驗證可行 (v1.2.144–147)，3360+ frame live 驗證乾淨。
4. **GLES path 至少保留到「Vulkan 穩定 6 個月」之後才移除**。期間 FRUC bug 修兩次（GLES + Vulkan）是已知 +20% 維護成本。

### 已就位的診斷工具（Phase B+ 會用到）

- `scripts/benchmark/android/android_baseline.sh` — thermal + fps + jank 採集（已 ship）
- `scripts/benchmark/android/analyze_baseline.py` — 30s bucket fps + pixel-thermal 高頻交叉驗證（已 ship）
- TBD：FRUC log 加 backend 名稱（GLES / Vulkan / Vulkan-async），方便事後 grep 比較

### ~~Phase A 的綠燈條件~~（已通過）

A 三個問題都拿到答案：
1. Pixel 5 Adreno 620 支援 `VK_GOOGLE_display_timing`？✅ YES（A2, v1.2.134）
2. `eglPresentationTimeANDROID` 能否替代？❌ NO，效果不彰（A1, v1.2.135 negative result）— 仍維持往 Vulkan 推進
3. `AHardwareBuffer` 能否 import 進 VkImage？✅ YES（B.2c.3b, v1.2.144 驗證）

---

## J. Desktop Client Vulkan-native（v1.3.45+，2026-04-28 規劃）

**動機：** §I.F NCNN-Vulkan FRUC backend 在 D3D11 主 renderer 上踩到結構性瓶頸：

1. **D3D11 → Vulkan bridge cost ~30-40ms/frame**（CPU staging 路徑），讓 RIFE inference 21ms 只佔總耗時的 33%。Cascade probe 退讓給 Generic 太頻繁，user 看不到 ML 補幀的視覺品質。
2. **Shared texture 路線在 NVIDIA 596.84 driver device lost** — `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT` 任何 raw Vulkan command 都被 reject。要 ID3D11Fence + VkSemaphore 完整同步基礎設施才有機會（v1.3.43 三次嘗試都失敗，見 §J.1）。
3. **D3D11 deprecation pressure** — Microsoft 主推 D3D12 + WinUI；D3D11 video decoder 不太可能加新 codec（AV2 / VVC）。
4. **Cross-platform 一致性** — 我們 Android client 已是 Vulkan-native（§I 全套 ship），desktop 也走 Vulkan 後 codebase 大幅簡化。

**長期目標：** 完整 Vulkan-native PC client，eliminate D3D11/Vulkan bridge，NCNN 從 unusable 變 usable，HDR pipeline 由 swapchain 硬體處理。

### 效能預估

| Path | 現狀 (D3D11 + bridge) | Vulkan-native 預估 | 改善 |
|---|---|---|---|
| Generic FRUC | ~5-7ms / frame | ~5-6ms | -5-7% (marginal) |
| **NCNN-Vulkan FRUC** | **43-80ms** | **17-38ms** | **2-5x speedup** |
| HDR pipeline | sRGB→PQ shader (~0.3ms) | swapchain HDR10 硬體 | -0.3ms + 色彩更準 |
| Frame pacing | DXGI frame stats（粗）| VK_KHR_present_wait（精準）| 高 refresh rate 場景 |

**真正受益：NCNN backend**。其他 path 改善 marginal，但長期統一 codebase 跟 future-proof 才是主因。

### 階段（每階段獨立 commit、可獨立 ship）

| Phase | 工時估 | 內容 | Ship 樣貌 | 狀態 |
|---|---|---|---|---|
| **J.1** D3D11.4 fence ↔ VkSemaphore bridge | 1-2 weeks | ID3D11Fence + VK_KHR_timeline_semaphore + DXGI shared handle，patch ncnn 加 external_semaphore_win32 ext。submitFrameShared 用 fence sync 取代 ID3D11Query event poll + vkQueueWaitIdle | NCNN shared path 真的能跑（cascade 真選上時不再 device lost） | ❌ **DEAD END on NV 596.84** (v1.3.47, 121dd72) — 4 種 ablation 全部 rc=-4 device lost。Driver-level 拒絕，application 無解。Fallback 機制 work。下一步要 path A (ID3D12Device bridge) 或 path B (跳 J.3) |
| **J.2** Vulkan post-processing pipeline | 2-3 weeks | Generic FRUC、HDR shader、blit、swapchain present 全部移到 Vulkan。Decoder 暫保 D3D11VA + 走 J.1 的 fence sync bridge | 預設仍走 D3D11；CLI flag `--renderer vulkan` opt-in | 規劃中 |
| **J.3** VK_KHR_video_decode AV1 | 3-4 weeks | 完整 Vulkan-native decoder，eliminate D3D11VA → Vulkan bridge。透過 ffmpeg 6.1+ Vulkan video hwaccel 包裝 DPB lifecycle | NCNN 無 bridge cost，真實 17-38ms / frame | 規劃中 |
| **J.4** HEVC + H264 decode + 跨平台驗證 | 2-3 weeks | Linux full coverage；macOS 路徑決策（VideoToolbox+Metal vs MoltenVK hybrid，後者 NCNN 在 mac 不可用） | 跨 OS 一致 | 規劃中 |
| **J.5** 整合測試 + fallback hardening + 預設切換 | 1-2 weeks | NV / AMD / Intel 各跑 bench；舊 driver 退回 D3D11 renderer；預設改 Vulkan | Vulkan 預設 ON，D3D11 留 fallback | 規劃中 |

**總工時估：** ~10-14 週（一個 engineer full-time），~2800-3000 LOC。

### J.1 細部子計畫（衝刺中）

NCNN shared path 在 NV 596.84 device lost (v1.3.43) → 缺跨 API sync。J.1 加 ID3D11Fence + VkSemaphore 完整同步，驗證 driver 加 fence 後是否買單。**此 phase 範圍小、scope 明確、可隨時 ship**，做了不浪費 — fence bridge 在 J.2/J.3 都會用到。

| Sub-phase | 內容 | 風險 |
|---|---|---|
| **J.1.a** Patch ncnn 加 timeline_semaphore + external_semaphore_win32 | ncnn 自家 VkDevice 不 enable timeline semaphore extension。仿 v1.3.30 的 `VK_KHR_external_memory_win32` patch，在 `gpu.cpp:1898` VulkanDevice ctor 加上：`VK_KHR_external_semaphore` / `VK_KHR_external_semaphore_win32` / `VK_KHR_timeline_semaphore` 三個 ext，加 `VkPhysicalDeviceTimelineSemaphoreFeaturesKHR`。重 build ncnn.dll | 中；ncnn 20220729 的 glslang 版本對 timeline semaphore 有沒有 implicit dep 要驗 |
| **J.1.b** D3D11.4 ID3D11Fence 創建 + DXGI shared NT handle 匯出 | `m_Device->QueryInterface(ID3D11Device5)` → `CreateFence(0, D3D11_FENCE_FLAG_SHARED)` → `ID3D11Fence::CreateSharedHandle`。NcnnFRUC 多 ID3D11Fence m_D3D11Fence 跟 HANDLE m_FenceSharedHandle | 低；D3D11.4 在 Windows 10 1703+ 全部支援 |
| **J.1.c** Vulkan 端 VkSemaphore import via D3D12_FENCE handle type | 動態 load `vkImportSemaphoreWin32HandleKHR` from vulkan-1.dll；create VkSemaphore with `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT`（spec 確認此 type 同時吃 D3D11 跟 D3D12 fence）+ `VkSemaphoreTypeCreateInfo`(timeline)；store m_VkFenceSemaphore | 中；handle type 命名誤導，要實測 NV driver 是否認 D3D11 fence |
| **J.1.d** submitFrameShared 用 fence Signal/Wait 取代 event-poll + waitIdle | 移除 ID3D11Query event poll loop。每 frame：(1) `ctx->Signal(m_D3D11Fence, m_FenceValue)` after RTV write (2) `vkQueueSubmit` 帶 `VkTimelineSemaphoreSubmitInfo` waitValue=m_FenceValue (3) image ops 跑（vkCmdCopyImageToBuffer 等）(4) submit 結尾 signalValue=m_FenceValue+1 (5) `ctx->Wait(m_D3D11Fence, m_FenceValue+1)` before D3D11 reads m_OutputTex (6) m_FenceValue += 2 | 中；submit info chain pNext 順序、timeline semaphore submit info 要 v1.2 core 或 KHR ext path |
| **J.1.e** End-to-end stream test | 用 `--fruc-backend ncnn` + 720p/60fps 觸發 NCNN cascade 真選上；觀察 submitFrameShared 全部 vkQueueSubmit 是否成功；real/interp 比例；無 device lost | 高；J.1 整體 risky 點 — 這步證明 NV driver 加 fence 後接受 imported VkImage access |
| **J.1.f** 整合 + commit | clean up；3-fail fallback 機制保留作 safety net；commit `vX.Y.Z: §J.1 — D3D11.4 fence + VkSemaphore bridge`. 如果 J.1.e 通過，update `project_phase_b_dead_end.md` memory 反映新進展 | 低 |

### J.1 後決策點

**情境 A — J.1 通過（NV driver 加 fence 後接受 image access）：**
NCNN shared path 真實可用。bridge cost 從 30ms 降到 ~5ms (timeline semaphore signal/wait + memory barriers)。**1080p NCNN 在 90fps half-rate budget 11ms 還是過不了，但在 60fps half-rate budget 16ms 過得了** — user 在中等 fps 場景就有 ML 補幀畫質提升。Phase J.2 開始規劃。

**情境 B — J.1 失敗（NV driver 還是 device lost）：** **← 實測進入此情境 (v1.3.47, 121dd72)**

v1.3.45-47 完整測試 4 種 ablation（裸 baseline / 加 fence / 換 D3D12_RESOURCE_BIT handle / 換 TOP_OF_PIPE wait stage），全部 vkQueueSubmit rc=-4 device lost。NV 596.84 driver 對 D3D11_TEXTURE_BIT import 嚴格到連 fence 都不認，**driver-level 拒絕，application 完全沒解**。

三條路（按優先順序）：
1. **路線 A** — 完整 ID3D12Device bridge (~250 LOC)：建第二個 D3D12 device 共用 adapter，從 D3D12 端 OpenSharedHandle 拿 ID3D12Resource，再 CreateSharedHandle 出新 handle 餵給 Vulkan。**未驗** — NV 對 D3D11-origin GPU memory 是否視為 D3D12 lineage 不確定。
2. **路線 B** — 跳 §J.3 (~800 LOC + ffmpeg 6.1+ integration)：完整 Vulkan-native decoder，bridge 整個消失。是 §J workstream 原本規劃的 phase 3，**長期答案**。
3. **路線 C** — 接受 dead end：NCNN backend 在 NVIDIA 永久 CPU staging path。fallback 機制 (v1.3.41-44) 保證 user 預設無感。**短期可行**。

**目前 (v1.3.47) 等於走路線 C** — m_SharedPathReady 還是 ENABLE 但 submitFrameShared 第 3 次 device lost 後永久 disable，等於 path C 行為。要繼續推 §J 必須選 A 或 B。

### 不可動的鐵律 (§J)

1. **fallback 機制保留** — v1.3.41 的 3-fail fallback、v1.3.44 的 process-lifetime singleton 都不能移除。Phase J 改動失敗時不能讓 user crash。
2. **預設行為跟 v1.3.44 等價** — 直到 Phase J.5 預設切 Vulkan，預設 user 走 D3D11 主路徑（CLI flag opt-in）。
3. **D3D11 renderer 留作 legacy fallback** — driver 不支援 VK_KHR_video_decode 的 user 退回 D3D11VA。預計支援到 2027 年（Win10 EOL + 主流 driver 都更新到 VK 1.3.274+）。
4. **每 Phase 都要有 baseline 對比** — 沿用 §I 的 baseline.sh 設計，desktop 版自寫一份。

### 已就位的診斷工具（會用到）

- `[VIPLE-FRUC-NCNN]` log family — Phase B/J 全用同一系列
- `VIPLE_FRUC_NCNN_SHARED=1` env var — Phase J.1 用此 opt-in 觸發 fence path
- `loadModel: step N/6` trace log — v1.3.39 加的 step trace，定位 init crash 位置
- 3-fail fallback counter — v1.3.41 機制，phase J 重用

每個子 phase 的 commit message 用 `vX.Y.Z: §J.N.M — <短摘要>` 格式。

---

## 如何使用這份清單

- 動某條相容性債 → **不要**單獨 commit「改名」，要一起 ship migration code + release notes
- 每動完一條 → 移到「已完成」區並寫上 commit hash / 版號
- 新發現的債照 §A.N / §B / §C 風格加進來
- 真的不打算做的（A.7、A.5）就明確寫「不該清」並給理由

---

## 優先級總覽（我的判斷）

依「使用者明顯受益 / 工作量 / 風險」排序，給你決定先動哪條的參考。

| 優先級 | 條目 | 理由 |
|---|---|---|
| **Medium** | **§A.1+A.11** QSettings org name + ini 名稱遷移 | 升級 v1.2.43 起的使用者「設定看起來像 reset 了」是真實 regression；只是大家手動把 paired hosts 重 add 過去就不痛了，所以沒人特別抱怨。動之前要寫並測 migration |
| **High** | **§J** Desktop Client Vulkan-native | NCNN ML 補幀真實可用的唯一解；長期統一跨平台 codebase；future-proof D3D11 deprecation。J.1 衝刺中（D3D11.4 fence + VkSemaphore 解 NV driver device lost），J.2-5 視 J.1 結果決定 |
| **Medium-High** | **§I** Android Vulkan 路徑（HDR + 降延遲） | 兩個 roadmap 需求一條解。**Phase A+B 已完成 (v1.2.134–150)**，VkPassthroughBackend live 跑通；現在進 Phase C（Vulkan FRUC 端到端 port），分 7 子 phase 估 5–7 day。C.1 spec 已 ship (v1.2.149)，C.2 等觸發 |
| **Medium-Low** | **§A.6** HTTP Basic auth realm | 改了使用者要重登 Web UI；除非有別的 reason 一起改，否則 standalone 改一條不划算 |
| **Low** | **§F** DirectML pipeline 搬 D3D12 / bundles | 4K120 real-time 才需要的架構改動；現在 1080p240 已達標 |
| **Low** | **§G.1** 11-channel RIFE v1 | A1000-tier 仍可能 launch overhead 超 budget（§G.3 negative result 之後），主要 win 在 RTX 30/40+；要 ship 得搭 §G.4 |
| **Low** | **§G.4** 模型下載管理 | RIFE fp32 + fp16 兩個 onnx 已 bundle (~33 MB)；IFRNet-S 5.8 MB 進 repo 當 reference 但**故意不 bundle 進 cascade**（§G.3 negative result）；要加新 model 才會用到 |
| **Low** | **§E** Android themed icon | Material You 一致性 nice to have；多數使用者不會在意 |
| **Low** | **§C** 其他 20 個語系檔 | 英文 + 繁中 cover 主要使用者；剩下純品牌一致性 |
| **Low** | **§D** wiki / docs 連結 | 在 docs 有實際內容前換連結沒意義 |
| **Low** | **§A.2** WiX installer | 我們根本沒用 MSI 出貨；要哪天改用才需要清 |
| **Low** | **§A.8** 內部 class names | 純開發者看到，做了 diff 又難 review |
| **Won't fix** | **§A.7** Wire-protocol 字串 | 動了等於跟 Moonlight 生態斷線，違背「混搭互聯」設計目標 |
