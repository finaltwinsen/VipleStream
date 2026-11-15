# vr_probe（開發用，不出貨）

VipleStream 2.0 §VR M1b 的 MSVC 測試工具（設計 §F.4）。由 server 的 `--vr-selftest` 以主控台使用者身分
啟動（`run_command(false, …)`，放進 kill-on-close 的 job），stdout 每一行都是 `[VIPLE-VR-PROBE] <內容>`，
server 以 `sanitize_probe_line` 過濾（限長 240、控制字元與 `[` 換成 `?`）後才解析 `key=value`。

## 建置

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Sunshine\src\platform\windows\steamvr_driver\Build-SteamVRDriver.ps1 -Target probe
```

- 輸出：`Sunshine\src\platform\windows\steamvr_driver\out\x64\Release\vr_probe\{vr_probe.exe, openvr_api.dll}`，
  另打包 `temp\vr_probe\VipleStream-VrProbe-dev.zip`（只在 `.195` ↔ host 之間 scp，不進 release）。
- 旗標與 driver 相同（`/MT /W4 /WX /permissive- /external:W0`），另加 `/DVRDRV_PEER_HOOKS`：
  `ipc_client.cpp` 的異常注入掛鉤只在 vr_probe 編進去，driver DLL 沒有這些程式碼路徑。
- V2 只取 OpenVR 的版本常數，`vr_probe.exe` 不匯入 `openvr_api.dll`；V4 的 whoami／space／timing／scene／watch 才會用到。

## 部署到 host

`<install>\tools\vr_probe\vr_probe.exe`（`<install>` = `C:\Program Files\VipleStream-Server`）。bridge 在 T2 只放行
這個映像（§B.1 第 5 步的 selftest peer 例外），`deploy_server_remote.ps1` 不會刪它。

## 模式（V2）

| 模式 | 內容 | 結果行 |
|---|---|---|
| `unit` | ABI 表；`ipc_proto.h` 純函式（訊息、驗證、seqlock、SPSC、退避）；server 映像路徑推導；driver_log 節流；loopback（本行程假 server ↔ 真 `ipc_client`，含 D3D11 共享 ring／fence） | `unit pass=<n> fail=<n>`，fail=0 時 exit 0 |
| `ipcpeer` | 以 driver 同一份 `ipc_client.cpp`（`peer_mode`）當假 driver：握手、開 server 建的 ring／fence、以 config 更新率畫圖並發布 descriptor、讀 tracking | `ipcpeer summary …`、`ipcpeer tracking …`、`ipcpeer reject reason=<n> detail=<n>` |

`ipcpeer` 的異常注入：`--abi N`、`--hello-size B`、`--peer-fence-max [--peer-fence-max-after N]`、
`--peer-stop-signal-after N`、`--peer-crash-after N`、`--peer-never-read`、`--peer-close-after-welcome`、
`--peer-escalate`、`--peer-no-flush`、`--peer-no-render`、`--hello-repeat N [--hello-interval-ms M]`、`--pipe NAME`
（不存在的 pipe 會一直重試到 `--seconds` 用完）。`--peer-gpu-hold-ms` 在 V3 實作。

兩種模式都在最前面印 `abi-row <key>=<value>` ×306 與 `abi-rows count=<n> digest=<16 hex>`，selftest T6 與
server（MinGW GCC）自己的表逐字比對。

CSV（ipcpeer）寫 `%LOCALAPPDATA%\VipleStream\vr_probe\<utc>-<pid>\ipcpeer.csv`；stdout 寫入失敗一律忽略。
所有輸出都不含 handle 值（只印 `GrantedAccess` 之類的權限遮罩）。

- 遊戲看到的控制器（2026-11-14；都要在 PCVR session 進行中、以主控台使用者身分跑，本機工具 `scripts/vr/run_probe_user.sh <模式> <秒> <tag>`）：
  - `--mode swing --seconds N [--out DIR]`：像 legacy scene app 那樣每個顯示格取一次右手的 game pose（`GetDeviceToAbsoluteTrackingPose`，
    預測到下一格的顯示時刻），看軌跡平不平滑。搭配 client `--vr-synthetic-hmd --vr-synthetic-hands`。摘要 `swing summary`：
    `dv`＝相鄰兩格位置差分的速度變化量、`apiErr`＝API 回報的速度與差分速度的差、`frozen`＝位置完全沒動的格數、`jumps`＝單格位移
    超過預期三倍的次數；逐格寫 `swing.csv`（`target_s,x,y,z,vx,vy,vz,qpc…`，離線算「偏離局部二次曲線多少」）。
  - `--mode toss --seconds N [--out DIR]`：拋球——左手按住扳機、往上、放開。搭配 client `--vr-synthetic-hmd --vr-synthetic-motion still
    --vr-synthetic-hands --vr-synthetic-toss`（每 2 秒拋一次，放開那一刻真值是往上 2.00 m/s，之後 0.12 秒內停住）。每個顯示格取左手的
    game pose 與 legacy `GetControllerState`；扳機由按住變成放開的那一格記下 `api`（API 回報的垂直速度）、`diff1`（和上一格的位置差分）、
    `diff3`（最近 3 格平均）。摘要 `toss summary tosses=N …` 之後三行各自的分佈、負值的次數、偏離真值一半以上的次數；
    逐次寫 `toss.csv`（`frames_held`＝扳機按住幾格，144 Hz 準時是 115 左右，多的就是放開的訊號晚到）。
- M4a 收尾：`--mode scene|timing --haptic-every-ms N`（50～60000）每 N ms 對每支控制器送一次 legacy `TriggerHapticPulse`（3 ms），log `haptic mode=… pulses=N`；用來驗 driver→server→client 的 haptic 轉送（client 看 `[VIPLE-VR-INPUT] … haptic applied=`）。以一般使用者身分在 PCVR session 中執行即成為 scene app，不必登記進 apps.json。
