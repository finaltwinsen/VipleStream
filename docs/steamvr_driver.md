# VipleStream SteamVR driver（`driver_viplestream.dll`）

> 對象：要改 driver、查 PCVR 問題、或在新的 host 上驗證 PCVR 的人。
> 設計來源：`docs/vr_architecture.md`（§3 架構、§8 驗證）、`docs/vr_protocol.md`（線路格式）；
> log 標籤見 `docs/log_tags.md` §5b。本文件描述 M1b（2.0.0）完成時的實作。

## 1. 它是什麼

Windows server 的 PCVR 模式（`vr_pcvr = enabled`）讓 SteamVR 以為接了一台頭盔：

```
SteamVR app ──SubmitLayer/Present──▶ vrcompositor ──DirectMode _009──▶ driver_viplestream.dll
                                                                     │  合成兩眼、Signal fence
                                                                     ▼
                                          共享記憶體 ring（vr_ipc_abi.h）＋ named pipe
                                                                     │
                            viplestream-server（SYSTEM service）：vr_bridge → display_vr_t → NVENC → client
client 的頭盔／控制器姿勢 ──0x5506──▶ server ──tracking ring──▶ driver ──TrackedDevicePoseUpdated──▶ SteamVR
```

- driver 是**獨立撰寫**的（沒有移植 ALVR 程式碼），介面鎖 OpenVR 2.15.6（`Sunshine/third-party/openvr`）。
- 只實作 `IVRDriverDirectModeComponent_009`，沒有 IVRVirtualDisplay 備案（V4 實測 DirectMode 可行）。
- driver 不 include moonlight-common-c；需要的常數（tracking flags 等）在 `driver_context.h` 另抄。

## 2. 原始碼（`Sunshine/src/platform/windows/steamvr_driver/`）

| 檔案 | 內容 |
|---|---|
| `driver_main.cpp` | `HmdDriverFactory`、`IServerTrackedDeviceProvider`：Init／RunFrame／Cleanup；加入 HMD 與左右控制器；RunFrame 的事件迴圈（haptic、space-delta 觸發事件、DriverRequestedQuit、standby 切換） |
| `driver_context.h` | 各元件共用狀態（armed／link_up／server_stale、HMD index、`pose_hist`） |
| `hmd_device.*` | HMD 屬性（顯示頻率、IPD、universe、chaperone JSON、proximity、`/input/system/click`） |
| `controller_device.*` | 最小 Touch 控制器（S2-09，見 §5） |
| `direct_mode.*` | swap texture set、SubmitLayer／Present、合成、frame descriptor（renderPose、echo、flags） |
| `frame_compositor.*`、`shaders/` | 兩眼合成（HLSL，建置時編成 header） |
| `virtual_vsync.*` | 虛擬 vsync（依 server 的 pacing 參數推進；`vr_probe --mode unit` 有測試） |
| `tracking.*` | tracking 執行緒：讀最新 slot → DriverPose_t（HMD＋控制器） |
| `pose_history.*` | SubmitLayer 的 mHmdPose ↔ 回報過的樣本配對、space-delta（S2-06，見 §4） |
| `ipc_client.*`、`ipc_proto.h` | pipe 握手、generation、ring 讀寫（frame／tracking／haptic／timing／log） |
| `d3d_device.*` | driver 自己的 D3D11 device（以 LUID 選卡，P-B1） |
| `driver_log.*` | `[VIPLE-VR-DRV]` log：寫進 vrserver.txt，也轉給 server |
| `seh_guard.h` | 每個 SteamVR 進入點包 SEH；出事就進 degraded，不讓 vrserver 崩潰 |
| `Build-SteamVRDriver.ps1` | MSVC 建置 driver 與 `vr_probe`（`-Target driver|probe|all`） |
| `Stage-VipleSteamVR.ps1` | 打包進 server zip 的 `steamvr\`（sha256 清單、`viplestream.version`） |
| `Install-VipleSteamVR.ps1` | 只診斷（`-Status`）與手動移除（`-Remove`，拒絕以提升身分執行） |

建置一律走 `build_sunshine.cmd`（會呼叫 Stage → Build）；只改 driver 時可以單獨跑
`Build-SteamVRDriver.ps1` 做編譯檢查與 `vr_probe --mode unit`，但出貨的 zip 一定要走 `build_sunshine.cmd`。

## 3. 執行緒與鎖

| 執行緒 | 做什麼 | 注意 |
|---|---|---|
| vrserver 主執行緒（RunFrame） | 事件、加裝置、standby 切換、proximity | SteamVR host API 只在這裡或裝置回呼呼叫 |
| tracking 執行緒 | `TrackedDevicePoseUpdated`、控制器輸入 | 持 `ctx.pose_lock` 共享；`Deactivate` 以獨佔取得後把 index 設無效 |
| compositor 執行緒 | `SubmitLayer`／`Present` | direct_mode 自己的鎖；`pose_hist.match()` 在這裡 |
| ipc 執行緒 | pipe、generation 對映 | 只改 atomic 旗標 |

## 4. 姿勢空間與 pose_history（S2-06）

- driver 回報給 SteamVR 的是 **client 空間**的原始姿勢；SteamVR 會再套自己的 universe／seated／standing 轉換，
  所以 app 在 `SubmitLayer` 帶來的 `mHmdPose`（SteamVR 世界空間）跟 client 空間之間差一個剛體轉換 **Δ**。
- 每筆新樣本 `record()`：`{sample_id, pose, lin_vel, ang_vel(world), reported_qpc, poseTimeOffset}`，環形 1024 筆。
- 每次合成 `match()`：`t_app = SubmitLayer 當下 + 預測秒數`；候選＝`reported_qpc ∈ [now−100 ms, now]`；
  每個候選從 `reported + offset` 線性外插到 `t_app`，算 `Δ_c = mHmdPose · P̂_c⁻¹`，取與目前 Δ 角距離最小的 c*。
  - < 1° → `ECHO_MATCHED`，echo = c*。
  - 否則 → `POSE_FALLBACK`，echo = 時間最近的候選，`posehistMiss++`，把該候選的 Δ 放進候選緩衝。
- **採用新 Δ** 需要同時符合：最近 30 筆候選兩兩 < 0.2°、< 5 mm；而且（這段期間 HMD 角速度 < 5°/s，**或** 2 s 內收到
  `SeatedZeroPoseReset`／`StandingZeroPoseReset`／`ChaperoneUniverseHasChanged`／`SceneApplicationChanged`）。
  角速度條件是為了排除「等速轉頭＋app 晚交幀」造成的穩定假偏移（yaw 30°/s、晚 44 ms ≈ 1.3°），單元測試
  `no-adopt-yaw30-lag44` 專門守這條。
- 採用時 log：`[VIPLE-VR-DRV] space-delta=<yaw>/<pitch>/<roll>deg pos=<x>,<y>,<z>mm trigger=<still|event:<name>>`，
  driver status 的 `space_delta_mdeg` 同步更新。
- descriptor 的 renderPose = `Δ⁻¹ · mHmdPose`（client 空間）；Δ 非恆等時加 `VRIPC_FRM_SPACE_DELTA`。
- 新 generation（新的 client session）時 Δ 回到恆等重新學。

## 5. 控制器（S2-09，最小 Touch）

- 左右各一個 `TrackedDeviceClass_Controller`，serial `VIPLE-CTRL-L`／`VIPLE-CTRL-R`，在 HMD 加入後立即加入。
- `Prop_ControllerType_String = oculus_touch`、`Prop_InputProfilePath_String = {oculus}/input/touch_profile.json`
  （借用 SteamVR 內建 oculus driver 的 profile 與 binding；沒有 skeleton、沒有 render model）。
- 元件：system、a/b（右）或 x/y（左）的 click／touch、joystick click／touch／x／y、trigger click／touch／value、
  grip touch／value、thumbrest touch、`/output/haptic`。
- 按鍵位元（`vripc_ctrl_input_t.buttons`／`touches`）：b0 SYSTEM、b1 MENU（都對到 system/click）、b2 A/X、b3 B/Y、
  b4 THUMBSTICK、b5 TRIGGER、b6 GRIP、b7 TRACKPAD（未用）、b8 THUMBREST。
- 姿勢：client 的 grip pose，`raw_from_grip`（位移 [0, 0, −0.11] m、旋轉 0）放在 `vecDriverFromHeadTranslation`，
  由 SteamVR 套用（速度的槓桿臂也由它算）。
- 失效規則：slot 沒帶該手的旗標、或 `flags.b0 active = 0` → OutOfRange、`poseIsValid=false`、**放開所有輸入**；
  樣本超過 `stale_oor_ctrl_us`（預設 100 ms）→ OutOfRange；pipe 斷線／standby 跟 HMD 一起失效。
  狀態切換 log：`[VIPLE-VR-DRV] controller hand=<left|right> <out-of-range|tracking> age_ms=<n>`。
- haptic：`VREvent_Input_HapticVibration` → `vripc_haptic_evt_t{device 1=L 2=R, duration_us, frequency_hz, amplitude}`
  → haptic ring → server。

## 6. stale 與 standby 政策

| 狀況 | HMD | 控制器 |
|---|---|---|
| > 2T（`stale_zero_vel_us`）沒新樣本 | 重送上一個 pose、速度歸零、poseTimeOffset=0 | 同步重送、速度歸零 |
| > `stale_oor_hmd_us`（預設 1 s） | OutOfRange（SteamVR 顯示灰畫面） | — |
| > `stale_oor_ctrl_us`（預設 100 ms） | — | OutOfRange |
| pipe 斷線、disarm、server heartbeat 逾時 | `poseIsValid=false`、OutOfRange；`deviceIsConnected` 維持 true | 同左、放開輸入 |

kill server 之後約 33 ms pose 就失效（V4 實測，門檻 1 s）；新 server 握手後自動恢復。

## 7. 部署、註冊與還原

由 server 的編排器（`vr_orchestrator`＋`vr_platform_win`）在 `/launch mode=pcvr` 時處理，**不需要手動安裝**：

1. 部署：`<install>\steamvr\viplestream` → `<install>\config\steamvr\<ver>\viplestream`（secure_fs 檢查 owner／DACL／
   reparse／sha256，不過就 `deploy-refused`，fail closed）。
2. 註冊：以**使用者 token** 跑 `vrpathreg adddriver`（不變式 9：SYSTEM 不寫使用者可寫的路徑）。
3. guard：以使用者 token 改 `steamvr.vrsettings`（`forcedDriver=viplestream`、`driver_vrlink.enable=false`），留 marker；
   vrserver 沒在跑時才逐鍵還原。
4. 啟動 SteamVR、等 HMD、啟動 app；結束時 disarm、prune 舊版本目錄、註銷。

手動檢查：`powershell -File <install>\steamvr\Install-VipleSteamVR.ps1 -Status`（以一般使用者身分）。

## 8. 測試

| 層級 | 指令 | 內容 |
|---|---|---|
| 單元 | `vr_probe.exe --mode unit` | ABI 表、訊息驗證、ring、log 節流、virtual vsync、pose_history（7 項）、loopback |
| selftest | `viplestream-server.exe --vr-selftest --only T0,T3 --cycles 20` | 由執行中的 service 代跑；T3 每輪啟動／結束 SteamVR，驗 settingsSame、safeMode=0、註冊恰好一筆 |
| selftest（手動 SteamVR） | `--vr-selftest --only T4 --manual-steamvr --probe scene|whoami --motion still|yaw --hold-sec N` | 讀回兩眼圖案與 renderPose 比對（mismatch=0）；`whoami` 列出 SteamVR 看到的裝置 |
| 端到端（S0） | `VipleStream.exe stream <host> "SteamVR Home" --display-target pcvr --vr-emulate --vr-synthetic-motion sine` | host 需 `vr_pcvr = enabled` |

## 9. 已知限制

- standby 後再 arm，vrcompositor 可能不再 Present（與 V4 看到的 `AcquireSync` 逾時同源）；編排器遇到
  「我們的 HMD 在 standby 且沒有 VR app」時直接重啟 SteamVR 迴避。
- vrcompositor 顯示系統面板時會自己重畫第 0 層，T4 的圖案解不出來（`compared`≈0），不代表傳錯畫面。
- 控制器沒有 skeleton 與 render model；SteamVR 裡看得到姿勢與輸入，但沒有手的模型。
- 只有 Windows。Linux／macOS server 的 VR 相關入口全部回「不支援」（`vr_stub`），`/serverinfo` 只宣告 stub 能力。
