# VipleStream 建置指南

> **鐵律：** 所有建置、所有打包、所有版號更動，都必須透過 build script：Windows 與
> Android 用 repo 根目錄的 `build_*.cmd`（本機檔，見 §0）；Linux 用已入 git 的
> `moonlight-qt/scripts/build-appimage-native.sh`、`Sunshine/scripts/linux_build.sh`（見 §7）。
> 不要直接呼叫 `qmake`、`nmake`、`make`、`gradlew`、`cmake --build`。

## 0. 哪些 script 在 git 裡

Windows 端的建置工具**全部不在 git 裡**，是每位開發者自己維護的本機檔（`.gitignore`
有明列）。公開 repo 裡只有子專案目錄下的 script。

| 路徑 | 在 git 裡？ | 內容 |
|---|---|---|
| `build_all.cmd`、`build_moonlight.cmd`、`build_sunshine.cmd`、`build_android.cmd` | 否 | 根目錄的建置入口 |
| `build-config.template.cmd`、`build-config.local.cmd` | 否 | 範本本身也不在 git 裡，新 clone 要自己建（內容見 §1.2） |
| `build-tools\` | 否 | `version.ps1`、`bump_version.cmd`、`propagate_version.cmd`、`build_moonlight_package.cmd`、`build_picoquic_client.cmd` 等。部分檔頭還寫著「VCS-tracked」，那是整個目錄移出 git 之前的舊說法 |
| `scripts\`（repo 根目錄） | 否 | 部署、benchmark、診斷、`wsl_*.sh` 等本機工具 |
| `moonlight-qt/scripts/`、`Sunshine/scripts/` | **是** | 子專案內的 script，包含 Linux 建置腳本（`.gitignore` 的 `/scripts/` 錨定在根目錄，不影響這兩處） |
| `moonlight-qt/app/shaders/*.fxc` | **是** | 預編譯的 D3D11 shader bytecode |

**公開 repo 的 clone 不附 Windows／Android 建置腳本**：`build_*.cmd`、`build-tools\`、
`build-config.template.cmd` 都不會隨 repo 發佈。目前不支援外部 Windows 建置，需要的話請向
維護者索取；沒有這些檔就無法照 §1–§6 在 Windows 上建置。新 clone 能直接使用的只有 §7 的
Linux 腳本。

## 1. 第一次設定（只需做一次）

### 1.1 安裝必要工具

| 元件 | 用途 | 下載 / 驗證 |
|---|---|---|
| **Visual Studio 2022 Build Tools** | MSVC 編譯 moonlight-qt + Sunshine 某些目標 | 勾選「Desktop development with C++」|
| **Qt 6.10+ msvc2022_64** | moonlight-qt 的 Qt6 SDK | `C:\Qt\6.10.3\msvc2022_64\bin\qmake.exe` 存在 |
| **MSYS2 / UCRT64** | Sunshine 用 GCC 交叉工具鏈編譯 | `C:\msys64\usr\bin\bash.exe` 存在 |
| **7-Zip** | 打包 release zip | `C:\Program Files\7-Zip\7z.exe` 存在 |
| **Windows SDK** | `compile_d3d11_shaders.ps1` 用裡面的 `fxc.exe` 把 `.hlsl` 重編成 `.fxc`（找不到只警告，沿用 repo 裡現有的 `.fxc`）。§SLIM 起已不再打包 `dxcompiler.dll` / `dxil.dll` | `C:\Program Files (x86)\Windows Kits\10\bin\<ver>\x64\fxc.exe` 存在 |
| **Android SDK + NDK** | moonlight-android | `ANDROID_HOME` 指向 SDK；NDK 在 `%ANDROID_HOME%\ndk\<ver>` |
| **JDK 17 或 21** | Gradle wrapper 需要 | Eclipse Adoptium / Microsoft OpenJDK 都可，build_android.cmd 會自動偵測 |

### 1.2 建立 `build-config.local.cmd`

這是個人機器的路徑設定檔，**gitignored**（不會被 commit）。手上有
`build-config.template.cmd` 的話直接複製；沒有（新 clone 不會有，範本也不在 git 裡）
就照下面的典型內容自己建一份。

```cmd
cd <repo>
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
set "DEPLOY_SERVER=C:\Program Files\VipleStream-Server"
set "VCPKG_ROOT=<vcpkg 根目錄>"
```

`VCPKG_ROOT` 要指向你自己裝的 vcpkg：`app.pro` 在 qmake 時讀它，而 `vcvars64.bat` 會把它
蓋成 Visual Studio 內建、沒有任何套件的 vcpkg，所以 `build_moonlight.cmd` 在 vcvars 之後會
重讀一次這個檔。

工作樹裡已經有 `build_*.cmd` 與 `build-tools\`（見 §0）的話，完成後就可以 build 了。以後
`git pull` 得到新版也不需重新設定（local.cmd 保留）。

## 2. 日常使用 — 建置腳本速查

> 下列 `.cmd` 都在 repo 根目錄（下文以 `<repo>\` 表示），從那裡執行。
> `build_all.cmd`、`build_moonlight.cmd`、`build_sunshine.cmd` 預設會先 bump patch；
> 加 `--no-bump` 就沿用 `version.json` 目前的版號（只做 propagate）。重建同一版、
> 或多支 script 要產出同版號的產物時，一律帶 `--no-bump`。

### 2.1 一鍵建置（Server + Client）

```cmd
build_all.cmd              :: bump patch 一次，再建 Server + Client
build_all.cmd --no-bump    :: 沿用 version.json 目前的版號
```

`build_all.cmd` 本身不編譯也不打包，只決定一次版號，然後依序呼叫單支 script：

1. 版號：預設 `version.json` patch +1 並同步到子專案；`--no-bump` 只同步
2. `build_sunshine.cmd --no-bump` → `release\VipleStream-Server-X.Y.Z.zip`
3. `build_moonlight.cmd --no-bump` → `release\VipleStream-Client-X.Y.Z.zip`
   （`--no-bump` 以外的旗標原樣轉給 `build_moonlight.cmd`）
4. （可選）本機有 WSL、而且 repo 位在 WSL 包裝腳本寫死的路徑時，順便用本機的
   `scripts\wsl_build_moonlight.sh` 建 Linux Client AppImage；這一步失敗只警告，不影響結果

任一單支 script 失敗就整個停下，不會做出半套；每支跑完都會核對版號沒有被中途改掉。

**注意：** **不會**建 Android（另外跑 `build_android.cmd`），也不會建 Linux Server `.deb`
（見 §7）。建置與打包細節一律改單支 script，不要讓 `build_all.cmd` 再長出自己的編譯步驟。

### 2.2 只建置 Moonlight-QT

```cmd
build_moonlight.cmd
build_moonlight.cmd --no-bump
```

做 version bump（或 `--no-bump` 只同步）+ MSVC 編譯 + 打包 Client zip。大約 3-8 分鐘（取決於
cache hit）。編譯產物是 `moonlight-qt\app\release\VipleStream.exe`，打包 staging 在
`temp\moonlight\`（可以直接拿來跑 `VipleStream.exe --help` smoke test）。

**MP-QUIC 預設編入**（2026-09-23 起，F18）：qmake 一律加上 `DEFINES+=VIPLE_MPQUIC`，把 MP-QUIC
多路徑傳輸編進 client（連結 picoquic／picotls），與 Linux AppImage（§7）一致。要建不含 MP-QUIC
的版本時帶 **`--no-mpquic`**；舊的 `--mpquic` 仍可接受，但已經沒有作用。`build_all.cmd` 會把這些
旗標原樣轉給本腳本。編入只代表功能可用，執行時是否走 QUIC 仍由設定頁的 MP-QUIC 選項或 CLI 決定。

**MP-QUIC 靜態庫（picoquic + picotls）**：不論有沒有帶 `--no-mpquic`，`build_moonlight.cmd`
都會在 qmake 之前呼叫 `build-tools\build_picoquic_client.cmd`，把
`Sunshine\third-party\picoquic\build\` 裡的 `picoquic-core.lib` 與
`_deps\picotls-build\picotls-*.lib` 以 **Release** 建好（快取已是 Release 時只做增量
ninja）；帶了 `--no-mpquic` 時才不連結它們。不要手動 `cmake -B build` 用預設組態去建：
2026-09-19 就是這樣建出 Debug（/MDd /Od）版，client 連結時每次噴 `LNK4098: 預設程式庫
MSVCRTD 與其他程式庫衝突`，而且 MP-QUIC 整條路徑跑的是未最佳化碼。腳本最後會用
`dumpbin /directives` 驗證沒有 MSVCRTD 參照。需要強制重新 configure 時，直接跑
`build-tools\build_picoquic_client.cmd --reconfigure`（`build_moonlight.cmd` 不會轉這個旗標）。

### 2.3 只建置 Sunshine

```cmd
build_sunshine.cmd
build_sunshine.cmd --no-bump
```

做 version bump（或 `--no-bump` 只同步）+ MSYS2 UCRT64 GCC 編譯 + 打包 Server zip。大約
5-12 分鐘。編譯產物是 `Sunshine\build_mingw\viplestream-server.exe`，打包 staging 在
`temp\sunshine\`。

打包前會檢查 SC-HID driver（`Sunshine\src\platform\windows\sc_hid_driver\`）：5 個檔案
（`.dll`／`.inf`／`.cat`／`.cer`／`Install-VipleSCHid.ps1`）缺一個、或 DLL 比原始碼舊，
就直接失敗，不會出半包。修法是在**系統管理員** PowerShell 跑同目錄的
`Build-ScHidDriver.ps1`（重建並簽章），再重跑 `build_sunshine.cmd`。

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
build-tools\propagate_version.cmd
```

把 `version.json` 目前的值同步到三個子專案的檔案（Sunshine CMakeLists.txt、
moonlight-qt/app/version.txt、moonlight-android/app/build.gradle）。不會 bump。

**使用時機：**
- 你手動改了 `version.json`（不建議，但有時會）
- 發現三個子專案版號 drift，想強制統一
- debug 版號相關問題時先確認同步狀態

### 2.6 Bump 版號（不建置）

```cmd
build-tools\bump_version.cmd                          :: patch + 1
pwsh build-tools\version.ps1 bump -Part minor         :: minor + 1, patch 歸 0
pwsh build-tools\version.ps1 bump -Part major         :: major + 1, minor/patch 歸 0
```

每個 build script 已經會自動呼叫 bump（`build_android.cmd` 除外；帶 `--no-bump` 時也不會）。
這個 script 一般不需要手動跑；建置前先手動 bump 再跑預設會 bump 的 script，版號會跳兩格。

### 2.7 查目前版號

```cmd
pwsh build-tools\version.ps1 get
```

### 2.8 部署

```cmd
scripts\deploy_client_now.cmd    :: 把 temp\moonlight\* 複製到 %DEPLOY_CLIENT%
pwsh scripts\deploy_server.ps1   :: 開發機本身也當 host 時：temp\sunshine\ → %DEPLOY_SERVER%，並重啟服務
```

`scripts\` 是本機目錄、不在 git 裡（§0），沒有這些檔就照同樣的步驟自己做。需要寫
`C:\Program Files\*` 的話要用 admin。路徑由 `build-config.local.cmd` 的 `DEPLOY_CLIENT` /
`DEPLOY_SERVER` 控制。Windows server 的 service 名是 `VipleStreamServer`。

要把 server 部署到**另一台** host，用 SSH 版的
`pwsh scripts\deploy_server_to_host.ps1 -RemoteHost <user>@<host>`（scp 上傳 zip → 在 host 停
service → 覆蓋 → 確認 SC-HID driver 版號 → 啟 service；預設挑 `release\` 裡最新的 Server zip，
`-Version X.Y.Z` 可指定）。腳本不驗 server 本身的版號，部署後請自己查 `viplestream-server.exe`
的 FileVersion，或 `sunshine.log` 開頭的 `version:` 那一行。

## 3. 輸出檔案位置

```
<repo>\
├── release\
│   ├── VipleStream-Server-X.Y.Z.zip                  ← Windows Server（可直接解壓安裝）
│   ├── VipleStream-Client-X.Y.Z.zip                  ← Windows Client
│   ├── VipleStream-Client-X.Y.Z-debug.zip            ← Client 的 PDB（當機分析用）
│   ├── VipleStream-Android-X.Y.Z.apk                 ← Android
│   ├── VipleStream-Client-X.Y.Z-linux-x64.AppImage   ← Linux Client（build_all 的 WSL 步驟，或從 Linux 機 scp 回來）
│   └── VipleStream-Server-X.Y.Z-linux-x64.deb        ← Linux Server（從 Linux 機 scp 回來，見 §7）
├── temp\
│   ├── sunshine\                           ← Sunshine 打包 staging
│   ├── moonlight\                          ← Moonlight 打包 staging（含 VipleStream.exe）
│   └── current_version.txt                 ← 這一趟 bump／propagate 後的版號（腳本自用）
├── Sunshine\build_mingw\                  ← Sunshine build output（viplestream-server.exe）
└── moonlight-qt\app\release\              ← Moonlight build output（VipleStream.exe）
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
:: 開發機本身當 host：pwsh scripts\deploy_server.ps1（停 VipleStreamServer → 覆蓋 → 啟動）
:: 另一台 host：pwsh scripts\deploy_server_to_host.ps1 -RemoteHost <user>@<host>
:: 沒有這些本機 script 時：停 VipleStreamServer 服務，把 zip 解壓到
::   C:\Program Files\VipleStream-Server\，再啟動服務
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
:: 要發 release 還得在 Linux 機建 AppImage 與 .deb（§7），五件同版號才算齊
```

### 情境 E：發現設定畫面的版號跟 release 檔名不一致

```cmd
pwsh build-tools\version.ps1 get                :: 確認 version.json 值
build-tools\propagate_version.cmd               :: 強制同步下游
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
  `build-tools\build_moonlight_package.cmd` 的 shader 清單（`[pkg 2/5]` 區段裡以
  `for %%F in (d3d11_vertex.fxc` 開頭那一行），**讓 script 記住**這個檔案，下次建置才會自動包進去

## 6. Script 維護

如果要新增 shader / DLL / data file 到打包清單（行號會隨改動漂移，所以下表用區段標記與清單
開頭定位；找不到就 grep `for %%F in`）：

| 要加的東西 | 改哪個 script |
|---|---|
| 新的 `.fxc` shader | `build-tools\build_moonlight_package.cmd`：`[pkg 2/5]` 區段的 `for %%F in (d3d11_vertex.fxc …)`。`.hlsl` → `.fxc` 由同一段呼叫的 `compile_d3d11_shaders.ps1` 自動重編 |
| 新的 DLL（放在 `moonlight-qt\libs\windows\lib\x64\`） | 同檔 `[pkg 1/5]` 區段的 `for %%F in (SDL2.dll …)` |
| 新的 DLL（其他來源，例如 onnxruntime、NvOFFRUC、ncnn、Aftermath） | 同檔 windeployqt（`[pkg 3/5]`）之後，比照既有的 `set "XXX_DLL=…"` 加一個獨立區塊 |
| 新的 Sunshine 輸出檔 | `build_sunshine.cmd` 的 `[3/4]` 收集區段：`tools\` 下的 exe 加進 `for %%F in (viplestream-svc.exe …)`；主目錄的 exe 比照 `viple-splash.exe` 加 `if exist` 區塊；SC-HID driver 檔在 `for %%F in (VipleSCHid_Driver.dll …)` |
| 新的子專案要同步版號 | `build-tools\version.ps1` 的 `Propagate-Version` 新增 block，並更新 `docs/versioning.md` §2 的表格 |
| 新的建置目標（如 iOS） | 建立 `build_ios.cmd`，呼叫 `build-tools\version.ps1 propagate` + 實際建置 |
| Linux AppImage／`.deb` 的內容 | `moonlight-qt/scripts/build-appimage-native.sh`（linuxdeploy 參數）／`Sunshine/cmake/packaging/linux.cmake` |

**注意：Windows 端這些 script 都不在 git 裡**（§0）。改了只存在你自己的工作樹，`git pull`
不會把新清單帶給其他開發者，PR 裡也審不到。清單有變動時，請在 commit 訊息或相關文件寫明
「哪個 script 的哪一段要加什麼」，讓其他人同步自己的本機副本。Linux 端的
`moonlight-qt/scripts/`、`Sunshine/scripts/`、`Sunshine/cmake/` 在 git 裡，改動會隨 commit 走。

## 7. Linux 建置（x64 AppImage／.deb）

Linux 兩件要在 Linux x64 機器上建（Ubuntu 26.04 驗證過）。入口是兩支**已入 git** 的腳本：

| 產物 | 腳本（在 git 裡） | 輸出 | 上 release 的檔名 |
|---|---|---|---|
| Client AppImage | `moonlight-qt/scripts/build-appimage-native.sh`（在 `moonlight-qt/` 目錄執行） | `moonlight-qt/build/installer-release/VipleStream-Client-X.Y.Z-linux-x64.AppImage` | 同左 |
| Server `.deb` | `Sunshine/scripts/linux_build.sh`（在 `Sunshine/` 目錄執行，例如 `bash scripts/linux_build.sh --skip-cuda --skip-cleanup`；`--help` 列出全部選項） | `Sunshine/build/cpack_artifacts/VipleStream-Server.deb`（檔名沒有版號，套件內的 Version 正確） | 改名成 `VipleStream-Server-X.Y.Z-linux-x64.deb` |

**版號**：兩支腳本讀的是已經 propagate 進 repo 的版號檔（`moonlight-qt/app/version.txt`、
`Sunshine/CMakeLists.txt`），**不要在 Linux 機上 bump**。版號由 Windows 開發機的
`build-tools\version.ps1` 決定並 commit；Linux 機 `git pull` 到同一個 commit 再建（還沒 push
時，在開發機用 `git diff` 產 patch、scp 過去 `git apply`）。建完核對產物版號和 `version.json`
一致。

**Client（`build-appimage-native.sh`）前置**：
- `qmake6`、`linuxdeploy`（含 Qt plugin）要在 `PATH` 上。
- ncnn 目前**必須裝在 `/usr/local`**：`app.pro` 只檢查 `/usr/local/include/ncnn/mat.h`，找不到就不定義
  `VIPLESTREAM_HAVE_NCNN`、也不連 `-lncnn`，但 `plvk.cpp` 無條件 include `<ncnn/…>` 標頭，所以編譯會失敗。
  腳本的 `NCNN_PREFIX`（預設 `~/.local/ncnn`）實際上只用來設 `LD_LIBRARY_PATH`，讓 linuxdeploy 找到並打包
  `libncnn.so`；腳本雖然也把它傳給 qmake，但 `app.pro` 沒有讀這個變數（腳本註解寫的 "app.pro honors it" 是錯的）。
  ncnn 裝在 `/usr/local` 時跑一次 `sudo ldconfig` 即可，不必設 `NCNN_PREFIX`。讓 `app.pro` 改讀 `NCNN_PREFIX`
  屬於 3.0 的 F8／C2（[`vr_architecture.md`](vr_architecture.md) §2.1）。
- `vkfruc.cpp` 還會 include `<ncnn/stb_image_write.h>`，但原始碼建的 ncnn 不會安裝這個標頭；要另外裝
  `libstb-dev`，做法見 `app.pro` 裡 §K.X 那段註解。
- 連結的是真正的 SDL2（不是 sdl2-compat），腳本會把它打包進 AppImage。
- 以 `DEFINES+=VIPLE_MPQUIC` 建置，所以 `Sunshine/third-party/picoquic`（submodule）要先初始化，
  而且 `Sunshine/third-party/picoquic/build/` 裡要有 picoquic／picotls 靜態庫（`app.pro` 的 `PICOQUIC_BUILD`）。
- 腳本固定帶 `CONFIG+=disable-wayland CONFIG+=disable-libdrm`，所以出貨的 AppImage 沒有 DrmRenderer，也沒有
  moonlight 自己的 Wayland 整合（見 [`rendering_paths.md`](rendering_paths.md) 的註 ❷）。
- 有裝 `fonts-noto-cjk` 就會把 CJK 字型包進去，沒裝就用系統 fontconfig。

**Server（`linux_build.sh`）前置**：
- `third-party/inputtino`、`third-party/wlr-protocols` 兩個 submodule 沒有 vendor 進 repo，要依本 fork
  所基於的上游 Sunshine commit 鎖定的 SHA 另外 clone 到 `Sunshine/third-party/` 下（不要用 branch tip）。
- 系統沒有合用的 Boost 時，CMake 會用 FetchContent 自己建，不必手動裝。
- `.deb` 的 Depends 要維持擇一寫法 `libayatana-appindicator3-1 | libappindicator3-1`
  （`Sunshine/cmake/packaging/linux.cmake`）。兩者在 Ubuntu 26.04 互相 Conflicts，兩個都硬列會讓 apt
  判定套件無法安裝；host 的自我更新也要靠這個 `.deb` 裝得起來。

**其他注意事項**：
- 已入 git 的 `.sh` 受 `.gitattributes`（根目錄與 `Sunshine/` 都有 `*.sh text eol=lf`）保護，Windows 工作樹
  即使 `core.autocrlf=true`，checkout 出來也是 LF。會踩到 CRLF（`$'\r': command not found`）的是根目錄
  `scripts\` 裡不入 git 的本機 `.sh`（例如 `wsl_*.sh`），以及從 Windows 工作樹 rsync／cp 過去的其他文字檔
  （`.sh`、`.cmd` 以外的文字檔在 `autocrlf=true` 下會 checkout 成 CRLF）。在 Linux 機上直接
  `git clone`／`git pull` 就能避開。
- 長時間建置用 `nohup setsid … &` 背景跑再輪詢 log，避免 SSH 斷線把建置一起帶走。
- 產物用 scp 拉回 Windows 開發機的 `release\`，核對 sha256；測試產物不上雲，只有正式 release 才上 GitHub。
- **Linux arm64（Steam Frame client，3.0 規劃中）**：預計新增 `moonlight-qt/scripts/build-steamframe.sh`
  產 Flatpak，見 [`vr_architecture.md`](vr_architecture.md) §2.1、§6。

## 8. 為什麼要這麼死守 script？

歷史教訓（別問為什麼這些都發生過）：
- 手動 `nmake` 過 → 版號 drift，release zip 裡是 1.2.5 但 Settings 顯示 1.2.3
- 忘了 windeployqt → exe 跑起來噴 `qt6core.dll not found`
- 新 shader 沒加到打包清單 → 使用者裝了新版但 FRUC 靜默失效
- Android 版號沒 bump → Play Store 拒絕 upload（versionCode 重複）
- 手動改 CMakeLists.txt 版號 → 忘了同步 moonlight-qt/version.txt，兩邊對不上

Script 把這些踩過的雷都 encode 起來。走 script 就不會再踩第二次。
