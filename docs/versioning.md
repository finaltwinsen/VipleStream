# VipleStream 版號更迭機制（Single Source of Truth）

> 本文件是 VipleStream 的版號規範。任何跨 moonlight-qt / moonlight-android /
> Sunshine（含 SteamVR driver）的版本調整都**必須**遵守這份流程。在你做任何
> 會 ship 的變更之前，請先 `git diff` 確認版號狀態一致。

## 1. 唯一事實來源（SSOT）

```
<repo 根目錄>/version.json          ← SSOT（進 git）
```

路徑一律相對於 repo 根目錄，跟工作樹 clone 在哪台機器、哪個磁碟無關
（舊版文件寫的 `C:\Project\VipleStream\version.json` 是早期工作樹位置，已過期）。

檔案格式：
```json
{ "major": 1, "minor": 5, "patch": 276 }
```

**絕對不要手動修改下游檔案**（`Sunshine/CMakeLists.txt`、`moonlight-qt/app/version.txt`、
`moonlight-android/app/build.gradle`、`Sunshine/src/platform/windows/steamvr_driver/driver_version.h`）。
這些檔案的版號是由 `build-tools/version.ps1` 從 `version.json` 自動同步，手改會造成 drift。

> `build-tools/` 整個目錄是本機檔（gitignored）：`version.ps1` 與它的單元測試
> `build-tools/test_version.ps1` 都**不入 git**，各 builder 機器各自持有一份。
> 本文件（進 git）是它們行為的規範；改了 `version.ps1` 就要同步改這裡。

## 2. 下游同步目標

`build-tools/version.ps1` 的 `Propagate-Version` 會把 `version.json` 同步到以下四個位置：

| 位置 | 檔案 | 被誰讀取 |
|---|---|---|
| Sunshine | `Sunshine/CMakeLists.txt` `project(VipleStream-Server VERSION X.Y.Z …)` | CMake → `.rc` → exe 內嵌版號、CPack 封裝檔名 |
| Moonlight-QT | `moonlight-qt/app/version.txt` | qmake `$$cat()` → 產生 `version_string.h` → 編譯到 exe |
| Moonlight-Android | `moonlight-android/app/build.gradle` `versionName` + `versionCode` | Gradle → APK manifest |
| SteamVR driver（§F10） | `Sunshine/src/platform/windows/steamvr_driver/driver_version.h`（整檔產生、進 git） | driver IPC 握手的 `driverVer`、driver DLL 的 FileVersion 資源；Stage 階段再拿 DLL FileVersion 和 `version.ps1 get` 比對 |

Sunshine 那一列的 regex 也認得 v1.2.43 改名以前的 `project(VipleStream VERSION …)`，
但寫回時一律用現在的專案名 `VipleStream-Server`。

### 2.1 `driver_version.h`（第四個目標）

- **Test-Path 保護**：以 `Sunshine/src/platform/windows/steamvr_driver/` **目錄**是否存在為閘。
  這個目錄在 M1b（server VR 本體）才會建立；目錄不存在時整段略過，**不會**替你建目錄。
  目錄存在時就產生（或覆寫）整個 header，所以 M1b 建目錄之後跑一次 propagate 就會出現。
- **內容相同就不寫檔**（比對時忽略 CRLF/LF 差異）：避免 mtime 變動讓 driver 每次建置都重編。
- **巨集格式**（header 檔頭的註解裡也有同一份說明）：

  | 巨集 | 型態 | 例（1.5.276） |
  |---|---|---|
  | `VIPLE_DRIVER_VERSION_MAJOR` / `_MINOR` / `_PATCH` | 十進位整數字面值 | `1` / `5` / `276` |
  | `VIPLE_DRIVER_VERSION_STR` | 字串字面值 `"MAJOR.MINOR.PATCH"` | `"1.5.276"` |
  | `VIPLE_DRIVER_VERSION_RC` | 逗號分隔四段（`.rc` 的 `FILEVERSION` / `PRODUCTVERSION`），第四段固定 0 | `1,5,276,0` |
  | `VIPLE_DRIVER_VERSION_PACKED` | 32-bit 無號整數 `(MAJOR << 24) \| (MINOR << 16) \| PATCH`，給握手比對（§3.5 的 major ≤ 255 保證不超出 32 bit） | `0x01050114u` |

- **編碼：全檔純 ASCII**（UTF-8 無 BOM 等同 ASCII）、CRLF。檔頭註解用英文，是「程式碼註解用
  繁中」規範的刻意例外，中文說明就是本節。理由：這個 header 會被 `rc.exe` 和 `cl` include，
  兩者預設都用系統 ANSI 碼頁（cp950）讀檔，任何非 ASCII 位元組（連 `§` 也算）都會在每次
  include 時觸發 C4819，開了 `/WX` 就直接建置失敗。單元測試在 byte 層檢查全檔沒有 > 0x7F 的位元組。
- **M1b 的建置約束**（`Build-SteamVRDriver.ps1` 必須遵守）：這個 header 不需要任何編碼旗標；
  但 driver 其他手寫原始碼只要含中文註解，`cl` 必須加 `/utf-8`，`rc` 必須加 `/c65001`
  （或在 `.rc` 開頭寫 `#pragma code_page(65001)`），否則同樣會吃 C4819。

### 2.2 Android versionCode

**推算公式**：`major*10000 + minor*1000 + patch`
- `1.2.12` → `12012`
- `1.3.0`  → `13000`
- `2.0.0`  → `20000`
- `2.0.0`  → `20000`

此公式保證每次 minor 進位必然 > 上一個 minor 的任何 patch，符合 Play Store 要求的
單調遞增。前提是 **minor ≤ 9 且 patch ≤ 999**，超過就會碰撞：

- `1.5.1000` → `16000` ＝ `1.6.0` 的 versionCode
- `1.10.0`   → `20000` ＝ `2.0.0` 的 versionCode

這個前提以前只是「綽綽有餘」的假設，現在由 §3.5 的防護強制。major 另有 ≤ 255 的上限
（來自 `driver_version.h` 的 PACKED 格式，見 §3.5），所以 versionCode 最大是 `255.9.999` →
`2559999`，遠低於 Play Store 的 2,100,000,000 上限，`version.ps1` 的 `[int]` 相乘也不會溢位。

## 3. 版號操作流程

### 3.1 bump（遞增版號）

```powershell
# patch bump（預設）
pwsh build-tools\version.ps1 bump

# minor bump（patch 歸零）
pwsh build-tools\version.ps1 bump -Part minor

# major bump（minor + patch 歸零）
pwsh build-tools\version.ps1 bump -Part major
```

或從 CMD：
```cmd
build-tools\bump_version.cmd        :: 永遠 bump patch
```

Bump 會**先算出新版號、過 §3.5 防護、再寫 `version.json`、最後呼叫 Propagate-Version**，
下游四處在一次動作內全部同步。被防護擋下時不寫任何檔案。

### 3.2 set（直接設定版號，§F10）

```powershell
pwsh build-tools\version.ps1 set -Version 2.0.0
```

`bump` 是相對操作（每跑一次就再加一），`set` 是絕對、冪等的操作，專門用在：
切 major/minor 版（例如 2.0.0，見 §5）。切版一律照 §5 手動執行（CoworkMCP 已於 2026-09-23
停用，原本規劃的任務圖修正 F19 隨之取消）。

| 目標版號 vs 目前版號 | 行為 |
|---|---|
| 格式不符 | throw（`reason=bad-format`），不寫任何檔案 |
| 目標 **<** 目前 | throw（`reason=downgrade`）——版號與 versionCode 只能單調遞增 |
| 目標 **=** 目前 | **不寫** `version.json`，只 propagate（冪等，可重跑；也能順便修好下游 drift） |
| 目標 **>** 目前 | 過 §3.5 防護 → 寫 `version.json` → propagate |

- **格式**：純數字三段 `X.Y.Z`。不接受 `v` 前綴、預發行尾綴（`-rc1`）、前導零
  （`02.0.0`、`2.00.0`）、第四段、全形數字；每段最多 9 位數（這只是解析層，確保轉成
  `[int]` 不溢位；實際可接受的範圍是 §3.5 的防護：major ≤ 255、minor ≤ 9、patch ≤ 999）。
- **防護擋不住「打錯但合法」的版號**：想打 `2.0.0` 卻打成 `20.0.0`，所有防護都會放行，
  而且 `set` 拒絕降版，事後不能再 `set` 回來。所以 `set` 之後一定要核對 stdout 印出的版號
  （以及 stderr 的 `舊版號 -> 新版號`），發現打錯就照 §5 第 2 步的方式回復。
- **參數組合**：`-Version` 只能搭配 `set`（`bump -Version …` 會被拒，`reason=version-without-set`）；
  `set` 不接受 `-Part`（`reason=part-with-set`）；`set` 沒給 `-Version` 也會被拒（`reason=missing-version`）。
- 同版號時一樣會跑防護，所以 patch > 900 的警告在冪等重跑時也看得到。

### 3.3 propagate（只同步、不遞增）

當你發現下游 drift 或想 rerun 同步：
```powershell
pwsh build-tools\version.ps1 propagate
```

propagate 不改版號，但寫下游之前一樣會跑 §3.5 的防護：`version.json` 若被手改成越界值
（例如 `256.0.0`、`1.5.1000`），會直接被擋下、任何下游檔都不會動，避免把溢位的
versionCode／PACKED 寫出去。

### 3.4 get（查目前版號）

```powershell
pwsh build-tools\version.ps1 get      # -> 1.5.276
```

### 3.5 防護（`Assert-VersionGuard`，bump、set、propagate 共用，§F10）

| 條件 | 行為 |
|---|---|
| major > 255 | throw（`reason=major-limit`）——`driver_version.h` 的 PACKED 只有 8 bit 給 major；這也同時涵蓋 `.rc` FILEVERSION 每段 16 bit 與 Android versionCode 的上限。實務上碰到這條幾乎一定是打錯版號 |
| minor > 9 | throw（`reason=minor-limit`）——改用 `bump -Part major` 或 `set` 到下一個 major |
| patch > 999 | throw（`reason=patch-limit`）——改用 `bump -Part minor` 或 `set` 到下一個 minor |
| patch > 900 | stderr 印警告（`reason=patch-near-limit`，附剩餘次數），**照常繼續** |

- bump／set 在寫 `version.json` **之前**執行防護，propagate 在寫下游檔**之前**執行；
  被擋下時任何檔案都不會動。
- `get` 是唯讀，不跑防護（`version.json` 壞掉時仍要能查出目前的值）。
- 看到 >900 警告就該規劃 minor 進位，不要等到 999 才被擋下。
- 防護只擋越界值，擋不住打錯但合法的版號（見 §3.2）。

### 3.6 輸出與錯誤契約

- **stdout 只有一行版號字串**；其他提示（`1.5.276 -> 1.5.277`、警告）一律走 stderr。
  `bump_version.cmd` / `propagate_version.cmd` 用 `>` 把 stdout 導向 `temp\current_version.txt`，
  build script 再 `set /p` 讀回，多一行就會讀錯。
- 失敗一律 throw（`powershell -File` 以 exit code 1 結束），訊息開頭是
  `[VIPLE-VERSION] refused reason=<token>`；警告開頭是 `[VIPLE-VERSION] warn reason=<token>`。
- **呼叫端必須檢查 exit code**：失敗時 `temp\current_version.txt` 是空檔。本機的
  `bump_version.cmd`／`propagate_version.cmd` 在 `version.ps1` 失敗時以 exit code 1 結束；
  `build_all.cmd`／`build_moonlight.cmd`／`build_sunshine.cmd` 檢查這個 errorlevel，並在讀回版號前
  先清掉 `VER`、讀回後檢查 `if not defined VER`；`build_android.cmd` 也先清掉 `VER`
  （2026-09-23 補上）。版號步驟失敗時建置會以 `[ERROR]` 停下，不會再打包出
  `VipleStream-Client-.zip` 這種沒有版號的檔名，也不會沿用環境裡殘留的舊 `VER`。
- `version.ps1` 必須存成 **UTF-8 with BOM**：build script 呼叫的是 Windows PowerShell 5.1，
  沒有 BOM 會用 ANSI（cp950）解析，含中文的訊息會直接 parse error。

### 3.7 單元測試（本機限定）

`build-tools/test_version.ps1` 和 `version.ps1` 一起放在本機、不入 git。改了 `version.ps1`
之後，Windows PowerShell 5.1（build script 用）與 pwsh 7（互動 shell 常用）兩種都要跑：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build-tools\test_version.ps1 -FixtureRoot <暫存目錄> -Shell powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File build-tools\test_version.ps1 -FixtureRoot <暫存目錄> -Shell pwsh
```

- 在 `-FixtureRoot` 建一個最小的假 repo（`version.json` + 四個下游檔），把 `version.ps1`
  複製過去再執行，**絕不碰真 repo**。`-FixtureRoot` 落在 repo 內會直接拒絕；
  結束時比對真 repo 的 `version.json` 與下游檔 hash，有變動就判失敗。
- 涵蓋：set 升版／同版冪等／降版／格式錯誤／參數組合、bump 與 set 的防護（含 major 上限：
  `256.0.0` 被擋、`255.0.0` 與 `255.9.999` 可接受、`bump -Part major` 越過 255 被擋）、
  propagate 擋下手改的越界 `version.json`、`get` 不擋、>900 警告邊界（900 不警告、901 警告）、
  `driver_version.h` 目錄不存在／檔案不存在／過期／內容相同／全檔純 ASCII、stdout 只有一行版號；
  有 MinGW gcc 時另用 `_Static_assert` 驗 header 巨集值。
- 全部通過 exit 0，任一失敗 exit 1。

## 4. 各個 build script 的版號行為

| 腳本 | 是否自動 bump | 同步範圍 |
|---|---|---|
| `build_all.cmd` | 是（呼叫 bump_version.cmd）；`--no-bump` 關閉（必須是第一個參數） | Sunshine + QT + Android（自 1.2.12 起）＋ driver header（目錄存在時） |
| `build_moonlight.cmd` | 是；`--no-bump` 關閉 | 四者（bump 會觸發完整 propagate）|
| `build_sunshine.cmd` | 是；`--no-bump` 關閉（必須是第一個參數） | 四者（同上）|
| `build_android.cmd` | **否** — 只 propagate | 四者 |

**重點：** 單獨跑任何一個 build script 會做完整的 propagate，所以各專案的版號
永遠同步，不會因為只 build 其中一個而 drift。

## 5. 切版 SOP（major / minor，例：2.0.0）

用 `set` 定版，之後**這一版 release 的所有建置（到發佈完成為止）都帶 `--no-bump`**。
不帶的話第一支 build script 就會把 2.0.0 bump 成 2.0.1，後面幾件的版號就對不上了。
發佈完成後的日常建置恢復預設的 bump（第 6 步）。

切版一律照本節手動執行。

1. **前提**：要切版的功能已經用 1.x 版號建置、驗證通過（2.0.0 的前提是 G-α 通過，
   見 `docs/vr_architecture.md` §6）；工作樹乾淨、`version.json` 停在最後一個 1.5.x。
2. **定版**：
   ```powershell
   pwsh build-tools\version.ps1 set -Version 2.0.0     # stdout 應為 2.0.0
   git diff --stat                                      # 只應有 version.json 與下游檔
   ```
   **先核對 stdout 印出的版號**：防護擋不住打錯但合法的版號（例：`20.0.0`），而且 `set`
   拒絕降版，不能再 `set` 回來。commit 之前發現打錯，就用
   `git checkout version.json` 還原 SSOT，再跑 `pwsh build-tools\version.ps1 propagate`
   把下游（含 `driver_version.h`）一起改回來，然後重做這一步。
3. **commit**：`version.json` 與下游檔放在同一個 commit，主旨 `v2.0.0: …`。
4. **建置**（全部 `--no-bump`；前後都用 `version.ps1 get` 確認仍是 2.0.0）：
   ```cmd
   build_sunshine.cmd --no-bump
   build_moonlight.cmd --no-bump
   build_android.cmd
   ```
   Linux builder 先 `git pull` 到同一個 commit 再建，建置前後都用 `version.ps1 get` 核對
   仍是目標版號。
5. **驗證**：全部 asset 重建之後重跑縮短版驗證（見 `docs/vr_architecture.md` §6），再備妥 release
   候選，由使用者決定是否 push 和發佈。
6. **之後**：這一版發佈完成後，日常建置恢復預設（bump patch：2.0.1、2.0.2…）。

注意：`set` 本身是冪等的，第 2 步重跑不會出錯；但絕對不要用「連跑幾次 `bump`」來湊目標版號。

## 6. moonlight-qt 的 VERSION_STR 流程（細節）

這是歷史上最容易 drift 的一環。完整流程：

```
app/version.txt
     │
     │ $$cat(version.txt)   （app.pro 在 qmake 時讀取）
     ▼
VERSION_STR_VALUE = 1.2.12
     │
     │ QMAKE_SUBSTITUTES（把 @VERSION_STR_VALUE@ 替換）
     ▼
build/release/version_string.h
  #define VERSION_STR "1.2.12"
     │
     │ #include "version_string.h"
     ▼
systemproperties.cpp  →  SystemProperties::versionString
main.cpp              →  QCoreApplication::setApplicationVersion
autoupdatechecker.cpp →  比對 GitHub releases
```

### 為什麼不用舊的 `DEFINES += VERSION_STR=...` 方式？

舊方式透過 `-DVERSION_STR=\"1.2.12\"` 注入到編譯命令列。qmake-run 時才會讀 version.txt，
但 **nmake 追蹤檔案 timestamp，不追蹤命令列變化**。所以即使 qmake 重跑、Makefile
換成新的 `-DVERSION_STR`，既有的 `.obj` 檔 mtime 沒變，nmake 不會重新編譯 —— 結果
exe 內埋的還是舊版號。

新做法透過 `version_string.h` 這個真實檔案把版號帶進去：

- qmake 時 `QMAKE_SUBSTITUTES` 會依 `version.txt` 內容**覆寫** `version_string.h`
- 如果內容有變，header 的 mtime 更新
- nmake 偵測到 `systemproperties.cpp` 依賴的 header 變了，只重新編譯該 .cpp
- 這是 nmake 原生支援的 dep chain，不會漏檔

build script 裡 `copy /b app.pro+,, app.pro`（touch `app.pro`）的作用是**強制 qmake
重跑**（nmake 看到 `app.pro` 比 `Makefile.Release` 新就會先跑 qmake）。

## 7. Settings 畫面顯示的版號流程

QML 端：
```qml
// moonlight-qt/app/gui/main.qml:291
text: "VipleStream v" + SystemProperties.versionString
```

後端綁定：
```cpp
// systemproperties.cpp
versionString = QString(VERSION_STR);   // 來自 version_string.h
```

QML property `SystemProperties.versionString` 的值就是 C++ 端的 `m_versionString`，
兩邊永遠一致。如果 Settings 右上角顯示的版號跟 release zip 檔名不一致，基本就是
Task 5 提到的 nmake dep 問題（已在 v1.2.12 修正為 generated header）。

## 8. 除錯 checklist

版號不一致時依序檢查：

1. `pwsh build-tools\version.ps1 get` 回報的值與 release 檔名是否一致？
   - 若不一致 → `pwsh build-tools\version.ps1 propagate` 強制同步
2. `moonlight-qt/app/version.txt` 的內容是否正確？
3. `moonlight-qt/app/release/version_string.h`（build output）存在嗎？內容對嗎？
   - 若存在但內容舊 → 刪掉重新 qmake + nmake
   - 若不存在 → qmake 還沒跑過、或是 QMAKE_SUBSTITUTES 設定壞了
4. `Sunshine/CMakeLists.txt` `project(VipleStream-Server VERSION …)` 的數字對嗎？
5. `moonlight-android/app/build.gradle` `versionName` 對嗎？
6. （M1b 之後）`Sunshine/src/platform/windows/steamvr_driver/driver_version.h` 的
   `VIPLE_DRIVER_VERSION_STR` 對嗎？driver DLL 的 FileVersion 對嗎？
7. build log 裡有沒有 `[VIPLE-VERSION] refused`？被防護擋下時 `version.ps1` 以 exit code 1
   結束、不寫任何檔案，`temp\current_version.txt` 是空的，build script 會以
   `[ERROR] Version step failed` 停下（§3.6）。修好版號（或改用 `set` 到合法的版號）再重建。

## 9. 禁止事項

- ❌ 手動編輯 `Sunshine/CMakeLists.txt` 的 `VERSION`
- ❌ 手動編輯 `moonlight-qt/app/version.txt`
- ❌ 手動編輯 `moonlight-android/app/build.gradle` 的 `versionName` / `versionCode`
- ❌ 手動編輯 `driver_version.h`（下次 propagate 就會整檔覆寫）
- ❌ 編輯 `version_string.h`（是 build output，會被覆蓋）
- ❌ 在 build script 裡自己寫 regex 去改上面任一檔案 —— 都改走 `version.ps1`
- ❌ 改 Android versionCode 公式（breaks monotonicity），或放寬 §3.5 的防護上限
- ❌ 用連跑 `bump` 湊目標版號 —— 要定到特定版號一律用 `set -Version`
- ❌ `set` 之後、該版發佈完成之前的建置不帶 `--no-bump`（會把剛定好的版號再往上推一格）
- ❌ 在自動產生的 `driver_version.h` 裡放任何非 ASCII 字元（§2.1）

## 10. 未來擴充

要新增第五個需同步的檔案（例如 iOS Info.plist），只需：
1. 在 `build-tools/version.ps1 Propagate-Version` 新增一個 block
2. 更新本文的第 2 節表格
3. 該 block 要以 Test-Path 防守，專案沒 clone（或目錄還沒建立）時不報錯
4. 在 `build-tools/test_version.ps1` 的 fixture 與斷言加上這個目標，兩種 shell 都跑過
