# VipleStream — 待辦 / 技術債清單

這份文件記錄**明確知道之後要做、但目前刻意沒做**的事。每一條都要
寫清楚「為什麼先這樣保留 / 何時該清 / 清的時候要做什麼遷移」，不
要只寫「以後改」。

---

## A. 相容性包袱（v1.2.43 rebrand 時刻意沒清）

v1.2.43 的 rebrand（Sunshine → VipleStream-Server、Moonlight →
VipleStream）只改了使用者看得到的 display 字串，底下這些**識別
字串 / 協定字串 / 檔案路徑**維持上游原名。理由都是一樣的：改了
會讓既有使用者的安裝、設定、paired hosts、saved credentials 在
升級時無聲無息地壞掉，而且 fork 目前還沒有 migration 機制把舊
狀態搬到新 key。

整批清除的時機應該綁在**下一個 major 版號跳點**（例如 v2.0），
並且**必須**同時實作 migration，而不是改名了就算。每一項都附上
要寫的 migration 邏輯。

### A.1 `QSettings` organizationName

**目前：** `moonlight-qt/app/main.cpp` 維持 `setOrganizationName("Moonlight Game Streaming Project")`。  
**現象：** Windows 上使用者的 paired hosts、偏好設定全部存在
`HKCU\Software\Moonlight Game Streaming Project\VipleStream` 底下。
改 org name → 路徑換成 `HKCU\Software\VipleStream\VipleStream`，
原先的 key 會變成無主，使用者體感是所有 paired hosts 一夕消失。  
**清的時候要做：**
1. 新版啟動時先用舊 org / app name 構造一個 `QSettings`，檢查
   `HKCU\Software\Moonlight Game Streaming Project\VipleStream` 還在不在
2. 如果在 → 讀出所有 key/value，寫到新的 `QSettings("VipleStream", "VipleStream")`
3. 把舊 registry 樹砍掉（或留一個 migrated-flag 避免重複搬）
4. macOS / Linux 同樣要處理 `~/.config/Moonlight Game Streaming Project/` → `~/.config/VipleStream/` 搬遷
5. 加一個 unit test 覆蓋：初始狀態 / 已遷移 / 部分遷移三種情境

### A.2 Windows registry paths

**目前：** WiX installer 的 `HKCU\Software\Moonlight Game Streaming Project` 路徑（裝一次安裝狀態旗標 `Installed` / `DesktopShortcutInstalled` / `DesktopShortcutInstallState`）維持原名。  
**影響檔案：** `moonlight-qt/wix/Moonlight/Product.wxs`、`moonlight-qt/wix/MoonlightSetup/Bundle.wxs`。  
**清的時候要做：**
1. 新的 HKCU 路徑選 `HKCU\Software\VipleStream`
2. WiX 升級邏輯要在 install 階段偵測舊 key 存在 → copy 到新 key → 砍掉舊 key
3. 注意 DeleteRegistryKey custom action 裡的路徑也要跟著改（不然
   uninstall 砍不到東西）

### A.3 WiX `InstallFolder` + `APPDATAFOLDER`

**目前：**  
- `InstallFolder = "Moonlight Game Streaming"` → 裝在 `C:\Program Files\Moonlight Game Streaming\` 裡面  
- `APPDATAFOLDER = %LOCALAPPDATA%\Moonlight Game Streaming Project`  
**現象：** 使用者從檔案總管看到的路徑跟品牌對不上（VipleStream app
結果裝在 `Moonlight Game Streaming` 資料夾）。  
**清的時候要做：**
1. 新 InstallFolder = `VipleStream`、新 APPDATAFOLDER = `%LOCALAPPDATA%\VipleStream`
2. MSI 升級邏輯：偵測舊路徑 → 先 copy 檔案 → 更新捷徑指向新路徑 → 砍舊資料夾
3. 需要新的 `UpgradeCode` GUID，否則 MSI 會嘗試 in-place upgrade 但找不到舊路徑
4. 準備 release notes 說明「從 v1.2.x 升上來 app 路徑會搬」

### A.4 Windows service name `SunshineService`

**目前：** `Sunshine/src/entry_handler.cpp:138` 維持 `OpenServiceA(scm_handle, "SunshineService", ...)`，對應的服務控制 class name 也是 `SunshineService`。  
**影響：** VipleStream-Server 裝起來後，Windows 服務列表顯示名稱是 `Sunshine Service`（upstream 設的 DisplayName）。  
**清的時候要做：**
1. 新 service name = `VipleStreamServerService`（類似 Win 慣例 —
   CamelCase，沒有底線）
2. Service installer 要：偵測舊 `SunshineService` 存在 → 停掉 →
   解除註冊 → 註冊新 name
3. 所有叫 `OpenServiceA(..., "SunshineService", ...)` 的地方全部改
   （grep `SunshineService` 看一次）
4. 別忘了 DisplayName 欄位（顯示給使用者看的名字）也要改成
   `VipleStream-Server`

### A.5 mDNS service type `_sunshine._tcp`

**目前：** Sunshine 宣告的 mDNS service type 是 `_sunshine._tcp`，
moonlight-qt / moonlight-android 的 discovery 只 query 這個 type。  
**影響：** 如果只改 server 端不改 client 端，client 找不到
server；反之亦然。這是所有相容性包袱裡**最敏感**的一條。  
**清的時候要做：**
1. 新 type = `_viplestream._tcp`
2. **Server 端同時 advertise 兩個 type**（`_sunshine._tcp` + `_viplestream._tcp`）一到兩個版本，讓舊 client 還找得到
3. Client 端**同時 query 兩個 type**，把結果去重（用 host UUID 比對）
4. 最後一個過渡版本移除 `_sunshine._tcp` advertise + query
5. 絕對不要一次改完 —— 會直接讓所有「舊 server + 新 client」或
   「新 server + 舊 client」的組合變成找不到對方

### A.6 HTTP Basic auth realm `"Sunshine Gamestream Host"`

**目前：** `Sunshine/src/httpcommon.cpp:132` 用這個字串當 HTTP Basic
auth 的 realm。Web UI 登入時 browser 會把 username/password 存在
這個 realm 下。  
**影響：** realm 一改，browser 的已存憑證就跟新 realm 對不上，使用
者會被要求重新輸入帳號密碼。  
**清的時候要做：**
1. 新 realm = `"VipleStream-Server Web UI"`
2. 跟 A.7 一起做：WWW-Authenticate header 更新、client 端如果有 hard-coded
   的 realm 字串也要改
3. Release notes 要寫「升級後 Web UI 會要求重新登入」

### A.7 協定 / wire-format 字串

**目前：** 以下字串出現在 HTTP 回應、serverinfo XML、user-agent header
等會被 peer 解析的地方，一律保留上游原名：
- `"NVIDIA GameStream"`（HTTP 回應的 server 識別）
- certificate CN (`"Sunshine Gamestream Host"`)
- User-Agent header 裡的 `Moonlight/...` 版本字串

**現象：** 這些是真正的「wire protocol」字串，peer 會拿來判斷對方
是不是合法的 GameStream 實作。改了 → 舊 peer 拒絕握手。  
**清的時候要做：**  
**不該清。** 這層就是 GameStream 協定的一部分，改了等於自己跟整個
Moonlight / Sunshine 生態脫鉤（使用者不能再用官方 Moonlight client
連到 VipleStream-Server、也不能用 VipleStream client 連到別人家的
upstream Sunshine）。這條保留到永遠。

### A.8 Class names / 內部識別字

**目前：** `NvHTTP` / `NvComputer` / `NvApp` / `SunshineHTTPS` /
`SunshineHTTPSServer` / `SunshineSessionMonitorClass`（Windows window
class）等等內部名字都沒改。  
**清的時候要做：**  
低優先度。這些只有開發者看到，不影響使用者。**真的想改**就在某個
refactor PR 裡 batch rename，但**不要**跟 feature / bugfix 混在一起
（diff 會完全不能 review）。

### A.9 Binary filenames `Moonlight.exe` / `Sunshine.exe`

**目前：** build 產物還是 `Moonlight.exe`、`Sunshine.exe`。  
**影響：** 很多 build script、installer、捷徑、PDB 檔名、windeployqt
的參數都 hardcode 了這兩個檔名。全部搜過才會找齊。  
**清的時候要做：**
1. Qt 端：改 `moonlight-qt/app/app.pro` 的 `TARGET = VipleStream`，
   配合改 main manifest、WiX Source 路徑、build_moonlight_package.cmd
2. Sunshine 端：改 CMake `add_executable()` target name
3. 使用者已裝的捷徑會指向舊檔名 → installer 要負責更新捷徑 /
   刪舊 exe
4. PDB 檔名會跟著改 → crash 回報系統要能同時解析舊 / 新 PDB 一段時間

### A.10 Linux desktop file ID `com.moonlight_stream.Moonlight`

**目前：** `moonlight-qt/app/deploy/linux/com.moonlight_stream.Moonlight.desktop`
的檔名本身就是 desktop app ID。`main.cpp:909-911` 的
`setDesktopFileName("com.moonlight_stream.Moonlight")` + Wayland /
X11 WMCLASS 也是這個字串。  
**影響：** 改了會讓：
- GNOME / KDE 的釘選捷徑遺失
- 桌面環境把 app icon 對不上 window（WMCLASS 跟 .desktop 檔名要一致）
- AppStream metainfo 的 `<id>` + `<launchable>` 也要跟著改  
**清的時候要做：**  
1. 新 ID = `com.viplestream.Client`（反向 FQDN）
2. `.desktop` 檔改名、`.appdata.xml` 的 `<id>` + `<launchable>` 改、
   `main.cpp` 裡 `setDesktopFileName` + WMCLASS env 改一致
3. Flatpak / Snap / AppImage 打包的 manifest 都要跟著改

### A.11 QSettings `applicationName` 目前叫 `VipleStream`

**目前：** 這條不是相容性包袱，是**主動做了**的改動 —— v1.2.43 的
main.cpp 已經把 `setApplicationName` 從 `"Moonlight"` 改成
`"VipleStream"`。  
**提醒：** 這代表 settings 檔案從 `.../Moonlight Game Streaming Project/Moonlight.ini`
搬到 `.../Moonlight Game Streaming Project/VipleStream.ini`。使用者
升到 v1.2.43 的第一次啟動會「看起來像是回到預設設定」。這個 regression
**還沒修**。  
**要做：** A.1 的 migration 也要順便處理這條 —— 讀舊
`Moonlight.ini` / `Moonlight` registry key → 寫到新的
`VipleStream.ini` / `VipleStream` key。

---

## B. Discord Rich Presence 的獨立 Discord App

**目前：** `moonlight-qt/app/backend/richpresencemanager.cpp:14` 使用
上游 Moonlight 的 Discord App ID `594668102021677159`。功能完全正常
（狀態字串 / 時戳 / 圖示全部會出現），但 Discord 那邊會把應用程式
名稱顯示成「Moonlight」。  
**要做：**
1. 在 <https://discord.com/developers/applications> 註冊新 Discord App，命名 `VipleStream`
2. 上傳 VipleStream 專用 icon（lime 色版本，跟 app icon 一致）
3. 把 richpresencemanager.cpp 裡的 App ID 換成新 App 的 client ID
4. 順便把 `discordPresence.largeImageKey = "icon"` 的資產名稱對齊
   新 App 後台上傳的 asset key
5. 拿掉 SettingsView.qml 那個長長的 tooltip 補充說明（「currently
   shown as 'Moonlight' because...」），因為不再需要了

---

## C. Sunshine Web UI 其他語系檔

**目前：** v1.2.43 只改了 `en.json` 和 `zh_TW.json`（英文 + 繁體中
文）裡的 Sunshine / Moonlight → VipleStream-Server / VipleStream。
另外 20 個語系檔（bg / cs / de / es / fr / hu / it / ja / ko / pl /
pt / pt_BR / ru / sv / tr / uk / vi / zh / en_GB / en_US）還是舊字
串。  
**要做：** 在 Sunshine Web UI 做 i18n 同步的任何 commit 裡順便對
這 20 個檔也做同樣的 `Sunshine` / `Moonlight` bulk replace。如果專
案未來有自動 Weblate / Crowdin sync，可以把這筆 rename 合進第一次
sync 的 seed data。  
**不急：** 英文 / 繁中 cover 了主要使用者，其他語系壞的只是品牌一
致性，不影響功能。

---

## D. 上游 wiki 連結

**目前：** `moonlight-android/app/src/main/java/com/limelight/utils/HelpLauncher.java`
的 setup guide / troubleshooting 現在都指向 `finaltwinsen/VipleStream#readme`
（專案首頁）；GameStream EOL FAQ 仍保留上游連結。  
**要做：** VipleStream 專案 wiki / docs 有實際內容（setup guide、
troubleshooting、常見錯誤碼解讀）後，把 HelpLauncher 的 URL 換成
指向 VipleStream 自己的 docs。或用 `#readme` 加 anchor 指向 README
裡的專門段落。

---

## F. DirectML FRUC — Option C+（interop 架構級優化）

**目前（v1.2.64 做完 Option C）：** DirectMLFRUC inline crossfade 跑到
real 89 fps（超越 Generic 85 fps）、p99 frame time 42 ms。per-stage
timing 量到 `ctx4->Signal` 是剩下的最大 dominant cost（~65 μs/frame
median，D3D11 UMD flush 到 GPU 才 signal fence 的固定成本）。

Option C 的戰術性優化（ring allocator 等）已經打完拿 +4-5 fps 收工。
再往下需要架構級改動：

### F.1 把 FRUC pipeline 整個搬進 D3D12

**預估 gain：** 消除每 frame 60-80 μs 的 `ctx4->Signal`（D3D11 Signal）
成本。可能換來 real +2-3 fps 並顯著降低 p99 tail。

**做法：**
- 現在流程：D3D11 decoded frame → D3D11 RTV → `ctx4->Signal` →
  D3D12 pack CS → DML → D3D12 unpack CS → D3D11 blit → Present
- 改成：D3D11 decoded frame → 直接 D3D12 CS (pack/DML/unpack) →
  D3D12 直接 present
- D3D11VA decoder 依然用 D3D11，但 decoded texture 透過 SHARED_NTHANDLE
  open 到 D3D12 後所有後續 work 都在 D3D12。Present 也用 D3D12 SwapChain。

**風險：** 高。整個 renderer 的 present / blit / overlay 管線都要重
改成 D3D12 path。Moonlight 原本的多 backend 抽象（d3d11va / dxva2 /
vaapi）要重新設計。預估工作量 1-2 週。

**觸發條件：** 未來要推 DirectML 跑 4K120 real-time 或更高 target
時做。現在 1080p240 已經達標，不必強推。

### F.2 Command bundles for pre-recorded DML pipeline

**預估 gain：** 30-40 μs/frame（省 SetPipelineState / SetRootSig /
RecordDispatch 的重複 CPU work）。

**做法：** 把 pack→DML→unpack 那段 cmd list 錄一次成 `ID3D12GraphicsCommandList5`
bundle，每 frame 只 `ExecuteBundle` + 更新 binding。Descriptor bind 還是
per-frame 因為 ping-pong slot 會變，但 shader PSO 切換可以跳過。

**風險：** 中。Bundle 建立後 state 限制多（不能改 PSO、不能改 root
sig），要重新驗證所有 barrier 都在 bundle 外部。

### F.3 Zero-copy D3D11→D3D12 texture sharing

**預估 gain：** 10-20 μs/frame（省去 open shared handle 的隱含 memory
fence）。

**做法：** 改用 D3D12 tier-2 heap + D3D11 alias（OpenSharedHandle on a
heap 而非 resource），pack CS 直接從 heap 讀。要求 GPU 支援
`D3D12_FEATURE_D3D12_OPTIONS` 的 `ResourceHeapTier >= 2`（多數現代 GPU）。

**風險：** 中。Heap alias 的 state 管理比 texture share 更嚴格，要小
心 barrier 放置。

### 診斷工具已就位（v1.2.64 加的）

以下 env var / log 維持 active，做 F.x 時直接有儀器可看：
- `VIPLE_DIRECTML_DEBUG=1` — D3D12 debug layer + DML validation（需 Graphics Tools）
- `VIPLE_DML_RES=540|720|1080|native` — tensor 解析度 override
- `[VIPLE-FRUC-DML]` log — 每 120 frame 印 per-stage EMA μs（rtv / sig /
  wait / alloc_reset / pack_record / dml_record / unpack_record / close_exec /
  d3d12_signal / total）
- `[VIPLE-FRUC-DML-SUB]` log — d3d11_sync 的細分（rtv / sig / wait）

---

## G. DirectML FRUC — 更多 ONNX model variants

**目前（v1.2.61 做完）：** 支援 1-input concat 6-9 channel / 2-input /
3-input layout。預設 ship `tools/fruc.onnx` = RIFE v4.25 lite v2 (7-ch
FP32)。在中低階 GPU（A1000 / 4060）跑不到 real-time；在 RTX 40/50
等有 Tensor Core 的 GPU 有機會 real-time。

### G.1 支援 11-channel 輸入（RIFE v4 v1 variant）

**目前狀況：** RIFE v4.x 的 v1 export（例如 `rife_v4.25_lite.onnx`）是
11-channel concat（prev RGB + curr RGB + 雙向 flow 2×2 + mask 1 = 11），
需要外部預算好 optical flow 再塞進來。我們 code 只認 6-9 channel，
現在 reject。

**要做：**
1. 加 11-channel branch 在 `DirectMLFRUC::tryLoadOnnxModel`（directmlfruc.cpp:1321 附近）
2. 在 pack CS 之前加一個 optical-flow-compute CS（或直接呼 DML optical flow
   operator），輸出 5-channel（2×2 flow + 1 mask）補到 6-ch RGB 之後
3. 或者不補 flow，直接塞 zeros 進去那 5 個 channel，讓 model 自己處理
   （某些 RIFE v1 variant 接受 zero-flow 輸入，會 degrade 到 RGB-only mode）
4. 測 RIFE v4 v1（tools/fruc.onnx 舊版）vs v2（目前版）的品質 / 效能差異

**預期 gain：** v1 node count 少（216 vs 456），可能比 v2 快 30-50%。
品質看 v1 有沒有 expose flow 跟 vs-mlrt 預處理 step 是不是能省。

### G.2 FP16 model variant（穩定 pipeline）

**目前狀況：** onnxconverter_common 的 `float16.convert_float_to_float16`
在 RIFE v4 的 graph 上會留 type mismatch（Cast / Mul inputs），試過 5
種組合（keep_io_types / disable_shape_infer / op_block_list / 手動 fix
value_info / onnxsim + convert）都 ORT load 失敗。

**要做：**
1. 改用 `onnxconverter_common.auto_convert_mixed_precision`，per-node
   驗證（比較 FP32 reference vs FP16 output 的 numeric diff），
   不能通過的 op 自動 keep FP32
2. 或用 ONNX Runtime 的 `SessionOptionsAppendExecutionProvider` 的
   `"float16_mode"` provider option（如果支援 DirectML EP）
3. FP16 預期 2-3× 速度（weight memory halved, 某些 op 也受益 packed math）
4. 品質降級要量化（PSNR / SSIM vs reference），對補幀品質 OK 再 ship

### G.3 試其他架構（FILM / IFRNet / M2M）

**目前狀況：** 只試過 RIFE 系列。其他 SOTA frame interpolation 各有優缺：

| 架構 | 優勢 | 節點數 | 備註 |
|---|---|---|---|
| **FILM** (Google) | 對大運動特好、2022 版本 | ~400 | 官方釋出 TF2 model，ONNX export 需自己做 |
| **IFRNet** | 輕量（4× 比 RIFE v4 快） | ~150 | 品質稍差 RIFE，但對 low-power GPU 比較友善 |
| **M2M_PWC** | sub-pixel accurate flow | 大 | 品質 top，但一般 GPU 吃不消 |
| **RIFE v4.26+ heavy** | 品質比 lite 好 | 636 | 已試過，A1000 吃不動 |

**要做：**
1. 準備一個 `tools/fruc_alternates/` 放其他模型
2. 寫一個 `VIPLE_FRUC_ONNX` env var 讓 user override 模型路徑
3. 跑 benchmark script 同時量速度 + 視覺品質（要做 reference-based
   metric），出一張「model vs GPU tier」推薦表寫進 docs/fruc_backends.md

### G.4 模型下載管理（可選）

**痛點：** 22 MB 的 fruc.onnx 放進 release zip 讓每個使用者都帶。但
不是每個使用者都會用 DirectML FRUC（多數 NVIDIA user 會用 NvOF，
Intel / AMD user 才會選 DirectML）。

**要做：**
1. Moonlight 初次選 DirectML 時出 dialog：「是否下載 FRUC ML model？
   22 MB」
2. 把 model 放到 `%LOCALAPPDATA%\VipleStream\fruc_models\` 而非 exe 同目錄
3. 提供 dropdown 選 model（RIFE v4.25 lite v2 / RIFE v4.26 heavy / IFRNet）
4. Build zip 不帶 fruc.onnx（省 22 MB）；仍保留 inline crossfade fallback

**優先度：** 低，除非發現 release size 是個問題。

---

## H. Steam library 自動匯入 + 多使用者 profile 切換

**目前：** 使用者要手動在 Sunshine Web UI 一個一個加遊戲，指定
executable 路徑、icon、arguments。裝 100 個 Steam 遊戲就要加 100 次。

**要做：** 伺服器自動掃 Steam 已安裝遊戲 + 識別擁有者（機器上所有
登入過的 Steam 帳號）+ client 端 UI 可選 profile filter + 可觸發 host
切 Steam 帳號。

### Steam 資料模型認識

| 層級 | 位置 | 內容 |
|---|---|---|
| 機器級 | `<SteamRoot>\steamapps\appmanifest_<AppID>.acf` | 已安裝遊戲（跟哪個使用者登入無關） |
| 機器級 | `<SteamRoot>\steamapps\libraryfolders.vdf` | 所有 library 路徑（D:/E: 多碟） |
| 機器級 | `<SteamRoot>\config\loginusers.vdf` | 曾登入過的帳號 + `RememberPassword` 旗標 |
| 使用者級 | `<SteamRoot>\userdata\<SteamID3>\config\localconfig.vdf` | 該 user 的 entitlement / 玩過的遊戲 |
| Session | HKCU `Software\Valve\Steam\AutoLoginUser` | Steam 設定的「自動登入」帳號 |
| Session | HKCU `Software\Valve\Steam\ActiveUser` | **目前實際登入中**（0 = Steam 沒開、非 0 = 那個 SteamID3 在線） |
| 啟動 | `steam://rungameid/<AppID>` | URL scheme，Steam 自動處理 DRM / overlay / cloud save |
| 啟動 | `steam.exe -login <username>` | 切帳號（若該帳號 RememberPassword=1 則無需密碼） |
| 啟動 | `steam.exe -shutdown` | 優雅結束 Steam，等 process exit 後再 `-login` 切 |

### 實作分 4 個 phase

#### Phase 1: Standalone Steam scanner（純 readonly，驗證邏輯）
- 寫一個 `scripts/scan_steam.ps1` 或 `tools/viplestream-steam-scan.exe`
- 讀 VDF → 產出 JSON 到 stdout：
  ```json
  {
    "steam_root": "C:\\Program Files (x86)\\Steam",
    "current_user": { "steam_id3": "12345", "persona_name": "Alice" },
    "profiles": [
      { "steam_id3": "12345", "persona_name": "Alice",
        "avatar_path": ".../userdata/12345/config/avatarcache/...",
        "remember_password": true, "switchable": true },
      { "steam_id3": "67890", "persona_name": "Chris",
        "remember_password": false, "switchable": false }
    ],
    "apps": [
      { "app_id": "730", "name": "Counter-Strike 2",
        "install_dir": "E:\\SteamLibrary\\steamapps\\common\\Counter-Strike Global Offensive",
        "image_path": "C:\\Program Files (x86)\\Steam\\appcache\\librarycache\\730_header.jpg",
        "owners": ["12345"] },
      { "app_id": "570", "name": "Dota 2", "owners": ["12345", "67890"] }
    ]
  }
  ```
- 驗證邏輯：`owners` 的判定規則是「該 SteamID3 的 localconfig.vdf 裡
  `Software.Valve.Steam.apps.<AppID>` section 存在」。這不能 100% 證明
  擁有（可能只是玩過 family share / demo），但實務上夠準。
- 交付：能在 host 跑一次、看到 JSON 對。0 Sunshine 改動。

#### Phase 2: Sunshine 整合掃描 + apps.json merge
- 新增 `Sunshine/src/platform/windows/steam_scanner.cpp / .h`
  - 把 Phase 1 的 PowerShell 邏輯改成 C++（用 `tyti::vdf_parser` 或自己寫 簡單 VDF parser — 格式單純）
  - Service 啟動時 + 每 5 分鐘 scan 一次（背景 thread）
  - Cache 在記憶體（1 小時 TTL）
- Sunshine apps.json 加兩個欄位：
  - `source`: `"steam"` / `"manual"` / 其他未來 launcher
  - `steam_owners`: `["12345", "67890"]`（只 source=steam 時有意義）
  - `steam_app_id`: `"730"`
- Merge 邏輯：每次 scan 完，把 `source=steam` 的項目整批 replace；
  `source=manual` 完全不動。使用者在 Web UI 手動加的還在。
- Launch command 自動填：`cmd = steam://rungameid/<AppID>`，
  image 用 `librarycache\<AppID>_header.jpg`
- 遊戲 teardown 偵測：用 `installdir` 推出主 exe 名稱，監聽
  process exit（目前 Sunshine 就這樣做）。Steam launcher 本身
  (steam.exe) 不當 teardown 信號。

#### Phase 3: 新 HTTP endpoints + client profile dropdown
- Sunshine 新增 endpoint：
  - `GET /api/steam/profiles` → 回 `{ profiles: [...], current: "12345" }`
  - `GET /api/steam/apps?profile=<SteamID3>` → 只回該 profile 擁有的 apps
    - `profile=all` / 省略 → 全部
  - **每次 client 請求都從 in-memory cache 讀**，server-side cache TTL 1 小時
  - 每次 Moonlight client 進 Apps view 時呼叫一次，本地 session cache
- Moonlight 改動：
  - `moonlight-qt/app/backend/nvhttp.cpp` 加 fetchSteamProfiles / fetchSteamApps
  - `moonlight-qt/app/gui/AppView.qml` 頂部加 profile dropdown
  - Settings 加 `defaultSteamProfile` per-host，進 AppView 自動套用預設
  - Apps list filter 純 client-side（已有 cache，不打 server）
  - 沒選到 profile（第一次開 / cache miss）→ 預設 server's `current_user`
- UI 細節：
  - dropdown 項目：`All users` / 每個 `persona_name`（avatar icon 若有 fetch 下來）
  - 如果 profile 的 `switchable=false`（沒 RememberPassword），選了 launch 時 Steam 會跳錯窗，我們顯 hover tooltip「可能要求輸入密碼」

#### Phase 4: Client 觸發 host 切 Steam 帳號
- Sunshine 新增：
  - `POST /api/steam/switch` body `{ target_steam_id3: "..." }`
  - Handler：
    1. 檢查 target profile 有 `RememberPassword=1`，沒有就回 400 「use manual switch」
    2. 若 target 已是 current_user → 204 no-op
    3. 否則：`steam.exe -shutdown` → poll `ActiveUser == 0` → `steam.exe -login <username>` → poll `ActiveUser == target_steam_id3`（最多 30 秒）
    4. 成功 → 回 200 + 新 current_user；失敗 → 回 500
  - 這個 endpoint 需要已 pair 過的 client 身分驗證（reuse Sunshine 現有 auth）
- Moonlight client：
  - Profile dropdown 選 != `current_user` 的 profile：
    - 顯 spinner + tooltip「正在切換 Steam 帳號…」
    - 呼 `/api/steam/switch`
    - 成功 → 重抓 apps list
    - 失敗 → 回 fallback 到原 profile，toast「切換失敗：<reason>」
  - 第一次遇到 profile 切換要出 confirmation dialog：「伺服器將切換為
    Alice 的 Steam 帳號，目前的 Chris 會登出。繼續？」（避免誤點）

### 已確認的設計決定

- 方案 3（union + profile 過濾）✅
- 擁有權資料 client-side cache per-Moonlight-session；每次啟動都重新問
  server ✅
- Server 要偵測當前 Steam 登入狀態（`HKCU\Software\Valve\Steam\ActiveUser`）
  並 bias UI（預設 profile = current_user）✅
- 「not owned」警告交給 Steam 自己顯示 → 不做 preflight 擁有權檢查 ✅

### 特殊情境處理

- **ActiveUser=0（Steam 沒開）**：不阻擋 launch — `steam://rungameid`
  會自動叫起 Steam。profile dropdown 顯當前「沒 user 登入」+ 紅點
- **target Steam 帳號沒 RememberPassword**：UI 顯鎖頭 icon + 「無法自動
  切換，請在主機上手動登入」
- **Sunshine 跑在 SYSTEM service session 0**：呼叫 `steam.exe -login`
  要跨 session 到 user session 1。需要用 `CreateProcessAsUser` + 目前登入
  desktop 的 token（Sunshine 已有處理類似邏輯，`process.cpp` 啟遊戲時會）
- **VDF parser**：Valve 的 KeyValues 格式簡單（`"key" "value"` / 
  `"parent" { ... }`）。第三方 header-only lib `TinyVDF` 或自己寫 ~100 行都行。
  不用大型 dependency
- **多 launcher 未來擴充**：scan 介面做成 `ISourceScanner`，Steam 是第一個
  實作；未來加 Epic / GoG / Xbox 各寫一個 scanner

### v1 scope（這次要做）

Phase 1 + Phase 2 + Phase 3 最小版（只讀取不切帳號）—交付：
- Steam 遊戲自動出現在 Sunshine apps
- Client Apps view 有 profile dropdown、即時 filter
- 還不能遠端切帳號（要切就本機去切）

Phase 4 當第二個 commit：
- 補上 account switch endpoint + client trigger

這樣 v1 快速 ship 有可見價值；Phase 4 獨立、風險集中在「切帳號成功率」
不會 block 前面的功能。

---

## E. moonlight-android app Icon / Splash

**這條跟 rename 沒關係，但屬於 rebrand 完整度：** v1.2.30 時已經
用 VipleStream icon（lime 色）取代 Moonlight 原 icon，但：
- Splash screen 需要確認有用到新 icon
- 「最近應用程式」列表顯示的 icon 需要驗證 adaptive-icon 能正確
  出現在各版本 Android 上
- Android 12+ 的 themed icon（Material You 把 app icon 變成單色的
  特性）目前**沒**提供單色 mask → Android 12+ 會 fallback 到一般
  adaptive-icon；若要支援 themed icon 需要一張 `ic_launcher_monochrome.xml`

---

## 如何使用這份清單

- 要動某條相容性包袱 → **不要**單獨 commit「改名」，要一起 ship
  migration code + release notes
- 每動一條就從這份清單移除（或標記 ✅ 並寫 commit hash）
- 新發現的相容性債也加進來，格式照 A.N 的樣子
- 真的不打算做的（例如 A.7 協定字串）就明確寫「不該清」並給理由，
  免得下次有人又想改
