# 遊戲內動態向量與深度輔助頭盔外插補幀（§VR-GAME-MV）

> 代號 **§VR-GAME-MV**，日期 **2026-11-26**。這份文件只寫設計，沒有建置、沒有改任何程式碼。
> 寫它是為了讓 VipleStream 在半速串流（`§VR-HALF-RATE`）搭配頭盔外插補幀（`§VR-SYNTH`）時，能從 PC 遊戲行程內直接取得引擎交給超解析度（DLSS／Streamline／FSR 3.1／XeSS）的每眼動態向量（Motion Vectors，以下簡稱 MV）與深度（Depth），在主機端降到 1/8 解析度、壓縮後隨視訊幀送到 Steam Frame 頭盔，取代或輔助頭盔目前用前後兩張解碼影像做 1/16、1/8 區塊比對猜出來的位移場；拿不到或不可信時無縫退回現有的影像區塊比對。
>
> 相關文件：[`../steam_frame_client.md`](../steam_frame_client.md)（§半速串流與補幀）、[`../vr_protocol.md`](../vr_protocol.md)、[`../vr_architecture.md`](../vr_architecture.md)、[`linux_vr_backend.md`](linux_vr_backend.md)。

**路徑縮寫**

- `SS/` = `Sunshine/src/`
- `SW/` = `Sunshine/src/platform/windows/`
- `MQ/` = `moonlight-qt/app/`
- `Q/` = `moonlight-qt/moonlight-common-c/moonlight-common-c/src/`
- `S3/` = `Sunshine/third-party/moonlight-common-c/src/`
- `UEVR_AFW/` = PureDark 的 UEVR AFW 修改版原始碼（本機參考副本，不在本 repo；**保留所有權利，只供研究與借鑑思路，絕不複製任何程式碼**）

文中所有檔案與行號均對應 2026-11-26 工作樹實際讀到的位置。

---

## 0. 現況與問題定義（設計要接上的事實）

### 0.1 現有 PCVR 影像管線與頭盔外插補幀（`§VR-SYNTH`）

目前 Windows PCVR 串流與頭盔補幀的端到端路徑如下：

```
遊戲（D3D12/D3D11） → SteamVR compositor → driver_viple（SubmitLayer / Present）
  → 合成 SBS（或注視點分塊 2tw×2th） → NT 共享貼圖 ×3 + ID3D11Fence
  → viplestream-server（display_vr_t CPU 等 fence → CopyResource → NVENC）
  → RTP/UDP（0x81 24 B 或 0x82 32 B 幀標頭）
  → Steam Frame（v4l2m2m 解碼 → XrVideo 影像 render thread → OpenXR projection）
```

1. **Driver 合成與跨行程交接**：
   - `SW/steamvr_driver/direct_mode.cpp:365-404` 的 `direct_mode_t::SubmitLayer` 收下 SteamVR 提交的左右眼貼圖與 `perEye[0].mHmdPose`；`:557-925` 的 `compose_and_publish_locked` 將兩眼畫進一張並排（SBS）或注視點分塊（`§VR-FOVEA-TILES`，`:804-818`）的共享貼圖（`VRIPC_TEX_COUNT = 3`，`SS/vr/vr_ipc_abi.h:80, 263-276`），在 GPU 上 `Signal(R.shared_fence, value)` 並 `Flush()`（`:819-833`），再透過 `pose_hist.match` 配對出 client 空間的 `render_rot`、`render_pos` 與 `echo_sample_id`，寫進 `vripc_frame_ring_t`（`:853-914`、`SS/vr/vr_ipc_abi.h:465-491`）。
   - `SW/display_vr.cpp:1049-1092` 在 CPU 端等待 `shared_fence` 到達 `d.fence_value`（**不變式 8**：絕不在 GPU 上 Wait 對方行程的 fence，見 `SW/steamvr_driver/direct_mode.h:7` 與 `SW/display_vr.cpp:9-10`），到值後於 `:1203` 執行 `CopyResource` 複製到編碼影像池，填入 `img->vr`（`:1215-1228`）交給編碼器，最後在 `SS/stream.cpp:6071-6184, 6288-6324` 封裝成 `0x81`（24 B）或 `0x82`（32 B）視訊幀標頭送出。

2. **頭盔端半速串流與補幀現況**：
   - 背景紀錄在 `docs/steam_frame_client.md:1903-1993`「半速串流與補幀（§VR-HALF-RATE、§VR-SYNTH）」。當顯示跑 120 Hz、串流跑 60 Hz（或由 `§VR-RATE-AUTO` 動態降為 `div=2`，`docs/vr_protocol.md:176-177`）並開啟補幀（`--vr-synth`，設定鍵 `vrsynth`）時，`XrVideo` 把 swapchain 高度加倍（`MQ/streaming/xr/xrvideo.cpp:1682`）。
   - 每張新解碼影像在 `XrVideo::renderNewFrame`（`MQ/streaming/xr/xrvideo.cpp:915-1557`）會 release 兩次：
     - **第一次發布**（`:1173-1405`）：只畫真的那一張到 swapchain 上半，立刻 `xrReleaseSwapchainImage` 並更新 `m_Published`，不讓補幀運算拖慢真幀的顯示（第五版修正，`docs/steam_frame_client.md:1937-1944`）。
     - **第二次發布**（`:1464-1537`）：再次 acquire 下一張 swapchain image，呼叫 `XrVideo::renderSynth`（`:2404-2616`）或分塊版 `XrVideo::renderTilesSynth`（`:3619-3873`），把真幀畫在上半、外插半個串流週期（`vs_k = 0.5f`）的合成畫面畫在下半（`synthRect.offset.y = synthTop`，`:1152, 1351`），以同一個 `seq` 更新 `m_Published.hasSynth = synthOk`。
   - XR frame thread 在 `XrVideo::current`（`:1801-1819`）：同一張影像第一次被挑選時顯示上半（真幀）；下一個顯示格再次挑到同一張且 `hasSynth == true` 時，將 `subImage.imageRect` 切換到下半 `synthRect`（分塊時連同兩眼 `insetRect[e].offset.y` 一起下移，`:1811-1816`），**而 projection layer 的算圖姿態 `meta` 保持不變**（`:2604-2608`：頭部轉動交給頭盔 OpenXR runtime 的重投影 ATW 處理，合成畫面只負責把場景中移動的物體往前推半格）。

3. **現有頭盔端區塊比對（`XrVideo::synthFlow`）的做法與極限**：
   - 縮圖與搜尋都在頭盔 GPU（Adreno）上跑：先把當前幀縮成 1/8（`m_Small[cur]`，8×8 區塊平均，`kDownBody` `:2253-2265` 或亮度平面版 `kTilesDownBody` `:3555-3565`），再雙線性縮成 1/16（`m_Mid[cur]`，`:2524-2543`、`:3731-3751`）。
   - `XrVideo::synthFlow`（`:2284-2400`）由前後兩幀的 `renderRot` 算出機體座標旋轉角速度向量 $\boldsymbol{\omega} = 2 \cdot \mathbf{d}_{xyz}$（`:2315-2338`），在 fragment shader `kFlowBody`（`:1954-2024`）逐像素算出頭部純轉動造成的影像位移 `g`（`:1960-1986`）並先扣掉；接著在 1/16 層以半徑 $\pm 6$ 格、1/8 層以半徑 $\pm 3$ 格（合計全尺寸約 $\pm 120$ 像素）比較 3×3 稀疏區塊的 L1 亮度差（`kFlowHeader` `:1919-1933`、`kFlowHeaderGray` `:1938-1952`）。
   - 只有當最佳比對成本 `best` 比零位移成本 `czero` 好超過 40%（`czero - best > 0.4 * czero + 0.08`，`:2010`）時才採用並做拋物線次像素內插，寫出 1/8 尺寸的 `m_Flow`（`RGBA16F`，`.xy` 為整張 SBS 的 UV 剩餘位移、`.z` 為比對成本 `best`）。
   - **現有做法的四個結構性限制**：
     1. **小型或超高速物體漏估**：位移是在 1/8 縮圖上以約 $4.5 \times 4.5$ 格（全尺寸 $36 \times 36$ px）的稀疏區塊估計，像桌球、帕魯球、遠處投射物這類在畫面上只有 1～2 格的小物體無法通過 40% 門檻，或是單幀移動超過 $\pm 15$ 格（全尺寸 120 px）的快速揮拍會超出搜尋半徑，導致該物體不外插、退回 60 Hz 重複顯示而產生疊影（`docs/steam_frame_client.md:1986-1992`）。
     2. **重複紋理與無紋理表面誤配**：窗框、格線、草地或大片純色表面在區塊比對下容易配到隔壁週期或無法通過門檻。
     3. **缺乏深度（Depth）決定前後遮擋**：`kCombineBody:2048-2069` 與 `kTilesSynthHead:3588-3607` 在物體邊緣只能取 2×2 鄰點中位移向量最長者（`mvMax`），再靠前後取樣點是否也有大位移猜測誰是前景，邊緣仍有約 8 像素格的碎塊與鋸齒（`docs/steam_frame_client.md:1935-1936, 1988`）。
     4. **頭盔 GPU 時間與功耗**：縮圖兩趟＋兩層搜尋共 4 個 pass 在頭盔 GPU 上每幀約耗時 2.5～3.3 ms（每眼 2160 時達 4.3 ms，見 `docs/steam_frame_client.md:1933-1934, 1942-1943`）。

### 0.2 遊戲行程內已有現成的精確動態向量與深度

現代 UE5／DX12 遊戲（例如 Palworld UE5.1）普遍開啟 DLSS、FSR 或 XeSS。遊戲引擎在每眼 G-Buffer 與 Velocity Pass 已經算好逐像素的精確螢幕空間動態向量與深度貼圖，並透過超解析度 SDK 的公開介面交給放大器。

PureDark 的 `UEVR-AFW` 已經證實這條路在 UEVR 下完全可行：
- 在 `UEVR_AFW/src/mods/VR.cpp:434-478` 攔截 `_nvngx.dll`／`nvngx.dll`（或相容層 `dxgi.dll`／`winmm.dll`）匯出的 `NVSDK_NGX_D3D12_CreateFeature`、`NVSDK_NGX_D3D12_ReleaseFeature` 與 `NVSDK_NGX_D3D12_EvaluateFeature`。
- 在 `VR.cpp:54-168` 的 `hk_NVSDK_NGX_D3D12_EvaluateFeature` 中，透過 `InParameters->Get(...)` 直接取得 `ID3D12Resource*` 型別的 `Color`、`Depth`、`MotionVectors`、`Output` 以及 `MV.Scale.X/Y`、`Jitter.Offset.X/Y`，並在 `UEVR_AFW/src/mods/vr/D3D12Component.cpp:481-575` 用於合成。
- Palworld（UE5.1）在 UEVR Native Stereo 模式下實測會呼叫 `hk_NVSDK_NGX_D3D12_CreateFeature FeatureID 1`（即 `NVSDK_NGX_Feature_SuperSampling`），每幀穩定送出雙眼的 MV 與 Depth。

本設計的目標：在遊戲行程內透過公開 SDK 介面擷取每眼的 MV（＋Depth），在主機 GPU 直接降採樣到約 1/8 解析度、跨行程交接給 `viplestream-server` 壓縮後隨影像送到頭盔，讓 `xrvideo.cpp` 直接用它合成中間格；拿不到或不可信時退回現有的 `synthFlow`。

---

## 1. 取得點、注入方式與授權邊界

### 1.1 超解析度 SDK 攔截點總表（只用公開 SDK 介面）

我們在遊戲行程內針對四種主流超解析度／中介層 DLL 設置 inline hook（或 export 轉發），全部只依賴各家公開 SDK 標頭定義的函式簽名與參數名稱：

| 介面 | 目標 DLL | 攔截函式 | 取得 MV／Depth／尺度／Jitter／Reset 的公開參數 |
|---|---|---|---|
| **NVIDIA DLSS (NGX)** | `_nvngx.dll`、`nvngx.dll`（若已載入相容層則含匯出 NGX 符號的模組） | `NVSDK_NGX_D3D12_CreateFeature`<br>`NVSDK_NGX_D3D12_ReleaseFeature`<br>`NVSDK_NGX_D3D12_EvaluateFeature`<br>（D3D11 對應 `NVSDK_NGX_D3D11_*`） | `CreateFeature` 過濾 `InFeatureID == NVSDK_NGX_Feature_SuperSampling (1)` 或 `RayReconstruction (13)`。<br>`EvaluateFeature` 呼叫 `InParameters->Get(...)`：<br>• `"MotionVectors"` → `ID3D12Resource*`<br>• `"Depth"` → `ID3D12Resource*`<br>• `"Color"` / `"Output"` → `ID3D12Resource*`<br>• `"MV.Scale.X"` / `"MV.Scale.Y"` → `float`<br>• `"Jitter.Offset.X"` / `"Jitter.Offset.Y"` → `float`<br>• `"Reset"` → `int`（鏡頭切換時為 1）<br>• `"DLSS.Render.Subrect.Dimensions.Width/Height"` → `unsigned int` |
| **NVIDIA Streamline** | `sl.interposer.dll` | `slSetTag`<br>`slSetTagForFrame`<br>`slSetConstants`<br>`slEvaluateFeature` | `slSetTag` 遍歷 `sl::ResourceTag*`（依 `viewport` 分組）：<br>• `sl::kBufferTypeMotionVectors` → `resource->native` (`ID3D12Resource*`) 及 `extent`<br>• `sl::kBufferTypeDepth`<br>• `sl::kBufferTypeScalingInputColor` / `OutputColor`<br>`slSetConstants` 讀取 `sl::Constants`：<br>• `mvecScale` (`float2`)、`jitterOffset` (`float2`)<br>• `clipToPrevClip`、`cameraViewToClip`、`clipToCameraView`<br>• `reset`、`depthInverted`、`motionVectorsJittered` |
| **AMD FSR 3.1 (FFX API)** | `amd_fidelityfx_dx12.dll`、`amd_fidelityfx_upscaler_dx12.dll` | `ffxCreateContext`<br>`ffxDestroyContext`<br>`ffxDispatch` | `ffxCreateContext` 當 `desc->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE` 時記下 `flags`（含 `FFX_UPSCALE_ENABLE_DEPTH_INVERTED`、`FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS`）。<br>`ffxDispatch` 當 `desc->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE`（`ffxDispatchDescUpscale*`）時讀取：<br>• `motionVectors.resource`、`depth.resource`<br>• `motionVectorScale.x / y`、`jitterOffset.x / y`<br>• `renderSize`、`upscaleSize`、`reset`、`cameraNear`、`cameraFar`、`cameraFovAngleVertical` |
| **Intel XeSS** | `libxess.dll` | `xessD3D12CreateContext`<br>`xessD3D12Init`<br>`xessGetVelocityScale` / `xessSetVelocityScale`<br>`xessD3D12Execute` | `xessD3D12Init` 記下 `initFlags`（`XESS_INIT_FLAG_HIGH_RES_MV`、`XESS_INIT_FLAG_INVERTED_DEPTH`、`XESS_INIT_FLAG_JITTERED_MV`）。<br>`xessSetVelocityScale` 攔截遊戲傳入的 `(scaleX, scaleY)`。<br>`xessD3D12Execute` 從 `xess_d3d12_execute_params_t*` 讀取：<br>• `pVelocityTexture`、`pDepthTexture`<br>• `jitterOffsetX / Y`、`inputWidth / inputHeight`、`resetHistory` |

> [!NOTE]
> **參考 `UEVR_AFW` 的具體對照**：`UEVR_AFW/src/mods/VR.cpp:29-51` 在 `CreateFeature` 把非 SuperSampling／非 RayReconstruction 的 handle 放進忽略表，並在 `:54-86` 於 `EvaluateFeature` 讀取 `Color`、`Depth`、`MotionVectors`、`Output`、`MV_Scale_X/Y` 與 `Jitter_Offset_X/Y`。我們借鑑此過濾思路，但加上兩項改進：(1) 額外讀取 `"Reset"` 與 `"DLSS.Render.Subrect.Dimensions.*"`（支援 UE5 動態解析度 `r.DynamicRes`）；(2) 不使用全域單一 `rawMotionVectorsTex` 指標，而是以 `(NVSDK_NGX_Handle*, 幀內呼叫序號)` 支援 Native Stereo 雙眼各自的資源。

### 1.2 注入方式：雙軌架構（UEVR 外掛優先 ＋ 獨立小 DLL）

為了同時支援 UEVR 遊戲（如 Palworld）與未來的原生 PCVR 遊戲，將遊戲內擷取模組拆成**「共用擷取核心（靜態庫 `viple_mv_core`）」**與**「兩種載入外殼」**：

1. **外殼 A：UEVR 官方外掛介面（`viple_uevr_mv_plugin.dll`，首期實作）**
   - **載入機制**：部署在遊戲或全域的 UEVR `plugins/` 目錄，由 UEVR 的 `PluginLoader`（見 `UEVR_AFW/src/mods/PluginLoader.cpp:1772-1875`）自動載入，匯出 `uevr_plugin_required_version` 與 `uevr_plugin_initialize`（`UEVR_AFW/include/uevr/Plugin.hpp:82-90`）。
   - **為什麼首選 UEVR 外掛介面**：
     - `param->renderer`（`UEVR_AFW/include/uevr/API.h:253-258`）直接提供 `renderer_type`（D3D11/D3D12）、`device`（`ID3D12Device*`）、`swapchain` 與 `command_queue`（`ID3D12CommandQueue*`），不必在遊戲行程內猜測哪一條 queue 才是主渲染佇列。
     - `param->functions->register_inline_hook`（`API.h:223`）由 UEVR 內部統一管理 inline hook，避免兩個模組對同一個函式重複打補丁。
     - `param->callbacks->on_present`（`API.h:206`）與 `param->sdk->callbacks->on_post_calculate_stereo_view_offset`（`API.h:246`）、`param->vr->get_pose` / `get_eye_offset` / `get_ue_projection_matrix`（`API.h:570-583`）能直接拿到每一眼的相機位置、旋轉與投影矩陣，解決雙眼識別與相機轉動扣除。
     - 若遊戲未開 DLSS/FSR/XeSS，外掛還可選擇性呼叫 `param->sdk->render_target_pool_hook->activate()` 與 `get_render_target(L"SceneVelocity")` / `L"SceneDepthZ"`（`API.h:421-424`，對照 `UEVR_AFW/src/mods/vr/D3D12Component.cpp:451-452`）直接從 UE RenderTarget Pool 讀取引擎原生 Velocity 與 Depth。

2. **外殼 B：獨立輕量注入 DLL（`viple_game_mv_hook.dll`，後續擴充給原生 VR 遊戲）**
   - **載入機制**：`SW/steamvr_driver/` 已經在 `vripc_driver_status_t.scene_pid`（`SS/vr/vr_ipc_abi.h:341`）即時回報 SteamVR 目前的場景程式 PID（來自 `VREvent_SceneApplicationChanged`）。由使用者空間的輔助啟動器（或 OpenXR implicit API layer）在確認該 PID 未載入 `UEVRBackend.dll` 且無核心態反作弊驅動（EAC／BattlEye）時載入。
   - **佇列與姿態取得**：攔截遊戲行程內的 `IVRCompositor::Submit`（OpenVR，`vr::D3D12TextureData_t` 內含 `m_pCommandQueue`，見 `UEVR_AFW/src/mods/vr/D3D12Component.cpp:618-622`）或 `xrCreateSession`（OpenXR，`XrGraphicsBindingD3D12KHR` 內含 `ID3D12Device*` 與 `ID3D12CommandQueue*`），並從 `Submit` 的 `VRTextureWithPose_t` 或 `xrEndFrame` 的 `XrCompositionLayerProjectionView` 取得雙眼提交姿態與 FOV。

### 1.3 授權與合規注意事項

1. **只用 MIT 授權的 UEVR 外掛標頭，不複製 UEVR／UEVR-AFW 實作**：
   - `UEVR` 主體與 PureDark 的 `UEVR-AFW`（如 `VR.cpp`、`D3D12Component.cpp`、`PDAFWPlugin.dll`）為**保留所有權利（All Rights Reserved）**，不可複製任何程式碼或預編譯二進位檔。
   - 但 `UEVR_AFW/include/uevr/API.h:1-23`、`API.hpp:1-23` 與 `Plugin.hpp:1-23` 在檔頭明確聲明：**「This file is licensed under the MIT license and is separate from the rest of the UEVR codebase. Copyright (c) 2023 praydog」**。因此我們在 repo 中只引入這三個 MIT 標頭（或依其 C ABI 自寫宣告），所有攔截、降採樣 Compute Shader 與 IPC 程式碼均從頭獨立撰寫。
2. **不得放入任何 Unreal Engine 原始碼**：
   - 嚴禁引入任何 Epic Games Unreal Engine 標頭或原始碼。所有對 UE 遊戲的操作僅止於公開超解析度 SDK 與 `API.h` 提供的不透明 handle。
3. **OptiScaler（GPLv3）僅供參考規範**：
   - OptiScaler 為 GPLv3 授權。我們的遊戲內注入 DLL 需載入至各式商業遊戲行程中，為維持授權單純，注入 DLL 採 clean-room 獨立實作，不複製 OptiScaler 任何程式碼。
4. **超解析度 SDK 標頭**：
   - NVIDIA NGX SDK、NVIDIA Streamline SDK、AMD FidelityFX SDK 標頭皆為 MIT 授權；Intel XeSS SDK 標頭公開可查。實作時以 `GetProcAddress` 動態解析符號，不在 repo 內夾帶第三方專有 `.dll` 或 `.lib`。

---

## 2. 座標系、雙眼配對與扣除頭部／相機轉動

### 2.1 MV 的單位、方向與 Jitter 統一

要讓遊戲的 MV 餵進頭盔端 `xrvideo.cpp` 的 `m_Flow`，必須先把兩端對「方向」與「單位」的定義完全對齊：

1. **頭盔端 `xrvideo.cpp` 的 `m_Flow` 定義（權威基準）**：
   - 見 `MQ/streaming/xr/xrvideo.cpp:1916, 2026-2027, 2334-2335`：
     - `m_Flow.xy` 的單位是**整張 SBS 影像（上排全視野）的正規化 UV**（左眼佔 $u \in [0, 0.5)$、右眼佔 $u \in [0.5, 1.0)$，$v \in [0, 1.0)$ 由上往下）。
     - `m_Flow.xy` 的方向定義為**「上一張的位置 − 這一張的位置」**：
       $$\mathbf{mv}_{\text{flow}}(\mathbf{uv}) = \mathbf{uv}_{\text{prev}} - \mathbf{uv}_{\text{cur}} - \mathbf{g}_{\text{head\_rot}}(\mathbf{uv})$$
     - 在 `kCombineBody`（`:2072`）與 `kTilesSynthHead`（`:3608-3609`）合成半個串流週期後的畫面時，若物體在當前幀位於 $\mathbf{uv}$，它在上一幀位於 $\mathbf{uv} + \mathbf{mv}_{\text{flow}}$，代表物體在這一個週期內的運動位移是 $-\mathbf{mv}_{\text{flow}}$；再過半個週期（`vs_k = 0.5f`），物體會前進到 $\mathbf{uv} - 0.5 \cdot \mathbf{mv}_{\text{flow}}$。因此在合成格的目標像素 $\mathbf{uv}$ 上，**反向取樣（backward warp）**真幀的位置正是：
       $$\mathbf{s} = \mathbf{uv} + 0.5 \cdot \mathbf{mv}_{\text{flow}}(\mathbf{uv})$$

2. **DLSS／Streamline／FSR 3.1／XeSS 的 MV 單位與方向**：
   - 在 NGX（DLSS）、Streamline、FSR 3.1 與 XeSS 的公開規範中，輸入的 2D 動態向量紋理值 $\mathbf{v}_{\text{raw}} = (v_x, v_y)$ 乘上尺度因子 $\mathbf{s}_{\text{mv}} = (\text{mvScaleX}, \text{mvScaleY})$ 後，其方向同樣定義為**「從當前幀像素 $(x_t, y_t)$ 指向前一幀對應像素 $(x_{t-1}, y_{t-1})$ 的位移」**（X 向右為正、Y 向下為正）：
     $$(\Delta x_{\text{px}},\ \Delta y_{\text{px}}) = (v_x \cdot \text{mvScaleX},\ v_y \cdot \text{mvScaleY}) = (x_{t-1} - x_t,\ y_{t-1} - y_t)$$
   - **重要發現**：各家超解析度 SDK 經 `mvScale` 換算後的向量方向（$\mathbf{uv}_{\text{prev}} - \mathbf{uv}_{\text{cur}}$）**與 `xrvideo.cpp` 的 `m_Flow` 定義完全同向**！
   - **分母（基準解析度）注意**：
     - 不要照搬 `UEVR_AFW/src/mods/VR.cpp:84-85` 的 `mvScale[0] * outputDesc.Width / mvDesc.Width`（那是因為 `PDAFWPlugin.dll` 內部預期以放大後的輸出像素為單位）。
     - 我們需要的是**單眼正規化 UV 位移** $\Delta \mathbf{uv}_{\text{eye}} = (\Delta u_{\text{eye}}, \Delta v_{\text{eye}}) \in [-1, 1]^2$。依各 SDK 規範：
       - **DLSS (NGX)**：`v_raw * mvScale` 的單位是 MV 貼圖所屬的座標系像素（低解析度 MV 時為算圖子矩形 `(renderW, renderH)`；若設了 `NVSDK_NGX_DLSS_Feature_Create_Flags_Is_Low_Res_MV == 0` 則為 `(outW, outH)`，通常直接等於 `(mvDesc.Width, mvDesc.Height)` 或 `Render.Subrect.Dimensions`）：
         $$\Delta u_{\text{eye}} = \frac{v_x \cdot \text{mvScaleX}}{\text{baseW}}, \quad \Delta v_{\text{eye}} = \frac{v_y \cdot \text{mvScaleY}}{\text{baseH}}$$
       - **FSR 3.1**：未設 `FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS` 時，`baseW, baseH` 為 `renderSize.width, renderSize.height`；已設時為 `upscaleSize.width, upscaleSize.height`（若遊戲把 `motionVectorScale` 設為 `1.0` 且 MV 已是 UV，則依 `motionVectorScale` 的數量級自動或照規範相乘後除以 `baseSize`）。
       - **Streamline**：若 `sl::Constants::mvecScale` 已設定，Streamline 規範 `raw * mvecScale` 直接就是正規化 UV `[-1, 1]`（當 `mvecScale` 為 `1.0 / renderSize` 時）；若 `mvecScale == (1, 1)` 而紋理為像素單位，則除以 `renderSize`。在 M-MV0 探測時會把 `raw` 的 p95 幅度與 `mvScale` 一起記錄以確認。
   - **從單眼 UV 轉成 `xrvideo.cpp` 的 SBS UV**：
     - 因為 SBS 影像中每一隻眼佔總寬度的一半（`0.5`）、總高度的全部（`1.0`）：
       $$\Delta \mathbf{uv}_{\text{sbs}} = \left(0.5 \cdot \Delta u_{\text{eye}},\ \Delta v_{\text{eye}}\right)$$

3. **TAA／DLSS Jitter 的扣除**：
   - 遊戲每幀在投影矩陣加上次像素偏移 $\mathbf{j}_t = (j_{x,t}, j_{y,t}) \in [-0.5, 0.5]^2$（單位：算圖像素）。
   - UE5 的 `BasePass`／`VelocityPass` 在寫出 Velocity 貼圖時已經把前後兩幀的 jitter 差扣掉（即預設是 unjittered MV）。
   - 只有當攔截到的建立旗標包含 `NVSDK_NGX_DLSS_Feature_Create_Flags_MV_Jittered (0x04)`、Streamline `motionVectorsJittered == sl::eTrue`、或 XeSS `XESS_INIT_FLAG_JITTERED_MV` 時，才在降採樣 Compute Shader 中額外減去抖動差：
     $$\Delta \mathbf{uv}_{\text{eye, unjittered}} = \Delta \mathbf{uv}_{\text{eye}} - \frac{\mathbf{j}_t - \mathbf{j}_{t-1}}{(\text{renderW},\ \text{renderH})}$$

### 2.2 UEVR 雙眼呼叫對應與跨眼 MV 污染防護

1. **每次超解析度呼叫對應哪隻眼**：
   - UEVR 有兩種主要的立體算繪模式（見 `docs/specs/linux_vr_backend.md:55-58` 與 `UEVR_AFW/src/mods/vr/D3D12Component.cpp:82-86`）：
     - **Native Stereo（原生立體，Palworld UE5.1 採用）**：同一個遊戲幀、同一次 `Present` 內，引擎依序畫左眼與右眼，因此**每幀會呼叫兩次 `NVSDK_NGX_D3D12_EvaluateFeature`**，然後才在 `D3D12Component::on_frame`（`:754-816`）一次提交左右兩眼給 OpenVR／OpenXR。
     - **Synchronized AFR / Alternating AFR**：每次 `Present` 只畫一隻眼，依 `m_render_frame_count % 2 == m_left_eye_interval` 奇偶交替（`UEVR_AFW/src/mods/VR.cpp:2483, 2708`）。
   - **為什麼不能照抄 `UEVR_AFW/src/mods/VR.cpp:97` 的分眼寫法**：
     - `VR.cpp:97` 寫的是 `EyeIndex nEye = (render_frame_count % 2 == 0) ? EyeLeft : EyeRight;`。這是因為 PureDark 的 AFW 強制跑在 AFR（一幀只畫一眼、另一眼用 FrameWarp 合成）下！在 **Native Stereo** 下，同一幀內的兩次 `EvaluateFeature` 看到的 `render_frame_count` 是同一個值，若用 `% 2` 會把兩次呼叫都判給同一隻眼！
   - **本設計的分眼判定規則（同時相容 Native Stereo 與 AFR）**：
     1. **首選（UEVR 外掛回呼）**：掛載 `on_post_calculate_stereo_view_offset`（`UEVR_AFW/include/uevr/API.h:246`）。UE 引擎在開始準備某一眼的 View 之前一定會呼叫 `CalculateStereoViewOffset(..., view_index, ...)`（見 `UEVR_AFW/src/mods/vr/FFakeStereoRenderingHook.cpp:5150-5157, 5253, 5280`：`true_index = index_starts_from_one ? ((view_index + 1) % 2) : (view_index % 2)`，`0` 為左眼、`1` 為右眼）。
     2. **次選（NGX Handle + 幀內呼叫計數）**：UE5 的 DLSS 外掛在 Native Stereo 下會為左、右兩個 `FSceneViewState` 各建一個 `NVSDK_NGX_Handle*`（第一次 `CreateFeature` 為左眼、第二次為右眼）；同時在每次 `on_present` 將 `frame_eval_count` 歸零：若同一 `on_present` 週期內呼叫了 2 次 `EvaluateFeature`，第 0 次為左眼（`EyeLeft = 0`）、第 1 次為右眼（`EyeRight = 1`）；若只呼叫 1 次（AFR 模式），則由 `view_index` 或幀號奇偶決定。

2. **UE5 跨眼 `PrevViewProj` 污染的偵測與修正**：
   - 在部分 UE 遊戲或特定 UEVR 設定下，左眼與右眼若共用同一個 `FSceneViewState` 的上一幀相機矩陣，引擎在算右眼（第 2 次呼叫）的 Velocity 時，拿到的 `PrevViewProjection` 可能是「剛畫完的左眼」而不是「上一幀的右眼」（參考 `UEVR_AFW/src/mods/VR.cpp:119-139, 2486-2525` 的 `cameraDataForMV` 背景）。
   - 一旦發生跨眼污染，右眼整張畫面的 MV 會平白多出一段「右眼 $\to$ 左眼 IPD 視差（水平約數十像素）」，直接拿去外插會讓右眼整張畫面往旁邊飛！
   - **自動偵測與重投影修正**：
     - 我們記錄左、右眼在幀 $t$ 與幀 $t-1$ 的 `ViewProjection` 矩陣：$\mathbf{VP}_{L,t}, \mathbf{VP}_{R,t}, \mathbf{VP}_{L,t-1}, \mathbf{VP}_{R,t-1}$。
     - 對當前眼 $e \in \{L, R\}$，利用深度貼圖 $D$ 挑出幾個遠處（接近遠平面、深度最小）的像素：遠處像素不受視差影響，也不太可能是近處貼臉的動態物體。比較該像素的 $\mathbf{MV}_{\text{total}}$ 與兩個候選重投影矩陣算出的相機位移：
       - 候選 1（正常同眼歷史）：$\mathbf{M}_{\text{same}} = \mathbf{VP}_{e, t-1} \cdot \mathbf{VP}_{e, t}^{-1}$
       - 候選 2（跨眼污染歷史，Native Stereo 右眼誤用同幀左眼，或左眼誤用上一幀右眼）：$\mathbf{M}_{\text{cross}} = \mathbf{VP}_{\bar{e}, t_{\text{prev}}} \cdot \mathbf{VP}_{e, t}^{-1}$
     - 若候選 2 的殘差明顯小於候選 1，即確認該遊戲有跨眼 `PrevViewProj` 污染！此時在降採樣 Compute Shader 中，對每個像素 $(u, v, z)$ 先扣掉跨眼相機位移 $\mathbf{MV}_{\text{cam}}(\mathbf{M}_{\text{cross}})$，就立刻把跨眼 IPD 視差與相機旋轉一併乾淨消除！

### 2.3 如何扣掉相機／頭部轉動（對齊 `kFlowHeader` 與頭盔 ATW）

這一步必須與頭盔 OpenXR runtime 的重投影（ATW）及 `xrvideo.cpp` 的職責分工嚴格對齊：

1. **為什麼只扣掉「頭盔算圖姿態的純旋轉（或相機運動）」，而且必須扣掉？**
   - 見 `MQ/streaming/xr/xrvideo.cpp:1913-1915, 1960-1986, 2604-2608`：
     - 頭盔在每個顯示格（包含半速串流顯示合成畫面的那一格）都會由 OpenXR compositor 依最新的頭盔旋轉對 projection layer 做 3DoF 旋轉重投影（ATW）。
     - `XrVideo::renderSynth` 產出的合成格**沿用真幀的 `renderRot`（`*synthMeta = meta`，`:2607`）**。
     - 因此，兩張串流幀之間由**頭盔轉動（`m_PrevMeta.renderRot` $\to$ `meta.renderRot`）**造成的全畫面旋轉位移，在合成格顯示時已經會被頭盔的 ATW 自動轉過去。如果 MV 裡還留著頭部轉動造成的位移，合成格會把背景與物體先推一次、頭盔 ATW 再轉一次，造成靜止景物雙重移動與拉扯。

2. **兩種扣除模式（依場景需求與深度可用性切換）**：

   - **模式 1：球面視線純旋轉扣除（與 `kFlowBody:1960-1986` 完全同構，在頭盔端或 Server 端執行，預設模式）**
     - 只扣掉兩張串流幀之間**頭盔 `renderRot` 的純旋轉差** $\Delta \mathbf{q} = \mathbf{q}_{\text{prev}}^{-1} \cdot \mathbf{q}_{\text{cur}}$（小角度角速度 $\boldsymbol{\omega} = 2 \cdot \Delta \mathbf{q}_{xyz}$，機體座標，見 `xrvideo.cpp:2315-2338`）。
     - 對單眼視線方向 $(t_x, t_y, -1)$（其中 $t_x \in [\text{tanLeft}, \text{tanRight}]$、$t_y \in [\text{tanDown}, \text{tanUp}]$），純旋轉造成的 tangent 變化為（`xrvideo.cpp:1982-1983`）：
       $$\Delta t_x = \omega_y (1 + t_x^2) + \omega_z t_y - \omega_x t_x t_y$$
       $$\Delta t_y = -\omega_x (1 + t_y^2) - \omega_z t_x + \omega_y t_x t_y$$
     - 換算成整張 SBS 影像（未變形空間）的 UV 位移 $\mathbf{g}_{\text{rot}}$（`:1986`）：
       $$\mathbf{g}_{\text{rot}}(\mathbf{uv}) = \left(-0.5 \cdot \frac{\Delta t_x}{\text{tanW}},\ \frac{\Delta t_y}{\text{tanH}}\right)$$
     - 扣掉後的剩餘位移：
       $$\mathbf{mv}_{\text{rem}}(\mathbf{uv}) = \Delta \mathbf{uv}_{\text{sbs}}(\mathbf{uv}) - \mathbf{g}_{\text{rot}}(\mathbf{uv})$$
     - **模式 1 的巨大優勢**：
       - OpenXR 的 ATW **只補償頭盔實時旋轉**，**不會補償**「玩家推搖桿走路（相機平移視差）」與「玩家推右搖桿平滑轉向（Smooth Turn / 載具轉向，此時頭盔 `renderRot` 沒變，但遊戲內相機轉了）」！
       - 當我們只扣掉頭盔 `renderRot` 的 $\mathbf{g}_{\text{rot}}$ 時，遊戲引擎 MV 裡的**「3D 動態物體運動 ＋ 角色走路的近景平移視差 ＋ 搖桿平滑轉向」**會全部保留在 $\mathbf{mv}_{\text{rem}}$ 裡，在中間合成格被平順地往前外插半格！而且這與現有 `kFlowBody` 的物理語意 100% 相容。

   - **模式 2：3D 深度＋相機矩陣重投影扣除（用於消除跨眼 `PrevViewProj` 污染與離線分離純物體運動）**
     - 當遊戲發生第 2.2 節的「跨眼 `PrevViewProj` 污染」，或者我們在遊戲端 Compute Shader 先把 MV 轉回「純動態物體運動 + 純頭盔外相機位移」時：
     - 利用當前眼像素的深度 $z = D(\mathbf{uv}_{\text{eye}})$ 建立當前幀 NDC 點 $\mathbf{p}_{\text{ndc}, t} = (2u_{\text{eye}}-1,\ 1-2v_{\text{eye}},\ z,\ 1)^T$。
     - 以引擎產生該 MV 時對應的靜態重投影矩陣 $\mathbf{M}_{\text{reproj}} = \mathbf{VP}_{\text{prev\_used}} \cdot \mathbf{VP}_{\text{cur}}^{-1}$ 算出靜態世界點在上一幀的 UV：
       $$\mathbf{p}_{\text{clip}, t-1} = \mathbf{M}_{\text{reproj}} \cdot \mathbf{p}_{\text{ndc}, t}, \quad \mathbf{uv}_{\text{static\_prev}} = \left(0.5 \frac{x_{\text{clip}}}{w_{\text{clip}}} + 0.5,\ 0.5 - 0.5 \frac{y_{\text{clip}}}{w_{\text{clip}}}\right)$$
     - 則純 3D 動態物體在螢幕上的位移為：
       $$\Delta \mathbf{uv}_{\text{obj}} = \Delta \mathbf{uv}_{\text{eye}} - (\mathbf{uv}_{\text{static\_prev}} - \mathbf{uv}_{\text{eye}})$$
     - 若需保留相對於頭盔旋轉的相機平移（走路視差）與搖桿轉向，只需再把「僅扣掉頭盔旋轉差 $\Delta \mathbf{q}_{\text{hmd}}$ 的同眼重投影位移」加回來即可。如此一來，無論引擎底層是 Native Stereo 還是 AFR、有沒有跨眼污染，送到 Server 的 MV 都保證是乾淨的「同眼上一幀 $\to$ 同眼這一幀」！

---

## 3. 跨行程交接到 Server（共享貼圖＋Fence 與幀號配對）

### 3.1 行程邊界與不變式

在 Windows 主機上涉及三個行程：
1. **遊戲行程**（如 `Palworld-Win64-Shipping.exe`，使用者 Medium Integrity，D3D12 device）
2. **SteamVR Driver 行程**（`vrserver.exe` 內的 `driver_viple.dll`，使用者 Medium Integrity，D3D11 device）
3. **Server 行程**（`viplestream-server.exe`，SYSTEM 或使用者，D3D11 capture device）

跨行程設計必須遵守現有架構的兩條鐵律（`SS/vr/vr_ipc_abi.h:6-25`、`SW/steamvr_driver/direct_mode.h:7`）：
- **不變式 8**：**絕不在 GPU 上 Wait 對方行程的 fence**（避免遊戲或對方行程卡死時，把 Server 的擷取與 NVENC GPU 佇列一起拖死）。一律由接收方在 CPU 端觀察 `GetCompletedValue()`，並設嚴格的等待上限。
- **不信任寫入方**：共享記憶體（SHM）讀取端一律「透過 seqlock 整份複製到本地 $\to$ 驗證欄位範圍 $\to$ 只用本地副本」（`SW/steamvr_driver/ipc_proto.h:7-8, 284-306`）。

### 3.2 遊戲內 1/8 降採樣與跨行程交接機制

1. **為什麼在遊戲 GPU 內先降到 1/8 解析度**：
   - 全解析度雙眼 MV（例如每眼 $2160 \times 2160$ `RG16F`）每幀高達 $37\text{ MB}$；而頭盔端 `m_Flow` 本來就是 **1/8 解析度**（`sw = fw / 8, sh = fh / 8`，`MQ/streaming/xr/xrvideo.cpp:2426, 3641`）。
   - 以每眼 $1440 \times 1440$（SBS $2880 \times 1440$）為例，1/8 解析度每眼僅 **$180 \times 180$**，雙眼並排（SBS 1/8）僅 **$360 \times 180$**（即使每眼 $2160 \times 2160$，雙眼 1/8 也只有 **$540 \times 270$**，上限訂為 `VRIPC_MV_MAX_W = 640, VRIPC_MV_MAX_H = 360`）。
   - 在遊戲行程的 `hk_NVSDK_NGX_D3D12_EvaluateFeature`（或 `ffxDispatch`／`xessD3D12Execute`）內，趁 `MotionVectors` 與 `Depth` 還在 `D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE` 時，直接在遊戲傳入的 `InCmdList` 上派發一個極輕量的 **8×8 $\to$ 1 降採樣 Compute Shader（`cs_downsample_mv_8x8`）**：
     - 一個 Thread Group（8×8 threads）讀取一個 8×8 區塊的 MV 與 Depth 到 `groupshared` 記憶體；
     - **前景優先池化（Closest-Depth Pooling）**：在 8×8 像素中找出離相機最近的前景像素（Reverse-Z 下 $z$ 最大值 $z_{\max}$），只對深度接近 $z_{\max}$ 的前景像素取平均 MV，避免物體邊緣的 MV 被背景的 `0` 稀釋成一半；
     - 左眼寫入目標 1/8 貼圖的左半 `[0, sw/2) × [0, sh)`，右眼寫入右半 `[sw/2, sw) × [0, sh)`；
     - 輸出格式為 `DXGI_FORMAT_R16G16B16A16_FLOAT`（每格 8 B：`R, G` = 單眼或 SBS UV 位移 `float16`；`B` = 正規化逆深度 $z \in [0, 1]$ `float16`；`A` = 8×8 區塊內的 MV 變異度／有效旗標）。一張 $360 \times 180$ 的貼圖僅 **$518.4\text{ KB}$**（$540 \times 270$ 為 **$1.16\text{ MB}$**），在桌面 RTX GPU 上執行時間 **$< 0.05\text{ ms}$**。

2. **跨行程交接（參考 `vr_bridge_win.cpp` 的共享貼圖＋Fence 模型，並附 D3D12 Readback 零跨裝置退路）**：

   ```
   遊戲行程（D3D12，viple_uevr_mv_plugin.dll）
     ├─ 左眼 EvaluateFeature：InCmdList 派發 cs_downsample_mv_8x8 → 寫入 slot 左半
     ├─ 右眼 EvaluateFeature：InCmdList 派發 cs_downsample_mv_8x8 → 寫入 slot 右半
     └─ on_present（雙眼齊備）：
          ├─ cmdQueue->Signal(mv_shared_fence, ++mv_fence_val)
          └─ 寫入 SHM vripc_mv_ring_t（per-slot seqlock：mv_frame_id, fence_value, tex_idx, game_submit_qpc, hmd_rot[4], reset_flags）
                │
                ▼ NT 共享貼圖 ×4 + 共享 Fence + SHM Seqlock
   Server 行程（D3D11，display_vr_t::capture）
     ├─ 等到畫面 shared_fence、CopyResource 畫面後，查 vripc_mv_ring_t 配對該幀
     ├─ CPU 檢查 mv_shared_fence->GetCompletedValue() >= mv_desc.fence_value（上限 0.5 ms，絕不 GPU Wait）
     ├─ CopyResource 到 staging → Map 讀回 CPU（約 0.5～1.1 MB，< 0.15 ms）
     └─ 扣掉頭部轉動 → 死區歸零 → Planar SoA + LZ4 壓縮 → 隨該編碼幀 frameIndex 送出
   ```

   - **握手與資源建立（擴充 `SW/vr_bridge_win.cpp`）**：
     - 比照 `SW/vr_bridge_win.cpp:1635-1645, 2127-2170`：Server 建立名為 `\\.\pipe\VipleStreamVRMV-<session_id>` 的專用 Named Pipe（DACL 同樣只給 `SYSTEM` 與當前主控台使用者的 Logon SID `kUserPipeAccess = 0x0012019Bu`，見 `:65, 1748-1766`），或在既有 `vripc_shm_t` 旁新增一塊 16 KB 的 `vripc_mv_shm_t` section。
     - 驗證連線方 PID：必須等於 `vripc_driver_status_t.scene_pid`（`SS/vr/vr_ipc_abi.h:341`）或與剛透過 `SubmitLayer` 提交第 0 層貼圖的 `swapset_t::pid`（`SW/steamvr_driver/direct_mode.cpp:52, 1015`）相符。
     - Server 在 `bdev_`（與 `display_vr_t` 同一個 `adapter_luid`）上建立 `VRIPC_MV_TEX_COUNT = 4` 張 `640×360`、`DXGI_FORMAT_R16G16B16A16_FLOAT`、帶 `D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE` 的共享貼圖，以及 `mv_shared_fence` 與 `mv_consumed_fence`（`D3D11_FENCE_FLAG_SHARED`），以 `DuplicateHandle` 交給遊戲行程。
     - 遊戲行程以 `ID3D12Device::OpenSharedHandle` 直接把這 4 張 NT 共享貼圖與 2 顆 Fence 開成 `ID3D12Resource*`（`ALLOW_UNORDERED_ACCESS` / `RENDER_TARGET`）與 `ID3D12Fence*`（Windows 10+ DXGI 原生支援 D3D11 與 D3D12 跨 API 開啟 NT handle 共享貼圖與 Fence）。
   - **備援交接通道（SHM 內嵌緩衝，當跨 API 開貼圖受限或遊戲使用 Vulkan 時自動啟用）**：
     - 因為 1/8 解析度的 MV＋Depth 非常小（量化成 `int16_t mv_x, int16_t mv_y, uint8_t depth, uint8_t flags` 每格 6 B，$360 \times 180$ 僅 $388.8\text{ KB}$；$540 \times 270$ 僅 $874.8\text{ KB}$），若 `OpenSharedHandle` 因格式或跨 API 旗標失敗，遊戲內 DLL 可直接在自己的 D3D12 裝置上把 1/8 結果 `CopyTextureRegion` 到 `D3D12_HEAP_TYPE_READBACK` 環狀緩衝（落後 1 幀或由 `ID3D12Fence::SetEventOnCompletion` 背景執行緒在 GPU 完成當下，約 `on_present` 後 $0.3\text{ ms}$），直接 `memcpy` 進 `vripc_mv_shm_t` 的 4 個 `1 MB` payload 槽！此時 Server 端連 D3D11 `CopyResource` 與 `Map` 都不需要，直接從 SHM 讀取並壓縮。

3. **SHM 描述符結構（`vripc_mv_desc_t`，64 B 對齊，per-slot seqlock）**：

```c
#define VRIPC_MV_SLOTS     8u
#define VRIPC_MV_TEX_COUNT 4u
#define VRIPC_MV_MAX_W     640u
#define VRIPC_MV_MAX_H     360u

/* vripc_mv_desc_t.flags */
#define VRIPC_MV_F_LEFT_VALID    0x01u
#define VRIPC_MV_F_RIGHT_VALID   0x02u
#define VRIPC_MV_F_BOTH_EYES     0x03u
#define VRIPC_MV_F_HAS_DEPTH     0x04u
#define VRIPC_MV_F_RESET         0x08u /* 鏡頭切換／重置歷史，本幀禁止外插 */
#define VRIPC_MV_F_POSE_VALID    0x10u /* hmd_rot / hmd_pos 有效 */
#define VRIPC_MV_F_SHM_PAYLOAD   0x20u /* 資料在 shm slot 內而非共享貼圖 */
#define VRIPC_MV_F_CROSS_FIXED   0x40u /* 已修正跨眼 PrevViewProj 污染 */

typedef struct vripc_mv_desc_t { /* 寫入方：遊戲 MV 外掛；讀取方：server display_vr_t */
  uint64_t seq;             /* seqlock：2*idx+1 寫入中、2*idx+2 完成 */
  uint64_t mv_frame_id;     /* 遊戲端單調遞增幀號 */
  uint64_t generation;      /* 必須等於當前 MV generation */
  uint64_t fence_value;     /* mv_shared_fence 要到達的值；SHM_PAYLOAD 時為 0 */
  uint32_t tex_idx;         /* < VRIPC_MV_TEX_COUNT */
  uint32_t flags;           /* VRIPC_MV_F_* */
  uint16_t grid_w;          /* 雙眼並排 1/8 總寬（例：360 或 540） */
  uint16_t grid_h;          /* 1/8 高度（例：180 或 270） */
  uint16_t render_eye_w;    /* 遊戲單眼算圖寬度 */
  uint16_t render_eye_h;    /* 遊戲單眼算圖高度 */
  int64_t  game_submit_qpc; /* 遊戲 on_present / Submit 時刻的 raw QPC */
  float    hmd_rot[4];      /* 遊戲該幀使用的 HMD 旋轉四元數 (x,y,z,w) */
  float    hmd_pos[3];      /* 遊戲該幀使用的 HMD 位置 (x,y,z) */
  uint32_t game_frame_step; /* 距離上一次發布的遊戲渲染幀數（通常為 1） */
  uint8_t  upscaler_kind;   /* 1=DLSS, 2=Streamline, 3=FSR3, 4=XeSS, 5=UE_Velocity */
  uint8_t  reserved[7];     /* 補齊至 128 B */
} vripc_mv_desc_t;
```

### 3.3 幀號配對到哪一張編碼影像（Frame Pairing）

遊戲行程、SteamVR compositor（`vrcompositor.exe`）、我们的 driver（`driver_viple.dll`）與 `viplestream-server` 是非同步交接的，必須定義嚴格的配對演算法，確保**每一張編碼送出的視訊幀 `frameIndex` 只配對到產生該畫面的那一筆 `vripc_mv_desc_t`**：

1. **現有時間線與標識符追蹤**：
   - 在遊戲行程：每一幀先對左、右眼呼叫 `EvaluateFeature`，接著在 `VR::on_present`（`UEVR_AFW/src/mods/VR.cpp:2529-2736`）進入 `D3D12Component::on_frame`（`UEVR_AFW/src/mods/vr/D3D12Component.cpp:610-663, 754-816`），以 `submit_pose = openvr->get_pose_for_submit()` 呼叫 `vr::VRCompositor()->Submit(Eye_Left/Right, ..., submit_pose)`。
   - 在 `driver_viple.dll`：`vrcompositor.exe` 呼叫 `direct_mode_t::SubmitLayer`（`SW/steamvr_driver/direct_mode.cpp:365-404`），把 `perEye[0].mHmdPose` 抄進 `layers_[0].pose`（`:389`），隨後呼叫 `Present`（`:927`）記下 `present_qpc = now_qpc()`（`:931`），並在 `compose_and_publish_locked`（`:853-914`）透過 `ctx_.pose_hist.match(app, d.t_target_qpc, present_qpc)`（`:882`）配對出 `d.echo_sample_id` 與 `d.render_rot`，發布 `vripc_frame_desc_t d`（序號為 `d.frame_id`）。
   - 在 `viplestream-server`：`SW/display_vr.cpp:1006` 讀出最新一筆 `vripc_frame_desc_t d`；若開啟 `§VR-RATE-AUTO` 半速跳幀（`:1165-1185`），被丟棄的幀在此提前跳過；真正要編碼的幀在 `:1203` `CopyResource`、`:1215-1228` 填入 `img->vr`，最後在 `SS/stream.cpp:6307, 6315, 6322` 以 `packet->frame_index()`（即 RTP `NV_VIDEO_PACKET.frameIndex`）送出。

2. **Server 端的雙鍵配對演算法（在 `SW/display_vr.cpp` 內執行）**：
   當 `display_vr_t::capture` 確定要編碼一筆 `vripc_frame_desc_t d` 時，從 `vripc_mv_ring_t` 倒序掃描最近 `VRIPC_MV_SLOTS`（8 筆）有效 slot，必須**同時滿足**以下條件才算配對命中：
   - **條件 1（雙眼齊備與未重複消費）**：`(mv.flags & VRIPC_MV_F_BOTH_EYES) == VRIPC_MV_F_BOTH_EYES`，且 `mv.mv_frame_id > last_matched_mv_frame_id`。
   - **條件 2（QPC 時間窗約束）**：遊戲在 `on_present` 提交 MV 的時刻 `mv.game_submit_qpc` 必須早於（或微幅晚於，若走 Readback 背景執行緒）driver 的 `d.present_qpc`，且時差落在一個合理視窗內：
     $$-1.0\text{ ms} \le (d.\text{present\_qpc} - mv.\text{game\_submit\_qpc}) \le 1.5 \times T_{\text{vsync}}$$
     （其中 $T_{\text{vsync}}$ 為 host 虛擬 vsync 週期，例如 60 Hz 為 $16.67\text{ ms}$、120 Hz 為 $8.33\text{ ms}$）。
   - **條件 3（HMD 姿態簽章核對，若有 `VRIPC_MV_F_POSE_VALID`）**：當遊戲外掛有填入 `mv.hmd_rot` 時，其與 driver 該幀收到的 `layers_[0].pose`（或轉換後的 `d.render_rot`）四元數內積 $|\mathbf{q}_{\text{mv}} \cdot \mathbf{q}_{\text{drv}}| > \cos(0.1^\circ / 2)$。若有多筆落在時間窗內，優先選姿態吻合且 `game_submit_qpc` 最接近 `d.present_qpc` 的那一筆。

3. **掉幀、重送與半速分頻（`§VR-RATE-AUTO`）的配對邊界處理**：
   - **遊戲掉幀（SteamVR compositor 重送上一張貼圖）**：此時 driver 仍會收到 `Present` 並產生新的 `d.frame_id`，但遊戲沒有新的 `on_present`，因此 `vripc_mv_ring_t` 裡最新一筆 `mv_frame_id` 已經等於 `last_matched_mv_frame_id`（或 `d.present_qpc - mv.game_submit_qpc > 1.5 * T_vsync`）。配對直接判定為 **Miss（無新 MV）**，Server 不送該幀的 MV（绝不把上一幀用過的 MV 再送一次，否則頭盔會把已經停住的物體繼續往外推）。
   - **`§VR-RATE-AUTO` 半速分頻（`div = 2`，`SW/display_vr.cpp:1165-1185`）**：
     - 當客戶端在串流中途請求 `div = 2` 時，SteamVR 與遊戲仍以全速（例如 120 Hz）算圖，但 `display_vr_t` 每 2 張只留 1 張送進編碼器（即編碼的是遊戲幀 $F, F+2, F+4 \dots$）。
     - 此時遊戲在幀 $F+2$ 交給 DLSS 的 MV 只涵蓋了**「遊戲幀 $F+1 \to F+2$（半個串流間隔，1/120 s）」**的位移，而頭盔收到的兩張編碼幀間隔卻是 **2 個遊戲幀（$F \to F+2$，1/60 s）**！
     - **解決方式**：`display_vr_t` 記下「上一張實際送進編碼器的影像所配對的 `mv_frame_id`（設為 $M_{\text{prev}}$）」與「本次配對到的 $M_{\text{cur}}$」。兩張編碼幀之間跨越的遊戲渲染幀數為：
       $$K_{\text{step}} = \text{clamp}(M_{\text{cur}} - M_{\text{prev}},\ 1,\ 4)$$
       - 當開啟 `§VR-HALF-RATE`（啟動時就協商 60 Hz 串流、120 Hz 顯示）時，遊戲本身跑 60 Hz，$K_{\text{step}} = 1$，單筆 MV 就是一整個串流週期的位移；
       - 當開啟 `§VR-RATE-AUTO`（遊戲跑 120 Hz、Server 跳幀送 60 Hz）時，$K_{\text{step}} = 2$。若環狀緩衝內同時留著 $M_{\text{cur}}-1$ 與 $M_{\text{cur}}$，Server 可將兩幀 MV 相加（或直接把 `K_step` 寫進封包標頭的 `frameStep`，讓頭盔端將位移量乘上 $K_{\text{step}}$，使外插半個串流週期 `0.5 * K_step * mv` 剛好等於推 1 個 120 Hz 遊戲格）！
   - **最终綁定至 RTP `frameIndex`**：
     - 配對成功後，壓縮好的 MV 二進位緩衝區直接掛在 `img->vr.mv_payload`（隨 `img_d3d_t` 進入 `video.cpp` 編碼佇列），在 `SS/stream.cpp:6288-6325` 拿到該幀確切的 `(uint32_t) packet->frame_index()` 與 `echoSampleId` 後，填入 MV 封包標頭並由同一條 `videoBroadcastThread` 送出。

---

## 4. 傳輸協定擴充、壓縮方式與頻寬估算

### 4.1 協定擴充與向下相容（讀 `docs/vr_protocol.md` 與 `VipleVr.h`）

設計嚴格遵守 `docs/vr_protocol.md:4, 23-31` 與 `S3/VipleVr.h:13-15` 的相容原則：**不改 RTSP／SDP、不用 H.264/HEVC SEI、所有擴充在雙方明確協商後才啟用，舊 client 與舊 server 的行為一個位元組都不變**。

1. **為什麼不塞進 `0x81`／`0x82` 視訊幀標頭，也不走 ENet `0x5508` TLV？**
   - `S3/VipleVr.h:133` 明定 ENet 控制通道（`0x5507`／`0x5508` TLV）單則訊息上限為 **`VIPLE_VR_MAX_CTRL_PAYLOAD = 251` B**（common-c 加密路徑使用 256 B stack buffer）。1/8 解析度的雙眼 MV 壓縮後約 **2～12 KB**，遠超過 251 B。
   - `0x81`（24 B）與 `0x82`（32 B）是每個 RTP 視訊封包開頭的固定長度 header（`Q/VideoDepacketizer.c:1248-1301`、`SS/stream.cpp:6071-6184`）。若把數 KB 的變長 MV 塞進視訊 bitstream，一旦掉一個 MV 封包就會導致整張視訊幀無法組包解碼，並誤觸 `VIPLE_VR_C2S_LOSS` 與 intra-refresh wave。

2. **能力協商擴充（三份 `VipleVr.h` 保持 byte-identical）**：

| 位置 | 新增定義 | 說明與相容行為 |
|---|---|---|
| `/serverinfo` `<VipleStreamVR>` | `VIPLE_VR_SERVER_CAP_GAME_MV = 0x2000`（b13；註：b12 `0x1000` 由 `§LINUX-VR` 保留，實作時以 `VipleVr.h:44-56` 下一個空位為準） | Windows server 且 `vr_game_mv != disabled` 時宣告。舊 client 忽略未知能力位元。 |
| `/launch` `vrCaps` | `VIPLE_VR_CLIENT_CAP_GAME_MV = 0x200`（b9；註：b8 `0x100` 由 `§LINUX-VR` 保留，實作時接在 `VipleVr.h:67` `0x80` 之後） | client 支援接收並解壓外部遊戲 MV 時帶上。 |
| `/launch` 查詢參數 | `&vrGameMv=1`（選填，十進位整數） | client 開啟補幀（`vrsynth`）或準備允許同場切換補幀時送出。舊 server 忽略未知的 `/launch` key（`docs/vr_protocol.md:74`）。 |
| `<VipleStreamVRSession>` | `;gameMv=1` | 雙方皆有能力位元且 server 啟用時回傳。client 只有在回應看到 `gameMv=1` 時才啟用 MV 接收路徑（舊 client 忽略未知 key，`docs/vr_protocol.md:313-315`）。 |
| `0x5507 C→S` 或 `0x81/0x82` | 不改 `0x81`（24 B）與 `0x82`（32 B）任何位元組 | 維持現有視訊標頭不變，視訊解包器零風險。 |

3. **資料封包格式：沿用視訊 UDP Socket 的 `'V','L'` 加密分片資料報（`VIPLE_VR_LINK_T_MV = 5`）**：
   - 現有 `S3/VipleVr.h:580-585, 632-642` 已在視訊 UDP socket（單連線與 `§VR-MULTILINK` 皆適用）定義了 12 B 的 `VIPLE_VR_LINK_HDR`（`magic = {'V', 'L'}`，`type = 1..4`）。
   - 我們新增 **`VIPLE_VR_LINK_T_MV = 5`**（S→C，走視訊 UDP socket；若啟用 `§VR-MULTILINK` 則與視訊 RTP 一樣鏡像送往每條活躍連線，接收端以 `(videoFrameIndex, chunkIdx)` 自動去重）：
   - **封包結構（每個 UDP 資料報 $\le 1200\text{ B}$，完全避開 IP 分片）**：

```c
#define VIPLE_VR_LINK_T_MV         5     /* S→C（影像 socket）：AES-GCM 加密的每幀遊戲 MV 分片 */
#define VIPLE_VR_MV_MAX_DGRAM_LEN  1200  /* 單一 MV 資料報上限（含 12 B link hdr + 16 B GCM tag） */
#define VIPLE_VR_MV_MAX_CHUNKS     32    /* 單幀最多 32 個分片（上限約 36 KB 壓縮資料） */

/* 明文分片標頭（16 B，僅第 0 個分片 chunkIdx == 0 後面緊接著壓縮流標頭，或每片皆帶完整描述） */
typedef struct _VIPLE_VR_MV_CHUNK_HDR {
    uint32_t videoFrameIndex; /* 對應 NV_VIDEO_PACKET.frameIndex（client 的 DECODE_UNIT.frameNumber） */
    uint32_t echoSampleId;    /* 對應 0x81/0x82 的 echoSampleId（雙重核對用） */
    uint8_t  chunkIdx;        /* 0 .. chunkTotal-1 */
    uint8_t  chunkTotal;      /* 本幀總分片數（1 .. VIPLE_VR_MV_MAX_CHUNKS） */
    uint8_t  flags;           /* b0: hasDepth, b1: reset, b2: headRotSubtracted, b3..b4: codec(0=LZ4) */
    uint8_t  frameStep;       /* 跨越的遊戲幀數 K_step（通常為 1；§VR-RATE-AUTO div=2 時為 2） */
    uint16_t gridW;           /* 雙眼並排 1/8 網格總寬（例：360） */
    uint16_t gridH;           /* 1/8 網格高度（例：180） */
    uint16_t uncompressedLen; /* 解壓後總位元組數（等於 gridW * gridH * bytesPerCell） */
    uint16_t chunkPayloadLen; /* 本分片夾帶的壓縮資料長度 */
} VIPLE_VR_MV_CHUNK_HDR;
```

   - **線上資料報版面**：
     `VIPLE_VR_LINK_HDR (12 B, magic={'V','L'}, type=5, seq=mvPktSeq++, arg=videoFrameIndex)` + `GCM_TAG (16 B)` + `AES_GCM_CIPHERTEXT(VIPLE_VR_MV_CHUNK_HDR (20 B) + compressedChunk[chunkPayloadLen])`。
     - AES-GCM 的 IV 構造與 `VIPLE_VR_LINK_T_S2C`（`S3/VipleVr.h:635`）完全一致：`seq (LE32) + 6 個 0 + 'H' + 'M'`（以 `'M'` 與 `T_S2C` 的 `'L'` 區隔 nonce domain，防止 IV 重用）。
   - **如何與同一個 Socket 上的視訊 RTP 封包（`NV_VIDEO_PACKET`）100% 無碰撞區分**：
     - 現有 `S3/VipleVr.h:613` 對 `PONG`／`T_S2C` 用 `len < 64` 過濾，是因為那些控制包很短。對於長度可達 $1200\text{ B}$ 的 `T_MV`，在 `gameMv=1` 協商成立時，接收端先做常數時間的 3-byte 檢查：
       `len >= 48 && len <= VIPLE_VR_MV_MAX_DGRAM_LEN && buf[0] == 'V' && buf[1] == 'L' && buf[2] == VIPLE_VR_LINK_T_MV`。
     - 接著直接用 session 的 AES-GCM 金鑰驗證 16-byte Tag：
       - 若 Tag 驗證通過 $\to$ 100% 是合法的 `T_MV` 分片，送進 `VrExternalMvAssembler`，不進 `VideoDepacketizer`。
       - 若 Tag 驗證失敗（例如萬一某個視訊 RTP 封包的 `streamPacketIndex` 低 24 bit 剛好等於 `0x054C56`，每 $2^{24} \approx 1677$ 萬個封包才可能撞一次前 3 bytes）$\to$ 128-bit GCM Tag 偽造機率為 $2^{-128}$，驗證必定失敗，接收端立刻把該封包原封不動交給原本的 RTP 視訊路徑！連那一個百萬分之一的視訊包都不會掉。

### 4.2 壓縮方式：死區夾限 ＋ Planar SoA 水平差分 ＋ LZ4

為了讓 Server 壓縮與頭盔 ARM64 解壓縮的 CPU 時間都控制在 **$< 0.05\text{ ms}$（50 微秒）** 內，同時達到高壓縮比，採用三步流水線：

1. **步驟 1：量化與微抖動死區歸零（Deadzone Clamping）**
   - 每個 1/8 格點 $(x, y)$ 的單眼（或 SBS）UV 位移換算為 `int16_t` 定點數：
     - 令全張 SBS 寬度為 $W_{\text{sbs}}$、高度為 $H_{\text{sbs}}$，或直接以 **归一化 SBS UV $\times 32768$** 儲存：`q_vx = clamp(round(mv_sbs_u * 32768.0f), -32767, 32767)`、`q_vy = clamp(round(mv_sbs_v * 32768.0f), -32767, 32767)`（精度為 $1/32768$ UV，在 $2880 \times 1440$ 下相當於 $0.088 \times 0.044$ 像素，遠高於 1/4 像素需求）。
   - 若在 Server 端已先用該幀與上一張編碼幀的 `renderRot` 扣掉頭部純旋轉 $\mathbf{g}_{\text{rot}}$（或遊戲鏡頭未轉動時），將幅度小於 $0.2$ 個算圖像素的微幅殘差直接歸零（`|q_vx| <= kDeadzone && |q_vy| <= kDeadzone` $\to$ `0`）。這一步能把畫面上 **75%～95% 的靜止背景全部變成精確的 `0`**！
   - 深度（M-MV3 開啟 `hasDepth` 時）：將逆深度 $z \in [0, 1]$ 量化為 `uint8_t q_depth = round(sqrt(z) * 255.0f)`（平方根壓擴讓近處物體擁有更高深度解析度），每格佔 **5 B**（不帶深度時每格 **4 B**）。

2. **步驟 2：Planar SoA（Structure of Arrays）＋ 列內水平差分（Delta Coding）**
   - 不要用 AoS（`vx, vy, vx, vy...`）交錯排列，因為 `int16_t` 的高位元組多半是 `0x00` 或 `0xFF`。
   - 改將整張 $W_g \times H_g$ 網格拆成獨立平面（Planar SoA）並做水平一階差分 $\delta[x] = q[x] - q[x-1]$，再經 ZigZag 編碼（`zz(v) = (v << 1) ^ (v >> 15)`，讓小正負數都變成 `0x00xx`）：
     - Plane 0：`zz(delta_vx)` 的低位元組陣列（$W_g \times H_g$ B）
     - Plane 1：`zz(delta_vx)` 的高位元組陣列（$W_g \times H_g$ B，在靜止與慢速區幾乎全為 `0x00`！）
     - Plane 2：`zz(delta_vy)` 的低位元組陣列（$W_g \times H_g$ B）
     - Plane 3：`zz(delta_vy)` 的高位元組陣列（$W_g \times H_g$ B，幾乎全為 `0x00`！）
     - Plane 4（選填，`hasDepth` 為真時）：`delta_depth`（$W_g \times H_g$ B）

3. **步驟 3：`LZ4_compress_fast` 無損壓縮**
   - 對上述 Planar 緩衝區跑一次 `LZ4_compress_fast(..., acceleration = 1)`。
   - 因為大面積靜止背景與平滑移動物體在 Planar + ZigZag Delta 後產生長串連續的 `0x00`，LZ4 能在 20～40 微秒內將數百 KB 壓到數 KB。

### 4.3 頻寬估算

以下針對 VipleStream 在 Steam Frame 上的三種標準每眼解析度（`docs/steam_frame_client.md:2226`：1152、1440、2160），計算 **半速串流 60 Hz（補幀實際啟用的頻率）** 與 **全速 90 Hz** 下的資料量與頻寬：

| 每眼解析度（SBS 打包尺寸） | 1/8 雙眼網格 (`gridW × gridH`) | 原始未壓縮大小／幀（僅 MV 4B ／ 含深度 5B） | 壓縮後每幀大小（一般場景 15% 動態 ／ 劇烈場景 60% 動態） | **60 Hz 半速串流頻寬**（一般 ／ 劇烈） | 90 Hz 串流頻寬（一般 ／ 劇烈） | 每幀 UDP 分片數（$\le 1200$ B/包，一般 ／ 劇烈） |
|---|---|---|---|---|---|---|
| **1152×1152** (`2304×1152`) | `288 × 144` (41,472 格) | 165.9 KB ／ 207.4 KB | **2.5～5.0 KB** ／ 12～18 KB | **1.2～2.4 Mbps** ／ 5.8～8.6 Mbps | 1.8～3.6 Mbps ／ 8.6～13.0 Mbps | **3～5 包** ／ 11～16 包 |
| **1440×1440** (`2880×1440`) | `360 × 180` (64,800 格) | 259.2 KB ／ 324.0 KB | **3.8～7.5 KB** ／ 18～28 KB | **1.8～3.6 Mbps** ／ 8.6～13.4 Mbps | 2.7～5.4 Mbps ／ 13.0～20.2 Mbps | **4～7 包** ／ 16～25 包 |
| **2160×2160** (`4320×2160`) | `540 × 270` (145,800 格) | 583.2 KB ／ 729.0 KB | **7.5～14.0 KB** ／ 32～45 KB | **3.6～6.7 Mbps** ／ 15.4～21.6 Mbps | 5.4～10.1 Mbps ／ 23.0～32.4 Mbps | **7～13 包** ／ 28～39 包 |

> [!TIP]
> **每眼 2160 時的固定網格上限優化**：頭盔端 `m_Flow` 的作用是提供區域外插向量（物體邊緣精細度在無深度時受限於網格，有深度時可搭配深度或保持 `360×180`）。若將傳輸網格上限夾在 **`360 × 180`**（即每眼 2160 時改用 **1/12** 縮圖，每格對應全尺寸 12×12 px），則即使是 2160×2160 解析度，60 Hz 半速串流下的外部 MV 頻寬也永遠控制在 **1.8～3.6 Mbps（每幀 4～7 個 UDP 封包）**，不到半速串流 40～80 Mbps 視訊頻寬的 5%！

---

## 5. 頭盔端：接點、位移場格式轉換、可信度與退回條件

### 5.1 `xrvideo.cpp` 改用外部 MV 的接點

頭盔端（`moonlight-qt`）的資料流與現有 `VrRenderMetaRing`（`MQ/streaming/vr/vrframemeta.h:26-46`、`vrframemeta.cpp:193-227`）完全對稱：

```
視訊 UDP Socket 收到 'V','L', T_MV (5)
  → AES-GCM 驗證與解密 → VrExternalMvAssembler（湊齊同一 videoFrameIndex 的 chunkTotal 片）
  → LZ4 解壓 + Planar ZigZag 還原 → VrExternalMvRing::publish(videoFrameIndex, mvFrame)
        │
        ▼
XrVideo::renderNewFrame（MQ/streaming/xr/xrvideo.cpp:915）
  ├─ :925  VrRenderMetaRing::lookup(frame->pts, &meta)
  ├─ :937  frameNo = frame->best_effort_timestamp（即 videoFrameIndex）
  ├─ [新增] VrExternalMvRing::lookup(frameNo, &m_CurExtMv)
  └─ 第二次發布（:1479-1481）：呼叫 renderSynth 或 renderTilesSynth
        │
        ▼
XrVideo::renderSynth（:2404） / XrVideo::renderTilesSynth（:3619）
  ├─ 若 m_CurExtMv 有效且通過第 5.3 節可信度檢查：
  │    ├─ 跳過 m_Mid[cur] 1/16 縮圖與 synthFlow 的兩趟區塊搜尋（省下最重的 GPU 工作）
  │    ├─ pl_tex_upload 把解壓後的 1/8 外部 MV 上傳至 m_ExtMvTex
  │    ├─ 跑一趟極輕量的 kExtFlowBody shader → 寫入 m_Flow（格式與 synthFlow 輸出 100% 相同）
  │    └─ 進入既有的 kCombineBody（:2551）或 kTilesSynthBody（:3781）合成下半部！
  └─ 若 m_CurExtMv 無效／缺包／不可信：
       └─ 原封不動走既有的 m_Small → m_Mid → synthFlow 區塊比對路徑！
```

- **為什麼保留 `m_Small[cur]` 的 1/8 縮圖（或延遲建立）**：
  - 現有 `synthFlow` 需要「這一張的 `m_Small[cur]` 與上一張的 `m_Small[prev]`」（`MQ/streaming/xr/xrvideo.cpp:2313, 2384-2385`）。
  - 若某場次協商了 `gameMv=1`，且連續命中外部 MV，我們可以**連 `m_Small[cur]` 與 `m_Mid[cur]` 都不畫**（整場省下 4 個 pass）；萬一中途某一幀 `N` 的 `T_MV` 封包在 Wi-Fi 上掉包，因為沒有幀 `N-1` 的 `m_Small[prev]`，幀 `N` 這一格就單純不外插（`synthOk = false`，重送真幀並由頭盔 ATW 補轉頭，同時在幀 `N` 補畫一張 `m_Small[cur]`）；若幀 `N+1` 外部 MV 就恢復了，繼續用外部 MV；若遊戲關掉了 DLSS 導致幀 `N+1` 也沒外部 MV，此時已經有幀 `N` 的 `m_Small[prev]`，從幀 `N+1` 起就無縫接回 `synthFlow`！

### 5.2 與現有位移場格式（`m_Flow`）的轉換

現有 `m_Flow` 是一張尺寸為 `sw × sh`（`sw = max(16, fw / 8), sh = max(16, fh / 8)`，`:2426`）、格式為 `RGBA16F`（`pl_find_fmt(g, PL_FMT_FLOAT, 4, 16, 16, caps)`，`:2425`）的貼圖。

在新增的 `kExtFlowBody` shader 中，我們將外部 MV 貼圖 `vs_ext_mv`（尺寸 `gridW × gridH`）轉換寫入 `m_Flow`（尺寸 `sw × sh`），處理三種投影／注視點模式：

1. **一般模式（未開注視點，`fv.x >= 0.999`）與注視點分塊模式（`§VR-FOVEA-TILES`，`renderTilesSynth` `:3753-3761`）**：
   - 在這兩種模式下，`m_Flow` 的座標 `uv = gl_FragCoord.xy / vec2(sw, sh)` 直接對應均勻取樣的左眼（`uv.x < 0.5`）與右眼（`uv.x >= 0.5`）。
   - 從 `vs_ext_mv` 雙線性取樣得到 $\Delta \mathbf{uv}_{\text{sbs}} = \text{texture}(\text{vs\_ext\_mv}, \text{uv}).xy \times \text{vs\_frame\_step}$。
   - 若封包標頭 `headRotSubtracted == 0`（Server 未預扣頭部旋轉，交由頭盔逐像素精確扣除）：直接套用 `kFlowBody:1964-1986` 同一段公式算出該像素的頭部純旋轉位移 $\mathbf{g}_{\text{uv}} = \text{vec2}(-0.5 \cdot dtx / \text{vs\_tan}.x,\ dty / \text{vs\_tan}.y)$，則：
     $$\mathbf{r}_{\text{uv}} = \Delta \mathbf{uv}_{\text{sbs}} - \mathbf{g}_{\text{uv}}$$
   - 寫出 `color = vec4(r_uv, ext_depth, 2.0);`（其中 `.z` 為正規化逆深度 $z \in [0, 1]$，`.w = 2.0` 標記為外部遊戲 MV，區別於 `kFlowBody:2023` 的 `.w = 1.0`）。

2. **非線性注視點編碼模式（`§VR-FOVEA` 三次曲線 ／ `§VR-FOVEA-INSET` 分段曲線，未分塊，`renderSynth` `:2412-2422`）**：
   - 這是非常關鍵的幾何轉換：在未分塊的注視點模式下，`m_Full` 與 `m_Flow` 的網格座標 `uv` 是**非線性壓縮空間**，而 `kCombineBody:2032, 2072` 是在壓縮空間做 `s = uv + vs_k * mv` 再去取樣 `vs_full`！但遊戲交給 DLSS 的 MV 是在**線性（均勻）算圖空間**！
   - 因此當 `fv.x < 0.999` 時，`kExtFlowBody` 必須：
     1. 先用 `kFlowBody:1964-1980` 的正向注視點映射，把 `m_Flow` 像素的壓縮座標 `uv` 還原成均勻空間的取樣座標 $\mathbf{uv}_{\text{linear}}$；
     2. 以 $\mathbf{uv}_{\text{linear}}$ 去取樣 `vs_ext_mv`，拿到均勻空間的位移 $\Delta \mathbf{uv}_{\text{sbs}}(\mathbf{uv}_{\text{linear}})$；
     3. 算出均勻空間的頭部旋轉位移 $\mathbf{g}_{\text{linear}} = \text{vec2}(-0.5 \cdot dtx / \text{vs\_tan}.x,\ dty / \text{vs\_tan}.y)$；
     4. 將扣掉頭部旋轉後的均勻位移**除以該點在注視點曲線上的斜率 `slope`**（完全對齊 `kFlowBody:1984-1986` 的 `/ slope`）：
        $$\mathbf{r}_{\text{compressed}} = \frac{\Delta \mathbf{uv}_{\text{sbs}}(\mathbf{uv}_{\text{linear}}) - \mathbf{g}_{\text{linear}}}{\text{slope}}$$
     如此算出來的 `m_Flow.xy` 就與 `kCombineBody` 的壓縮空間取樣 100% 吻合！

3. **利用 `.z` 逆深度強化 `kCombineBody`／`kTilesSynthHead` 的物體邊緣選取（M-MV3）**：
   - 現有 `kCombineBody:2048-2069` 在 2×2 鄰點遇到物體邊緣時，是選 `dot(m, m)` 最大的 `mvMax`。
   - 當 `m_Flow.z > 0.0`（帶有外部逆深度，越大代表越靠近相機）且四個鄰點的深度差 $\max(z) - \min(z) > \tau_z$（代表跨越了前景與背景的深度斷層）時，改以**逆深度最大的前景點 MV** 作為主導位移，若往回取樣點 `uv + vs_k * mvFg` 的逆深度也屬於前景，就畫出前景物體；若這一點現在是前景而半格後離開（遮擋暴露區 disocclusion），則採用逆深度較小的背景點 MV 取樣背景，大幅消除揮動球拍／武器邊緣的 8 像素鋸齒與背景拉扯殘影！

### 5.3 可信度判定與退回條件（Fallback Policy）

頭盔端對每一幀外部 MV 執行六道閘門，**任何一道未通過即自動退回現有 `synthFlow`（或該單幀不外插、重送真幀）**，保證不會比現況更差：

| 編號 | 退回／抑制條件 | 觸發原因 | 頭盔端行為 |
|---|---|---|---|
| **F1** | 未協商 `gameMv=1` 或 `VrExternalMvRing::lookup(frameNo)` 查無資料 | 舊 Server、遊戲未開超解析度、或該幀的 `T_MV` 分片在網路上掉包 | 走既有 `synthFlow` 區塊比對（若上一幀剛用過外部 MV 尚未建 `m_Small[prev]`，該單幀 `synthOk = false` 重送真幀，下一幀起恢復 `synthFlow`）。 |
| **F2** | `flags & RESET` 為真，或遊戲剛切換解析度／Feature 重建 | 遊戲切換鏡頭（Camera Cut）、傳送、開關選單導致 DLSS/FSR `Reset == 1` | **該幀禁止外插**（`synthOk = false`，連 `synthFlow` 也不跑），直接重送真幀，避免切鏡頭時把兩個無關場景混成怪異殘影。 |
| **F3** | 前後兩張編碼幀不連續（`!consecutive`，`ptsDelta > 1.6 * m_NominalDelta`）或無 `hasMeta` | 網路或解碼掉過視訊幀（`MQ/streaming/xr/xrvideo.cpp:2311-2313`） | **該幀禁止外插**（`synthOk = false`），與現有 `synthFlow` 行為完全一致。 |
| **F4** | 兩幀之間頭盔轉動超過 $10^\circ$（`d[3] <= 0.9962f`，`:2327`） | 快速甩頭時小角度近似誤差變大，且人眼在快速掃視時對中間格位移不敏感 | **該幀禁止外插**（`synthOk = false`），純靠頭盔 OpenXR ATW 補旋轉，與現有 `synthFlow` 行為一致。 |
| **F5** | 局部格點位移異常（$\|\mathbf{r}_{\text{uv}}\| > 0.25$ UV 或含 `NaN`/`Inf`） | 粒子特效、離群錯誤向量或跨眼污染未完全消除 | 在 `kExtFlowBody` 內對該單一格點夾成 `vec2(0.0)`（局部退回不移動），不影響同畫面其他正常物體。 |
| **F6** | **2D HUD／選單文字保護**（畫面亮度零變化但 3D MV 很大） | DLSS/FSR 在遊戲畫 HUD **之前**執行，因此外部 MV 不知道某像素上面疊了靜止的準星、血條或背包文字；若直接用底下 3D 場景的 MV 外插，會把靜止 HUD 拉歪 | 在 `kCombineBody`／`kExtFlowBody` 中比對上一張與這一張在同一螢幕座標的亮度差 $\Delta Y = \|Y_{\text{cur}}(\mathbf{uv}) - Y_{\text{prev}}(\mathbf{uv})\|$：若 $\Delta Y < 0.015$（像素完全沒變，極可能是固定在螢幕上的 HUD）但 $\|\mathbf{mv}_{\text{total}}\| > 4\text{ px}$，則將該像素的外插位移衰減為 `0`！ |

---

## 6. 分期計畫、驗證方法、風險與替代方案

### 6.1 分期與交付範圍

每一期都在開關（Server `vr_game_mv`、Client `--vr-game-mv`／預設跟隨 `vrsynth`）保護下推進，開關關閉時行為與今天 100% 相同：

- **M-MV0：遊戲端攔截與離線傾印驗證（只寫獨立 UEVR 外掛，不改 Sunshine 與 moonlight-qt）**
  - 以 MIT 授權的 `UEVR_AFW/include/uevr/API.h` 與 `Plugin.hpp` 建立 `viple_uevr_mv_plugin.dll`。
  - 在 Palworld（UE5.1，UEVR Native Stereo + DLSS）攔截 `NVSDK_NGX_D3D12_CreateFeature`／`EvaluateFeature` 與 `on_present`、`on_post_calculate_stereo_view_offset`。
  - 實作 D3D12 `cs_downsample_mv_8x8` Compute Shader，在按下熱鍵或指定幀號時將連續 10 幀的雙眼 1/8 MV、Depth、Color 與相機矩陣傾印至磁碟，用離線 Python 腳本驗證座標系與雙眼配對。
- **M-MV1：跨行程交接到 Server、幀號配對與壓縮**
  - 在 `SW/vr_bridge_win.cpp` 與 `SW/display_vr.cpp` 加入 MV 共享貼圖／SHM seqlock ring 與 CPU 端 Fence 檢查（遵守不變式 8，等待上限 `0.5 ms`）。
  - 實作第 3.3 節的 `(Pose, QPC)` 雙鍵幀號配對與第 4.2 節的 Planar SoA + LZ4 壓縮；支援 `vr_dev_mvdump = <startSec>:<frames>`（比照 `SW/display_vr.cpp:321-420` 的 `vr_dev_rawdump`）同時傾印配對好的 SBS 原圖與 1/8 MV。
- **M-MV2：協定傳輸與頭盔端 `xrvideo.cpp` 接線（DLSS 端到端閉環）**
  - 三份 `VipleVr.h` 同步加入 `VIPLE_VR_SERVER_CAP_GAME_MV`、`VIPLE_VR_CLIENT_CAP_GAME_MV` 與 `VIPLE_VR_LINK_T_MV = 5`；`check_commonc_sync.ps1` 檢查通過。
  - 頭盔端實作 `VrExternalMvAssembler`、`VrExternalMvRing` 與 `XrVideo::renderSynth`／`renderTilesSynth` 的 `kExtFlowBody` 接點及 F1～F5 退回機制。
  - 擴充 `--xr-dump-frame` 的 `dumpSynthInputs`（`MQ/streaming/xr/xrvideo.cpp:861-913`），當該幀採用外部 MV 時，存下的 `-flow.bin` 第 4 通道 `.w == 2.0`。
- **M-MV3：1/8 逆深度遮擋選取與 F6 靜態 HUD 保護**
  - 啟用 `m_Flow.z` 逆深度通道，在 `kCombineBody` 與 `kTilesSynthHead` 改以深度決定物體邊緣的前景／背景取樣，並加入 F6 靜態 HUD 零亮度差保護。
- **M-MV4：Streamline、FSR 3.1、XeSS 擴充與非 UEVR 獨立注入**
  - 加入 `sl.interposer.dll`（`slSetTag`／`slSetConstants`）、`amd_fidelityfx_dx12.dll`（`ffxDispatch`）、`libxess.dll`（`xessD3D12Execute`）攔截點，以及無超解析度時從 UEVR `render_target_pool_hook` 讀取 `SceneVelocity`／`SceneDepthZ` 的退路。

### 6.2 每期的驗證方法（無人可驗 vs 需配戴驗證）

遵循專案實證規範（見 `docs/steam_frame_client.md:1965-1988, 2113`），所有能離線或用無人場次量測的指標必須先過關，才安排配戴測試：

#### A. 無人可驗（Unattended / 自動化與離線驗證）

| 期數 | 驗證工具與方法 | 通過標準（Pass Criteria） |
|---|---|---|
| **M-MV0** | 在 `<host>` 啟動 Palworld + UEVR Native Stereo（不必接真頭盔，可用 driver 搭配 client `--vr-emulate`），傾印連續幀的 `color_prev.png`、`color_cur.png` 與 `mv8.bin`。以 Python 腳本依 `mv8` 將 `color_cur` 反向 warp 重建 `color_prev` | 1. 每次 `Present` 穩定攔截到左、右眼各 1 次 `EvaluateFeature`；<br>2. 動態區域（移動的帕魯或角色）经 `mv8` 反向 warp 後的 PSNR 比未 warp 提升 $\ge 6\text{ dB}$；<br>3. X/Y 方向無顛倒、左眼與右眼無交叉污染。 |
| **M-MV1** | 1. `vr_probe --mode unit` 加入 MV seqlock 撕裂、Planar+LZ4 round-trip 與配對單元測試；<br>2. 在 `<host>` 開 `vr_dev_mvdump = 10:60` 跑 60 幀傾印；<br>3. 故意在遊戲外掛內注入 Fence 延遲 $5\text{ ms}$，檢查 `[VIPLE-VR-CAP]` 的 `evtToPush` | 1. 單元測試 100% 通過；壓縮＋解壓耗時 $< 0.05\text{ ms}$；<br>2. 穩態配對命中率 $\ge 99\%$；<br>3. 注入 MV Fence 延遲時，`display_vr_t` 在 $\le 0.5\text{ ms}$ 內放棄該筆 MV，視訊幀 `evtToPush` p95 增加 $< 0.6\text{ ms}$，零掉幀。 |
| **M-MV2** | 頭盔在桌面上（無人配戴），執行半速 60 Hz＋補幀＋`--vr-synthetic-hmd --vr-synthetic-motion sine`＋`--xr-dump-frame`（涵蓋「不開注視點」、「注視點 150%」與「注視點分塊 `foveaTiles`」三種組合），跑 `flow_check.py`（`docs/steam_frame_client.md:1974-1988`）並檢查 log | 1. 存下的 6 組 `-flow.bin` 中 `.w == 2.0`（確認走到外部 MV）；<br>2. `flow_check.py` 的方向餘弦相似度 $\cos \ge 0.98$、位移大小比 $0.95 \sim 1.03$；<br>3. 頭部照 sine 轉動時，靜止背景被判成在動的像素 $< 0.2\%$（優於現有光流的 $0.62\%$）；<br>4. 頭盔 `m_SynthGpuUs` 中位數從 $3.3\text{ ms}$ 降至 $< 1.2\text{ ms}$；<br>5. 注入 10% `T_MV` 掉包或關掉遊戲 DLSS 時，無縫退回 `synthFlow`，`xrMissed = 0`。 |
| **M-MV3** | 同上無人場次，在角色握持武器快速橫移經過遠處背景、以及開著遊戲 2D 選單／血條時，用 `--xr-dump-frame` 比對合成格（下半）與真幀（上半） | 1. 靜止 HUD／文字區域在上下兩半的 PSNR $\ge 45\text{ dB}$（不被底下 3D MV 拉動）；<br>2. 前景物體邊緣的背景拉扯殘影寬度比 M-MV2 縮小。 |
| **M-MV4** | 分別在開 FSR 3.1、XeSS、Streamline 及關閉超解析度（改走 UE `SceneVelocity`）的測試場景跑 M-MV0／M-MV2 傾印檢查 | 各路徑輸出的 `-flow.bin` 與 `flow_check.py` 方向／大小比皆符合 M-MV2 標準。 |

#### B. 需使用者配戴驗證（Wear-Test，每次只改一項、同場 A/B 切換）

> [!IMPORTANT]
> 根據 `docs/steam_frame_client.md:2072, 2113` 的經驗，跨場次的體感比較會受場與場之間的時序相位干擾。因此 M-MV2 會提供**同場 A/B 切換機制**（Server `vr_game_mv_ab = 15` 每 15 秒在「外部遊戲 MV」與「頭盔光流 `synthFlow`」之間切換，或由頭盔手把快捷鍵切換），讓使用者在同一個位置、同一個場景內直接盲測或對照。

1. **小型與高速移動物體（M-MV2 配戴重點 1）**：
   - 情境：在 Palworld 中投擲帕魯球、發射弓箭／高速技能，或快速揮動近戰武器。
   - 觀察重點：現有 `synthFlow` 因 1/8 縮圖區塊比對抓不到 $1 \sim 2$ 格大的小物體或超過 $\pm 120$ px 的高速揮動，眼睛盯著球看會有 60 Hz 雙重疊影（`docs/steam_frame_client.md:1990-1992`）；切到外部遊戲 MV 後，帕魯球與投射物的軌跡是否恢復 120 Hz 的連續平滑感。
2. **重複圖樣背景與轉頭平移（M-MV2 配戴重點 2）**：
   - 情境：面對據點的木柵欄、窗框、森林樹葉或草地，一邊推搖桿橫移（或平滑轉向）一邊轉頭。
   - 觀察重點：木柵欄與窗框在合成格是否不再出現偶發的「配到隔壁一格而跳動」；搖桿平滑轉向與走路時的地面近景視差是否比純 ATW 流暢。
3. **物體邊緣鋸齒與 HUD 文字穩定度（M-MV3 配戴重點）**：
   - 情境：手持武器或手把在複雜背景與介面文字前快速左右揮動；打開帕魯背包或觀察畫面角落的血條／準星。
   - 觀察重點：(1) 武器與手把邊緣的 8 像素塊狀鋸齒在加入逆深度選取後是否明顯收斂；(2) 當 3D 物體從準星或血條文字後面走過時，文字是否穩固不動、沒有被跟著扯歪。

### 6.3 風險與替代方案（由大到小）

| 排名 | 風險描述 | 影響 | 緩解與替代方案 |
|---|---|---|---|
| **1** | **3D 場景 MV 拉扯疊在畫面上的 2D HUD／UI**（DLSS/FSR 在遊戲畫 HUD 之前執行，MV 貼圖裡沒有 HUD 的遮罩，但串流畫面已經把 HUD 畫上去了） | 當底下有 3D 物體移動或角色走路時，固定在畫面上的準星、血條或選單文字會被底下的 3D MV 往外推半格，造成文字抖動或重影。 | 1. **F6 零亮度差遮罩（不依賴遊戲介面）**：在頭盔 shader 比較上一張與這一張在同一位置的亮度差 $\Delta Y$；靜止 HUD 在兩幀之間 $\Delta Y \approx 0$，當 $\Delta Y < 0.015$ 且 $\|\text{MV}\| > 4\text{ px}$ 時直接將該點外插權重歸零。<br>2. **UEVR UI Target 遮罩**：在 UEVR 外掛中透過 `param->sdk->stereo_hook->get_ui_render_target()`（`UEVR_AFW/include/uevr/API.h:428`）讀取 UI 貼圖的 Alpha 通道，在 `cs_downsample_mv_8x8` 直接把有 UI 覆蓋的格點 MV 歸零。 |
| **2** | **UE5 Native Stereo／AFR 下的跨眼 `PrevViewProj` 污染**（右眼 Velocity 用到左眼的矩陣，帶入固定 IPD 視差） | 右眼（或雙眼）在合成格整張往水平方向跳動幾十個像素，畫面劇烈閃爍。 | 第 2.2 節的**遠景深度重投影殘差自動檢測**：在 `on_present` 比對遠平面像素的 MV 與同眼／跨眼重投影矩陣，一旦偵測到跨眼污染，在 Compute Shader 內以深度 $Z$ 與跨眼矩陣先行重投影歸正；若無深度則觸發 F5 將異常整張偏移拒絕並退回 `synthFlow`。 |
| **3** | **`§VR-RATE-AUTO` 半速分頻（`div=2`）時遊戲跑 120 Hz、串流送 60 Hz，單次 DLSS MV 只涵蓋半個串流週期** | 合成格只推了應推距離的一半（1/4 個串流週期），動態物體外插不足。 | 第 3.3 節的 `$K_{\text{step}}$ 幀步長補償**：Server 由前後兩張編碼幀配對到的 `mv_frame_id` 差算出 $K_{\text{step}} = M_{\text{cur}} - M_{\text{prev}}$（`div=2` 時為 2），若兩幀 MV 皆在 ring 內則在 Server 端累加，否則透過 `VIPLE_VR_MV_CHUNK_HDR.frameStep` 傳給頭盔端將位移乘上 $K_{\text{step}}$。 |
| **4** | **遊戲未開 DLSS／FSR 3.1／XeSS（或使用 TSR／TAA／DX11）** | 攔截不到超解析度 `EvaluateFeature` 呼叫。 | 1. **替代方案 A（UEVR RenderTarget Pool）**：在 UEVR 外掛中透過 `render_target_pool_hook->get_render_target(L"SceneVelocity")` 與 `L"SceneDepthZ"`（`API.h:421-424`）直接抓取 UE 引擎原生的 Velocity（`R16G16B16A16_UNORM` 等）與 Depth。<br>2. **替代方案 B**：完全不送 `T_MV`，頭盔端 100% 維持現有的 `synthFlow` 影像區塊比對。 |
| **5** | **反作弊系統（EAC／BattlEye）阻擋外部 DLL 注入** | 強行注入會導致遊戲拒絕啟動或封號風險。 | 嚴格限制邊界：首期只做 **UEVR 外掛**（玩家已經載入 UEVR 的單機／連線遊戲本來就無反作弊或已關閉反作弊）；後續獨立 DLL 模式在注入前檢查目標行程與系統服務是否有 `EasyAntiCheat`／`BEService`／`vgk`，偵測到即放棄注入，退回頭盔端 `synthFlow`。 |
| **6** | **跨行程 D3D11↔D3D12 共享貼圖在某些驅動或筆電雙顯卡上開啟失敗** | `OpenSharedHandle` 回傳失敗，Server 拿不到 MV 貼圖。 | 第 3.2 節的**備援交接通道（`VRIPC_MV_F_SHM_PAYLOAD`）**：遊戲端在 `cs_downsample_mv_8x8` 後直接 `CopyTextureRegion` 到 `D3D12_HEAP_TYPE_READBACK` 並寫入 SHM 緩衝槽（每幀僅數百 KB），完全繞過跨 API 共享貼圖限制。 |

---

## 7. 預計新增與修改的檔案清單（實作階段用，本次未改）

| 端點 | 檔案路徑 | 預計改動內容 |
|---|---|---|
| **遊戲端外掛（新增）** | `Sunshine/tools/uevr_mv_plugin/` | 僅引入 MIT 授權的 `uevr/API.h`、`Plugin.hpp`；實作 NGX／Streamline／FSR 3.1／XeSS 攔截、`cs_downsample_mv_8x8.hlsl`、跨行程 Pipe 握手與 `vripc_mv_ring_t` 寫入。 |
| **IPC ABI** | `SS/vr/vr_ipc_abi.h` | 新增 `vripc_mv_desc_t`、`vripc_mv_ring_t` 與 `VRIPC_MV_*` 常數（獨立 section 或擴充 ABI，維持 `VRIPC_STATIC_ASSERT` 鎖定）。 |
| **Server IPC 與擷取** | `SW/vr_bridge_win.cpp`<br>`SW/display_vr.cpp` | 建立 MV 共享貼圖 ×4 與共享 Fence；在 `display_vr_t::capture` 完成 `(Pose, QPC)` 配對、CPU Fence 檢查（$\le 0.5\text{ ms}$）、Planar+LZ4 壓縮並掛入 `img->vr`。 |
| **協定（三份同步）** | `Q/VipleVr.h`<br>`S3/VipleVr.h`<br>`moonlight-android/.../VipleVr.h` | 新增 `VIPLE_VR_SERVER_CAP_GAME_MV`、`VIPLE_VR_CLIENT_CAP_GAME_MV`、`VIPLE_VR_LINK_T_MV = 5` 與 `VIPLE_VR_MV_CHUNK_HDR`。 |
| **Server 廣播** | `SS/stream.cpp`<br>`SS/vr/vr_session.cpp` | 協商 `vrGameMv=1` $\to$ `;gameMv=1`；在 `videoBroadcastThread`（`SS/stream.cpp:6288-6325` 之後）將 `packet->vr_meta.mv_payload` 切塊、AES-GCM 加密後由視訊 UDP socket（含 multilink）送出。 |
| **頭盔端接收與配對** | `MQ/streaming/vr/vrframemeta.h`<br>`MQ/streaming/vr/vrframemeta.cpp`<br>`Q/VideoStream.c` | 視訊 socket 辨識 `'V','L', T_MV` 並驗證 GCM tag；組齊後 LZ4 解壓放入 `VrExternalMvRing`（以 `videoFrameIndex` 為 key）。 |
| **頭盔端補幀合成** | `MQ/streaming/xr/xrvideo.h`<br>`MQ/streaming/xr/xrvideo.cpp` | `renderNewFrame`（`:925` 旁）查 `VrExternalMvRing`；`renderSynth`（`:2404`）與 `renderTilesSynth`（`:3619`）新增 `kExtFlowBody` 轉換寫入 `m_Flow`、加入 F1～F6 退回與 HUD 保護、擴充 `dumpSynthInputs`（`:861`）。 |
