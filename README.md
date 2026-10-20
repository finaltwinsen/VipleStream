# VipleStream

A self-hosted game-streaming stack — a fork of [Sunshine](https://github.com/LizardByte/Sunshine) (host) and [Moonlight](https://github.com/moonlight-stream) (clients) with built-in NAT traversal, AI frame interpolation, Steam library auto-import, and a Traditional Chinese UI. Wire-protocol-compatible with vanilla Sunshine / Moonlight so VipleStream and upstream installs interoperate.

> **Current version:** see the [latest release](https://github.com/finaltwinsen/VipleStream/releases/latest). Releases ship the Windows client and server, the Android APK, the Linux x64 client and server, and (from 2.0.0) the Steam Frame headset client at one shared version number.

Project home: <https://github.com/finaltwinsen/VipleStream>

---

## Components

| Component | Replaces | Platform | Release asset → binary | Install location |
|---|---|---|---|---|
| **VipleStream-Server** | Sunshine | Windows x64 | `VipleStream-Server-X.Y.Z.zip` → `viplestream-server.exe` + `viplestream-svc.exe` | `C:\Program Files\VipleStream-Server\` (service `VipleStreamServer`) |
| | | Linux x64 | `VipleStream-Server-X.Y.Z-linux-x64.deb` → `viplestream-server` | `/usr/bin/` (systemd user service) |
| **VipleStream** (PC client) | Moonlight-Qt | Windows x64 | `VipleStream-Client-X.Y.Z.zip` → `VipleStream.exe` | `C:\Program Files\Moonlight Game Streaming\` |
| | | Linux x64 | `VipleStream-Client-X.Y.Z-linux-x64.AppImage` | portable, no install step |
| **VipleStream Android** | Moonlight-Android | Android | `VipleStream-Android-X.Y.Z.apk` (application id `com.piinsta`) | sideload only |
| **VipleStream for Steam Frame** | — | Steam Frame headset (Linux arm64) | `VipleStream-Client-X.Y.Z-linux-arm64.flatpak` (app id `io.github.finaltwinsen.VipleStream`) + `VipleStream-SteamFrame-Install-X.Y.Z.sh` | Flatpak, per-user, installed by the script |
| **viplestream-relay** | — | any (Python 3.10+) | `tools/viplestream-relay/relay_server.py` (source only, not a release asset) | any always-on host |

The wire protocol on `_nvstream._tcp` mDNS, `/serverinfo` `/launch` `/applist`, RTSP, and the client cert chain are unchanged from upstream. A VipleStream client can connect to a vanilla Sunshine host, and a vanilla Moonlight client can connect to a VipleStream-Server host — VipleStream-only features (Steam profile dropdown, FRUC backends, etc.) are quietly hidden when the peer doesn't advertise the `<VipleStreamProtocol>` capability marker.

---

## Install

Download everything from the [latest release](https://github.com/finaltwinsen/VipleStream/releases/latest). The host runs the server; every device you stream to runs a client. All pieces carry the same version number — update them together.

### Host (the PC you stream from)

**Windows** — Windows 10/11 with an NVIDIA, AMD or Intel GPU that has a hardware video encoder.

1. Extract `VipleStream-Server-X.Y.Z.zip` to `C:\Program Files\VipleStream-Server\`.
2. In an **Administrator** PowerShell, register the service and open the firewall:
   ```powershell
   cd "C:\Program Files\VipleStream-Server"
   New-Item -ItemType Directory tools -Force | Out-Null
   Copy-Item viplestream-svc.exe tools\      # the service wrapper must live one folder below the install dir
   New-Service -Name VipleStreamServer -DisplayName "VipleStream Server" -StartupType Automatic `
     -BinaryPathName '"C:\Program Files\VipleStream-Server\tools\viplestream-svc.exe"'
   New-NetFirewallRule -DisplayName "VipleStream Server" -Direction Inbound -Action Allow `
     -Program "C:\Program Files\VipleStream-Server\viplestream-server.exe"
   Start-Service VipleStreamServer
   ```
3. Open `https://localhost:47990` (accept the self-signed certificate warning) and create the Web UI login. Settings are saved to `config\sunshine.conf`.
4. *Optional* — Steam Controller passthrough: run `.\Install-VipleSCHid.ps1` from the same Administrator PowerShell.
5. *For VR on Steam Frame* — install Steam and SteamVR on the host and keep a user signed in to Windows (a locked or signed-out desktop streams a solid colour). The server deploys and registers its own SteamVR driver when a VR session starts; you don't install it by hand. **2.0.0 only:** add `vr_pcvr = enabled` to `config\sunshine.conf` and restart the service — 2.0.0 shipped with VR turned off by default; later versions turn it on.

Updating: right-click the tray icon → **Check for updates...**, or stop the service, extract the new zip over the old one, and start it again. `VipleStream-Drivers-X.Y.Z-windows.zip` holds the same two drivers as the server zip, for reinstalling or repairing them on their own (see its `README.txt`).

**Linux** — Ubuntu 26.04 (x64). VR streaming is Windows-only.

```bash
sudo apt install ./VipleStream-Server-X.Y.Z-linux-x64.deb
```

The package enables a systemd user service for the installing user, so the server starts at boot without a desktop login. The Web UI is at `https://localhost:47990`. On a headless GNOME/Wayland host set `capture = kms`.

### Clients

| Device | Install |
|---|---|
| Windows | Extract `VipleStream-Client-X.Y.Z.zip` anywhere and run `VipleStream.exe`. No installer or admin rights needed. |
| Linux x64 | `chmod +x VipleStream-Client-X.Y.Z-linux-x64.AppImage` and run it. |
| Android | Sideload `VipleStream-Android-X.Y.Z.apk` (file manager, or `adb install -r`). |
| Steam Frame | See below. |

**Steam Frame headset**

Use the installer script, not `flatpak install` alone. A plain Flatpak install only gets Desktop mode: Flatpak always runs apps in their own process-ID sandbox, and the headset's SteamVR can't work with an app inside one, so VipleStream turns VR and Video mode off. The installer adds a launcher that starts VipleStream without that one isolation (the rest of the Flatpak sandbox stays) and with access to SteamVR.

1. Download `VipleStream-Client-X.Y.Z-linux-arm64.flatpak` and `VipleStream-SteamFrame-Install-X.Y.Z.sh` into the same folder on the headset, and open a terminal there (desktop mode, or SSH with Developer Mode on).
2. Run the installer as your normal user (no `sudo`). The first install also downloads the KDE 6.11 runtime from Flathub:
   ```bash
   bash VipleStream-SteamFrame-Install-X.Y.Z.sh
   ```
   It installs the Flatpak, sets up the launcher, checks that the launcher really runs without the process-ID sandbox, and adds **VipleStream** to your Steam library (Steam must be running).
3. Launch **VipleStream** from your Steam library inside the headset. It opens on a virtual screen.

Updating: download both files of the new release and run the new installer the same way. To remove everything: `bash VipleStream-SteamFrame-Install-X.Y.Z.sh --uninstall`, then delete the shortcut from your Steam library.

---

## Using it

### Pair once

1. In the client, add the host: it usually shows up on its own on the same network; otherwise press **+** (*Add PC manually*) and type the host's IP address.
2. The client shows a 4-digit PIN. On the host's Web UI (`https://<host-ip>:47990`) open **PIN** and enter it.
3. The host appears with its apps. Installed Steam games are listed automatically.

Pairing is per device; each client pairs once.

### Stream a game or the desktop

Pick the host, then an app (**Desktop** streams the whole screen). Resolution, frame rate, bitrate, codec and frame interpolation (FRUC) are in **Settings**; the defaults are a good start. To end a session:

| Input | Exit |
|---|---|
| Keyboard | `Ctrl+Alt+Shift+Q` |
| Gamepad | hold `Start+Select+L1+R1` |
| VR controller (Steam Frame, desktop and VR mode) | hold `A+X+trigger+grip` (on the Frame controller A and X are both on the right hand; trigger and grip can be on either hand) |

The tip is also shown on the screen while the stream starts.

### On the Steam Frame

When VipleStream starts it asks for a mode (you can switch later from the top bar):

| Mode | What it does |
|---|---|
| **Desktop mode** | Your PC's desktop or a game on a large virtual screen, same as the PC client. |
| **VR mode** | Plays SteamVR games from your PC in the headset. Pick the host, then **SteamVR Home** or a VR game; the host starts SteamVR and the game. Before starting, the client checks the round trip to the host and waits if the connection is too slow for VR (for example, a VPN going through a relay). |
| **Video mode** | Plays videos in the headset — flat, 3D side-by-side / top-bottom, 180° and 360°. Sources: files on the headset, a URL, folders on a paired PC (the whole PC by default; limit it with `media_dirs`, or turn it off with `media_share = disabled` in the host's `sunshine.conf`), and YouTube and other sites through the built-in browser's **Play in VR**. |

Video mode controls: **A** play/pause · **B** exit · **trigger** show the control bar · **grip** recentre the screen · **stick left/right** skip ±10 s · **stick up/down** move the screen nearer/farther (handy when a VR180 video is shot too close).

To leave a desktop or VR stream, hold **A+X+trigger+grip**; you return to the VipleStream VR interface. Quitting the stream from the SteamVR quick menu also brings you back to VipleStream.

When you start a video or VR session, SteamVR may open the Steam menu over it — press the Steam button to dismiss it.

More detail: [`docs/setup_guide.md`](docs/setup_guide.md) (configuration and troubleshooting), [`docs/steam_frame_client.md`](docs/steam_frame_client.md) (headset client), [`docs/steamvr_driver.md`](docs/steamvr_driver.md) (VR on the host).

---

## Features beyond upstream

### NAT traversal (zero hardware cost)
Stream over the public internet with no port forwarding, UPnP, or VPN.

- **STUN prober** detects public IP + NAT type at startup
- **Relay signaling server** — lightweight Python WebSocket service, terminate TLS via Cloudflare Tunnel / nginx
- **HTTP proxy** — `/serverinfo` / `/launch` / `/cancel` go through the relay in relay-only mode
- **RTSP TCP tunnel** — wraps the 7-connection RTSP handshake (OPTIONS → DESCRIBE → SETUP×3 → ANNOUNCE → PLAY) over a single WebSocket
- **UDP hole-punch** — direct peer-to-peer UDP for video / audio / control after RTSP succeeds
- **PSK auth** — relay rejects any client whose `HMAC-SHA256(psk, uuid)[:16]` doesn't match
- Fully ported to the Android client (RelayClient + RelayTcpTunnel)

### MP-QUIC multi-path streaming
When the host advertises multiple reachable interfaces (e.g. Tailscale, Ethernet, WiFi), the client streams across all of them simultaneously for higher throughput and seamless failover when a single path degrades. Capability-negotiated — falls back to ENet transport against vanilla Moonlight / Sunshine.

### Frame Rate Up-Conversion (FRUC)
2× frame interpolation on the PC client. Two renderer paths, each with its own backend menu:

| Renderer | Backends | Notes |
|---|---|---|
| **Direct3D 11** (default) | Generic Compute / NVIDIA Optical Flow / DirectML RIFE / NCNN-Vulkan RIFE | Generic is the recommended low-latency default; NVOF needs RTX 20+; DirectML / NCNN run RIFE 4.25-lite on capable GPUs |
| **Vulkan** (experimental) | Built-in compute (Generic-equivalent) + Native RIFE β opt-in | Vulkan renderer bundles its own FRUC compute pipeline (9 shaders + 389-layer RIFE graph executor); Active / Passive mode toggle |

Android client uses its own Vulkan FRUC backend (AHardwareBuffer zero-copy import, smart-mode dual present 60→120 FPS, VK_GOOGLE_display_timing, SIGSEGV canary fallback to GLES on driver crash).

DirectML / NCNN diagnostics:
- `VIPLE_DIRECTML_DEBUG=1` / `VIPLE_DIRECTML_VERBOSE=1` — D3D12 debug layer + ORT VERBOSE log
- `VIPLE_DML_RES=540|720|1080|native` — caps DirectML tensor resolution
- `VIPLE_FRUC_MODEL=fp16|fp32|auto` — overrides model cascade order
- `[VIPLE-FRUC-DML]` / `[VIPLE-FRUC-NCNN]` / `[VIPLE-VKFRUC-Stats]` log lines print per-stage timings

### Steam library auto-import (host)
Server scans the local Steam install at startup and auto-injects every installed game into `/applist` as a launchable app. No manual configuration — click `Counter-Strike 2` in the client and the host runs `steam://rungameid/730`.

- Reads `loginusers.vdf` for accounts, `libraryfolders.vdf` for install paths, each `appmanifest_*.acf` for per-app metadata
- Per-app `<Source>` `<SteamAppId>` `<SteamOwners>` `<LastPlayed>` `<Playtime>` XML tags fed back to clients
- Clients sort by `RECENT` / `PLAYTIME` / `NAME` (manual entries still pinned to top)

### Steam account switch (client → host)
A `STEAM ACCOUNT` dropdown sits above the apps grid (PC + Android). Pick a different account → host runs `steam.exe -shutdown` then `-login <account>`. Live progress (`Asking Steam to shut down…` → `Logging in as XXX…`) shown in a busy overlay.

- Only accounts with `RememberPassword=1` are switchable; others rejected client-side
- Server-side switch is **async** (returns `202 + task_id` in <100 ms) — client polls `/steamswitch/status` every ~1 s until terminal state, so the long-running switch never starves `/serverinfo` polls or causes spurious "host disconnected" UI
- Force-kills straggler `steam.exe` / `steamwebhelper.exe` before re-login (otherwise the new `-login` gets intercepted by Steam's stuck login window and silently no-ops)
- Detects Steam Guard 2FA prompts and surfaces a specific error
- 60 s task GC keeps the registry small

### HDR support (Android)
End-to-end HDR pipeline on Android Vulkan FRUC backend:
- `VK_EXT_swapchain_colorspace` (instance) + `VK_EXT_hdr_metadata` (device) — capability-gated by user opt-in (`Settings → Enable HDR`) + driver advertise
- HDR10 swapchain — `VK_FORMAT_A2B10G10R10_UNORM_PACK32` + `VK_COLOR_SPACE_HDR10_ST2084_EXT` when capable
- `vkSetHdrMetadataEXT` — BT.2020 primaries, 1000 nits MaxCLL, 400 nits MaxFALL
- Fragment shader sRGB → linear → ST.2084 PQ encoding for HDR10 swapchain on SDR content (BT.2408 100-nit reference white)
- Three-tier safety: user opt-in + capability probe + SIGSEGV canary fallback (Pixel 9 Mali-G715 driver bug history)

PC client HDR is upstream Moonlight-Qt's existing HDR path (Sunshine `hdrMode=1` launch param + MediaCodec / D3D11 HDR colorspace negotiation).

### Other
- Single-instance guard on PC client — second launch shows a notice instead of silently fighting over QUIC ports / server session
- Traditional Chinese UI translation (`qml_zh_TW.ts`) on PC client; Android pulls system locale
- Web UI English + Traditional Chinese strings rebranded to VipleStream
- Editorial-style PcView / AppView design with IBM Plex Mono + Space Grotesk + lime accent (`#D4FF3A`)
- Performance overlay (Android v1.2.184+) — lime-on-ink2 monospace, PC §05 NET HUD style, FRUC stats (interpolated count, dual-mode ratio, effective output FPS)
- `/serverinfo` advertises `<VipleStreamProtocol>` capability marker so clients can hide VipleStream-only features when connected to vanilla Sunshine
- Adaptive bitrate, network-status overlay (LAN / Relay / DERP), debug pairing tool (ADB-based PIN-free)
- Status log to `Downloads/viple_vkbe_status.log.txt` on Android — adb-less diagnostic for Pixel 9 / locked-down devices

---

## Architecture

```
┌──────────────────────┐        relay or direct       ┌──────────────────────┐
│  VipleStream client  │ ◄──────────────────────────► │  VipleStream-Server  │
│  (PC / Android)      │                              │  (Windows / Linux)   │
│                      │   1. STUN → public IP        │                      │
│  RelayClient.java    │   2. relay lookup            │  stun.cpp            │
│  RelayTcpTunnel.java │   3. HTTP proxy /launch      │  relay.cpp           │
│  relaytcptunnel.cpp  │   4. TCP tunnel (RTSP)       │  rtsp.cpp            │
│  relaylookup.cpp     │   5. UDP hole-punch          │  stream.cpp          │
└──────────────────────┘                              └──────────────────────┘
                                      ▲
                                      │ WebSocket (ws:// or wss://)
                              ┌───────┴────────┐
                              │  Relay server  │
                              │ relay_server.py│
                              │  (Python 3.10+)│
                              └────────────────┘
```

---

## Security notes

- **PSK auth** — every relay WebSocket connection must present `HMAC-SHA256(psk, uuid)[:16]`; without the right PSK the relay rejects registration.
- **No credentials in source** — PSK + relay URL live in `build-config.local.cmd` or app preferences.
- **Trust-all SSL on Android relay** — the relay WebSocket uses a trust-all `SSLContext` because TLS is terminated by a reverse proxy. The Moonlight ↔ Sunshine game-streaming channel still uses upstream Moonlight's certificate pinning — that part is unchanged.
- **Sunshine pairing unchanged** — the PIN pairing flow and client cert trust chain are untouched.
- **Steam credentials** — the host never sees the Steam password; the host only invokes `steam.exe -login <account>`, which uses Steam's own RememberPassword cookie. Accounts that haven't been signed in once on the host are rejected with HTTP 409.

---

## Version highlights

| Version | Changes |
|---|---|
| **2.0.9** | **頭盔全 VR 介面**：Steam Frame 上的 VipleStream 整個在 VR 裡操作（不再用 SteamVR 劇院裡的平面視窗）：雷射指標點選、扳機＝左鍵、握把＝右鍵、B＝返回、虛擬鍵盤；影片模式在同一個 VR 畫面裡播；VR 串流與桌面模式另開串流程式、進出時換手，不會跳出 SteamVR 介面（桌面模式改用 XR 虛擬螢幕，打字改用虛擬鍵盤）。**VR 控制器結束串流**：按住 A+X+扳機+握把；從 SteamVR 快速選單結束串流也會回到 VipleStream。啟動轉場的提示加上 VR 控制器的按法。**離開區網也連得到主機**：host 在主機資訊裡附上自己的 Tailscale 位址，client 記下來當最後一個嘗試的位址。Steam 收藏庫加上圖示與封面。 |
| **2.0.2** | **Steam Frame 安裝腳本**：2.0.0／2.0.1 的頭盔版照說明只裝 `.flatpak`，Flatpak 一定開獨立的 PID 沙盒、頭盔的 SteamVR 對不上，程式會自己停用 VR 模式與影片模式，**只剩桌面模式能用**。新增 asset `VipleStream-SteamFrame-Install-X.Y.Z.sh`：安裝 Flatpak、裝一個只拿掉 PID 隔離（Flatpak 其他隔離不變）並開放 SteamVR 路徑的啟動器、自我檢查後加進 Steam 收藏庫；`--uninstall` 移除。停用時的提示改成指向這個捷徑。README 的頭盔安裝步驟改用腳本。 |
| **2.0.1** | **VR 串流預設開啟**：host 的 `vr_pcvr` 預設改成 `enabled`（2.0.0 漏改、出貨仍是 `disabled`，頭盔的 VR 模式要手動開）；對平面串流沒有影響，VR app 只列給 VR client、SteamVR driver 只在 VR session 開始時才部署，沒裝 SteamVR 的 host 不會宣告 VR。README 加安裝與使用指引；修正 `docs/setup_guide.md` 的 Windows zip 安裝步驟（服務程式要放 `tools\`、要自己加防火牆規則）。 |
| **2.0.0** | **Steam Frame 頭盔 client（第六件 asset：`-linux-arm64.flatpak`）**：頭盔上的 VipleStream 可以 VR 串流 PC 的 SteamVR 遊戲（host 自帶 SteamVR driver `driver_viplestream`，`vr_pcvr = enabled` 開啟；多連線＋補包跟著往返時間走、姿態預測、半速＋補幀、注視點編碼、進 VR 前量往返時間），也有**頭盔影片模式**（本機／網址／配對 PC 的磁碟機／YouTube 與內建瀏覽器；2D／3D／180°／360°；手把 A／B／扳機／握把、搖桿調遠近）。修正 server HTTPS 在 client 無聲斷線後整個卡死（阻塞式 TLS shutdown）。另附 `VipleStream-Drivers-2.0.0-windows.zip`（VipleSCHid＋SteamVR driver 獨立包）；同時首度發佈 1.5.276（§SELF-UPDATE、§UPDATE-HEAD）。 |
| **1.5.275** | **§S.19-INIT-FIX 永久凍結修復（host）**：IDR cooldown 初值 `steady_clock::time_point::min()` 相減溢位→「cooldown active」永遠成立；§K.14 在 frame 16 掩蓋，但 encoder 重建（顯示拓樸切換）後 frame_nr 已過 16 → 該 encoder 生命期內 client 的每一次 IDR 請求都被壓制（log：每個 session 的 f=1 都 IDR-SUPPRESS、事故 14 秒 22 次 SUPPRESS／0 次 EMIT），網路閃斷 3 秒後畫面永久凍結直到重連。初值改「一個 cooldown 之前」、比較式改不溢位；刻意不補發被壓掉的請求（client 每 ≤1 s 自行重送，補發會在每次復原後多一張糊 IDR）。host 假 ARP 黑洞 3 秒重現：修前 14 秒不復原，修後鏈路恢復 28 ms 即 IDR-EMIT。**鍵鼠斷續診斷儀器**：client 主迴圈 watchdog `[VIPLE-INPUT-STALL]`（停頓 ≥50 ms 且期間有使用者輸入才記，≥300 ms 抄主執行緒堆疊）、host `[VIPLE-INPUT-GAP]`／`[VIPLE-INPUT-QUEUE-LAG]`／`[VIPLE-INPUT-SENDINPUT-SLOW]`、`§S.19 cooldown armed` 行。**§RSHIFT-DUP-FIX**：IME Right Shift 補償沒排除本事件就是 RShift → host 收到 DOWN,DOWN,UP,UP、中文輸入法切換兩次等於沒切。**§SC-HID-LOG-RATE**：控制器休眠時「Feature SET failed」曾佔 client log 92% 撞 10 MB 上限，改前 8 筆＋每 30 秒一筆。本版同時首度發佈 1.5.272（§K.dd.select 顯示器選擇修復＋§CFG.defer 設定警告救回）、1.5.273（§HID-PROBE 卡死 HID 裝置啟動防護＋§UPNP-DIAG）、1.5.274（§SC-THREAD-OWNER SC-HID 執行緒所有權＋SDL 忽略清單＋連結警告清零）；五件同版號（Linux 兩件自 1.5.270 後首次重建）。 |
| **1.5.271** | Android client 的 §SC-HID Steam Controller 轉發對齊桌面修法（USB-C 直連／Puck、Nereid 接收器經 OTG）：`0x42` 改標轉發、feature 查詢改 SET 後 1 ms 輪詢 GET＋型別比對＋快取、不回零、`rx stats`／`Feature req` 可觀測 log、活躍介面優先。**藍牙 SC 在 Android 屬平台限制**（系統獨佔 HID over GATT、描述符無 gamepad，Pixel 5 實測只建出 KEYBOARD|MOUSE）——client 偵測到 `0x28DE:0x1303` 時以 toast 明講改用 USB-C／OTG。本版只發 Windows Client／Server＋Android 三件（Windows 重 stamp；Linux 兩件無改動、沿用 1.5.270）。 |
| **1.5.270** | §SC-HID Steam Controller 原生轉發全傳輸修復（Puck 無線接收器／藍牙／USB 直連，Windows 與 Linux client）。實測定案四根因：client 只放行 0x45 而 Puck/USB 吐的是 `0x42`（同佈局，改標轉發）；feature 回應是一次性暫存器（SET 後 13～21 ms 內要輪詢 GET，舊碼必失）；**host UMDF driver 的 feature IOCTL buffer 慣例誤解**（GET/SET handler 從未執行，Steam 握手 100% 失敗、每 12 秒 zombie 重開——USB 直連時同樣壞，v1.5.263 的「已辨識」只是 path 身分）；部署鏈從不重裝 driver。driver 1.0.5.0：handler 依 vhidmini2 UMDF2 慣例重寫、GET(0x01) 閘控模擬實體一次性語義（400 ms 逾時）、0x42 通道、Feature 0x05 統計；client：Valve report id 家族、feature 1 ms 輪詢＋型別比對、rx/feature 統計 log、Linux hidraw 後備（Ubuntu 26.04 的 SDL 2.32 不回報 usage）；server：per-session 統計、driver 統計；部署：Install 腳本版本不符自動 -Reinstall（以 DLL 簽章者為信任依據）、deploy 停服務後重裝、zip 補 .cer、`sc_hid_driver/diag/` 兩支診斷腳本。實測：Windows Puck 6 分鐘 87,688 筆 0x42 零丟、host Steam 取得真實序號／韌體、零 Read failure；Linux 藍牙與 USB 直連皆通。 |
| **1.5.269** | §P1-PACE 幀節拍解除 V-Sync 綁定——上游把 frame pacing 綁死在 V-Sync 上，使用者為求低延遲關掉 V-Sync 就連帶失去唯一能吸收網路到達抖動的機制（log 實證「Frame pacing disabled: target 60 Hz with 60 FPS stream」）。代價是 LAN 1080p60 有 25-75% 的 5 秒視窗 p99 幀間隔 > 33ms（＝60Hz 掉兩幀），且限縮到「server 全速交付」的視窗仍有 42.8%。同 codebase 直接對照：179Hz/180fps 那台 `V-sync enabled` + `Frame pacing: target 180 Hz`（節拍開）卡頓 0%，60Hz/60fps 那台兩者皆關卡頓 25-75%。現改為兩者解耦：Pacer 照真實 vblank 發幀（順），Present 仍走 tearing 立即模式（不多等一次交換），串流快過螢幕時維持關閉。另附 client log 分析器 `scripts/benchmark/analyze_client_log.ps1` 與 log tag 字典 `docs/log_tags.md`；修正 `failover_flap_test.ps1` 因 §LOGHYG-NET10 改格式而失效的視訊中斷偵測（改用 NET10 節奏重建，實測消除 200s/32s/8s 三筆假陽性）。 |
| **1.5.263** | §Q-REGAIN MP-QUIC failover 後路徑自動回歸（standby 回歸 / INACTIVE 復活 / rejected 快取 TTL + 斷路器；server sticky 逃生門 + ABR floor 脫困 + CNXQ 暖機）——修有線 LAN 瞬斷後視訊困在 VPN 備援造成的間歇變糊；§SC-HID 虛擬裝置身分修復（`HID\VID_28DE&PID_1302` HWID/path + product string），host Steam 正式辨識為 Steam Controller，附 read-pipe keepalive 與 session 收尾崩潰修復；SC passthrough 對齊 Linux（hidraw udev rules）與 Android（USB 驅動 + feature tunnel）。五平台同版號釋出。 |
| **1.5.254** | §Q MP-QUIC 多路徑 failover/failback + Reed–Solomon FEC / jitter buffer 強化（控制通道故障改走 QUIC stream #0）；§FRUC-XPLAT 跨平台補幀（NVIDIA NVOF/RIFE、AMD RADV native RIFE、Android GLES compute）；§ABR 自適應位元率（AIMD + 方向性 IDR + ping-tick 零丟包回升）；§SC-HID Steam Controller HID 轉發；§K AMD VAAPI-VK / RADV 串流修復（nil-buffer 守門、綠線 plane layout、10-bit 黑畫面、Vulkan decode-queue probe）。五平台（Win Client/Server、Android、Linux Client AppImage / Server .deb）同版號釋出。 |
| **1.5.114** | 每台主機獨立「點擊直接進桌面」開關（PcView 右鍵選單）+ Send file to client picker 改在 user session 內展開（解 LocalSystem 跳 systemprofile\Desktop 警示窗 + cmd 黑窗）。 |
| **1.5.108** | MP-QUIC multi-path streaming + single-instance guard. Linux Client AppImage + Server `.deb` re-aligned in release. |
| **1.4.41** | Idle reconcile watchdog (auto-release owner lock after 60s); real zenity `fs_picker` on Linux; CJK font bundle path wired. |
| **1.4.40** | Vulkan+FRUC budget regression fix; multi-user ownership guards; in-stream bidirectional file transfer; quit-key release graceful exit. |
| **1.4.0** | Native RIFE Vulkan ships (Path β); Android FRUC port aligned to Windows; NVIDIA Optical Flow Vulkan extension; UI i18n × 27 locales. |
| **1.3.337** | First Linux ship — Server `.deb` + Client AppImage aligned in release; icon rebrand finalised; FRUC hotkey override fix. |
| **1.3.314–336** | FFmpeg 8.1 Vulkan HW decode (`h264_vulkan` / `hevc_vulkan` / `av1_vulkan`); Android async-compute FRUC stable on Pixel 5. |
| **1.3.311–312** | DirectML ONNX models moved out of release zip to on-demand fetch (`%LOCALAPPDATA%\VipleStream\fruc_models\`). |
| **1.3.295–310** | NVIDIA Aftermath GPU crash decoder; Sunshine Web UI rebrand + 19-locale i18n; SSH one-tap server deploy. |
| **1.3.250–294** | Native `VK_KHR_video_decode` integration (H.265 / H.264 / AV1); 84 fps PARALLEL stable / 5000+ frames zero device-lost. |
| **1.3.40–249** | NCNN-Vulkan FRUC backend (cross-vendor RIFE) with custom `rife.Warp` layer. |
| **1.2.186–189** | HDR end-to-end pipeline on Android (HDR10 swapchain + BT.2020 metadata + ST.2084 PQ encoding). |
| **1.2.185** | SIGSEGV canary fallback — driver-crash auto fall through to GLES on next launch. |
| **1.2.165–183** | Android Vulkan FRUC §I.C/D — smart-mode dual present 60→120 FPS + Pixel 9 Mali-G715 driver compatibility chain. |
| **1.2.150–164** | Android Vulkan FRUC §I.B — AHardwareBuffer zero-copy import. |
| **1.2.124–149** | DirectML cascade rewrite — native-res model variant cascade (fp16 → fp32 → Generic). |
| **1.2.108–123** | Steam profile dropdown on PC + Android; Steam-switch async rewrite eliminates "host disconnected" during 9s restart. |
| **1.2.93–96** | VipleStream rebrand — Sunshine→VipleStream-Server, Moonlight-Qt→VipleStream, Android `com.piinsta`; capability marker added. |
| **1.2.59–91** | DirectML RIFE backend ships with auto-cascade probe; reset-race fix takes submitFrame 50ms → 3ms. |
| **1.2.67** | Steam library auto-import (Phase 2) — host scans Steam install + injects games into `/applist`. |
| **1.1.60–80** | Full NAT traversal (PC + Android) — relay lookup, HTTP proxy, RTSP TCP tunnel, UDP hole-punch. |
| **1.1.30s–50s** | Initial VipleStream fork; Traditional Chinese UI; NVIDIA Optical Flow FRUC; presets + adaptive bitrate. |

Full per-version notes: `git log --oneline`.

---

## License

VipleStream is licensed under **GPL-3.0** ([LICENSE](LICENSE)) — the same license as its upstream components, as required for derivative works:

- [Sunshine](https://github.com/LizardByte/Sunshine) — GPL-3.0
- [Moonlight](https://github.com/moonlight-stream/moonlight-qt) — GPL-3.0
- [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c) — LGPL-3.0

The `moonlight-common-c` portions remain under LGPL-3.0; everything else (server, Qt client, Android client, build scripts, relay server) is GPL-3.0. Per GPL-3.0 you may run, study, share, and modify the software, including for commercial use, provided derivative works are distributed under the same terms with source code available.
