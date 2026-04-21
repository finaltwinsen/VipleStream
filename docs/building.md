# VipleStream 建置指南

> **鐵律：** 所有建置、所有打包、所有版號更動，都必須透過 `build_*.cmd`
> 或 `scripts\*.cmd`。不要直接呼叫 `qmake`、`nmake`、`gradlew`、`cmake`。

## 1. 第一次設定（只需做一次）

### 1.1 安裝必要工具

| 元件 | 用途 | 下載 / 驗證 |
|---|---|---|
| **Visual Studio 2022 Build Tools** | MSVC 編譯 moonlight-qt + Sunshine 某些目標 | 勾選「Desktop development with C++」|
| **Qt 6.10+ msvc2022_64** | moonlight-qt 的 Qt6 SDK | `C:\Qt\6.10.3\msvc2022_64\bin\qmake.exe` 存在 |
| **MSYS2 / UCRT64** | Sunshine 用 GCC 交叉工具鏈編譯 | `C:\msys64\usr\bin\bash.exe` 存在 |
| **7-Zip** | 打包 release zip | `C:\Program Files\7-Zip\7z.exe` 存在 |
| **Windows SDK (D3D redist)** | 打包 `dxcompiler.dll` / `dxil.dll` | `C:\Program Files (x86)\Windows Kits\10\Redist\D3D\x64` 存在 |
| **Android SDK + NDK** | moonlight-android | `ANDROID_HOME` 指向 SDK；NDK 在 `%ANDROID_HOME%\ndk\<ver>` |
| **JDK 17 或 21** | Gradle wrapper 需要 | Eclipse Adoptium / Microsoft OpenJDK 都可，build_android.cmd 會自動偵測 |

### 1.2 建立 `build-config.local.cmd`

這是個人機器的路徑設定檔，**gitignored**（不會被 commit）。

```cmd
cd C:\Project\VipleStream
copy build-config.template.cmd build-config.local.cmd
notepad build-config.local.cmd
```

改成符合你機器的路徑，典型內容：
```cmd
set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"

set "MSYS2=C:\msys64\usr\bin\bash.exe"
set "SEVENZIP=C:\Program Files\7-Zip\7z.exe"
set "QT_DIR=C:\Qt\6.10.3\msvc2022_64"
set "WINDEPLOYQT=%QT_DIR%\bin\windeployqt.exe"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "WINSDK_D3D=C:\Program Files (x86)\Windows Kits\10\Redist\D3D\x64"
set "DEPLOY_CLIENT=C:\Program Files\Moonlight Game Streaming"
set "DEPLOY_SERVER=C:\Program Files\Sunshine"
```

完成後就可以 build 了。以後 `git pull` 得到新版也不需重新設定（local.cmd 保留）。

## 2. 日常使用 — 建置腳本速查

> 所有 script 都要從 repo 根目錄 `C:\Project\VipleStream\` 執行。

### 2.1 一鍵建置（Server + Client）

```cmd
build_all.cmd
```

**做的事：**
1. `version.json` patch +1 並同步到 Sunshine / moonlight-qt / moonlight-android
2. MSYS2 UCRT64 編譯 Sunshine → `Sunshine\build_mingw\sunshine.exe`
3. 收集 Sunshine 檔案 → 打包成 `release\VipleStream-Server-X.Y.Z.zip`
4. MSVC 編譯 moonlight-qt → `moonlight-qt\app\release\Moonlight.exe`
5. 收集 DLL / shaders / windeployqt → 打包成 `release\VipleStream-Client-X.Y.Z.zip`

**注意：** **不會**同時 build Android（需另外跑 `build_android.cmd`）。

### 2.2 只建置 Moonlight-QT

```cmd
build_moonlight.cmd
```

做 version bump + MSVC + 打包 Client zip。大約 3-8 分鐘（取決於 cache hit）。

### 2.3 只建置 Sunshine

```cmd
build_sunshine.cmd
```

做 version bump + MSYS2 GCC + 打包 Server zip。大約 5-12 分鐘。

### 2.4 只建置 Android

```cmd
build_android.cmd
```

**做的事：**
1. `version.ps1 propagate`（**不**自動 bump — 版號跟著 `version.json` 目前的值）
2. Gradle `assembleDebug` 建 APK
3. 複製到 `release\VipleStream-Android-X.Y.Z.apk`

**為什麼 Android 不 bump？** 因為 Android 通常 Server/Client 已經 bump 過了。要
新版號就先跑 `build_moonlight.cmd` 或 `build_sunshine.cmd`，再跑 `build_android.cmd`
帶上新版號。

### 2.5 只同步版號、不建置

```cmd
scripts\propagate_version.cmd
```

把 `version.json` 目前的值同步到三個子專案的檔案（Sunshine CMakeLists.txt、
moonlight-qt/app/version.txt、moonlight-android/app/build.gradle）。不會 bump。

**使用時機：**
- 你手動改了 `version.json`（不建議，但有時會）
- 發現三個子專案版號 drift，想強制統一
- debug 版號相關問題時先確認同步狀態

### 2.6 Bump 版號（不建置）

```cmd
scripts\bump_version.cmd                          :: patch + 1
pwsh scripts\version.ps1 bump -Part minor         :: minor + 1, patch 歸 0
pwsh scripts\version.ps1 bump -Part major         :: major + 1, minor/patch 歸 0
```

每個 build script 已經會自動呼叫 bump（除了 `build_android.cmd`）。這個 script
一般不需要手動跑。

### 2.7 查目前版號

```cmd
pwsh scripts\version.ps1 get
```

### 2.8 部署到本機

```cmd
scripts\deploy_client_now.cmd    :: 把 temp\moonlight\* 複製到 %DEPLOY_CLIENT%
```

需要寫 `C:\Program Files\*` 的話要用 admin。路徑由 `build-config.local.cmd` 的
`DEPLOY_CLIENT` / `DEPLOY_SERVER` 控制。

## 3. 輸出檔案位置

```
C:\Project\VipleStream\
├── release\
│   ├── VipleStream-Server-X.Y.Z.zip       ← Sunshine（可直接解壓安裝）
│   ├── VipleStream-Client-X.Y.Z.zip       ← Moonlight-QT
│   └── VipleStream-Android-X.Y.Z.apk      ← Android
├── temp\
│   ├── sunshine\                           ← Sunshine 打包 staging
│   ├── moonlight\                          ← Moonlight 打包 staging
│   └── current_version.txt                 ← bump 完後的最新版號（腳本自用）
├── Sunshine\build_mingw\                  ← Sunshine build output
└── moonlight-qt\app\release\              ← Moonlight build output
```

`temp/` 與 `release/` 都 gitignored。`temp/moonlight/` 可以直接拿來跑
`deploy_client_now.cmd`，不用每次重打包。

## 4. 常見情境

### 情境 A：改了 Moonlight 的 C++ 程式碼，要測試

```cmd
build_moonlight.cmd
scripts\deploy_client_now.cmd
:: 到 Moonlight 跑你的測試案例
```

### 情境 B：改了 Sunshine 的 C++ 程式碼，要測試

```cmd
build_sunshine.cmd
:: 手動解壓 release\VipleStream-Server-<ver>.zip 到 C:\Program Files\Sunshine
:: 或用 scripts\deploy_server.ps1（如果存在的話）
:: 重啟 Sunshine 服務
```

### 情境 C：改了 Android 程式碼，要測試

```cmd
build_android.cmd
adb install -r release\VipleStream-Android-<ver>.apk
```

### 情境 D：Server 跟 Client 都改了，要一起發版

```cmd
build_all.cmd
:: Android 需要同版號的話再加一步：
build_android.cmd
```

### 情境 E：發現設定畫面的版號跟 release 檔名不一致

```cmd
pwsh scripts\version.ps1 get                :: 確認 version.json 值
scripts\propagate_version.cmd               :: 強制同步下游
build_moonlight.cmd                         :: 重 build
```

更詳細的除錯步驟見 `docs/versioning.md`。

## 5. 千萬別做的事

- ❌ 直接跑 `qmake moonlight-qt.pro && nmake` — 會漏 windeployqt、漏 shader、
  可能不會 bump 版號
- ❌ 直接跑 `gradlew assembleDebug` — 會漏版號同步，APK 檔名版號錯
- ❌ 手動修改 `moonlight-qt\app\version.txt`、`Sunshine\CMakeLists.txt`、
  `moonlight-android\app\build.gradle` 的版號 — 下次 `propagate_version`
  會被覆蓋，或造成 drift
- ❌ 把 build output（`temp\`、`release\`）提交到 git — 都已 gitignored，但
  `.fxc` shader 檔是例外（VCS-tracked）
- ❌ 用「一次性的 copy 命令」把新 shader 丟進 release zip — 改
  `scripts\build_moonlight_package.cmd` 裡的 shader 清單（第 54 行左右的
  `for %%F in (...)`），**讓 script 記住**這個檔案，下次建置才會自動包進去

## 6. Script 維護

如果要新增 shader / DLL / data file 到打包清單：

| 要加的東西 | 改哪個 script |
|---|---|
| 新的 `.fxc` shader | `scripts\build_moonlight_package.cmd` 第 54 行的 `for %%F` 清單 |
| 新的 DLL | 同檔第 37 行的 DLL 清單 |
| 新的 Sunshine output | `build_sunshine.cmd` 第 60 行的 `for %%F` |
| 新的子專案要同步版號 | `scripts\version.ps1` 的 `Propagate-Version` 新增 block |
| 新的建置目標（如 iOS） | 建立 `build_ios.cmd`，呼叫 `scripts\version.ps1 propagate` + 實際建置 |

Script 改動會被 git tracked，下次同事 `git pull` 就拿到新版清單，不需要他們
重新調整機器設定。

## 7. 為什麼要這麼死守 script？

歷史教訓（別問為什麼這些都發生過）：
- 手動 `nmake` 過 → 版號 drift，release zip 裡是 1.2.5 但 Settings 顯示 1.2.3
- 忘了 windeployqt → exe 跑起來噴 `qt6core.dll not found`
- 新 shader 沒加到打包清單 → 使用者裝了新版但 FRUC 靜默失效
- Android 版號沒 bump → Play Store 拒絕 upload（versionCode 重複）
- 手動改 CMakeLists.txt 版號 → 忘了同步 moonlight-qt/version.txt，兩邊對不上

Script 把這些踩過的雷都 encode 起來。走 script 就不會再踩第二次。
