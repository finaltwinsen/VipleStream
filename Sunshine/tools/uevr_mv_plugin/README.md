# VipleStream UEVR 遊戲內動態向量與深度擷取外掛（`viple_uevr_mv_plugin.dll`）

本工具為 **§VR-GAME-MV（M-MV0 階段）** 的獨立 UEVR（Unreal Engine VR）外掛，用於在開啟 DLSS（Super Resolution／Ray Reconstruction）的 UE 遊戲（如 Palworld UE5.1）行程內，直接從 D3D12 算圖管線擷取每眼的螢幕空間動態向量（Motion Vectors）與深度（Depth），在遊戲 GPU 上執行 8×8 前景優先池化（Closest-Depth Pooling）降採樣至 1/8 解析度，並提供非同步磁碟傾印與離線反向 Warp 驗證工具。

設計規格詳見 [`docs/specs/vr_game_motion_vectors.md`](../../../docs/specs/vr_game_motion_vectors.md)（§1、§2、§3.2、§6.1 M-MV0、§6.2）。

---

## 1. 檔案結構

| 檔案 | 說明 |
|---|---|
| `plugin_main.cpp` | UEVR 外掛主體：NGX D3D12 攔截、分眼判定、Compute Shader 派發、資源狀態還原、Readback Heap + Fence + 背景執行緒非同步傾印、每 600 幀 `[VIPLE-GAME-MV]` 統計日誌 |
| `ngx_minimal.h` | 依 NVIDIA 官方 DLSS SDK（MIT）自寫的最小介面宣告（含 `NVSDK_NGX_Parameter` 虛擬函式表與公開參數名稱） |
| `cs_downsample_mv_8x8.hlsl` | D3D12 `8×8 → 1` 前景優先池化降採樣 Compute Shader（同時內嵌於 DLL 作為備援，支援部署同目錄 `.hlsl` 熱修改） |
| `check_mv8.py` | 離線驗證腳本：讀取連續兩幀傾印，以 `mv8` 反向 Warp 重建上一幀，驗證 PSNR 增益、X/Y 方向與跨眼污染 |
| `CMakeLists.txt` | 獨立 CMake 建置設定（不連動 Sunshine 主專案） |
| `build.cmd` | 獨立建置腳本（讀取 repo 根目錄 `build-config.local.cmd` 的 `VCVARS`，產出至 `temp/uevr_mv_plugin/`） |
| `third_party/uevr/` | UEVR 官方外掛 SDK 的三個 MIT 授權標頭（`API.h`、`API.hpp`、`Plugin.hpp`） |

---

## 2. 建置方式

1. 確認 repo 根目錄已建立 `build-config.local.cmd` 且設定好 `VCVARS`（指向 Visual Studio 2022 的 `vcvars64.bat`）。
2. 執行本目錄下的 `build.cmd`：
   ```cmd
   Sunshine\tools\uevr_mv_plugin\build.cmd
   ```
3. 建置完成後，x64 Release 產物位於：
   - `<repo_root>\temp\uevr_mv_plugin\viple_uevr_mv_plugin.dll`
   - `<repo_root>\temp\uevr_mv_plugin\cs_downsample_mv_8x8.hlsl`

---

## 3. 安裝方式

將建置產出的 `viple_uevr_mv_plugin.dll`（建議連同 `cs_downsample_mv_8x8.hlsl` 一併複製）放入 UEVR 的 `plugins` 資料夾：

- **全域 UEVR 外掛目錄**：`%APPDATA%\UnrealVRMod\UEVR\plugins\`
- **或單一遊戲設定目錄**：`%APPDATA%\UnrealVRMod\<GameExecutableName>\plugins\`

啟動遊戲並注入 UEVR（確認遊戲開啟 D3D12 與 DLSS Super Resolution）後，可在 UEVR 的 `log.txt` 中看到 `[VIPLE-GAME-MV]` 標籤的初始化、攔截成功與每 600 幀的雙眼呼叫統計訊息。

---

## 4. 非同步傾印與離線驗證方法

### 4.1 觸發傾印

在遊戲執行中，於 `viple_uevr_mv_plugin.dll` 所在的同一層 `plugins/` 目錄下建立純文字檔 `viple_mv_dump.txt`，內容寫入要連續傾印的幀數（例如 `10`）：

```cmd
echo 10 > "%APPDATA%\UnrealVRMod\UEVR\plugins\viple_mv_dump.txt"
```

外掛偵測到 `viple_mv_dump.txt` 後：
1. 接下來連續 `N` 幀會在遊戲 `on_present` 時透過 `D3D12_HEAP_TYPE_READBACK` 環狀緩衝區與 `ID3D12Fence` 交由背景執行緒非同步寫入 `<外掛目錄>\dump\`（**不在遊戲 CPU/GPU 執行緒上同步等待**）：
   - `dump/mv8_0000.bin`：雙眼並排 1/8 解析度 `RGBA16F`（`.xy` = 單眼正規化 UV 位移 `uv_prev - uv_cur`，`.z` = 正規化逆深度，`.w` = 有效度；左半為左眼、右半為右眼）。
   - `dump/mv8_0000.json`：記錄網格尺寸、算圖尺寸、格式、`mvScale`、`jitter`、`reset`、分眼判定來源（`stereo_view_offset` 或 `frame_call_ordinal`）以及 UEVR HMD Pose 與雙眼 UE Projection 矩陣。
   - `dump/color8_0000.png`：雙眼並排 1/8 解析度 `RGBA8` Color 縮圖。
2. 連續 `N` 幀全部寫入磁碟完成後，外掛會自動刪除 `viple_mv_dump.txt`。

### 4.2 執行離線驗證腳本

使用 Python 虛擬環境（需具備 `numpy`）執行 `check_mv8.py`：

```cmd
python -m venv .venv
.venv\Scripts\pip install numpy
.venv\Scripts\python Sunshine\tools\uevr_mv_plugin\check_mv8.py --dump-dir "%APPDATA%\UnrealVRMod\UEVR\plugins\dump"
```

腳本會對每一對連續幀 `(t-1, t)` 進行雙線性反向 Warp，並依設計文件第 6.2 節（M-MV0）標準檢查：
1. 每次 `Present` 是否穩定攔截到左、右眼各 1 次 `EvaluateFeature`；
2. 動態區域經 `mv8` 反向 Warp 後的 PSNR 是否比未 Warp 提升 $\ge 6.0\text{ dB}$；
3. X/Y 方向是否有顛倒（測試 `+X/-X/+Y/-Y` 與軸交換）、左眼與右眼是否有交叉污染（含跨眼 `PrevViewProj` IPD 視差污染檢測）。

---

## 5. 授權說明（Licensing）

1. **UEVR 外掛 SDK 標頭（MIT License）**：
   - `third_party/uevr/API.h`、`third_party/uevr/API.hpp`、`third_party/uevr/Plugin.hpp` 三個標頭檔為 praydog 以 **MIT License** 獨立授權釋出（檔頭明確聲明 *"This file is licensed under the MIT license and is separate from the rest of the UEVR codebase. Copyright (c) 2023 praydog"*）。
2. **本目錄其餘程式碼（Clean-room 自寫）**：
   - `plugin_main.cpp`、`ngx_minimal.h`、`cs_downsample_mv_8x8.hlsl`、`check_mv8.py`、`CMakeLists.txt`、`build.cmd` 皆為 VipleStream 專案依公開 SDK 規範從頭獨立撰寫。
   - `ngx_minimal.h` 僅依 NVIDIA 官方公開的 DLSS/NGX SDK（MIT License，`github.com/NVIDIA/DLSS`）函式簽名、參數名稱與虛擬函式表順序自行宣告，不包含也不夾帶任何 NVIDIA `.dll` 或 `.lib` 二進位檔。
3. **無侵權保證**：
   - UEVR 主體與 PureDark 的 UEVR-AFW 為保留所有權利（All Rights Reserved）；OptiScaler 為 GPLv3；Unreal Engine 為 Epic Games 專有授權。本外掛**未複製上述專案的任何一行程式碼或二進位檔**。
