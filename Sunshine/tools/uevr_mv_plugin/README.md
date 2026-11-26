# VipleStream UEVR 遊戲內動態向量與深度擷取外掛（`viple_uevr_mv_plugin.dll`）

本工具為 **§VR-GAME-MV（M-MV0 ／ M-MV1 階段）** 的獨立 UEVR（Unreal Engine VR）外掛，用於在開啟 DLSS（Super Resolution／Ray Reconstruction）的 UE 遊戲（如 Palworld UE5.1）行程內，直接從 D3D12 算圖管線擷取每眼的螢幕空間動態向量（Motion Vectors）與深度（Depth），在遊戲 GPU 上執行 8×8 前景優先池化（Closest-Depth Pooling）降採樣至 1/8 解析度，並透過跨行程 Named Pipe 握手與共享記憶體（SHM seqlock 環狀緩衝）即時交接給 `viplestream-server`，同時保留非同步磁碟傾印與離線反向 Warp 驗證功能。

設計規格詳見 [`docs/specs/vr_game_motion_vectors.md`](../../../docs/specs/vr_game_motion_vectors.md)（§1、§2、§3、§6.1 M-MV0/M-MV1、§6.2），跨行程 ABI 定義於 [`Sunshine/src/vr/vr_mv_ipc_abi.h`](../../src/vr/vr_mv_ipc_abi.h)。

---

## 1. 檔案結構

| 檔案 | 說明 |
|---|---|
| `plugin_main.cpp` | UEVR 外掛主體：NGX D3D12 攔截、分眼判定、Compute Shader 派發、資源狀態還原、M-MV1 Named Pipe 連線與 LocalSystem 身分驗證、D3D12 Readback 環狀緩衝 + Fence + Publish 執行緒 SHM 發布、延遲注入（`viple_mv_delay.txt`）、每 600 幀 `[VIPLE-GAME-MV]` 與 `[VIPLE-GAME-MV-IPC]` 統計日誌、M-MV0 非同步磁碟傾印 |
| `ngx_minimal.h` | 依 NVIDIA 官方 DLSS SDK（MIT）自寫的最小介面宣告（含 `NVSDK_NGX_Parameter` 虛擬函式表與公開參數名稱） |
| `cs_downsample_mv_8x8.hlsl` | D3D12 `8×8 → 1` 前景優先池化降採樣 Compute Shader（同時內嵌於 DLL 作為備援，支援部署同目錄 `.hlsl` 熱修改） |
| `check_mv8.py` | 離線驗證腳本：讀取連續兩幀傾印，以 `mv8` 反向 Warp 重建上一幀，驗證 PSNR 增益、X/Y 方向與跨眼污染 |
| `CMakeLists.txt` | 獨立 CMake 建置設定（引入相對路徑 `../../src/vr` 以使用 `vr_mv_ipc_abi.h`） |
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

啟動遊戲並注入 UEVR（確認遊戲開啟 D3D12 與 DLSS Super Resolution）後，可在 UEVR 的 `log.txt` 中看到 `[VIPLE-GAME-MV]` 與 `[VIPLE-GAME-MV-IPC]` 標籤的初始化、攔截成功、Named Pipe 握手連線與每 600 幀的統計訊息。

---

## 4. M-MV1 跨行程交接（Named Pipe ＋ SHM Seqlock）與延遲注入測試

### 4.1 自動連線與即時發布流程

外掛載入後會自動啟動兩條背景執行緒（遊戲算圖執行緒上零阻塞等待）：

1. **IPC 連線執行緒（Named Pipe 握手與映射）**：
   - 自動連線至同一個 Windows Session 的 `\\.\pipe\VipleStreamVRMV-<session_id>`（Message Mode）。
   - **Server 身分驗證**：透過 `GetNamedPipeServerProcessId` + `OpenProcess` + `OpenProcessToken(TokenUser)` 及 `GetSecurityInfo` 驗證 Server 必須為 `S-1-5-18`（LocalSystem）且位於同一 Session，不符即刻斷線。
   - 送出 `vrmv_hello_t`，接收 `vrmv_welcome_t` 或 `vrmv_reject_t`（固定以 `VRMV_PIPE_MAX_MSG` 緩衝先驗 Header 再驗 ABI 與 `sizeof`）。
   - 對 `WELCOME.dup_section_handle` 執行 `MapViewOfFile`（映射整個 `VRMV_SECTION_SIZE` = 7,376,896 B），以 `VirtualQuery` 確認映射區大小並驗證 `vrmv_server_block_t` 的 `magic`、`abi_version`、`generation`。
   - **退避重試**：連不上或一般被拒時採指數退避（1 秒起、上限 10 秒）；若被拒原因為 `VRMV_REJ_DISABLED` 或 `VRMV_REJ_IDENTITY` 則改為每 30 秒重試。Pipe 斷線或收到 `VRMV_MSG_BYE` 時自動 `UnmapViewOfFile` 並回到重連。

2. **Readback 與 Publish 執行緒（GPU Fence 等待與 SHM 寫入）**：
   - 當已連上 Server 且雙眼齊備（左眼已就緒、右眼剛完成 `cs_downsample_mv_8x8` 派發與 UAV Barrier）時，於同一張 `ID3D12GraphicsCommandList` 將 `m_out_mv8_tex` 轉為 `COPY_SOURCE`，以 `GetCopyableFootprints` 取得的 Footprint 執行 `CopyTextureRegion` 複製至 `VRMV_SLOTS`（4 個）`D3D12_HEAP_TYPE_READBACK` 環狀緩衝之一，再轉回 `UNORDERED_ACCESS`。缺一眼時不複製並遞增 `frames_dropped_incomplete`。
   - 在該雙眼齊備遊戲幀的第一次 `on_present` 於遊戲 `ID3D12CommandQueue` 上 `Signal` 外掛專用 Fence 並記下 `game_present_qpc`。
   - Publish 執行緒透過 `SetEventOnCompletion` 等待 GPU 完成（上限 **50 ms**；逾時則放棄該筆並遞增 `frames_dropped_gpu_late`），完成後記下 `gpu_done_qpc`，將 Readback 緩衝逐列緊密複製至 `vrmv_payload_offset(slot)`，並以 Per-Slot Seqlock（`2n+1` → Payload ＋ `vrmv_desc_t` 欄位 → `2n+2`）與 `vrmv_store_release_u64(&write_index, mv_frame_id)` 發布。
   - 每 **100 ms** 定時更新 `shm->writer` 的 `writer_heartbeat_qpc`、`frames_published`、`frames_dropped_gpu_late`、`frames_dropped_incomplete` 與 `writer_pid`。

### 4.2 延遲注入測試（`viple_mv_delay.txt`）

為驗證設計文件 §6.2 M-MV1 的「遊戲外掛內注入延遲時，`display_vr_t` 在 $\le 0.5\text{ ms}$ 內放棄該筆遲到 MV、不拖慢視訊幀擷取」，可在 `viple_uevr_mv_plugin.dll` 所在的 `plugins/` 目錄下建立 `viple_mv_delay.txt`，內容寫入要注入的延遲毫秒數（整數，`0` 或刪除檔案即關閉；Publish 執行緒每 2 秒自動重讀一次）：

```cmd
:: 在 GPU Readback 完成後多 Sleep 5 ms 再寫入 SHM：
echo 5 > "%APPDATA%\UnrealVRMod\UEVR\plugins\viple_mv_delay.txt"

:: 關閉延遲注入（寫入 0 或直接刪除檔案）：
del "%APPDATA%\UnrealVRMod\UEVR\plugins\viple_mv_delay.txt"
```

### 4.3 `[VIPLE-GAME-MV-IPC]` 統計日誌

外掛在 UEVR `log.txt` 中會以 `[VIPLE-GAME-MV-IPC]` 記錄連線狀態、被拒原因，以及每 600 個遊戲幀一行統計：

```text
[VIPLE-GAME-MV-IPC] 600-game-frame stats (window_gf=600, connected=1, gen=1, delay_ms=0): published=1800 (win=600), dropped_incomplete=0 (win=0), dropped_gpu_late=0 (win=0, rb_full=0), gpu_done->publish_us=(p50:85.2, p95:142.0), game_present->publish_us=(p50:410.5, p95:680.3)
```

---

## 5. 非同步傾印與離線驗證方法（M-MV0，可與 M-MV1 同時開啟）

### 5.1 觸發傾印

在遊戲執行中，於 `viple_uevr_mv_plugin.dll` 所在的同一層 `plugins/` 目錄下建立純文字檔 `viple_mv_dump.txt`，內容寫入要連續傾印的幀數（例如 `10`），若需一併傾印每眼 MV 原始貼圖的 1/4 縮小除錯版（直接 4×4 平均、不做前景池化，格式 `RG32F`），可在幀數後加上 `raw`（例如 `12 raw`）：

```cmd
echo 10 > "%APPDATA%\UnrealVRMod\UEVR\plugins\viple_mv_dump.txt"
:: 或同時傾印 1/4 原始 MV 除錯檔 (mv4_raw_*.bin)：
echo 12 raw > "%APPDATA%\UnrealVRMod\UEVR\plugins\viple_mv_dump.txt"
```

外掛偵測到 `viple_mv_dump.txt` 後：
1. 接下來連續 `N` 個雙眼齊備遊戲幀（固定由同幀左眼開啟、右眼結算）會在遊戲 `on_present` 時透過 `D3D12_HEAP_TYPE_READBACK` 環狀緩衝區與 `ID3D12Fence` 交由背景執行緒非同步寫入 `<外掛目錄>\dump\`（**不在遊戲 CPU/GPU 執行緒上同步等待**）：
   - `dump/mv8_0000.bin`：雙眼並排 1/8 解析度 `RGBA16F`（`.xy` = 單眼正規化 UV 位移 `uv_prev - uv_cur`，`.z` = 正規化逆深度，`.w` = 有效度；左半為左眼、右半為右眼）。
   - `dump/mv8_0000.json`：記錄網格尺寸、算圖尺寸、格式、`mvScale`、`jitter`、`reset`、NGX 每次評估的子區域參數（`subrect`／`render_subrect`：含 `DLSS.Input.Color/Depth/MV.Subrect.Base.X/Y`、`DLSS.Output.Subrect.Base.X/Y`、`DLSS.Render.Subrect.Dimensions.Width/Height`）、分眼判定來源以及 UEVR HMD Pose 與雙眼 UE Projection 矩陣。
   - `dump/color8_0000.png`：雙眼並排 1/8 解析度 `RGBA8` Color 縮圖。
   - `dump/mv4_raw_0000.bin`（當觸發檔包含 `raw` 時輸出）：雙眼並排 1/4 原始 MV 貼圖縮小版 `RG32F`（直接 4×4 算術平均、不做前景深度池化，尺寸記錄於 json 的 `raw_mv4_eye_grid_w/h` 與 `raw_mv4_sbs_grid_w/h`）。
2. 連續 `N` 幀全部寫入磁碟完成後，外掛會自動刪除 `viple_mv_dump.txt`。

### 5.2 執行離線驗證腳本

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

## 6. 授權說明（Licensing）

1. **UEVR 外掛 SDK 標頭（MIT License）**：
   - `third_party/uevr/API.h`、`third_party/uevr/API.hpp`、`third_party/uevr/Plugin.hpp` 三個標頭檔為 praydog 以 **MIT License** 獨立授權釋出（檔頭明確聲明 *"This file is licensed under the MIT license and is separate from the rest of the UEVR codebase. Copyright (c) 2023 praydog"*）。
2. **本目錄其餘程式碼（Clean-room 自寫）**：
   - `plugin_main.cpp`、`ngx_minimal.h`、`cs_downsample_mv_8x8.hlsl`、`check_mv8.py`、`CMakeLists.txt`、`build.cmd` 皆為 VipleStream 專案依公開 SDK 規範從頭獨立撰寫。
   - `ngx_minimal.h` 僅依 NVIDIA 官方公開的 DLSS/NGX SDK（MIT License，`github.com/NVIDIA/DLSS`）函式簽名、參數名稱與虛擬函式表順序自行宣告，不包含也不夾帶任何 NVIDIA `.dll` 或 `.lib` 二進位檔。
3. **無侵權保證**：
   - UEVR 主體與 PureDark 的 UEVR-AFW 為保留所有權利（All Rights Reserved）；OptiScaler 為 GPLv3；Unreal Engine 為 Epic Games 專有授權。本外掛**未複製上述專案的任何一行程式碼或二進位檔**。
