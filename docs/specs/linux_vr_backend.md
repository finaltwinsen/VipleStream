# Linux VR 擷取後端（§LINUX-VR）

> 代號 **§LINUX-VR**，日期 **2026-11-26**。這份文件只寫設計，沒有改任何程式。
> 寫它是因為 VipleStream 的 PCVR 擷取只存在於 Windows，而 PalWorld 一卡一眼需要在 Linux 上「每張卡編碼自己那隻眼、不把原始畫面搬去另一張卡」。
>
> 狀態：設計，尚未實作。不動 `docs/vr_architecture.md` 的不變式 10（非 Windows 的 `/serverinfo` b1 仍是 0、KMS 擷取行為不變），直到以後真的改程式、而且新模式預設關閉。
>
> 相關文件：[`../vr_architecture.md`](../vr_architecture.md)、[`../vr_protocol.md`](../vr_protocol.md)、[`../steamvr_driver.md`](../steamvr_driver.md)。
> 需求來源：外部的 Palworld VR 計畫（不在本 repo，2026-11-26）。可借鑑的實作在 `temp/vrdual_src`（Apache-2.0，不進產品建置，不要把它當執行期依賴）。

**路徑縮寫**

- `SS/` = `Sunshine/src/`
- `SW/` = `Sunshine/src/platform/windows/`
- `SL/` = `Sunshine/src/platform/linux/`
- `MQ/` = `moonlight-qt/app/`
- `Q/` = `moonlight-qt/moonlight-common-c/moonlight-common-c/src/`
- `UEVR/` = UEVR 上游原始碼（praydog/UEVR）的根目錄（本機參考副本，不在本 repo；只供研究；UEVR 授權不可散布修改版）
- `VD/` = `temp/vrdual_src/`

文中行號是 2026-11-26 工作樹讀到的位置。

---

## 0. 現況（設計要接上的事實）

### 0.1 Windows 已經在做的事

Windows PCVR 是兩個行程：

```
遊戲 → SteamVR compositor → driver SubmitLayer/Present
     → 合成 SBS → NT 共享紋理 + fence
     → viplestream-server：display_vr_t CopyResource → 單一 NVENC → 一條 RTP
頭盔姿態 0x5506 → server → driver TrackedDevicePoseUpdated → SteamVR → 遊戲
```

- 合成發生在 driver：`SW/steamvr_driver/direct_mode.cpp:365` 的 `SubmitLayer` 收下兩眼，再編成一張 SBS（`docs/steamvr_driver.md:9-18`、`docs/vr_architecture.md:34`）。
- server 擷取端在 CPU 確認 fence 之後才 `CopyResource`（`SL` 沒有這條；`SW/display_vr.cpp:9-10`、`:1203`）。不變式 8：不在 GPU 上等對方行程的 fence。
- 協定把形狀寫死成左右並排：`SS/vr/vr_session.cpp:429` 的 `layout=sbs`，`SS/vr/vr_session.h:161` 的 `packed_width = eye_width * 2`，`SS/vr/vr_ipc_abi.h:348` 的 `packed_width` 註解。
- 姿態回去的最後一哩是 OpenVR：`SW/steamvr_driver/tracking.cpp:131` 的 `TrackedDevicePoseUpdated`。
- 這整條用 D3D11、NT handle、named pipe。Linux 對不到。

### 0.2 Linux 今天有什麼

- `SL/vr_stub.cpp:21-23` 的 `vr::platform_supported()` 恆為 false。同檔 `:44-49` 的 `pcvr_available()` 理由是 `platform unsupported`。`SS/vr/vr_platform.h:5-8`、`SS/vr/vr_bridge.h:9-11` 寫明 Linux 不建任何 VR 物件。
- `SL/misc.cpp:1172-1177`：`captureSource != 0` 直接拒絕。桌面仍走 KMS／Wayland／NvFBC。
- `/serverinfo` 的 PCVR bit（`VIPLE_VR_SERVER_CAP_PCVR = 0x02`，`Sunshine/third-party/moonlight-common-c/src/VipleVr.h:45`）在非 Windows 固定是 0。這是不變式 10（`docs/vr_architecture.md:239`）。
- Linux 桌面 NVENC 存在（`SL/kmsgrab.cpp:638-644` 會為 CUDA／NVENC 挑 NVIDIA 的 KMS card），但那是「抓一張桌面、在一張卡上編碼」。不是 VR，也沒有兩張卡各編一隻眼的入口。

### 0.3 遊戲側實際把畫面交到哪

目標不是「任意 Vulkan 遊戲」，是 Palworld 經 Proton（DXVK，D3D11→Vulkan）＋ UEVR `SYNCHRONIZED`。

- 模式定義：`UEVR/src/mods/VR.hpp:28-31`（`NATIVE_STEREO=0`、`SYNCHRONIZED=1`、`ALTERNATING=2`）。
- `SYNCHRONIZED` 與 `ALTERNATING` 都算 AFR：`VR.hpp:408-416` 的 `is_using_afr()`。同一個世界 tick 畫左眼、再畫右眼，各觸發一次 Present（`VR.cpp:2057` 的 `on_present`，`:2129` 用 `m_render_frame_count` 的奇偶分眼）。
- 畫完的那一眼不靠視窗 Present 進頭盔。`D3D11Component::on_frame` 把 backbuffer 的一塊拷進 OpenXR swapchain，**只有右眼那次**才 `end_frame`（`UEVR/src/mods/vr/D3D11Component.cpp:719`）。
- AFR 時左眼拷的是 backbuffer **左半**（同檔 `:530-550`，極端相容模式才拷全寬）。右眼在 `is_afr` 為真時拷的也是 **左半**（`:641-643`，註解寫 "Copy the left eye on AFR"），不是右半。兩次 Present 各自把「這一眼畫在左半」的結果送進 `AFR_LEFT_EYE`／`AFR_RIGHT_EYE`（`UEVR/src/mods/vr/runtimes/OpenXR.cpp:1799-1803`）。
- 姿態來自 runtime，不是來自遊戲自己猜：`OpenXR::synchronize_frame` 呼 `xrWaitFrame`（`OpenXR.cpp:119-141`），`update_poses` 呼兩次 `xrLocateViews`（view space 與 stage space，`:263-280`）。
- Proton 上這組 OpenXR 呼叫經 wineopenxr 進 **Linux** 的 OpenXR runtime（`XR_RUNTIME_JSON`）。UEVR 原始碼在 Windows 前綴裡，不直接開 Linux 的 Vulkan。

因此影像與姿態在 UEVR 裡本來就是兩條路：影像是「每眼一次 D3D Present／拷貝」，姿態是 OpenXR。Linux 後端維持這個拆法，不要合成回一條。

### 0.4 vrdual 能借、不能借

`VD/docs/ARCHITECTURE.md:147-177` 的 mirror：遊戲只看得到一張卡；第二張卡由 layer 用 ICD 直接建裝置、重播指令；兩張卡的 memory type 必須相同（`VD/layer/vrdual_mirror.hpp:2096`）。1660 Ti 配 1660 Ti 符合「同型號」這個前提，**還要過第 1.3 節的位址重播閘門**。

Present 時 vrdual 把 swapchain **原圖**讀回主機、寫進 `/dev/shm` 的 seqlock ring（`VD/common/vrdual_shm.h:1-7`、`VD/layer/vrdual_layer.cpp:1939-1960`）。那是每幀數十 MB 的 BGRA。本設計**不採用**這段輸出。只借用：藏第二張卡、裝置重播、seqlock、以及「第 2F 張是左眼、第 2F+1 張是右眼」的配對（同檔 `:1951-1952`）。

不要借用：

- clip-space 眼位偏移與頭部旋轉（`VD/docs/ARCHITECTURE.md:98-110`、`VD/docs/tasks/M4-head-pose.md`）。UEVR 已經用真的 view matrix。再改 shader 會雙重立體。
- `lockstep_wait`（`VD/layer/vrdual_layer.cpp:644-686`）那種在 Present 裡 `usleep` 等另一眼。那是兩個行程對齊用的。UEVR 在同一個行程裡先左後右，本來就對齊。在遊戲裡空等會把兩眼的 GPU 時間串成相加。配對改在 server，見第 2.3 節。
- `vrdual-xr` 當顯示端。它是 OpenXR **客戶端**，把 shm 裡的原圖送進另一個 runtime。我們的頭盔在網路另一端。

---

## 1. 影像從哪裡來

目標約束（`PLAN.md:24-26`）：一張卡算一隻眼；該卡的 NVENC 編碼該眼；原始像素不跨卡。位元流很小，可以跨卡、跨行程。

### 1.1 四個選項

| | SteamVR Linux driver | OpenXR runtime（Monado）內建合成 | Vulkan layer 攔 Present | DXVK 分支 |
|---|---|---|---|---|
| 資料路徑 | 遊戲 → wineopenxr → SteamVR compositor（通常一張卡）→ driver 再合成 → 交給 server 編碼。對應 Windows 的 `SubmitLayer` → SBS → `CopyResource` → NVENC | UEVR `xrEndFrame` 的兩張 swapchain 進 Monado compositor，由 runtime 的輸出模組編碼 | DXVK 的 `vkQueuePresentKHR` 上，這一眼的繪製已經在某一張卡上。layer 在**那張卡**登錄 NVENC，只把位元流寫進 shm。server 不碰像素 | 在 DXVK 的 Present／資源建立處做與 layer 相同的分流與編碼。DXVK 自己寫 GPU 位址，所以不依賴外部的 capture-replay |
| 延遲 | 多一段 compositor，再加一次「兩眼湊到同一張卡」的搬運。Windows 預算裡合成＋IPC＋NVENC 是 0.6＋0.4＋3.5 ms（`docs/vr_architecture.md:776-778`），那是**同一張卡**。跨到晶片組 x4 的原圖搬運不在這個數字裡，而且直接違反約束 | `xrWaitFrame` 會卡住遊戲（`OpenXR.cpp:141`）。Monado 還要 acquire／合成。若合成器在第三個裝置或其中一張卡上湊 SBS，又是一次原圖搬運 | 左眼編碼可以跟右眼繪製重疊（CPU 先提交左眼 Present，再錄右眼）。server 等兩份位元流都到再送。預期增加的是 `max(左眼編碼, 右眼繪製+編碼)` 裡多出來的那段，不是兩段相加。沒有 compositor 合成 | 與 layer 同一級，少一層攔截。延遲不該比 layer 低一個數量級 |
| 改動量 | 大，而且對不齊現有 driver。`direct_mode.cpp` 是 D3D11／DirectMode_009。Linux 沒有 NT 共享紋理。合成 SBS 這一步與「不搬原圖」相反 | 大。要改 Monado 的合成器，讓它不要把兩眼湊到一張卡，又要接 wineopenxr 的 swapchain 匯入。Monado 的幀迴圈是為本機顯示器寫的 | 中。新的 layer（注入遊戲行程）＋ server 收位元流的擷取源。不移植 `steamvr_driver/`。桌面 KMS 不動 | 中到大，而且是**持續**成本。Proton 每次換 DXVK 都要重接。分流邏輯與 layer 重疊 |
| 能不能就地編碼 | 不能，除非 driver 在 compositor 之前就拿到分屬兩張卡的影像。SteamVR 的工作就是把兩眼收到同一個 compositor | 預設不能。runtime 建立的 swapchain 在它自己的裝置上；UEVR 的 `CopySubresourceRegion` 必須與遊戲同一個 D3D11 device。要「每眼一張卡」等於把 runtime 重寫成兩裝置，已不是「用 Monado」 | 能。像素從產生到 NVENC 都在同一張 `VkDevice` | 能，理由同 layer |
| 判定 | 不採用 | 不採用當影像出口。姿態可以是一個**更小**的 runtime，見第 3 節，不要用 Monado 的合成器 | **採用**（有第 1.3 節的閘門） | 閘門失敗時的備案，不先做 |

### 1.2 推薦

**Vulkan layer 在遊戲行程裡攔 `vkQueuePresentKHR`，每張卡用自己的 NVENC 編碼該次 Present 的那隻眼，只交出位元流。** 頭部姿態由另一個只做 OpenXR 姿態與幀時序的 runtime 送回 UEVR，不經這個 layer，也不經 Monado／SteamVR 的合成器。

推薦這條，是因為另外三條都要嘛先把兩眼的原圖湊到同一張卡（SteamVR driver、Monado 合成器），要嘛把雙卡重播做進每一次 Proton 更新都要重接的 DXVK（分支）。layer 與 DXVK 解耦，又是 vrdual 已經走通的注入點。

推薦路徑：

```
Proton 內 UEVR（SYNCHRONIZED）
  偶數 Present：指令在 GPU A 執行 → A 的 backbuffer 左半是左眼
  奇數 Present：指令在 GPU B 執行 → B 的 backbuffer 左半是右眼
        │ 同一張卡
        ▼
  layer：裁切／縮放（仍在該卡）→ 該卡 NVENC → 位元流
        │ /dev/shm seqlock（每眼一個檔，槽裡是 HEVC，不是 BGRA）
        ▼
  viplestream-server（Linux）：配對同一 frame id → 兩條 RTP（各一埠）
        │ 既有 FEC、0x81、0x5506
        ▼
  Steam Frame：兩個解碼器 → 兩張眼圖 → projection（不再把一張 SBS 對半切）

頭盔姿態（與上面平行，不搬像素）
  Frame xrLocateSpace → 0x5506 → server 時鐘對映
        → shm 姿態區塊 → pose runtime 的 xrLocateViews
        → wineopenxr → UEVR 相機
```

layer 的分流對齊 `PLAN.md` 的階段，不要一次做完：

1. 兩張卡都執行全部指令（vrdual mirror 的重播），輸出輪流取：A 發布偶數 Present、B 發布奇數 Present。這一步不求加速，只求兩眼都對。
2. 之後才讓閒置的那張卡跳過繪圖。跨幀依賴（TAA、自動曝光、遮擋查詢）是 PalWorld 計畫第 3 階段的事，**不在這份後端的協定裡解決**。後端只要求「發布出來的那張圖是完整的那一眼」。

擷取矩形不要寫死成整張 swapchain。以 `D3D11Component.cpp:547` 與 `:641-643` 為準，SYNCHRONIZED 兩次都可能只在左半有這一眼。P2 用一次 dump 確認 Palworld 的矩形，寫進遊戲 profile。極端相容模式（同檔 `:544-545`、`:649-650`）才是全寬。

編碼做在 layer 行程內、同一張 `VkDevice` 上。不要把 `VkImage` 用 dma-buf 送去 server 再編碼：那一步在「server 的 CUDA 裝置 ≠ 這張卡」時就是跨卡搬原圖。1660 Ti 是 Turing，桌面路徑已經用 NVENC SDK；Vulkan Video Encode 能不能在這張卡上用，不當設計前提（P0 確認，見第 4 節）。

位元流 shm 沿用 seqlock 的形狀（奇數＝寫入中、帶 frame index），不要沿用 vrdual 的像素佈局，也不要沿用 Windows 的 `vr_ipc_abi.h`（那個 ABI 是 x64 共享紋理＋fence，`SS/vr/vr_bridge.h:16-19`）。建議每眼 4 槽、每槽上限 2 MB。200 Mbps、90 Hz 時一幀約 280 KB，2 MB 蓋得住短時間的尖峰。原始 2160² BGRA 一幀約 18 MB，這條路徑不配置那種緩衝。

### 1.3 必須先過的閘門（不過就改走 DXVK 分支）

vrdual 把第二張卡重播到與第一張卡**相同的 GPU 位址**，依賴 `bufferDeviceAddressCaptureReplay`。文件寫的是 RADV 有這個功能（`VD/docs/ARCHITECTURE.md:239-244`）。同一段也寫 Proton／DXVK 標題**沒有**驗過（`VD/docs/ARCHITECTURE.md:84`）。

DXVK 會把 buffer device address 寫進緩衝。layer 在外面看不到那些寫入的語意。NVIDIA 專有驅動若沒開 capture-replay，第二張卡上的位址是錯的，mirror 會像 vrdual 那樣停掉（或畫出壞畫面）。NVENC 又必須用專有驅動，不能改靠 RADV 避開。

P0 在 CachyOS、兩張 1660 Ti、**將要用來編碼的那版專有驅動**上查 `VkPhysicalDeviceVulkan12Features::bufferDeviceAddressCaptureReplay`。兩張都是 `VK_TRUE` 才做 layer 重播。有一張是 false：雙卡重播改做進 DXVK 分支（DXVK 自己分配位址，可以每張卡寫一份），layer 只留「Present 時在該裝置上編碼」或者連編碼也放進分支。server／頭盔的兩路協定不變，所以這個閘門不推翻第 2 節。

單卡、單眼的編碼（P2）不需要 capture-replay，可以跟閘門平行做，用來先接通 server。

### 1.4 明確不做

- 不把 `SW/steamvr_driver/` 移植到 Linux。姿態不走 `TrackedDevicePoseUpdated`，影像不走 SBS 合成。
- 不讓 Monado 或 SteamVR compositor 當像素出口。
- 不用 vrdual 的 shader 眼位。姿態只從 OpenXR 進去。
- 不在 Linux 上把 `captureSource=1` 偷偷接回 KMS 再裁左右半。那張桌面圖在一張卡上，另一眼已經丟了。
- 不改不變式 10 的預設行為。新能力是另一個 bit，見第 2.2 節。

---

## 2. 兩路位元流，不合成一條

### 2.1 取捨

合成一條 SBS 的前提是編碼器看得到一張 `2W×H` 的圖。兩眼的像素在兩張卡上。要拼這張圖，就要把至少一隻眼的原圖搬到另一張卡或主機。這就是本需求禁止的事。

HEVC／H.264 也不能事後把兩條獨立位元流黏成一張 SBS：參考幀、slice、SPS 都是單張圖的。兩顆 NVENC 各編各的，產出就是兩條位元流。

所以：

| | 一條 SBS（現況） | 兩路（本設計） |
|---|---|---|
| 誰湊兩眼 | server 側的 driver 合成器，在編碼前 | 頭盔在**解碼後**，用兩張已解碼的眼圖填 projection |
| 跨卡的東西 | 原圖（每眼數 MB 到數十 MB） | 位元流（每眼數百 KB） |
| 解碼器 | 一個。`VideoDepacketizer.c:1251-1284` 一條視訊、一顆 0x81 | 兩個。各自的參考幀、IDR、intra-refresh |
| 解析度協商 | `packed = 2 × eye`（`vr_session.h:161`） | 每一路就是 `vrEye` 的 `W×H`。`packed` 不再代表並排後的寬度 |
| 頭盔 GPU | 一張 2W×H swapchain，`imageRect` 對半切 | 兩張眼圖（或一張 swapchain 上兩塊**各自上傳**的區域）。不再假設左右各佔一半是因為編碼器把它們排進去 |
| 掉一隻眼 | 整幀延遲或凍結 | 不能只更新一隻眼就送出新 pose。見第 2.4 節 |
| 注視點分塊 | 四塊排在同一張圖（`docs/vr_protocol.md:55-64`） | P3 不做。兩路與 `foveaTiles` 同時開啟直接拒絕。以後每一路自己排「全視野在上、注視點在下」，仍不跨卡 |

1660 Ti 沒有 AV1 編碼器。兩路的 codec 是 HEVC（做不到再談 H.264）。沿用 `VIPLE_VR_CODEC_HEVC`。

### 2.2 協定

原則：不擴充 RTSP／SDP（`docs/vr_protocol.md:26`）。舊 client 沒講雙路就永遠看不到新東西。Windows 的 `layout=sbs` 一個位元組都不改。

**能力 bit。** `<VipleStreamVR>` 下一個空位是 b12＝`0x1000`（`docs/vr_protocol.md:37`）。定義 `VIPLE_VR_SERVER_CAP_DUAL_EYE`。**不要**把 Linux 設進 b1 `VIPLE_VR_SERVER_CAP_PCVR`（`VipleVr.h:45` 註明只有 Windows 會設）。b1 仍表示「Windows SteamVR driver 那條路」。

client 的 `vrCaps` 最後一個是 `VIPLE_VR_CLIENT_CAP_FOVEA_TILES = 0x80`（`VipleVr.h:67`）。下一個 `0x100` 定義 `VIPLE_VR_CLIENT_CAP_DUAL_EYE`。server 兩個 bit 都有、而且 `vr_linux_backend=enabled`、而且擷取端握得到手，才在回應裡給雙路。否則 Linux `/launch vr=1` 維持今天的 `VR_DISABLED`（`SL/vr_stub.cpp:44-48`）。

**`<VipleStreamVRSession>` 多兩個欄位**（`SS/vr/vr_session.cpp:429` 今天寫死 `layout=sbs`）：

- `layout=dual`。client 已經把 `layout` 當字串收下（`MQ/streaming/vr/vrlaunchparams.cpp:104-106`），但沒有人依它分支，畫面仍照 SBS 切。
- `eyePort=<udp>`。右眼的 RTP／FEC 埠。左眼沿用現有視訊埠。

同一 UDP 埠用兩個 SSRC 拆開也可以少一個埠，但現有解包器不看 SSRC（`Q/VideoDepacketizer.c` 沒有 SSRC 分流；`SS/stream.cpp:6684` 只看得到音訊 SSRC 被設成 0）。兩個埠各接一個現成的 depacketizer，序號與 FEC 自然分開，改動面比較小。

**幀標頭仍用 0x81（24 B）。** 每一路各帶一份相同的頭部 render pose 與 `echoSampleId`。配對鍵是**兩路相同的 frame number**，不新增 0x83。哪一路是哪隻眼由埠決定，不在標頭裡猜。

0x82（注視點、32 B，`VideoDepacketizer.c:1256-1275`）在 `layout=dual` 時不送。P3 的 server 若被同時要求 `vrFoveaTiles` 與雙路，回 `VR_BAD_PARAMS`。

**LOSS 要分眼。** 現有 `0x5507/09` 沒有眼索引（`docs/vr_protocol.md:163`）。雙路時改送新的 subtype `0x11`（`docs/vr_protocol.md:151` 寫 2026-11-20 時 C→S 下一個空號是 `0x11`；實作前再對一次 `VipleVr.h`）：`{eye u8, firstLost u32, lastLost u32, reason u8}`。左眼 0、右眼 1。舊 server 依 TLV 長度跳過。雙路 session 不再用 `0x09`，避免兩眼的丟幀混在同一個序號空間。

intra-refresh、IDR cooldown、`repeated` 旗標都是**每一路各算各的**。server 不要把右眼的 P 幀補進左眼的參考鏈。

**送出時機。** server 等同一 frame number 的兩份位元流都到了再一起送。先編完的那一路留在記憶體（數百 KB），不先上網。這樣頭盔不會在新 pose 下只換一隻眼。等太久的政策見第 2.4 節。

追蹤上行 0x5506、haptic 0x5508、LATCH、時鐘對映都沿用。它們本來就不是跟 SBS 綁在一起的。

### 2.3 為什麼不在遊戲裡 lockstep

vrdual 在 Present 裡等另一眼發布（`vrdual_layer.cpp:1960`）。UEVR 的兩次 Present 已經是同一個 tick 的先後順序，frame id 由 layer 用 `present_index / 2` 編成一對即可。

左眼的 NVENC 在 GPU A 上跑的時候，CPU 可以繼續錄右眼、GPU B 繼續畫。若在左眼 Present 裡睡到右眼編碼結束，這段重疊就沒了。配對緩衝放在 server（或 layer 與 server 之間的 shm 消費者），遊戲行程不等。

### 2.4 頭盔端行為

今天 PCVR 的投影假設**一張** SBS：

```5553:5561:moonlight-qt/app/streaming/xr/xrcontext.cpp
    for (int e = 0; e < 2; e++) {
        const XrVector3f off = quatRotate(head.orientation, eye[e].position);
        views[e].pose.orientation = quatMul(head.orientation, eye[e].orientation);
        views[e].pose.position = {head.position.x + off.x, head.position.y + off.y, head.position.z + off.z};
        views[e].fov = fov[e];
        views[e].subImage.swapchain = swapchain;
        views[e].subImage.imageArrayIndex = 0;
        views[e].subImage.imageRect.offset = {rect.offset.x + (e == 0 ? 0 : w / 2), rect.offset.y};
        views[e].subImage.imageRect.extent = {w / 2, h};
    }
```

`layout=dual` 時每一眼的 `imageRect` 蓋住**那一路自己的整張**解碼圖，不再 `w/2`。pose 仍是 `renderPose ∘ eyeToHead`（`xrcontext.cpp:2614-2644` 的註解）。兩路的 0x81 應帶同一組頭部 pose；若不一致，用左眼那份並記 log，不要混用。

解碼後的配對：

- 兩路都解出同一個 frame number → 組成一對，送進現有的 projection。
- 只有一路到：不推進顯示用的 pose。上一對完整眼圖繼續交給頭盔 runtime 做 ATW。沿用現有 stale：500 ms 淡出、2 s 換 loading（`xrcontext.cpp:2617-2629`）。
- 晚到的那一路補上後，若對家已經是更新的 frame number，丟掉較舊的那張（解碼器仍要吃進位元流，參考幀才不會斷），畫面維持較新且**兩眼都齊**的那一對。
- 禁止「新 pose ＋ 舊的另一眼」。那是 `ALTERNATING` 的時間差，`PLAN.md:12` 已排除。

`MQ/streaming/xr/xrvideo.cpp` 今天把一幀畫進一張 swapchain（含注視點、合成位移的 UV，搜尋時不跨左右眼，見該檔 `:1916` 一帶的註解）。雙路要有兩個 mailbox、兩次上傳。注視點 quad（`buildInsetQuads`，`xrcontext.cpp:5623`）P3 不啟用。

Steam Frame 的解碼是 `hevc_v4l2m2m` 一顆（`docs/vr_architecture.md:278-282`）。兩顆同時解是這份設計最大的頭盔風險，必須在改協定之前量到（P0）。量不到能同時解兩路「每眼目標解析度、目標 Hz」的 HEVC，就停，不進入 P3。降解析度或降到 72 Hz 可以再量一次；不可以改回「server 拼 SBS」當作靜默退路，那會把原圖搬運帶回來。

Android client 不做 OpenXR（`docs/vr_architecture.md:6`）。雙路只實作在 moonlight-qt。三份 `VipleVr.h` 仍要逐位元組相同，Android 那份只是多兩個常數，不協商。

### 2.5 要改的檔案

下面是實作時的清單。**這次沒有改它們。**

**協定（三份必須一起改，`VipleVr.h` 標頭自己要求 byte-identical）**

| 檔案 | 改什麼 |
|---|---|
| `Sunshine/third-party/moonlight-common-c/src/VipleVr.h` | `VIPLE_VR_SERVER_CAP_DUAL_EYE 0x1000`、`VIPLE_VR_CLIENT_CAP_DUAL_EYE 0x100`、C2S subtype `0x11` |
| `moonlight-qt/moonlight-common-c/moonlight-common-c/src/VipleVr.h` | 同上 |
| `moonlight-android/.../moonlight-common-c/src/VipleVr.h` | 同上，行為不開 |
| `Q/VideoDepacketizer.c`（`:1248-1294` 是單一路 0x81／0x82） | 不改標頭格式。session 要能掛**第二個** depacketizer 實例；`0x11` 的 LOSS 從控制通道送出時帶眼索引 |
| `SS/stream.cpp` | 第二個視訊 RTP／FEC 發送；frame number 兩路共用；等齊再送 |
| `SS/vr/vr_session.cpp:427-449` | `layout=dual`、`eyePort=`；雙路時拒絕 fovea tiles |
| `SS/nvhttp.cpp`（`/serverinfo` 組 bitmask 處） | 只有 Linux 後端握手成功才設 `0x1000`，**不設** `0x02` |

**server 擷取**

| 檔案 | 改什麼 |
|---|---|
| `SL/vr_stub.cpp` | macOS 維持全 stub。Linux 在新模式關閉時行為與現在相同（`platform_supported()` 仍為 false，b1 仍是 0） |
| （新增）`SL/vr_eye_ingest.cpp` | 讀兩份位元流 shm、配對、交給 `stream.cpp`。不建立 D3D／不 `CopyResource` |
| `SL/misc.cpp:1172-1177` | 新的 `captureSource` 值走 ingest，不走 KMS。值不要復用 Windows 的 `1`（`display_vr_t`），避免以後日誌分不出來源 |
| `SS/video.cpp` | 雙路幀**跳過**桌面編碼執行緒。位元流已經是 HEVC。探測（probe）仍可在每張卡上各開一次 NVENC 測能力，但正式幀不重編 |
| `SS/config.*` | `vr_linux_backend`，預設 `disabled` |
| Linux cmake（今天把 `vr_stub.cpp` 編進來，`docs/vr_architecture.md:607`） | 加入 ingest；layer 與 pose runtime **不要**連進 `viplestream-server` |

**遊戲行程裡的兩個新程式（不在 Sunshine 行程）**

| 檔案 | 改什麼 |
|---|---|
| （新增）Vulkan layer，建議 `Sunshine/tools/linux_vr/layer/` | 藏第二張卡、重播、Present 時在該卡 NVENC、寫位元流 shm。不改 shader。可由 `VK_INSTANCE_LAYERS` 注入；包裝腳本只套在遊戲行程 |
| （新增）pose runtime，建議 `Sunshine/tools/linux_vr/runtime/` | 第 3 節。`XR_RUNTIME_JSON` 指到它 |

layer 連結 `libnvidia-encode.so`。這是遊戲行程的依賴，不要塞進 server 的 `.deb` 去給 KMS 用。

**頭盔（moonlight-qt）**

| 檔案 | 改什麼 |
|---|---|
| `MQ/streaming/vr/vrlaunchparams.cpp:104-106` | 認得 `layout=dual` 與 `eyePort`；雙路時 `packed` 的寬度是單眼寬，不是 2 倍 |
| `MQ/streaming/session.cpp` | 第二個 UDP、第二個 decoder；拆除時兩個都停 |
| `MQ/streaming/video/ffmpeg.cpp` | 第二個 decode context。0x81 的 pts／frame number 查表分眼各做一份（架構文件裡「一個 map」的假設，見 `docs/vr_architecture.md:295-297`） |
| `MQ/streaming/xr/xrvideo.cpp` | 兩個 mailbox、兩次上傳。SBS 用的 UV 對半切在雙路模式關掉 |
| `MQ/streaming/xr/xrcontext.cpp:5553-5561` | `layout=dual` 時 `imageRect` 各用各的整張 |
| `MQ/streaming/vr/vrframemeta.*` | 配對鍵 `(eye, frameNumber)`；只有兩眼都齊才讓 projection 換圖 |

不變的檔：`SW/display_vr.cpp`、`SW/steamvr_driver/**`、`SW/vr_bridge_win.cpp`、桌面 KMS 在 `vr_linux_backend=disabled` 時的路徑、RTSP 的 SDP 產生。

---

## 3. 頭部姿態怎麼回到遊戲

### 3.1 Windows 對照

```
Frame：xrLocateSpace（顯示時刻的預測姿態）
  → 0x5506（232 B，docs/vr_protocol.md:90-112）
  → server 時鐘對映
  → driver tracking.cpp:131 TrackedDevicePoseUpdated
  → SteamVR 當成真的頭盔
  → 遊戲的 OpenVR／OpenXR xrLocateViews 拿到的就是這份姿態
```

server 不直接寫遊戲記憶體。中間那個「遊戲相信的 runtime」在 Windows 是 SteamVR。

### 3.2 Linux 上 runtime 的角色

UEVR 在 Proton 裡呼的是 OpenXR（`OpenXR.cpp:141`、`:268`、`:280`、`:1898` 的 `xrEndFrame`）。wineopenxr 把這些呼叫送到 Linux runtime。所以 Linux 上扮演 SteamVR 這個角色的，必須是 **OpenXR runtime**，不是 OpenVR driver，也不是 Vulkan layer。

這個 runtime **只做四件事**：

1. **幀時序。** `xrWaitFrame` 回傳的 `predictedDisplayPeriod` 等於這一場協商的週期（client 的 `vrPeriodNs`）。`predictedDisplayTime` 用與 0x5506 同一套時鐘對映後的目標顯示時刻。不要在 runtime 裡再預測一次。client 已經帶了 `predictNs`（`docs/vr_protocol.md:94`），Windows 端還會再加上 SteamVR 的 `poseTimeOffset`。Linux 少了 SteamVR 這層，若 runtime 又外插，遊戲會畫到一個更遠的未來，頭盔的 ATW 再拉回來，延遲數字會好看但畫面是錯的時間點。
2. **姿態。** `xrLocateViews`／`xrLocateSpace` 讀 server 寫進 shm 的最新樣本（HMD、左右控制器的 pose 與速度，0x5506 裡都有）。樣本的目標時間對上這次 `displayTime`。樣本太舊就沿用上一筆並把速度清零，對齊 Windows driver 的 stale 行為（`docs/steamvr_driver.md` 與 IPC 的 `stale_pose_count`，`SS/vr/vr_ipc_abi.h:322`）。
3. **視圖大小。** view configuration 的寬高等於協商的 `vrEye`。UEVR 用 runtime 回報的寬高當渲染解析度（`UEVR/src/mods/VR.cpp:2265` 的 `get_runtime()->get_width()`）。runtime 說多大，遊戲就畫多大，layer 才能 1:1 編碼，不必在編碼前跨卡縮放。
4. **把 `xrEndFrame` 收下然後丟掉像素。** UEVR 仍會把 backbuffer 拷進 OpenXR swapchain（`D3D11Component.cpp:550`、`:657`）並在右眼 Present 呼 `end_frame`。這些 swapchain 建在遊戲看得到的那張卡上，拷貝才不會失敗（D3D11 `CopySubresourceRegion` 不能跨 device）。runtime 不把這兩張圖拿去顯示，也不把它們湊去另一張卡。真正送進頭盔的是 layer 從 backbuffer 編成的位元流。swapchain 拷貝是浪費，P4 以後可以在 layer 裡把「目的地是 runtime 的 image」改成 no-op；P1–P3 先讓拷貝成功，避免 UEVR 以為提交失敗而重設 session。

控制器動作（`xrSyncActions`、`xrGetActionStatePose`）用 0x5506 的兩手輸入。這是 UEVR 能轉頭以外的前提，放在 P1 就做最小集合（pose、trigger、grip、搖桿）。haptic 從 `xrApplyHapticFeedback` 回 server、再走現有 0x5508，放在 P4 之後，不擋畫面。

### 3.3 為什麼不是 Monado，也不是 SteamVR Linux

Monado 能當 runtime，但它的 compositor 會在 `xrEndFrame` 把提交的層合成到本機裝置，`xrWaitFrame` 跟本機掃描走。要它「不要碰像素」等於關掉它的主功能，剩下的追蹤插件還是要自己寫。直接寫一個小 runtime，呼叫面只覆蓋 UEVR 實際用到的那些（instance、system、session、space、swapchain、frame、locate、action），範圍比較看得到。

SteamVR Linux 加一個移植的 driver，只能服務走 OpenVR 的遊戲。這份需求的 UEVR 走 OpenXR（`PLAN.md:31`）。多掛一個 SteamVR 還會把兩眼又吸進 compositor，和第 1 節的結論衝突。

vrdual 的頭部是改 clip-space（`VD/docs/tasks/M4-head-pose.md:1-19`），遊戲相機不知道頭盔。UEVR 的相機知道。姿態必須從 `xrLocateViews` 進去，讓遊戲的剔除與 TAA 用對的方向。layer 不讀姿態、不寫 shader。

### 3.4 時鐘與空間

- shm 裡的姿態用 `CLOCK_MONOTONIC` 奈秒，加上 server 已經算好的「client 時鐘 → 單調時鐘」偏移。runtime 的 `XrTime` 就用這個奈秒，不要再定義第三個 epoch。
- 參考空間用 STAGE。client 上行與 projection 已經規定 STAGE 優先（`docs/vr_architecture.md:367-372`）。runtime 的 stage space 與 view space 差一個固定的眼位；`xrLocateViews` 回的兩眼 pose 要含 `eyeToHead`（launch 的 `vrEyeToHead`），因為 UEVR 會直接拿 view pose 當相機，不會再問 VipleStream。
- recenter（0x5507/02）時 runtime 更新 stage 原點，並讓下一幀的 layout epoch 加一。不重新解釋 client 空間。這與 Windows「recenter 只改 seated zero」同一句（`docs/vr_architecture.md:373`）。
- 遊戲畫出去的 pose 經 0x81 回到頭盔，投影仍用 `renderPose ∘ eyeToHead`。runtime 不要另外乘一次 IPD。

### 3.5 注入方式

遊戲行程需要兩個只有它看得到的設定：`VK_INSTANCE_LAYERS` 指到 layer，`XR_RUNTIME_JSON` 指到 pose runtime。Vulkan loader 與 OpenXR loader 就是靠這兩個環境變數。這不違反「server 設定不走環境變數」：`viplestream-server` 自己仍只讀 config（`vr_linux_backend`）。包裝腳本在啟動 Proton 時設給遊戲行程，比照 vrdual 的 `./vrdual run`，不要求使用者自己 `export`。

server 與遊戲在 CachyOS 上是同一個使用者時，`/dev/shm` 的權限用 `0600` 加一個目錄裡的隨機檔名（handshake 用本機 socket 把檔名交給 server）。不要做成所有使用者可讀的固定路徑。

---

## 4. 分期、驗收、風險

與 `PLAN.md:28-34` 對齊。那邊的階段 0（Windows 單卡）不在這份文件。下面每一期都預設**不**改 Windows 路徑；`vr_linux_backend` 預設關閉時，Linux `.deb` 的 KMS 與 `/serverinfo` b1＝0 跟現在一樣。

### P0 — 只量，不改協定

在 CachyOS（專有驅動、兩張 1660 Ti）與 Steam Frame 上量三件事：

1. 兩張卡的 `bufferDeviceAddressCaptureReplay`（第 1.3 節）。
2. 每張卡 NVENC 編一隻眼 HEVC 的極限：至少 1440²@90、以及協議裡的每眼目標（Frame 路徑現在的預設每眼是 2160² 量級，`docs/vr_architecture.md:375`）。記錄編碼時間 p50／p95 與有沒有掉幀。
3. Steam Frame 同時兩個 `hevc_v4l2m2m` 解上述解析度的能力：解碼時間、掉幀、溫度。用兩條現成的 RTP 或本機檔即可，不必等 layer。

驗收：三個數字都有，而且 (3) 在選定的解析度／Hz 下能持續解。兩路解碼失敗就停，不開始 P3。capture-replay 失敗不停止端，只把雙卡重播改列 DXVK 分支。

另外記 PCIe：`lspci` 的 LnkSta，確認哪張卡在晶片組 x4。這不擋 P0，但 P3 的「沒有原圖跨卡」要用它解讀。

### P1 — pose runtime，單卡，不編碼

Proton 裡 UEVR 的 `xrWaitFrame`／`xrLocateViews` 接到 server 轉送的 0x5506。影像可以仍是 UEVR 拷進 runtime 的 swapchain，runtime 丟掉。不要求雙卡，不要求 NVENC。

驗收：

- UEVR log 出現成功的 `xrWaitFrame` 與 `xrLocateViews`（對照 `OpenXR.cpp:146`、`:271` 的錯誤路徑沒有一直出現）。
- 戴上頭盔轉頭，遊戲相機跟著轉，方向沒有再被 shader 偏一次。
- 靜止時 `predictedDisplayTime` 與 0x5506 的目標時間差是穩定偏移，不是每幀多預測一個週期（第 3.2 節）。
- 關掉 runtime 之後，同一個 Proton 前綴仍能改回系統原本的 `active_runtime.json`，Palworld 不以 VR 啟動時行為不變。

停止條件沿用 `PLAN.md:31`：UEVR 注入失敗，或 wineopenxr 拒這個 runtime。

### P2 — 單卡 layer，一路位元流

還不分流。layer 在遊戲唯一的那張卡上，於 Present 把 profile 指定的矩形編成 HEVC，寫入 shm。server 用一個**實驗旗標**把這條位元流當成「單眼」送出，或只寫檔用 `ffmpeg` 解。不上 `layout=dual`，不改頭盔投影。

驗收：

- shm 檔的大小是位元流等級（每幀百 KB 級），不是 `寬×高×4`。
- 解出來的圖是 UEVR 的一隻眼，矩形與 `D3D11Component.cpp` 的拷貝區域一致（左半或全寬，以 dump 為準）。
- 遊戲幀時間相對「沒有 layer、只有 pose runtime」的增加量 ≤ 編碼時間＋1 ms。若遠大於編碼時間，就是在 Present 裡做了不該做的同步。
- `vr_linux_backend=disabled` 時 server 不開 shm、不聽第二埠。

### P3 — 雙卡、兩路、頭盔配對

capture-replay 閘門已過才做 layer 重播；否則這期改在 DXVK 分支裡做同樣的輸出（shm 格式相同）。server 實作第 2.2 節，頭盔實作第 2.4 節。兩張卡都執行全部指令，輪流輸出。

驗收：

- 左右眼有視差，HUD 與遠景的差異符合 UEVR 的相機，不是 vrdual 的 clip 偏移。
- 配對後兩眼的 frame number 相同；skew 的 p95 ＜ 一個顯示週期。超過就視為沒過，不准用「先顯示先到的那一眼」換過。
- 跨卡流量：編碼期間不應出現「每幀一隻眼的原圖」那種量（1440² BGRA@90 約 0.7 GB/s，2160² 約 1.7 GB/s）。用 NVML／`nvidia-smi dmon` 的 PCIe 計數看。位元流等級的流量可以有。
- Windows PCVR（`layout=sbs`、一條 RTP、`buildProjection` 的 `w/2`）用現有頭盔連 Windows host，畫面與改之前相同。
- Linux 桌面串流（沒有 `vr=1`）的 KMS 路徑不變。

### P4 — 閒置卡跳過繪圖

這是 PalWorld 階段 3，改 layer／DXVK，不改協定。驗收是兩眼不閃、幀時間比 P3 下降，而且 P3 的 PCIe 與配對條件仍成立。閃爍表示跨幀依賴還沒處理完，後端不要用「重送上一幀」蓋過去。

haptic、控制器完整按鍵、注視點（每路自己的上／下排，仍不跨卡）放在 P4 之後，各自有對應的 Windows 行為可對照時再做。

### 4.1 最大風險（由大到小）

1. **Steam Frame 同時解兩路 HEVC 吃不下。** 吃不下就沒有雙路。P0 沒過就停止，不改協定。這是唯一會推翻第 2 節的風險。
2. **NVIDIA 專有驅動沒有 `bufferDeviceAddressCaptureReplay`。** 推翻的是「用 layer 重播 DXVK」，不是兩路協定。退到 DXVK 分支，成本變成跟著 Proton 的 DXVK 改版。
3. **1660 Ti 的 NVENC 編不了一隻眼的目標解析度。** Turing 的編碼器不是 Ada。P0 若 2160²@90 失敗，把 runtime 回報的 view 尺寸降下來再量，不要在 server 把兩眼縮小後拼貼。
4. **擷取到錯誤的矩形。** AFR 的兩次 Present 都可能只有左半是這一眼（`D3D11Component.cpp:641-643`）。抓整張會把沒畫的右半編成黑邊或上一眼的殘留。P2 的 dump 是用來擋這個的。
5. **三重預測。** runtime 若在 0x5506 已經預測過的 pose 上再外插一個 `predictedDisplayPeriod`，遊戲畫的時間與頭盔 ATW 的目標會差一幀。P1 的時鐘驗收就是在擋這個。
6. **wineopenxr 要的圖形綁定比小 runtime 先做的多。** UEVR 用 D3D11 swapchain；wine 會把它翻成 Linux runtime 的 Vulkan swapchain。P1 若連 swapchain 都建不起來，就還不到「轉頭」。停止條件寫在 P1。
7. **P3 全指令重播時，兩張卡各畫兩眼，NVENC 與繪製搶同一張卡。** 這是預期的慢，不是協定 bug。加速靠 P4。P3 的驗收不要把幀時間訂成「比單卡快」。
8. **跨幀依賴在 P4 閃爍**（TAA、自動曝光、HZB）。現象會像「後端有時送錯眼」。判準是：P3 全指令重播時不閃、P4 跳過繪圖後才閃 → 問題在跳過清單，不在 RTP。

---

## 5. 不確定（設計當下沒有實機證據）

- 1660 Ti 在 CachyOS 目前這版專有驅動上，capture-replay 與 NVENC 同時開一個 session 能不能共存。vrdual 沒有在 NVIDIA＋NVENC 上驗過。
- UEVR `SYNCHRONIZED` 在 Proton 裡，Present 的 backbuffer 到底是左半、全寬、還是中間再縮過。第 0.3 節是原始碼，不是 Palworld 的 dump。
- wineopenxr 對 runtime 的最低擴充清單（`XR_KHR_vulkan_enable2`、swapchain 格式、D3D11 對應）沒有在這棵樹裡找到文件。P1 要用 UEVR 的錯誤 log 對。
- Steam Frame 兩路硬解的餘裕。架構文件的解碼預算 4 ms 是**一路** SBS（`docs/vr_architecture.md:781`）。兩路各解較小的圖，時間不是簡單的一半，也不是兩倍。
- 晶片組 x4 那張卡的上傳（P3 全指令重播時，兩張卡都要吃 CPU 上傳）會不會讓右眼 Present 晚超過一個週期。這不違反「不搬原始畫面」，但會讓配對 skew 不及格。P3 要分開記「PCIe 是上傳」還是「PCIe 是眼圖」。
- 0x5507 的下一個空號以 `VipleVr.h` 為準。`docs/vr_protocol.md:151` 的「0x11」是 2026-11-20 的句子，實作那天若已經被佔用，順延，不要撞號。
- 第二個 UDP 埠與現有防火牆、多連線（multilink）怎麼共存。P3 建議雙路先**不要**跟 `VIPLE_VR_SERVER_CAP_MULTILINK` 同時開。多連線假設的是一條視訊修補路徑。

## 6. 實機要驗證的（實作開始前／各期）

| 時機 | 做什麼 | 過的標準 |
|---|---|---|
| P0 | `vulkaninfo` 或小程式印兩張 1660 Ti 的 capture-replay | 兩張都 true，或明確改走 DXVK 分支 |
| P0 | 每卡 NVENC：1440²@90 與目標每眼解析度的 HEVC，各 60 秒 | p95 編碼時間 ＜ 一個週期的一半，否則降解析度再量 |
| P0 | Steam Frame 兩顆 v4l2m2m 解同樣的兩條 HEVC | 60 秒無累積延遲、掉幀可以解釋（不是持續落後） |
| P0 | `lspci -vv` 的 LnkSta | 知道哪張卡是 x4，寫進測試紀錄，不當成通過條件 |
| P1 | Proton＋UEVR＋pose runtime，頭盔轉頭 | 相機跟著轉；log 沒有持續的 `xrWaitFrame failed`／`xrLocateViews failed` |
| P1 | 靜止頭部，比對 0x5506 目標時間與 `predictedDisplayTime` | 差值穩定，不隨幀數每幀加一個週期 |
| P2 | dump 一對 Present 的裁切與 UEVR 拷貝區域 | 與 `D3D11Component.cpp:530-550`、`:630-657` 一致 |
| P2 | 看 shm 槽的位元組數 | 遠小於 `寬×高×4` |
| P3 | 左右眼截圖與 PCIe 計數 | 有視差；沒有每幀數 MB 級的跨卡原圖 |
| P3 | 舊的 Windows host＋現有頭盔 | `layout=sbs` 投影仍是 `w/2` 對切，`xrcontext.cpp:5570` 那行 log 的尺寸不變 |
| 回歸 | Linux server `vr_linux_backend=disabled` 的桌面串流 | KMS 擷取與今天相同；`/serverinfo` 沒有 b1、也沒有 b12 |
