# VipleStream 版號更迭機制（Single Source of Truth）

> 本文件是 VipleStream 的版號規範。任何跨 moonlight-qt / moonlight-android /
> Sunshine 的版本調整都**必須**遵守這份流程。在你做任何會 ship 的變更之前，
> 請先 `git diff` 確認版號狀態一致。

## 1. 唯一事實來源（SSOT）

```
C:\Project\VipleStream\version.json          ← SSOT
```

檔案格式：
```json
{ "major": 1, "minor": 2, "patch": 12 }
```

**絕對不要手動修改下游檔案**（Sunshine CMakeLists.txt、moonlight-qt/app/version.txt、
moonlight-android/app/build.gradle）。這些檔案的版號是由 `scripts/version.ps1` 從
`version.json` 自動同步，手改會造成 drift。

## 2. 下游同步目標

`scripts/version.ps1 Propagate-Version` 會把 `version.json` 同步到以下三個位置：

| 位置 | 檔案 | 被誰讀取 |
|---|---|---|
| Sunshine | `Sunshine/CMakeLists.txt` `project(VipleStream VERSION X.Y.Z)` | CMake → `.rc` → exe 內嵌版號、CPack 封裝檔名 |
| Moonlight-QT | `moonlight-qt/app/version.txt` | qmake `$$cat()` → 產生 `version_string.h` → 編譯到 exe |
| Moonlight-Android | `moonlight-android/app/build.gradle` `versionName` + `versionCode` | Gradle → APK manifest |

**Android versionCode 推算公式**：`major*10000 + minor*1000 + patch`
- `1.2.12` → `12012`
- `1.3.0`  → `13000`
- `2.0.0`  → `20000`

此公式保證每次 minor 進位必然 > 上一個 minor 的任何 patch，符合 Play Store 要求的
單調遞增。最多支援 minor ≤ 9 與 patch ≤ 999，對 VipleStream 綽綽有餘。

## 3. 版號操作流程

### 3.1 bump（遞增版號）

```powershell
# patch bump（預設）
pwsh scripts\version.ps1 bump

# minor bump（patch 歸零）
pwsh scripts\version.ps1 bump -Part minor

# major bump（minor + patch 歸零）
pwsh scripts\version.ps1 bump -Part major
```

或從 CMD：
```cmd
scripts\bump_version.cmd        :: 永遠 bump patch
```

Bump 會**先遞增 `version.json`、再呼叫 Propagate-Version**，下游三處在一次動作內
全部同步。

### 3.2 propagate（只同步、不遞增）

當你發現下游 drift 或想 rerun 同步：
```powershell
pwsh scripts\version.ps1 propagate
```

### 3.3 get（查目前版號）

```powershell
pwsh scripts\version.ps1 get      # -> 1.2.12
```

## 4. 各個 build script 的版號行為

| 腳本 | 是否自動 bump | 同步範圍 |
|---|---|---|
| `build_all.cmd` | 是（呼叫 bump_version.cmd） | Sunshine + QT + Android（自 1.2.12 起）|
| `build_moonlight.cmd` | 是 | 三者（bump 會觸發完整 propagate）|
| `build_sunshine.cmd` | 是 | 三者（同上）|
| `build_android.cmd` | **否** — 只 propagate | 三者 |

**重點：** 單獨跑任何一個 build script 會做完整的 propagate，所以三個專案的版號
永遠同步，不會因為只 build 其中一個而 drift。

## 5. moonlight-qt 的 VERSION_STR 流程（細節）

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

## 6. Settings 畫面顯示的版號流程

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

## 7. 除錯 checklist

版號不一致時依序檢查：

1. `pwsh scripts\version.ps1 get` 回報的值與 release 檔名是否一致？
   - 若不一致 → `pwsh scripts\version.ps1 propagate` 強制同步
2. `moonlight-qt/app/version.txt` 的內容是否正確？
3. `moonlight-qt/app/release/version_string.h`（build output）存在嗎？內容對嗎？
   - 若存在但內容舊 → 刪掉重新 qmake + nmake
   - 若不存在 → qmake 還沒跑過、或是 QMAKE_SUBSTITUTES 設定壞了
4. `Sunshine/CMakeLists.txt` `project(... VERSION)` 的數字對嗎？
5. `moonlight-android/app/build.gradle` `versionName` 對嗎？

## 8. 禁止事項

- ❌ 手動編輯 `Sunshine/CMakeLists.txt` 的 `VERSION`
- ❌ 手動編輯 `moonlight-qt/app/version.txt`
- ❌ 手動編輯 `moonlight-android/app/build.gradle` 的 `versionName` / `versionCode`
- ❌ 編輯 `version_string.h`（是 build output，會被覆蓋）
- ❌ 在 build script 裡自己寫 regex 去改上面任一檔案 —— 都改走 `version.ps1`
- ❌ 改 Android versionCode 公式（breaks monotonicity）

## 9. 未來擴充

要新增第四個需同步的檔案（例如 iOS Info.plist），只需：
1. 在 `scripts/version.ps1 Propagate-Version` 新增一個 block
2. 更新本文的第 2 節表格
3. 該 block 要以 Test-Path 防守，專案沒 clone 時不報錯
