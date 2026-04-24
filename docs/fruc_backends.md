# VipleStream Moonlight — FRUC backend 備註

FRUC = Frame Rate Up-Conversion，client 端對收到的 decoded 畫面做補幀，
視覺 fps 是 server 實際輸出的 2 倍。有三個實作路徑，Moonlight UI 可切：

| Backend | 實作 | 補幀品質 | 效能成本 |
|---|---|---|---|
| **Generic compute shader** | D3D11 compute shader（block-match ME + bilinear warp） | 輕量 motion-compensated | GPU 最輕，~0.5 ms/frame @ 1080p |
| **NVIDIA Optical Flow (NvOF)** | NvOFFRUC SDK + CUDA（硬體 optical flow unit） | 高品質 motion | 吃 NVOFA 硬體時間，A1000 @ 1080p ~15-35 ms/frame |
| **DirectML** | DML graph / ONNX model | 真 ML 補幀（跑 ONNX model 時） / 單純 crossfade（fallback inline graph） | DML kernel-launch 密集，A1000 跑 RIFE lite v2 ~20-25 ms/frame |

## 三條路上的共通改動

- **180fps runtime cap 已拿掉**（session.cpp + d3d11va.cpp）— 使用者設 > 180 fps
  時 FRUC 不再被靜默停用
- `SetMaximumFrameLatency(3)` 搭配 two-Present per real frame，vsync
  pacing；移掉了 interp → real Present 之間的 `WaitForSingleObjectEx`
  stall（那個 stall 在 244Hz display 上每幀吃 ~4 ms，會把 real fps 砍半）

## 各 backend 專屬細節

### Generic compute shader
- shader 檔案：`moonlight-qt/app/d3d11_motionest_compute.hlsl` /
  `d3d11_warp_compute.hlsl`，編譯出三套（Quality / Balanced / Performance）
- Quality = 無 MV median filter，Balanced = 3×3 median，Performance =
  5×5 median + 大 block size
- iGPU（Intel UHD / Microsoft Basic Render）自動 cap 到 1080p，
  否則 4K TDR

### NVIDIA Optical Flow (NvOF)
- **需要 bundle**：`NvOFFRUC.dll` + `cudart64_110.dll` 跟 Moonlight.exe 同
  目錄（build script 已設好）
- `NvOFFRUC.dll` 來自 NVIDIA Optical Flow SDK，import `cudart64_110.dll`
  (CUDA 11.0 runtime) + `nvcuda.dll` + `nvofapi64.dll`；driver 帶 nvcuda
  + nvofapi，cudart 要另外附（NVIDIA CUDA Toolkit 11.8 redistributable
  取得，EULA 允許 redistribute）
- timestamp 單位是 **秒**（不是毫秒）— v1.2.56 把 `SDL_GetTicks()/1000.0`
  改過去；之前傳 ms 造成 NvOFFRUC 把每兩幀間距看成 17 秒，
  觸發 `bRepeat=true` 放棄補幀
- `processUs` 每幀真實耗時 log 每 60 幀印一次（`[VIPLE-FRUC-NvOF]`）
- debug 環境變數：`VIPLE_NVOF_FORCE_USE=1` 強制吃 output 即便
  `bRepeat=true`
- A1000 實測：1080p @ 122fps 目標下 processUs 15-35 ms，吃不到 target

### DirectML
- v1.2.59 之前的 `BindingTable->Reset(exec)` 路徑在 RTX A1000 DML validator
  被拒（`0x887a0005` + `GetDeviceRemovedReason = S_OK`）。拆成兩張獨立
  `IDMLBindingTable`、同 heap 不同 descriptor slice 解掉。
- ONNX model lookup 順序（`Path::getDataFilePath`）：
  1. `%LOCALAPPDATA%\Moonlight Game Streaming Project\VipleStream\data\` （cache）
  2. current working directory
  3. `AppDataLocation`
  4. **Moonlight.exe 同目錄**（build script 把 `tools/fruc.onnx` copy 來這）
  5. QRC embedded（目前沒塞）
- ONNX 支援的 input layout：
  - 1-input concat C ∈ {6, 7, 8, 9}：prev(RGB/RGBA) + curr(RGB/RGBA) + optional timestep plane
  - 2-input：separate prev + curr tensors
  - 3-input：separate prev + curr + scalar timestep
- 預設 bundle model：**RIFE v4.25 lite v2**（來自
  `https://github.com/AmusementClub/vs-mlrt/releases/tag/external-models`
  的 `rife_v4.25_lite.7z` → `rife_v2/` 資料夾，7-channel concat 輸入）
- **DirectML tensor 解析度 cap**（v1.2.60 加）：
  - 預設 **720p** — ONNX model 還是 shared weights 跑 native 但 D3D12
    tensor buffer cap 在 1280x720，最後 blit 時 bilinear upscale 到
    display 解析度
  - 環境變數 `VIPLE_DML_RES=540|720|1080|native` 可 override
  - A1000 實測 FP32 RIFE lite v2：1080p ~44fps → 720p ~45fps →
    540p ~51fps（kernel-launch overhead 佔比大，降解析度 gain 有限）
- inline crossfade graph（fallback）：`idA(0.5·prev) + idB(0.5·curr) →
  ADD1 → CLIP`；視覺上跟 pure blend compute shader 等價，不是 motion
  compensated。ONNX load / compile 任何一步失敗都自動 fallback
- debug 環境變數：`VIPLE_DIRECTML_DEBUG=1` 開 D3D12 debug layer +
  DML_CREATE_DEVICE_FLAG_DEBUG；Graphics Tools 沒裝會自動 fallback
  非 debug 路徑（ORT 回 0x887a002d = SDK_COMPONENT_MISSING 的時候）

## 如何自製 / 替換 fruc.onnx

RIFE-compatible ONNX model 放到下列任一處會被拾起：
- `%EXEDIR%\fruc.onnx`（正式 release 的位置）
- `%LOCALAPPDATA%\Moonlight Game Streaming Project\VipleStream\data\fruc.onnx`
- CWD

input shape 必須是：
- `[N, 7, H, W]` 或 `[N, 6, H, W]` concat 佈局（timestep 塞在 channel 7）
- 或 2-3 個 separate input tensor（`img0`, `img1`, `timestep`）

output shape：`[N, 3, H, W]`（RGB）或 `[N, 4, H, W]`（RGBA）

FP32 / FP16 都行，但 FP16 model 在某些 converter tool 會產生
type mismatch（Cast / Mul inputs），建議 FP32。

### 從 vs-mlrt 取其他 RIFE variant
```
https://github.com/AmusementClub/vs-mlrt/releases/tag/external-models
- rife_v4.0 ~ v4.26 (heavy / lite / ensemble)
- 解壓後優先用 rife_v2/ 裡面那個（7-channel, self-contained，支援 dynamic H/W）
```
