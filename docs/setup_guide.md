# VipleStream — Setup Guide

This is the user-facing setup guide for VipleStream — a Sunshine + Moonlight
fork for low-latency game streaming with built-in FRUC frame interpolation.

If something doesn't work, see [`troubleshooting.md`](./troubleshooting.md).

---

## What you need

| Side | Hardware / OS | Software |
|---|---|---|
| **Streaming host** (the gaming PC) | Windows 11 + NVIDIA GTX 10-series or newer (RTX recommended), AMD RX 5000 or newer, or Intel Arc (NVENC / AMF / QuickSync video encoder must be present) | VipleStream-Server (`viplestream-server.exe`) |
| **Client** (the device you watch on) | Windows / Linux / macOS desktop, or Android 7.0+ phone / tablet, or LTPO / 90Hz+ panel for FRUC dual-mode | VipleStream Client (Windows: `VipleStream.exe`; Android: VipleStream APK) |
| **Network** | Both endpoints reachable to each other on LAN, or via Tailscale / VPN. WAN streaming via `relay_url` is supported but adds 30-60ms latency. | — |

The server side **must** be a Windows machine; macOS/Linux server support
is not yet shipped (see [`docs/TODO.md`](./TODO.md) §J.4 for status).

---

## Install

### Server (Windows, the host PC)

1. Download `VipleStream-Server-<version>.zip` from the GitHub Releases
   page.
2. Extract to `C:\Program Files\VipleStream-Server\` (any path works as
   long as the service can read it; this is the canonical location).
3. From an **Administrator** Command Prompt or PowerShell:
   ```powershell
   sc create VipleStreamServer binPath= "C:\Program Files\VipleStream-Server\tools\viplestream-svc.exe" start= auto
   sc start VipleStreamServer
   ```
4. Open `https://localhost:47990` in a browser. You'll get a TLS warning
   (self-signed cert — expected; click through). The Web UI prompts for a
   username/password to set up — pick anything; this protects the Web UI
   only and is unrelated to the streaming pairing PIN.
5. The default config in `C:\Program Files\VipleStream-Server\config\sunshine.conf`
   is sane for a single-monitor 1080p120 setup. See **Configuration tips**
   below before changing anything.

### Client — Desktop (Windows)

1. Download `VipleStream-Client-<version>.zip` from GitHub Releases.
2. Extract anywhere (e.g. `C:\Tools\VipleStream\`).
3. Run `VipleStream.exe` — first launch creates per-user settings under
   `HKCU\Software\VipleStream\VipleStream`; no installer / admin
   elevation needed.

### Client — Android

1. Install `VipleStream-Android-<version>.apk` via `adb install -r ...`
   or sideload through your phone's file manager.
2. The first launch needs network and microphone permission; mic is
   only used during in-stream voice chat.

---

## Pair a client to the server

1. On the client, tap "Add PC" / "Add Computer" and enter the server's
   LAN IP (`192.168.x.y`). Hostname works too if mDNS resolves on your
   network.
2. The client shows a 4-digit PIN.
3. On the server's Web UI (`https://<server-ip>:47990`), open the PIN
   pairing page and enter the 4 digits. The pairing TLS handshake takes
   1-2 seconds.
4. The client's PC list will show the server with all listed apps.

After pairing once, the client remembers the server. Pairing data lives
under `HKCU\Software\VipleStream\VipleStream\paired_clients` on Windows
client and `/data/data/com.piinsta/databases/computers4.db` on Android.

If pairing fails, see [`troubleshooting.md`](./troubleshooting.md#pairing-issues).

---

## Configuration tips

### Stream resolution / framerate

In the client's Settings, the "video" section has resolution + FPS
dropdowns. Default is 1920×1080 @ 60 fps which works on every supported
GPU. Notable picks:

- **1920×1080 @ 120 fps** — recommended for desktop client, all 3 codecs
  (H.264 / HEVC / AV1) PASS the SW + HW decode bench at this res.
- **2560×1440 @ 120 fps** — works on the default D3D11 renderer (HW
  decode). On Vulkan + FRUC + HEVC specifically the SW decoder caps at
  ~50 fps because FRUC bypasses HW decode for CPU-side frame access; if
  you need FRUC at 1440p120, use H.264 or AV1 (both pass), or stay on
  D3D11. See [`troubleshooting.md`](./troubleshooting.md) "HEVC 1440p120
  only delivers ~50 fps when using Vulkan + FRUC".
- **3840×2160 @ 120 fps** — decoder-bound on i7-11800H-class CPUs even
  in HW decode; consider 60 fps at 4K instead.

### FRUC (frame interpolation)

VipleStream ships built-in frame interpolation: ML-based on Windows
(DirectML) and ME/warp on Android (Vulkan compute). Enable in client
Settings → "Frame interpolation".

- **Windows desktop client**: best on RTX 30/40-series. Older GPUs
  (A1000, GTX 16-series) may not hit the 14 ms inference budget — UI
  will show a warning if cascade fallback to non-FRUC happens.
- **Android client**: works on Pixel 5 / Adreno 620 and equivalent
  Vulkan-capable devices. Best with 90Hz+ panels (60 fps source on 90Hz
  panel produces visible smoothness gain). On 60Hz panels the dual-mode
  threshold (1.40× display × 2 input ≤ display × 1.40) doesn't trigger
  for 60 fps source — single mode is used and FRUC is effectively
  off.

### Bitrate

Default 30 Mbps for 1080p120; scale linearly with res×fps for higher
modes. Manual override in client Settings → "Video bitrate".

### Codec preference

- **H.264** — most compatible, bigger bitstream
- **HEVC** — better quality at same bitrate, default for most server GPUs
- **AV1** — best compression, requires AV1-capable encoder (RTX 40-series
  or AMD RX 7000 / Intel Arc) AND AV1-capable decoder (libdav1d on client
  side handles this even on older CPUs, just slower)

VipleStream's client picks the most compressed codec the server-client
pair both support — if AV1 isn't an option it falls back to HEVC, then
H.264.

### Frame pacing

Default OFF. In client Settings → "Advanced" → "Frame pacing" / "Vsync".
Enable if you have visible frame stutter; disable if you want lowest
input latency.

---

## Per-platform notes

### Windows host

- Sunshine config lives at `C:\Program Files\VipleStream-Server\config\sunshine.conf`.
- Service name is `VipleStreamServer` (not the upstream `SunshineService`).
- Default install does NOT auto-flip the host display to client-requested
  resolution. Hosts with a physical / VDD monitor locked at 1080p60 will
  render the client at 1080p60 even if the client requests 1440p120 —
  configure VDD or use a higher-res dummy plug if you need 1440p+.
- Adaptive bitrate (AIMD): if the client reports packet loss > 2%, server
  reduces bitrate by 25% per second until loss clears, then ramps back.
  Override in `sunshine.conf` with `adaptive_bitrate_disabled=true`.

#### Steam Controller 虛擬裝置（§SC-HID）：host 端安裝／驗證

client 端接的實體 Steam Controller（USB 直連 PID `0x1302`，或經 Puck 無線接收器
PID `0x1304`）會以原生 HID 轉發到 host；host 端由一個 UMDF2 虛擬 HID 裝置
（root 節點 `ROOT\VID_28DE&PID_1302`、子節點 `HID\VID_28DE&PID_1302`）把它呈現給
host 的 Steam，所以 Steam 看到的是真的「Steam Controller」而不是模擬的 Xbox 手把。
串流本身**不**依賴這個 driver；裝不上只會退回 ViGEm X360。

**隨 server zip 出貨的 5 個檔（都在 exe 同目錄；`build_sunshine.cmd` 缺任一檔即中止打包）：**
`VipleSCHid_Driver.dll`、`VipleSCHid.inf`、`VipleSCHid.cat`、`VipleSCHid_SelfSign.cer`、
`Install-VipleSCHid.ps1`。

**信任面（`.cer`）：** driver DLL／CAT 以自簽憑證簽署，Windows 只有在該憑證同時在
`Cert:\LocalMachine\Root`（自簽＝鏈的根，缺了鏈就不可信）與
`Cert:\LocalMachine\TrustedPublisher`（發行者信任，`pnputil /add-driver` 非互動安裝
才不會跳「是否信任此發行者」而失敗）兩個電腦層級店時才會接受這個套件。Install 腳本
會把隨包的 `.cer` 匯入這兩個店，**匯入後回讀店內確認 thumbprint 真的存在**才算成功；
`-Reinstall`（含 gentle 模式的自動升級）只在「憑證 thumbprint 此刻確實在店內」時才會
刪舊套件——`.cer` 檔存在但匯入失敗不放行，避免刪了舊 driver 卻裝不回。這是把一張
自簽根憑證裝進 host 的電腦信任根，只該在你自己的 host 上做。

**安裝／重裝（系統管理員 PowerShell；Windows PowerShell 5.1 或 PowerShell 7 皆可）：**

```powershell
cd "C:\Program Files\VipleStream-Server"
# gentle：沒有健康節點才裝；節點健康但綁定版本 != 包內 INF DriverVer 時自動升級成 -Reinstall
powershell -ExecutionPolicy Bypass -File .\Install-VipleSCHid.ps1
# 強制：移除節點 + 舊 driver 套件後乾淨重綁（driver 改版後用這個）
powershell -ExecutionPolicy Bypass -File .\Install-VipleSCHid.ps1 -Reinstall
# 唯讀診斷：模擬 server 的 openHidByVidPid，看虛擬裝置能否以 R+W 開啟
powershell -ExecutionPolicy Bypass -File .\Install-VipleSCHid.ps1 -Diag
```

最後一行 `RESULT:{json}` 是機器可讀結果：`ok`／`bound`（function driver 真的是
`MsHidUmdf`，不只是 Status=OK）／`driverVersion`（節點實際綁定的 DriverVer）／
`infVersion`（包內 INF 的 DriverVer）／`versionMatch`／`autoReinstall`／`refused`。
**通過 = `ok=true, bound=true, versionMatch=true`。** `-Reinstall` 在「簽章憑證的
thumbprint 此刻不在 Root + TrustedPublisher」時會拒絕（exit 3、`refused=true`）並保留
舊套件，避免刪了舊 driver 卻裝不回（見上面的信任面說明）。

**部署自動化：** host worker 的 `deploy_from_release.ps1` 在停服務、覆蓋檔案之後會
自動跑 gentle 模式（`-ForceDriverReinstall` 則跑 `-Reinstall`）；覆蓋前把既有 driver 檔
備份到 `sc_hid_driver_prev\<既有 INF DriverVer>\`（子目錄先清空；INF 缺或解析不到就是
`unknown\`），結果附錄在 `config\schid-deploy.log`。Install 以獨立 `powershell.exe` 程序跑、
180 s 逾時強制結束；driver 失敗／逾時只印 `DRIVER ENSURE FAILED`，服務啟動放在
`finally`，**不論覆蓋或 driver 步驟成敗都會被拉起**。**只複製檔案不會讓新 driver 生效**
（`pnputil /add-driver` 對同 DriverVer 靜默跳過、PnP 不替健康節點換 driver、server
的 `tryInstallDriver()` 只在開不到裝置時才跑），所以 driver 改版一定要 bump INF
`DriverVer` 並走這條路。

**取證流程（win-builder 透過 CoworkMCP，零 SSH）：** 兩支診斷腳本在
`Sunshine\src\platform\windows\sc_hid_driver\diag\`（隨 repo 版控；`scripts\` 是本機
gitignore，別放那裡）。

1. host 端健檢：把 `Sunshine\src\platform\windows\sc_hid_driver\diag\sc_hid_host_check.ps1`
   放上黑板 `scripts`（`bb_write` + `sync-scripts`）後發 run-script 任務。**服務 Running
   時腳本預設自動退為 `-NoFeature`**（feature 探測會 SET 0x01 到虛擬裝置，跟進行中的
   Steam 握手搶同一個一次性回應暫存器；warning 會同時印 sunshine.log 看到的 poll thread
   狀態供判斷）；串流中就照預設（或明講 `noFeature`），部署後確認沒有串流在跑才帶
   `forceFeature` 驗 feature 通道。參數可放 `args.*` 或 spec 頂層：
   ```powershell
   . scripts\cowork_rpc.ps1; Cowork-Init
   # 串流中（只看裝置／節點／log／Steam log，不碰 feature 通道）
   $t = Cowork-RunTask -Type run-script -Role viplestream-host `
          -Spec @{ op = 'run-script'; script = 'sc_hid_host_check.ps1'; args = @{ noFeature = $true } }
   # 部署後、沒有串流在跑：驗 feature 通道（GATE-OK）
   $t = Cowork-RunTask -Type run-script -Role viplestream-host `
          -Spec @{ op = 'run-script'; script = 'sc_hid_host_check.ps1'; args = @{ forceFeature = $true } }
   ```
   讀輸出末行 `RESULT:{json}`：`deviceFound`／`bound`／`driverVersion`／`versionMatch`／
   `driverStats`（Feature 0x05 解碼：`drvSet`／`drvGet01`／`pending`／`ready`／`delivered`／
   `gated`／`zeroDropped`／`drvVer`，driver < 1.0.5.0 不宣告 0x05 → `supported=false`）／
   `feature.verdict`：
   - `GATE-OK`＝driver ≥ 1.0.5.0 語義正常（SET 事件進 ring、pending 期間 GET 0x01 回 err 31）；
   - `GATE-OK-RESPONSE-DELIVERED`＝同上且輪詢期間看到 err→ok 轉折（有 server + client 把回應交付了）；
   - `GATE-OK-BUT-NO-SET-EVENT`＝GET 被閘控但 ring 沒有 SET 事件、服務又沒在跑 → driver 的事件 ring 有問題；
   - `GATE-SEEN-EVENT-CONSUMED`＝閘控看到了、事件被在跑的 server poll thread 先 pop 走（正常）；
   - `OLD-DRIVER-ECHO`＝舊 driver 的 GET 回呼叫者原 buffer（RC2b）；`RING-OK-NO-GATE`／`UNCLEAR` 看原始行。

   另看 `sunshineLogState`（`ok`／`missing`／`unreadable: …`，區分「讀不到 log」與「有 log 但無
   `[SC-HID]` 行」）／`steamReadFailure`／`steamZombie`（host Steam 握手失敗特徵計數，應為 0）。
2. host server log：`collect(what=["log-grep"], pattern="\\[SC-HID\\]", tail=20000, max=400)`。
3. host Steam log：`collect(what=["log-grep"], log="C:\\Program Files (x86)\\Steam\\logs\\controller.txt",
   pattern="(?i)28de|1302|steam controller|GetControllerInfo|Read failure|couldn't get|firmware|disconnect")`。
4. client 端：`Sunshine\src\platform\windows\sc_hid_driver\diag\sc_hid_probe.ps1 -Seconds 8 -LegacyCheck`
   （列舉 0x28DE 介面／report 佈局／N 秒 report id 直方圖／SET→1 ms 輪詢 GET 的 feature
   時序），串流 log 看 `[SC-HID] rx stats(final)` 與 `Feature req … lat=`（tag 字典見
   `docs/log_tags.md`）。

**通過門檻（依序；任一失敗就停在該層回報）：**

| 門檻 | 看什麼 | 通過條件 |
|---|---|---|
| G0 裝置 | host_check `bound`／`driverVersion`；sunshine.log `Polling thread started, handle open` | driver 版本 = 包內 INF、無 `device not found` |
| G1 client 收到 | client `[SC-HID] rx stats(final)` | `id42 ≥ 500`（Puck slot 約 266 Hz） |
| G2 client 轉發 | 同上 `fwd` | `fwd ≈ id42` |
| G3 host 注入 | host `[SC-HID] write stats(10s)` 的 **per-session 差值欄位** | `okS − keepalive ≥ 500`（`okS`＝本 session 寫入成功數；`ok` 是 process 級累計、跨 session 不歸零，不能拿來減）、`skipped=0`、`failed=0` |
| G4 握手 | host `drvSet>0`（Feature 0x05 統計）+ 成對的 host `Steam feature → client`（input 路徑 `Steam feature op → client` 或 poll thread `Poll-thread: Steam feature → client`，grep pattern `Steam feature (op )?→ client`）／ client `Feature req … lat=ms` ／ host `Feature response delivered`；host Steam log | 不再每秒 `Read failure`、`Firmware Version` 非 0、無 zombie 重開 |
| G5 使用者可見 | host Steam 設定 → 控制器；遊戲內 | 顯示「Steam Controller」、按鍵／搖桿／觸控板有反應、閒置 90 s 不掉 |

注意：client 本機若有 Steam 常駐，它會跟轉發鏈搶實體控制器的**一次性** feature 回應暫存器
（實測 report id／頻率／時序不受影響，只是多一個競爭者）；握手成功率明顯偏低時，先退出
client 本機的 Steam 再試。

### Android client

- Android 12+ themed icon: VipleStream's "V" mark renders as a
  monochrome silhouette when "Themed icons" is enabled in Pixel
  Launcher / system Settings. Default style (off) shows the full-color
  adaptive icon.
- Minimum supported: Android 7.0 (Nougat). Vulkan FRUC backend requires
  Android 9+ with Vulkan 1.1 (driver-dependent).
- `debug.viplestream.vkprobe=1` system property opts into the Vulkan
  FRUC backend (default GLES). Set via `adb shell setprop`.

### Desktop client (Windows / Linux / macOS)

- Settings stored in `HKCU\Software\VipleStream\VipleStream` (Windows),
  `~/.config/VipleStream/VipleStream.conf` (Linux), `~/Library/Preferences/com.viplestream.VipleStream.plist` (macOS).
- Default renderer is `RS_D3D11` on Windows. RS_VULKAN is opt-in via
  Settings dropdown (currently labeled "experimental"; production
  default flips to Vulkan in TODO §J.5).

#### Steam Controller passthrough on Linux (§SC-HID)

The desktop client forwards a physical Steam Controller's raw HID stream
to the host so the host's Steam sees a real Steam Controller (instead of
an emulated Xbox pad). The code ships enabled on Linux, but reading the
controller needs `/dev/hidraw*` access, which is root-only by default:

1. If Steam (or your distro's `steam-devices` package) is installed on
   the client machine, its udev rules already grant access — nothing to do.
2. Otherwise install our rule and replug the controller:

   ```
   sudo cp moonlight-qt/scripts/linux/60-viplestream-steam-controller.rules /etc/udev/rules.d/
   sudo udevadm control --reload-rules && sudo udevadm trigger
   ```

Diagnosis: start a stream and check the client log — `[SC-HID] Opened N
Steam Controller vendor interface(s)` means passthrough is active;
`Found N ... but could not open any` means the udev rule is missing.
Note: do not run Steam Input against the same controller on the client
machine while streaming — it reconfigures the device and interferes with
passthrough.

---

## Where to file issues

GitHub Issues at `https://github.com/finaltwinsen/VipleStream`. Include:

- Server version (Web UI About page)
- Client version (Settings → About)
- The actual sunshine.log slice for the affected stream session, if
  server-side
- The actual VipleStream client log slice + Windows event log entry, if
  client-side

Logs:
- Windows server: `C:\Program Files\VipleStream-Server\config\sunshine.log`
- Windows client: `%TEMP%\VipleStream-*.log` (latest by mtime)
- Android client: `adb logcat -d | grep -E 'com.piinsta|VKBE|VIPLE-'` from
  a connected device

---

## See also

- [`docs/building.md`](./building.md) — building from source (server + client + android)
- [`docs/versioning.md`](./versioning.md) — version number policy
- [`docs/fruc_backends.md`](./fruc_backends.md) — FRUC backend internals
- [`docs/TODO.md`](./TODO.md) — open items + technical debt + won't-fix list
