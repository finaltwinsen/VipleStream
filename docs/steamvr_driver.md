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

**pose_flags（2026-10-05，session config 的 `reserved2` 拆出 `pose_flags`；版面不變、不升 ABI 版號，0＝上表的舊行為）**
由 server 設定檔決定，全部預設關，等 Frame 上 A/B 之後再定預設值：

- **bit0 §VR-CTRL-OFFSET**（`vr_ctrl_pose_offset = enabled`）：控制器也設 `poseTimeOffset`，與 HMD 用同一個目標時間
  （client 以同一個預測時間 locate 頭與雙手）、同樣的 −50 ms 下限，上限改讀 `ctrl_extrap_cap_us`（舊版寫了但沒讀）。
  舊版控制器的 offset 一直是 0：SteamVR 把 client 已經預測到「取樣＋A」的拍子姿態當成「到達時刻」的姿態再外插一次，
  第八輪推算拍子比頭超前約 25～31 ms，上行抖動也直接變成拍子的時間誤差。速度歸零（stale、OutOfRange）時 offset 也歸零。
- **bit1 §VR-STALE-HOLD**（`vr_stale_policy = hold`）：2T < 空窗 ≤ `stale_oor_ctrl_us`（預設 100 ms）時，HMD 與控制器都
  保留線速度、`poseTimeOffset` 照實設成「目標 − 現在」（下限放寬到 −100 ms，否則空窗約 81 ms 就撞到 −50 ms），
  SteamVR 從樣本的目標時間繼續外插；角速度的座標語意還沒以探測確認，hold 期間 50 ms 內線性衰減到 0。重送的樣本
  （角速度已衰減）也記進 pose_history。超過 100 ms 才回到舊規則（`holdExpired` 計數）。舊規則每輪約 150 次讓頭與拍子
  先往回跳（外插整段被拿掉）、新樣本到了再往前跳；上行空窗多半是延遲尖峰而不是掉包（POSE-RX gap 只有 0～9）。

- **bit2 §VR-ANGVEL-LOCAL**（`vr_angvel_local = enabled`）：HMD 與控制器的角速度轉成機體座標（`ω_local = q⁻¹ ω q`）
  再交給 SteamVR。`vr_probe --mode predict`（client `--vr-emulate --vr-synthetic-motion tilt30yaw`：頭與右手先傾 30°、
  再繞世界 Y 轉 30°/s）實測 SteamVR 以 `q·exp(ω t)` 外插——**把 `vecAngularVelocity` 當本地座標**；client（OpenXR
  `XrSpaceVelocity`）給的是追蹤空間（世界）座標。沒轉時外插增量的旋轉軸與世界 Y 的 |dot|＝0.866（cos 30°，繞錯軸），
  轉了之後＝1.000。頭或拍子一傾斜，舊版就繞錯的軸外插（揮拍時拍面角度偏掉）。pose_history 照舊記世界座標
  （它以 `exp(ω t)·q` 外插，與 SteamVR 對本地 ω 的結果相同）。
- 同一支探測量到的其他事實（SteamVR 2.17.10）：poseTimeOffset 依文件的正號採用（HMD offset +33 ms、控制器 0 時，
  兩者在 pred=0 的姿態差正好 +33.3 ms；`vr_ctrl_pose_offset` 開啟後 0.0 ms）；外插量在約 100 ms 以內是線性的
  （gain 1.00），超過後封頂在距樣本時間約 97～100 ms（pred 200 ms 時 gain 0.65）。所以 §VR-STALE-HOLD 的空窗
  加上 SteamVR 的預測時間超過約 100 ms 時，姿態會停在那裡（仍比舊規則整段拿掉外插好）。

觀察：driver 的 `tracking 10s` 尾端有 HMD／控制器 offset 分布、`arrivalMaxMs`／`gt2T`、`hold`／`holdExpired`、
`poseFlags`；`tracking recovered` 多了 `total_ms`（整段空窗）與 `hold`。server 的 `[VIPLE-VR-UPLINK] 10s` 是同一段
空窗在 server 收到的時刻（加上控制執行緒單輪耗時），兩邊對照可分出網路與 server 轉送。

**姿態預測時間（§VR-PREDICT，2.0.0）**：HMD 的 `Prop_SecondsFromVsyncToPhotons` 決定 SteamVR 把遊戲用的頭部姿態預測到
多遠。session 開始時取 session config 的 `vsync_to_photons_us`（server `vr::predict` 決定：設定檔固定值、上一個
session 學到的值，或一個週期＋30 ms）；串流中 server 依 client 回報的姿態落後送 STATE `SET_V2P`（0x05，arg＝µs，
1000～200000，超出範圍 driver 忽略並記 `ipc state-ignored reason=range`），driver 在 RunFrame 改屬性並送
`VREvent_PropertyChanged`，log `set-v2p us=<N>`。舊的 `DEV_SET_V2P`（0x04）只在 selftest 的 dev mode 有效。
重新握手（新 generation）時屬性回到 session config 的值。
2026-10-05 修正（server `vr_predict`，Frame 第六～八輪的教訓）：每個 session 開頭 10 個可靠視窗不用（暖機）；探測前後的
3 個視窗最大差 > 6 ms 視為不穩（探測前不探測、探測後退回起始值重量）；「不採用」只停該 session，連續兩個 session 才
不再探測；學到的值依更新率分開存，沒學過的更新率由最接近的換算（`v' = 14 ms + (v − 14 ms)·T'/T`）。設定檔固定值時
（目前 `<host>` 是 `vr_vsync_to_photons_us = 55000`）整段不跑。

**frame ring 交還（§VR-RING-RELEASE，2.0.0）**：driver 發布一幀後要等 server 的 consumedFence 到值才會重用那個 slot。
server 換消費者（VR 斷線後 `/resume`、encoder 重建）時，ring 裡最後幾幀可能已過期 1 s 以上；server 用
`frame_reader_t::release_latest()` 只驗 generation 與 fence 就交還，不複製。舊版用一般讀取排空，過期那筆被判不合格
而不交還，driver 三個 slot 全被佔住（driver log `noslot` 一直增加、`composed=0`），串流只剩 10 fps 重送幀。

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
| 單元 | `vr_probe.exe --mode unit` | ABI 表、訊息驗證、ring、log 節流、virtual vsync、pose_history（7 項）、pose-policy（21 項：offset／hold 規則、揮拍空窗模擬、角速度座標換算）、loopback |
| selftest | `viplestream-server.exe --vr-selftest --only T0,T3 --cycles 20` | 由執行中的 service 代跑；T3 每輪啟動／結束 SteamVR，驗 settingsSame、safeMode=0、註冊恰好一筆 |
| selftest（手動 SteamVR） | `--vr-selftest --only T4 --manual-steamvr --probe scene|whoami --motion still|yaw --hold-sec N` | 讀回兩眼圖案與 renderPose 比對（mismatch=0）；`whoami` 列出 SteamVR 看到的裝置 |
| 端到端（S0） | `VipleStream.exe stream <host> "SteamVR Home" --display-target pcvr --vr-emulate --vr-synthetic-motion sine` | host 需 `vr_pcvr = enabled` |
| 外插語意（2026-10-05） | client `--vr-emulate --vr-synthetic-motion tilt30yaw` 串流中，host 以主控台使用者身分跑 `vr_probe.exe --mode predict --seconds 30 --out <dir>` | `predict summary dev=<hmd|right> pred=… angGain／worldDotY／localDotY／posGain`、`predict offset hmdMinusRight`；判讀見第 6 節 pose_flags |

## 9. 已知限制

- standby 後再 arm，vrcompositor 可能不再 Present（與 V4 看到的 `AcquireSync` 逾時同源）；編排器遇到
  「我們的 HMD 在 standby 且沒有 VR app」時直接重啟 SteamVR 迴避。
  **§VR-REARM（2026-10-04）**：有 VR 遊戲在跑時重啟 SteamVR 會把遊戲關掉，而使用者斷線後常常就是想接回原本的遊戲。
  §VR-RING-RELEASE 修好 frame ring 死結之後，這種情況先試直接重新 arm：driver 5 s 內要就緒，WAIT_HMD 只等 10 s 的 Present。
  等不到才照舊回 `STEAMVR_RESTART_REQUIRED`（client 會顯示原因並結束，見 `steam_frame_client.md` §8.6）。
  沒有 VR app 在跑時照舊直接重啟 SteamVR。
  `/launch` 的同步衝突檢查原本只要看到執行中的 VR 遊戲（kind=vr-app）就回 `503 VRLINK_ACTIVE`，連我們自己的 SteamVR
  上、上一個 session 留下的遊戲也擋，請求根本到不了編排器。現在 SteamVR 用的是我們的 driver 時（`conflict_t::our_driver`）
  不擋，交給編排器處理。
  驗證（2026-10-04，`<host>`，Eleven Table Tennis 在上一個 session 結束後仍在跑、HMD standby）：`<dev-client>` 以
  `--vr-emulate` 啟動 SteamVR Home（每眼 2160、120 Hz），流程為 `conflict … kind=vr-app restart=0` → `arm reason=rearm` →
  `rearm from standby ok`（arm 後立即恢復 Present）→ ACTIVE。擷取每 10 s 約 1199 幀、黑幀 0，SteamVR 沒有重啟，遊戲繼續跑。
- vrcompositor 顯示系統面板時會自己重畫第 0 層，T4 的圖案解不出來（`compared`≈0），不代表傳錯畫面。
- 控制器沒有 skeleton 與 render model；SteamVR 裡看得到姿勢與輸入，但沒有手的模型。
- 只有 Windows。Linux／macOS server 的 VR 相關入口全部回「不支援」（`vr_stub`），`/serverinfo` 只宣告 stub 能力。
- **chaperone JSON 的鍵名是 `jsonid`**（§CHAP-JSONID，2026-10-02）：`Prop_DriverProvidedChaperoneJson_String` 要照 SteamVR
  自己寫的 `chaperone_info.vrchap` 格式（最外層 `jsonid`、`version`；每個 universe 帶 `time`、`universeID`、`play_area`、
  `collision_bounds`、`standing`、`seated`）。寫成 `json_id` 時 vrserver 記「Failed to parse chaperone file because
  jsonid was missing」整份拒收：OpenVR app（SteamVR Home）正常，但 **OpenXR app 的參考空間會繞視線軸轉 180°**
  （Unity OpenXR 遊戲整個世界上下顛倒、host 上遊戲自己的視窗也是倒的）。驗法：vrserver.txt 出現
  `Found universe <id> in chaperone file`；`vr_probe --mode space` 的 `seatedVsRawMm` 應為 1200。
- **不要在 SteamVR 啟動期結束它**（§QUIT-SETTLE，2026-10-02 在 Win11 `<host>` 實測）：vrserver 起來幾秒內就結束
  vrmonitor，vrserver 會走「Lost master process → Quitting all immediately」，偶發在自己的 IPC 連線物件上
  use-after-free 當掉（0xC0000005、堆疊沒有 driver 的 frame）；當掉時 uptime 很短，下一次 SteamVR 以 safe mode
  啟動並擋掉第三方 driver（`blocked_by_safe_mode`）。編排器與 selftest 結束 SteamVR 前會先等 vrserver 滿 20 s；
  滿 20 s 後 `DriverRequestedQuit` 也會生效（約 1 s 結束，不必再結束 vrmonitor）。
- SteamVR 以 safe mode 啟動時（§SAFE-RETRY），編排器在 WAIT_DRIVER 每秒查一次 `blocked_by_safe_mode`，查到就結束
  SteamVR、清掉封鎖、重啟一次；第二次仍被擋回 `VIPLE_VR_STATE_CODE_SAFE_MODE`。
- **§VR-EXIT（2026-10-04，Frame 實測回報）**：
  - 遊戲結束：原本 PCVR 的 app 是否在跑完全由編排器決定，遊戲關掉後 host 的 SteamVR 還開著，串流就一直送 SteamVR
    主控台（Frame 上變成雙介面）。現在平面模式的 Steam 遊戲結束監看（`process.cpp` 的 `start_steam_watchdog_`）也套用到
    PCVR：先看到 RunningAppID 是這個遊戲，之後變掉就 `proc.terminate()`，停編排器並送 graceful termination。
  - SteamVR 被關掉（使用者在主控台按「退出 VR」）：以前 driver 斷線 10 s 後會自動重開 SteamVR，Frame 上畫面亂閃、
    使用者出不去。現在 vrserver 不在了就正常結束 session（`run_session` 正常返回 → `active()` 為 false →
    `proc.running()` 回 0）。SteamVR 真的當掉時遊戲也已經跟著結束，重開 SteamVR 接不回遊戲。
- **§VR-RESTORE-IDLE（2026-10-04，Frame 實測）**：session 結束後 SteamVR 若還開著，guard 就一直還原不了，因為設定只在
  vrserver 不在時才還原。host 的 `forcedDriver` 會一直是 viplestream、vrlink 也一直被關著，使用者改用 Steam 自己的
  串流就開不起來（實測如此）。現在編排器在 RESTORE_PENDING 時，如果我們的 HMD 在 standby、沒有 VR 遊戲在跑，滿 30 s
  就結束 SteamVR，下一個維護 tick 還原設定。遊戲結束後 RunningAppID 常會變成 SteamVR 本身（250820，它也在 vrmanifest
  裡），這不算遊戲；CONFLICT 的重新 arm 判斷也同樣排除 250820。遊戲還在跑時不動 SteamVR，留給斷線後接回。沒有遊戲時，
  下一個 session 本來就會重啟 SteamVR，所以提早結束不會讓接續變慢。
- **§STEAM-LOGIN（2026-10-04）**：§VR-NODASH 改用 `vrstartup.exe` 之後，Steam 沒在跑時不會被帶起來。編排器在
  LAUNCH_STEAMVR 前先查：Steam 沒登入、又沒記住帳號（HKU 的 `AutoLoginUser` 是空的，例如為了換別的帳號登出後沒登回來），
  就立刻回 code 17，以前要白等 60 s。有記住帳號但 Steam 沒在跑，就先以使用者身分執行 `steam.exe -silent`，再照舊等登入。
  client 的訊息會提示使用者先在主機登入 Steam（可用桌面模式）。
- 全新安裝的 Windows 第一次由 Steam 啟動 SteamVR 時，Steam 會要求以管理員身分裝 VC++ 2013 runtime（UAC 視窗；
  沒人按就卡 2 分鐘後才繼續，期間編排器回 code 10）。先在主機上手動啟動一次 SteamVR 並同意，或預先裝好。
