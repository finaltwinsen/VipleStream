# VipleStream — 待辦 / 技術債清單

這份文件記錄**明確知道之後要做、但目前刻意沒做**的事。每一條都要
寫清楚「為什麼先這樣保留 / 何時該清 / 清的時候要做什麼遷移」，不
要只寫「以後改」。

---

## 已完成（保留紀錄，後續維護用）

未 commit 但已落實在 working tree（v1.2.93–v1.2.96 那批）。註明為了
之後要 commit 時這份檔案能跟 git log 交叉對照。

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
| H.Phase 1+2+3 | Steam library 自動匯入 + applist 多送 5 個 metadata XML tags + multi-mode sort | v1.2.67 + v1.2.96 | applist 端 emit `<Source>` / `<SteamAppId>` / `<SteamOwners>` / `<LastPlayed>` / `<Playtime>`（空值省略）；client 端 NvApp 補對應欄位 + comparator 把 `source==""` pin 到頂；Sort 模式 DEFAULT/RECENT/PLAYTIME/NAME 三端（Qt + Android）。Phase 4（client 觸發切 Steam 帳號）還沒做。 |
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

## B. Discord Rich Presence 獨立 App

**目前：** `moonlight-qt/app/backend/richpresencemanager.cpp:14` 仍用上游 Moonlight 的 Discord App ID `594668102021677159`。功能正常，但 Discord 顯示 app 名稱是「Moonlight」。

**要做：**
1. <https://discord.com/developers/applications> 註冊新 App，命名 `VipleStream`，上傳 lime icon
2. 換 App ID
3. 對齊 `largeImageKey = "icon"` 的 asset key
4. 拿掉 SettingsView.qml 那段「currently shown as 'Moonlight' because...」tooltip 註解

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

### G.1 11-channel 輸入 (RIFE v4 v1)

**Gain：** v1 node count 少 50%（216 vs 456），可能比 v2 快 30-50%。
**做法：** `tryLoadOnnxModel` 加 11-channel branch；pack 前加 optical flow CS 或塞 zero flow。

### G.2 FP16 model variant

**Gain：** 2-3× 速度（weight memory 砍半 + Tensor Core packed math）。
**Risk：** v1.2.61 試過 onnxconverter_common 簡單轉換失敗，需要 `auto_convert_mixed_precision` 配 calibration set，per-node fallback 不能 mixed-precision 的 op。

### G.3 嘗試其他架構（IFRNet / FILM / M2M）

**動機：** A1000 等中階 NVIDIA 卡跑 RIFE 4.25 lite 撐不到 1080p；可能 IFRNet-S 或 EMA-VFI-light 在同硬體上能 1080p。
**做法：** 加 `VIPLE_FRUC_ONNX` env var override 模型路徑，benchmark harness 量速度 + 視覺品質，出「model × GPU tier」推薦表。

### G.4 模型下載管理

**動機：** `fruc.onnx` 22 MB 跟 release zip 一起出。多數 NVIDIA user 用 NvOF / Generic，DML 是少數派。
**做法：** Moonlight 第一次選 DML 時跳「下載 22 MB」dialog，model 放 `%LOCALAPPDATA%\VipleStream\fruc_models\`。低優先度。

### 已就位的診斷工具（沿用）

- `VIPLE_DIRECTML_DEBUG=1` — D3D12 debug layer + DML validation
- `VIPLE_DML_RES=540|720|1080|native` — tensor 解析度 override
- `[VIPLE-FRUC-DML]` log — 每 120 frame 印 per-stage EMA μs
- `[VIPLE-FRUC-DML-ORT]` log（v1.2.83）— ort_concat / ort_run / ort_post / wait_pack / wait_post / wait_concat 拆解

---

## H. Steam library 自動匯入 — Phase 4

Phase 1+2+3 已 ship（v1.2.67 + v1.2.96 補回 applist Source emission）。剩 Phase 4 + UI affordance。

### H.4 Client 觸發 host 切 Steam 帳號

- Sunshine `POST /api/steam/switch` body `{ target_steam_id3: "..." }`：
  1. 檢查 target profile 有 `RememberPassword=1`，沒有就回 400
  2. target 已是 current_user → 204 no-op
  3. `steam.exe -shutdown` → poll `ActiveUser==0` → `steam.exe -login <username>` → poll `ActiveUser==target` (max 30s)
  4. 成功 → 200 + 新 current_user；失敗 → 500
- Moonlight client：profile dropdown 選 ≠ current_user → spinner + tooltip → call API → 重抓 apps；失敗回 fallback。

### H.5 Capability marker UI badge（v1.2.93 加了 marker 但 UI 沒接）

**目前：** `<VipleStreamProtocol>1</VipleStreamProtocol>` 在 serverinfo 已 emit、Qt `NvComputer.isVipleStreamPeer` 跟 Android `ComputerDetails.isVipleStreamPeer` 都會被填好，**但 UI 沒有用到這個旗標**。

**要做：**
1. PcView 的 host card 上：peer 是 VipleStream 時顯個小 badge（lime 色 `V` icon 或「VipleStream-Server」標籤）
2. 連到 vanilla Sunshine 時可在 Settings 入口隱藏 VipleStream-only 設定（譬如 Steam profile dropdown — 反正連 vanilla 也沒這資料）
3. 或反過來：vanilla peer 的 host card 顯示「Standard Sunshine / GFE」灰色 badge，提示某些功能不會作用

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
| **High** | **§H.4** Steam 帳號切換 | 跟 Phase 1-3 是同一條 feature 的尾巴；現在使用者要切帳號得跑去 host 端手動切，UX 落差大；技術風險可控（已有 Phase 3 endpoint scaffolding 可參考） |
| **High** | **§H.5** Capability marker UI badge | 投入最小（每端 ~30 行 QML / Java），讓 v1.2.93 加的 marker 真的有用；不接 UI 等於白做 |
| **Medium** | **§A.1+A.11** QSettings org name + ini 名稱遷移 | 升級 v1.2.43 起的使用者「設定看起來像 reset 了」是真實 regression；只是大家手動把 paired hosts 重 add 過去就不痛了，所以沒人特別抱怨。動之前要寫並測 migration |
| **Medium** | **§B** Discord App ID | 純 cosmetic 但 Discord 那邊出現「Moonlight」很明顯不對勁；只要去 Discord developer portal 註冊 + 換 ID 兩行 code |
| **Medium** | **§G.2** FP16 ONNX 模型 | DirectML 在 RTX 30/40 主流卡的 sweet spot 解鎖；A1000 上預期 2-3× 速度，搞不好能直接吃 1080p。實作風險中，但 v1.2.61 已經知道哪幾個坑要避開 |
| **Medium-Low** | **§A.6** HTTP Basic auth realm | 改了使用者要重登 Web UI；除非有別的 reason 一起改，否則 standalone 改一條不划算 |
| **Low** | **§G.3** 試其他模型架構（IFRNet / FILM） | 跟 §G.2 同一個目標（DML 變實用），但比 §G.2 工作量大、收益不確定；建議先做 §G.2 確認能不能省事 |
| **Low** | **§F** DirectML pipeline 搬 D3D12 / bundles | 4K120 real-time 才需要的架構改動；現在 1080p240 已達標 |
| **Low** | **§G.1** 11-channel RIFE v1 | 跟 §G.2/G.3 重疊；只在前面兩條都失敗才考慮 |
| **Low** | **§G.4** 模型下載管理 | 22 MB 沒人抱怨；release size 不是痛點 |
| **Low** | **§E** Android themed icon | Material You 一致性 nice to have；多數使用者不會在意 |
| **Low** | **§C** 其他 20 個語系檔 | 英文 + 繁中 cover 主要使用者；剩下純品牌一致性 |
| **Low** | **§D** wiki / docs 連結 | 在 docs 有實際內容前換連結沒意義 |
| **Low** | **§A.2** WiX installer | 我們根本沒用 MSI 出貨；要哪天改用才需要清 |
| **Low** | **§A.8** 內部 class names | 純開發者看到，做了 diff 又難 review |
| **Won't fix** | **§A.7** Wire-protocol 字串 | 動了等於跟 Moonlight 生態斷線，違背「混搭互聯」設計目標 |
