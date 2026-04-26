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

全部 ship（見「已完成」表的 H.Phase 1+2+3 / H.4 / H.5 列）。本章 close。

未來如果 SettingsView 加了「只對 VipleStream-Server peer 才有意義」的設定（目前還沒有，所有 client-side 設定如 Relay / FRUC / 影格節奏都跟 host 類型無關），再回頭加 `isVipleStreamPeer` 條件隱藏。Vanilla peer 反向 badge（「Standard Sunshine / GFE」灰色 chip）討論過但暫不做 — 對純 vanilla 使用者會變成常駐視覺雜訊。

---

## E. moonlight-android Icon 補完

**目前：** v1.2.30 已用 lime VipleStream icon，但：
- Splash screen 需確認用新 icon
- 「最近應用程式」列表 icon 在各版本 Android 上的顯示要驗證
- Android 12+ themed icon（Material You 單色化）沒提供 monochrome mask → 會 fallback 到 adaptive-icon。要支援需加 `ic_launcher_monochrome.xml`

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
| **Medium-Low** | **§A.6** HTTP Basic auth realm | 改了使用者要重登 Web UI；除非有別的 reason 一起改，否則 standalone 改一條不划算 |
| **Low** | **§F** DirectML pipeline 搬 D3D12 / bundles | 4K120 real-time 才需要的架構改動；現在 1080p240 已達標 |
| **Low** | **§G.1** 11-channel RIFE v1 | A1000-tier 仍可能 launch overhead 超 budget（§G.3 negative result 之後），主要 win 在 RTX 30/40+；要 ship 得搭 §G.4 |
| **Low** | **§G.4** 模型下載管理 | RIFE fp32 + fp16 兩個 onnx 已 bundle (~33 MB)，IFRNet-S 5.8 MB 還沒 ship；要加新 model 才會用到 |
| **Low** | **§E** Android themed icon | Material You 一致性 nice to have；多數使用者不會在意 |
| **Low** | **§C** 其他 20 個語系檔 | 英文 + 繁中 cover 主要使用者；剩下純品牌一致性 |
| **Low** | **§D** wiki / docs 連結 | 在 docs 有實際內容前換連結沒意義 |
| **Low** | **§A.2** WiX installer | 我們根本沒用 MSI 出貨；要哪天改用才需要清 |
| **Low** | **§A.8** 內部 class names | 純開發者看到，做了 diff 又難 review |
| **Won't fix** | **§A.7** Wire-protocol 字串 | 動了等於跟 Moonlight 生態斷線，違背「混搭互聯」設計目標 |
