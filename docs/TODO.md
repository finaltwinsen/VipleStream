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
