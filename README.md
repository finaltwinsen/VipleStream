# VipleStream

**English** · [繁體中文](#繁體中文)

A self-hosted game-streaming stack for Windows, Linux, Android and the **Steam Frame** headset. It is a fork of [Sunshine](https://github.com/LizardByte/Sunshine) (host) and [Moonlight](https://github.com/moonlight-stream) (clients), extended with PC VR streaming to the Steam Frame, internet play without port forwarding, multi-path networking, frame interpolation and Steam integration. It stays wire-compatible with upstream Sunshine and Moonlight.

> **Download:** [latest release](https://github.com/finaltwinsen/VipleStream/releases/latest). Every release ships all parts at one version number; update them together.

**Contents:** [Comparison](#comparison) · [Components](#components) · [Install](#install) · [Using it](#using-it) · [Playing from outside](#playing-from-outside) · [Features](#features-beyond-upstream) · [Architecture](#architecture) · [Security](#security-notes) · [Version highlights](#version-highlights) · [License](#license)

## Comparison

How VipleStream compares with Steam's built-in streaming (Steam Link / Remote Play, including PC VR streaming to the Steam Frame) and with upstream Sunshine + Moonlight. Steam changes often; the Steam column is based on Valve's published pages and release notes, not our own tests. (As of 2026-11-05.)

| | VipleStream | Steam built-in | Sunshine + Moonlight |
|---|---|---|---|
| **Source / license** | Open source, GPL-3.0, self-hosted | Proprietary, needs a Steam account | Open source, GPL-3.0 |
| **Host OS** | Windows (2D + VR), Linux (2D) | 2D: Windows, macOS, Linux<br>VR to Steam Frame: Windows (desktop Linux "not fully supported" yet) | Windows, Linux, macOS 15+ |
| **Clients** | Windows, Linux x64, Android 9+, Steam Frame | Steam Link app: Windows, macOS, Linux, Android (incl. TV), iOS / Apple TV / Vision Pro, Raspberry Pi, Meta Quest; Steam Frame | Many platforms (incl. iOS, Apple TV, Chromebook, Raspberry Pi) |
| **PC VR to Steam Frame** | ✓ own SteamVR driver | ✓ | — |
| **Eye-tracked foveated encoding** | ✓ | ✓ | — |
| **Streams any app or the whole desktop** | ✓ | ✓ | ✓ |
| **Internet play without port forwarding or VPN** | ✓ relay + UDP hole punching; Tailscale-aware | ✓ 2D (Remote Play); VR: home network only (no official internet VR) | Needs port forwarding, UPnP or a VPN |
| **Several network paths at once** | ✓ 2D: MP-QUIC over every interface; VR: one full copy per link | ✓ VR: adapter + Wi-Fi | — |
| **Adaptive bitrate** | ✓ delay trend + loss type | ✓ automatic bitrate selection | — (fixed bitrate) |
| **Frame interpolation on 2D clients** | ✓ Windows (D3D11 / Vulkan), Android | — | — |
| **Steam library auto-import, account switching** | ✓ | (native) | Manual app list |
| **Frame controllers as a gamepad in VR games that ignore VR controllers** | ✓ virtual Xbox 360 pad, chosen from the game's Steam store category, 2.0.68+ | ✓ generic gamepad, SteamVR 2.18+ (Oct 2026); Valve has not said how games are detected | — |
| **Video player in the headset (files, PC folders, URLs, YouTube; 2D/3D/180°/360°)** | ✓ | No built-in player documented for the Frame (SteamVR Media Player is a PC feature); third-party apps exist | — |
| **Low-battery warning in the headset while streaming** | ✓ icon + beep at 15 %, 2.0.70+ | No in-VR warning documented; battery % in the dashboard bar | — |
| **Steam Controller passthrough (native HID)** | ✓ | ✓ | — |
| **Works with upstream Moonlight / Sunshine** | ✓ | — | ✓ |
| **UI languages** | 30 (incl. Traditional Chinese) | 29 | Moonlight-Qt 30; Sunshine web UI 19 |

### Which one should I use?

- **Steam built-in** is the zero-setup choice when you only play Steam games at home and are happy with what it offers.
- **VipleStream** fits when you want to play from outside without opening ports, use several networks at once, stream to Android or Linux devices with frame interpolation, or watch videos in the headset. It also helps when you want to see and tune everything yourself (logs, host settings, source).
- **Sunshine + Moonlight** is the choice for the widest range of client devices (iOS, Apple TV, smart TVs). VipleStream clients and hosts interoperate with them.

## Components

| Component | Replaces | Platform | Release asset | Installed to |
|---|---|---|---|---|
| **VipleStream-Server** | Sunshine | Windows x64 | `VipleStream-Server-X.Y.Z.zip` | `C:\Program Files\VipleStream-Server\` (service `VipleStreamServer`) |
| | | Linux x64 (Ubuntu 26.04) | `VipleStream-Server-X.Y.Z-linux-x64.deb` | `/usr/bin/viplestream-server` (systemd user service) |
| **VipleStream** (PC client) | Moonlight-Qt | Windows x64 | `VipleStream-Client-X.Y.Z.zip` | any folder |
| | | Linux x64 | `VipleStream-Client-X.Y.Z-linux-x64.AppImage` | portable |
| **VipleStream Android** | Moonlight-Android | Android 9+ | `VipleStream-Android-X.Y.Z.apk` (`com.piinsta`) | sideload |
| **VipleStream for Steam Frame** | — | Steam Frame (Linux arm64) | `VipleStream-Client-X.Y.Z-linux-arm64.flatpak` + `VipleStream-SteamFrame-Install-X.Y.Z.sh` | per-user Flatpak |
| **Windows drivers** | — | Windows x64 | `VipleStream-Drivers-X.Y.Z-windows.zip` (same drivers as in the server zip) | for repair only |
| **viplestream-relay** | — | any, Python 3.10+ | `tools/viplestream-relay/relay_server.py` (source only) | an always-on machine |

The protocol (`_nvstream._tcp` mDNS, `/serverinfo`, `/launch`, `/applist`, RTSP, client certificates) is unchanged from upstream. VipleStream-only features are hidden automatically when the other side is plain Sunshine or Moonlight.

## Install

The **host** is the PC that runs your games; every device you play on is a **client**.

### Host on Windows

Windows 10/11 with an NVIDIA, AMD or Intel GPU that has a hardware video encoder.

1. Extract `VipleStream-Server-X.Y.Z.zip` to `C:\Program Files\VipleStream-Server\`.
2. In an **Administrator** PowerShell, register the service and allow it through the firewall:
   ```powershell
   cd "C:\Program Files\VipleStream-Server"
   New-Item -ItemType Directory tools -Force | Out-Null
   Copy-Item viplestream-svc.exe tools\      # the service wrapper lives one folder below
   New-Service -Name VipleStreamServer -DisplayName "VipleStream Server" -StartupType Automatic `
     -BinaryPathName '"C:\Program Files\VipleStream-Server\tools\viplestream-svc.exe"'
   New-NetFirewallRule -DisplayName "VipleStream Server" -Direction Inbound -Action Allow `
     -Program "C:\Program Files\VipleStream-Server\viplestream-server.exe"
   Start-Service VipleStreamServer
   ```
3. Open `https://localhost:47990`, accept the self-signed certificate warning, and create the Web UI login. Settings are stored in `config\sunshine.conf`.
4. *Optional:* Steam Controller passthrough. Run `.\Install-VipleSCHid.ps1` in the same Administrator PowerShell.
5. *Optional:* Xbox controller mode for VR games that ignore VR controllers needs [ViGEmBus](https://github.com/nefarius/ViGEmBus/releases) on the host.
6. *For VR on the Steam Frame:* install Steam and SteamVR, and keep a Windows user signed in (a locked or signed-out desktop streams a solid colour). The server installs and registers its own SteamVR driver when a VR session starts.

**Updating:** automatic by default. The server checks GitHub once a day and installs a new version, verified against its sha256, once nobody has streamed for 10 minutes. Change this under **Automatic updates** in the Web UI (General tab): off, notify only, or install automatically. You can also right-click the tray icon → **Check for updates...**.

### Host on Linux

Ubuntu 26.04 x64. 2D streaming only (VR streaming needs a Windows host).

```bash
sudo apt install ./VipleStream-Server-X.Y.Z-linux-x64.deb
```

The package enables a systemd user service for the installing user, so the server starts at boot without a desktop login. The Web UI is at `https://localhost:47990`. On a headless GNOME/Wayland host, set `capture = kms`.

### Clients

| Device | Install |
|---|---|
| Windows | Extract `VipleStream-Client-X.Y.Z.zip` anywhere and run `VipleStream.exe`. No installer, no admin rights. |
| Linux x64 | `chmod +x VipleStream-Client-X.Y.Z-linux-x64.AppImage`, then run it. |
| Android 9+ | Sideload `VipleStream-Android-X.Y.Z.apk` (file manager, or `adb install -r`). |
| Steam Frame | See below. |

Clients update themselves by default: Windows and the Linux AppImage download, verify and install a new version when they start (never during a stream); Android downloads it on an unmetered network and asks you to tap **Install** (the first time, allow VipleStream to install apps). Change this under **App updates** in the settings.

### Steam Frame headset

Use the installer script, not `flatpak install` alone. A plain Flatpak install only gives you Desktop mode: Flatpak always runs apps in their own process-ID sandbox, and the headset's SteamVR cannot work with an app inside one, so VipleStream turns VR mode and Video mode off. The installer adds a launcher that drops only that one isolation (the rest of the Flatpak sandbox stays) and gives access to SteamVR.

1. Put `VipleStream-Client-X.Y.Z-linux-arm64.flatpak` and `VipleStream-SteamFrame-Install-X.Y.Z.sh` in the same folder on the headset, and open a terminal there (desktop mode, or SSH with Developer Mode on).
2. Run the installer as your normal user (no `sudo`). The first install also downloads the KDE runtime from Flathub.
   ```bash
   bash VipleStream-SteamFrame-Install-X.Y.Z.sh
   ```
   It installs the Flatpak, sets up and checks the launcher, and adds **VipleStream** to your Steam library (Steam must be running).
3. Launch **VipleStream** from your Steam library inside the headset.

**Updating:** automatic. The installer sets up a background check (every 6 hours) that downloads the new release, verifies it, and installs it while VipleStream is closed. To update right away, download both files of the new release and run the new installer the same way. Choose **App updates** in VipleStream's settings to turn this off. **Removing:** `bash VipleStream-SteamFrame-Install-X.Y.Z.sh --uninstall`, then delete the shortcut from your Steam library.

## Using it

### Pair once

1. In the client, add the host. It usually appears by itself on the same network; otherwise press **+** (*Add PC manually*) and type the host's address.
2. The client shows a 4-digit PIN. On the host's Web UI (`https://<host>:47990`) open **PIN** and enter it.
3. The host appears with its apps; installed Steam games are listed automatically. Each device pairs once.

### Stream

Pick the host, then an app (**Desktop** streams the whole screen). Resolution, frame rate, bitrate, codec and frame interpolation are in **Settings**; the defaults are a good start.

| Input | End the stream |
|---|---|
| Keyboard | `Ctrl+Alt+Shift+Q` |
| Gamepad | hold `Start+Select+L1+R1` |
| Steam Frame controllers | hold `A+X+trigger+grip` (A and X are both on the right controller) |

### On the Steam Frame

VipleStream opens in its own VR interface (laser pointer: trigger = click, grip = right click, B = back). Pick a mode; you can switch later from the top bar.

| Mode | What it does |
|---|---|
| **VR mode** | SteamVR games from your PC. Pick the host, then **SteamVR Home** or a VR game; the host starts SteamVR and the game. Before starting, the client measures the round trip and waits if the connection is too slow for VR. |
| **Desktop mode** | Your PC desktop or a game on a large virtual screen. |
| **Video mode** | Flat, 3D side-by-side / top-bottom, 180° and 360° videos from the headset, a URL, folders shared by a paired PC (turn off with `media_share = disabled`), or YouTube through the built-in browser's **Play in VR**. |

**Video mode controls:** A play/pause · B exit · trigger control bar · grip recentre · stick left/right ±10 s · stick up/down nearer/farther.

**Xbox controller mode (2.0.68+):** some VR games (for example Assetto Corsa, Assetto Corsa Competizione, GRIP) only read a gamepad or wheel, so VR controllers do nothing in them. When the running game's Steam store page does not list *Tracked Controller Support*, the host plugs in a virtual Xbox 360 controller and drives it from both Frame controllers (right A/B/X/Y, left d-pad, bumpers → LB/RB, triggers → LT/RT, sticks → LS/RS, right menu → Start, left view → Back). Hold **both menu buttons for 2 seconds** to switch by hand; the choice is remembered per game. Needs ViGEmBus on the host; turn it off with `vr_padmode = disabled`.

**Steering wheel (2.0.75+):** in Xbox controller mode you can steer by turning the two controllers like a wheel; the angle replaces the left stick's left/right. Turn it on in the settings, or press and hold both thumbsticks for 1.5 seconds during a stream, and pick the steering range (180° to 900°).

**Virtual keyboard and flat-screen streams (2.0.175+):** in a VR stream, hold **menu + grip on one hand for 1 second** to open a keyboard in front of you; point and pull the trigger to type on the host, and drag the white bar under it to move it. While it is open, controller buttons are not sent to the game. In a flat-screen stream on the headset the same two gestures as in VR work: both menu buttons for 2 seconds turns the controllers into an Xbox controller instead of a laser mouse, both thumbsticks for 1.5 seconds switches the steering wheel. Some racing games need their own input setting changed first (Assetto Corsa Competizione: Options → Controls → Driving control → gamepad). The **?** button in the toolbar lists every shortcut.

**VR settings (2.0.122):**
- **Sharper where you look (foveated streaming)** — Off / Small / Medium / Large. Each eye is sent as a reduced picture of the whole view plus a full-detail picture that follows your gaze (it stays straight ahead without eye tracking). Small is the default and draws the least headset power; a larger size keeps more detail away from where you look, at the cost of bandwidth, power and a little delay. Needs a 2.0.122 host.
- **Frame rate** — Automatic (half rate while the network is unstable, back to full by itself), Full, Half, or Half with synthesized in-between frames.
- **Test network** — streams a fixed clip from the host at rising resolutions, once with home Wi-Fi kept and once over the adapter only, and remembers which one held the full frame rate. The host needs a test clip (`vr_netbench_clip`); none is shipped.
- **Send the headset microphone to the PC** — your voice arrives on the host's *Steam Streaming Microphone* device; select it in the game or voice chat app.
- **Recentre** — a SteamVR recentre (hold the Steam button) also re-places the VipleStream panels; in the VipleStream interface, hold the menu button for 1 second.

**Low battery (2.0.70+):** while streaming, a small battery icon appears at the top right of your view at 15 % or less (not charging), with one beep. Note that under a heavy streaming load the Frame can power off well above 15 % (see [SteamOS#3040](https://github.com/ValveSoftware/SteamOS/issues/3040)); keep a power bank at hand for long sessions.

To leave a desktop or VR stream, hold **A+X+trigger+grip**; you return to the VipleStream interface. If SteamVR opens the Steam menu over a new session, press the Steam button to close it.

More: [`docs/setup_guide.md`](docs/setup_guide.md) (configuration and troubleshooting) · [`docs/steam_frame_client.md`](docs/steam_frame_client.md) (headset client) · [`docs/steamvr_driver.md`](docs/steamvr_driver.md) (VR on the host).

## Playing from outside

- **Tailscale (recommended):** install Tailscale on the host and the client. The host advertises its Tailscale address, and clients remember it as a fallback, so the same pairing works at home and away. If the connection falls back to a Tailscale relay (DERP), VipleStream keeps streaming and switches back to a direct path when one becomes available.
- **Relay server (no VPN):** run `tools/viplestream-relay/relay_server.py` on an always-on machine (TLS can be terminated by Cloudflare Tunnel or nginx). The client finds the host through the relay, tunnels the RTSP handshake over one WebSocket, then punches a direct UDP path for video, audio and input. Connections must present a pre-shared key.
- **VR over the internet:** works, but a relayed path adds latency; the client measures the round trip before entering VR and tells you when it is too slow.

## Features beyond upstream

**Steam Frame / VR**
- PC VR streaming with its own SteamVR driver; pose prediction, latency-aware pacing, adaptive buffer (1 or 2 frames), eye-tracked foveated streaming (two pictures per eye, selectable size), automatic half rate on an unstable network, half-rate streaming with frame synthesis.
- Multi-link: one full copy of the stream per network link (home Wi-Fi, the headset hotspot to the host's adapter, Ethernet), with repair requests that follow the measured round trip.
- Full VR interface on the headset, video mode, Frame controllers exposed as Frame controllers (with hand skeleton, 2.0.68+), Xbox controller mode with steering-wheel emulation, headset microphone, a network test that chooses between home Wi-Fi and the adapter, low-battery icon.
- Coexists with Steam's own streaming: when Steam Link asks for SteamVR, the host hands it over.

**Networking**
- NAT traversal without port forwarding (STUN, relay signalling, RTSP-over-WebSocket, UDP hole punching, PSK).
- MP-QUIC multi-path for 2D streams with failover and path regain; falls back to ENet against upstream peers.
- Adaptive bitrate driven by delay trend and loss classification; Tailscale relay-to-direct recovery.

**Image**
- Frame interpolation (FRUC) on Windows (D3D11: generic compute, NVIDIA Optical Flow, DirectML / NCNN RIFE; Vulkan renderer) and Android (Vulkan, zero-copy).
- HDR on Android; PyroWave wavelet codec (Windows, manual selection).

**Steam & host**
- Steam library auto-import with playtime and last-played sorting; switch the host's Steam account from the client.
- Ending an app closes the Steam game gracefully; Steam Controller native HID passthrough; automatic updates on every platform (host, PC, Android, Steam Frame); file transfer during a stream.
- 30 UI languages, including Traditional Chinese.

## Architecture

```
┌──────────────────────┐   direct, Tailscale or relay   ┌──────────────────────┐
│  VipleStream client  │ ◄────────────────────────────► │  VipleStream-Server  │
│  PC / Android /      │   1. STUN → public address     │  Windows / Linux     │
│  Steam Frame         │   2. relay lookup              │                      │
│                      │   3. HTTP proxy /launch        │  + SteamVR driver    │
│                      │   4. RTSP over WebSocket       │    (Windows, VR)     │
│                      │   5. UDP hole punch / MP-QUIC  │                      │
└──────────────────────┘                                └──────────────────────┘
                                      ▲
                                      │ WebSocket (ws:// or wss://)
                              ┌───────┴────────┐
                              │  Relay server  │
                              │ relay_server.py│
                              └────────────────┘
```

## Security notes

- **Pairing unchanged:** the PIN pairing flow and client certificate trust chain are the same as upstream.
- **Relay authentication:** every relay connection must present `HMAC-SHA256(psk, uuid)[:16]`. No keys or relay addresses are stored in the source.
- **Android relay TLS:** the relay WebSocket trusts any certificate because TLS is terminated by a reverse proxy; the game stream itself keeps Moonlight's certificate pinning.
- **Steam credentials:** the host never sees your Steam password. Account switching only runs `steam.exe -login <account>`, which relies on Steam's own remembered sign-in.
- **Hotspot keys:** the headset passes its hotspot details to the host over the encrypted control channel; keys are never written to logs.

## Version highlights

| Version | Highlights |
|---|---|
| **2.0.175** | Virtual keyboard inside VR streams (draggable); Xbox controller and steering wheel modes for flat-screen streams on the Steam Frame; the slight picture shift every second or two in VR is gone; fewer bits spent on the periphery (32 Mbps per link in our table tennis test, headset decode 3.2 → 2.0 ms); shortcut list in the toolbar; a full code review worked through, with unpaired `/serverinfo` reduced to pairing fields, Steam account switching limited to the session owner, video sharing limited to one folder by default and updates verified by sha256; QUIC stalls on large frames fixed. |
| **2.0.122** | Steam Frame foveation rebuilt as two pictures per eye (a reduced full view plus a full-detail picture that follows the gaze) with an Off / Small / Medium / Large setting, taking headset power from about 13 W to under 10 W in our test; automatic half rate when the network is unstable; "Test network" in the headset settings; background Wi-Fi scans blocked while streaming; steering-wheel emulation; headset microphone; automatic updates on every platform; 2D bitrate fixes for congestion and a relative mouse that survives packet loss. |
| **2.0.63** | Automatic bitrate that adapts to the network (delay trend, loss type, QP-based cap); Tailscale relay-to-direct recovery during a stream; headset buffer fixed and switching between 1 and 2 frames automatically; a large batch of review fixes; Android minimum is now Android 9; Linux .deb depends on Ayatana AppIndicator. |
| **2.0.31** | Eye-tracked foveated encoding on the Steam Frame; faster pacing lock at session start; worn-tested VR settings become the defaults; links added mid-stream join multi-link; redundant haptics. |
| **2.0.25** | Fast VR bitrate drop and recovery on weak signal; Frame controllers exposed as Frame controllers; real play area and seated height; coexistence with Steam's built-in streaming. |
| **2.0.22** | Ending an app closes the Steam game; multi-path stability fixes; Android translations completed. |
| **2.0.9 – 2.0.11** | Full VR interface on the headset; VR-controller exit combo; reach the host from outside via its Tailscale address; video thumbnails; 30 languages. |
| **2.0.0 – 2.0.2** | Steam Frame client (VR, desktop and video mode) with the host's own SteamVR driver; headset installer script. |
| **1.x** | NAT traversal, MP-QUIC multi-path, FRUC, Steam library import and account switching, Steam Controller passthrough, Linux and Android ports. |

Full notes: [Releases](https://github.com/finaltwinsen/VipleStream/releases) and `git log`.

## License

VipleStream is licensed under **GPL-3.0** ([LICENSE](LICENSE)), the same license as its upstream components:

- [Sunshine](https://github.com/LizardByte/Sunshine) — GPL-3.0
- [Moonlight](https://github.com/moonlight-stream/moonlight-qt) — GPL-3.0
- [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c) — LGPL-3.0

The `moonlight-common-c` portions remain under LGPL-3.0; everything else (server, Qt client, Android client, build scripts, relay server) is GPL-3.0. The SteamVR driver's hand-skeleton code is adapted from Valve's OpenVR SDK sample (BSD-3-Clause).

---

# 繁體中文

[English](#viplestream) · **繁體中文**

自架的遊戲串流套件，支援 Windows、Linux、Android 與 **Steam Frame** 頭盔。以 [Sunshine](https://github.com/LizardByte/Sunshine)（主機端）與 [Moonlight](https://github.com/moonlight-stream)（用戶端）為基礎分支，加上 Steam Frame 的 PC VR 串流、不必設定連接埠轉送的外網連線、多路徑傳輸、補幀與 Steam 整合，並維持與原版 Sunshine／Moonlight 的通訊協定相容。

> **下載：** [最新版](https://github.com/finaltwinsen/VipleStream/releases/latest)。每次發佈的所有元件都是同一個版號，請一起更新。

**目錄：** [比較](#比較) · [元件](#元件) · [安裝](#安裝) · [使用](#使用) · [從外面連回家](#從外面連回家) · [功能](#比上游多的功能) · [架構](#架構) · [安全](#安全) · [版本摘要](#版本摘要) · [授權](#授權)

## 比較

VipleStream 與 Steam 內建串流（Steam Link／遠端暢玩，含串流到 Steam Frame 的 PC VR）以及原版 Sunshine＋Moonlight 的差異。Steam 常常更新；Steam 那欄依據 Valve 公開的頁面與更新說明，不是我們自己測的。（2026-11-05 的狀況）

| | VipleStream | Steam 內建串流 | Sunshine＋Moonlight |
|---|---|---|---|
| **原始碼／授權** | 開源 GPL-3.0、自架 | 封閉、需要 Steam 帳號 | 開源 GPL-3.0 |
| **主機系統** | Windows（2D＋VR）、Linux（2D） | 2D：Windows、macOS、Linux<br>串流到 Steam Frame 的 VR：Windows（桌機 Linux 官方說「尚未完整支援」） | Windows、Linux、macOS 15 以上 |
| **用戶端** | Windows、Linux x64、Android 9 以上、Steam Frame | Steam Link app：Windows、macOS、Linux、Android（含電視）、iOS／Apple TV／Vision Pro、樹莓派、Meta Quest；Steam Frame | 平台很多（含 iOS、Apple TV、Chromebook、樹莓派） |
| **Steam Frame 的 PC VR** | ✓ 自帶 SteamVR driver | ✓ | — |
| **眼動注視點編碼** | ✓ | ✓ | — |
| **串流任何程式或整個桌面** | ✓ | ✓ | ✓ |
| **外網免轉送連接埠、免 VPN** | ✓ 中繼＋UDP 打洞，認得 Tailscale | ✓ 2D（遠端暢玩）；VR 只在家用網路（官方沒有外網 VR） | 要轉送連接埠、UPnP 或 VPN |
| **同時走多條網路** | ✓ 2D：MP-QUIC 走所有網卡；VR：每條連線各送一份 | ✓ VR：串流轉接器＋Wi-Fi | — |
| **自適應碼率** | ✓ 看延遲趨勢與丟包類型 | ✓ 自動選擇碼率 | —（固定碼率） |
| **2D 用戶端補幀** | ✓ Windows（D3D11／Vulkan）、Android | — | — |
| **自動匯入 Steam 遊戲、切換 Steam 帳號** | ✓ | （本來就是 Steam） | 手動加入程式 |
| **不用 VR 手把的 VR 遊戲把 Frame 手把當一般手把** | ✓ 虛擬 Xbox 360 手把，依遊戲的 Steam 商店分類自動判斷，2.0.68 起 | ✓ 通用手把，SteamVR 2.18 起（2026-10）；Valve 沒說明怎麼判斷遊戲 | — |
| **頭盔影片播放（本機、PC 資料夾、網址、YouTube；2D／3D／180°／360°）** | ✓ | 沒有 Frame 內建播放器的官方資料（SteamVR Media Player 是 PC 端功能）；有第三方 app | — |
| **串流中頭盔低電量提示** | ✓ 15% 顯示圖示並嘟一聲，2.0.70 起 | 沒有 VR 中低電量警告的官方資料；儀表板列會顯示電量百分比 | — |
| **Steam Controller 原生轉發** | ✓ | ✓ | — |
| **與原版 Moonlight／Sunshine 互通** | ✓ | — | ✓ |
| **介面語言** | 30 種（含繁體中文） | 29 種 | Moonlight-Qt 30 種；Sunshine 網頁介面 19 種 |

### 該用哪一個？

- **Steam 內建串流**：只在家玩 Steam 遊戲、現有功能就夠用時，免設定最省事。
- **VipleStream**：想從外面連回家又不想開連接埠、想同時用多條網路、要串流到 Android 或 Linux 裝置並補幀，或要在頭盔看影片時適合；想自己看 log、調主機設定、讀原始碼的也適合。
- **Sunshine＋Moonlight**：需要最多種用戶端（iOS、Apple TV、智慧電視）時。VipleStream 的主機與用戶端都能和它們互通。

## 元件

| 元件 | 取代 | 平台 | 下載檔 | 安裝位置 |
|---|---|---|---|---|
| **VipleStream-Server** | Sunshine | Windows x64 | `VipleStream-Server-X.Y.Z.zip` | `C:\Program Files\VipleStream-Server\`（服務 `VipleStreamServer`） |
| | | Linux x64（Ubuntu 26.04） | `VipleStream-Server-X.Y.Z-linux-x64.deb` | `/usr/bin/viplestream-server`（systemd 使用者服務） |
| **VipleStream**（電腦版用戶端） | Moonlight-Qt | Windows x64 | `VipleStream-Client-X.Y.Z.zip` | 任何資料夾 |
| | | Linux x64 | `VipleStream-Client-X.Y.Z-linux-x64.AppImage` | 免安裝 |
| **VipleStream Android** | Moonlight-Android | Android 9 以上 | `VipleStream-Android-X.Y.Z.apk`（`com.piinsta`） | 側載 |
| **VipleStream for Steam Frame** | — | Steam Frame（Linux arm64） | `VipleStream-Client-X.Y.Z-linux-arm64.flatpak`＋`VipleStream-SteamFrame-Install-X.Y.Z.sh` | 個人 Flatpak |
| **Windows 驅動程式** | — | Windows x64 | `VipleStream-Drivers-X.Y.Z-windows.zip`（與 Server zip 內相同） | 修復用 |
| **viplestream-relay** | — | 任何系統，Python 3.10 以上 | `tools/viplestream-relay/relay_server.py`（只有原始碼） | 一台常開的機器 |

通訊協定（`_nvstream._tcp` mDNS、`/serverinfo`、`/launch`、`/applist`、RTSP、用戶端憑證）與上游相同。對方是原版 Sunshine 或 Moonlight 時，VipleStream 專屬功能會自動隱藏。

## 安裝

**主機（host）**是跑遊戲的電腦；你拿來玩的每台裝置都是**用戶端（client）**。

### Windows 主機

Windows 10／11，顯示卡（NVIDIA、AMD 或 Intel）要有硬體影像編碼器。

1. 把 `VipleStream-Server-X.Y.Z.zip` 解壓縮到 `C:\Program Files\VipleStream-Server\`。
2. 用**系統管理員**身分開 PowerShell，註冊服務並允許通過防火牆：
   ```powershell
   cd "C:\Program Files\VipleStream-Server"
   New-Item -ItemType Directory tools -Force | Out-Null
   Copy-Item viplestream-svc.exe tools\      # 服務程式要放在下一層
   New-Service -Name VipleStreamServer -DisplayName "VipleStream Server" -StartupType Automatic `
     -BinaryPathName '"C:\Program Files\VipleStream-Server\tools\viplestream-svc.exe"'
   New-NetFirewallRule -DisplayName "VipleStream Server" -Direction Inbound -Action Allow `
     -Program "C:\Program Files\VipleStream-Server\viplestream-server.exe"
   Start-Service VipleStreamServer
   ```
3. 開啟 `https://localhost:47990`，接受自簽憑證的警告，建立 Web UI 的帳號密碼。設定存在 `config\sunshine.conf`。
4. *選用：* Steam Controller 轉發。在同一個系統管理員 PowerShell 執行 `.\Install-VipleSCHid.ps1`。
5. *選用：* 「Xbox 手把模式」（給不用 VR 手把的 VR 遊戲）需要在主機裝 [ViGEmBus](https://github.com/nefarius/ViGEmBus/releases)。
6. *要在 Steam Frame 玩 VR：* 裝好 Steam 與 SteamVR，並保持有 Windows 使用者登入（鎖定或登出時畫面會是一整片單色）。VR 串流開始時，server 會自己部署並註冊 SteamVR driver，不必手動安裝。

**更新：** 預設自動更新。server 每天向 GitHub 檢查一次，有新版就在連續 10 分鐘沒有人串流時下載、用 sha256 驗證後安裝。可以在 Web UI 的 General 分頁「**自動更新**」改成關閉、只通知或自動安裝；也可以在系統匣圖示按右鍵 →「**Check for updates...**」手動檢查。

### Linux 主機

Ubuntu 26.04 x64，只支援 2D 串流（VR 串流需要 Windows 主機）。

```bash
sudo apt install ./VipleStream-Server-X.Y.Z-linux-x64.deb
```

安裝時會替安裝者啟用 systemd 使用者服務，開機後不必登入桌面就會啟動。Web UI 在 `https://localhost:47990`。沒接螢幕的 GNOME／Wayland 主機請設 `capture = kms`。

### 用戶端

| 裝置 | 安裝 |
|---|---|
| Windows | 把 `VipleStream-Client-X.Y.Z.zip` 解壓縮到任何資料夾，執行 `VipleStream.exe`。不需要安裝程式或系統管理員權限。 |
| Linux x64 | `chmod +x VipleStream-Client-X.Y.Z-linux-x64.AppImage` 後直接執行。 |
| Android 9 以上 | 用檔案管理員或 `adb install -r` 側載 `VipleStream-Android-X.Y.Z.apk`。 |
| Steam Frame | 見下方。 |

用戶端預設會自動更新：Windows 與 Linux AppImage 啟動時下載、驗證並安裝新版（串流中不會）；Android 在不計流量的網路下載好之後，請你按「**安裝**」（第一次要允許 VipleStream 安裝應用程式）。可以在設定的「**軟體更新**」修改。

### Steam Frame 頭盔

請用安裝腳本，不要只執行 `flatpak install`。單純裝 Flatpak 只能用桌面模式：Flatpak 一定會把程式放進獨立的行程 ID 沙盒，頭盔的 SteamVR 無法跟沙盒裡的程式合作，VipleStream 就會停用 VR 模式與影片模式。安裝腳本會加一個啟動器，只拿掉這一項隔離（Flatpak 其他隔離不變），並開放 SteamVR 的存取。

1. 把 `VipleStream-Client-X.Y.Z-linux-arm64.flatpak` 和 `VipleStream-SteamFrame-Install-X.Y.Z.sh` 放在頭盔上的同一個資料夾，在那裡開終端機（桌面模式，或開啟開發者模式後用 SSH）。
2. 以一般使用者執行安裝腳本（不要 `sudo`）。第一次安裝會從 Flathub 下載 KDE runtime。
   ```bash
   bash VipleStream-SteamFrame-Install-X.Y.Z.sh
   ```
   它會安裝 Flatpak、設定並檢查啟動器，然後把 **VipleStream** 加進 Steam 收藏庫（Steam 要開著）。
3. 在頭盔裡從 Steam 收藏庫開啟 **VipleStream**。

**更新：** 自動更新。安裝腳本會設定一個背景檢查（每 6 小時），在 VipleStream 關著時下載新版、驗證後安裝。想馬上更新就下載新版的兩個檔案，用同樣方式執行新的安裝腳本。在 VipleStream 的設定裡「**軟體更新**」可以關掉。**移除：** 執行 `bash VipleStream-SteamFrame-Install-X.Y.Z.sh --uninstall`，再從 Steam 收藏庫刪掉捷徑。

## 使用

### 第一次配對

1. 在用戶端加入主機。同一個網路通常會自動出現；沒有的話按 **+**（手動加入電腦）輸入主機位址。
2. 用戶端會顯示 4 位數 PIN 碼，到主機的 Web UI（`https://<主機>:47990`）的 **PIN** 頁輸入。
3. 主機和它的程式會出現，已安裝的 Steam 遊戲會自動列出。每台裝置只要配對一次。

### 開始串流

選主機、再選程式（**Desktop** 是整個桌面）。解析度、更新率、碼率、編碼格式與補幀在**設定**裡，預設值就能用。

| 輸入 | 結束串流 |
|---|---|
| 鍵盤 | `Ctrl+Alt+Shift+Q` |
| 手把 | 按住 `Start+Select+L1+R1` |
| Steam Frame 手把 | 按住 `A+X+扳機+握把`（A 與 X 都在右手） |

### 在 Steam Frame 上

VipleStream 會開在自己的 VR 介面裡（雷射指標：扳機＝點選、握把＝右鍵、B＝返回）。選一個模式，之後可以從上方列切換。

| 模式 | 用途 |
|---|---|
| **VR 模式** | 在頭盔玩 PC 上的 SteamVR 遊戲。選主機，再選 **SteamVR Home** 或 VR 遊戲，主機會啟動 SteamVR 和遊戲。開始前會先量往返時間，連線太慢時會先等。 |
| **桌面模式** | 把電腦桌面或遊戲放在大型虛擬螢幕上。 |
| **影片模式** | 播放平面、3D 左右／上下、180°、360° 影片；來源可以是頭盔本機、網址、配對 PC 分享的資料夾（`media_share = disabled` 可關閉），或內建瀏覽器的「**在 VR 播放**」（YouTube 等）。 |

**影片模式操作：** A 播放／暫停 · B 離開 · 扳機叫出控制列 · 握把重新置中 · 搖桿左右快轉 ±10 秒 · 搖桿上下調遠近。

**Xbox 手把模式（2.0.68 起）：** 有些 VR 遊戲（例如 Assetto Corsa、Assetto Corsa Competizione、GRIP）只讀手把或方向盤，VR 手把在裡面完全沒反應。執行中的遊戲在 Steam 商店沒有標「Tracked Controller Support」時，主機會插上一支虛擬 Xbox 360 手把，用兩支 Frame 手把操作（右手 A／B／X／Y、左手十字鍵、肩鍵→LB／RB、扳機→LT／RT、搖桿→LS／RS、右手 menu→Start、左手 view→Back）。**兩支選單鍵同時按住 2 秒**可以手動切換，每款遊戲會記住你的選擇。主機要裝 ViGEmBus；用 `vr_padmode = disabled` 可以關閉。

**方向盤（2.0.75 起）：** Xbox 手把模式下，可以把兩支手把像握方向盤一樣轉動來轉向，角度取代左搖桿的左右。在設定頁打開，或串流中同時按住兩支搖桿 1.5 秒切換；轉向角度可選（180°～900°）。

**虛擬鍵盤與 2D 串流（2.0.175 起）：** VR 串流中**同一手的選單鍵＋握把按住 1 秒**，面前會出現鍵盤；指著鍵扣扳機就是在主機上打字，拖鍵盤下方的白條可以移動它。鍵盤開著時控制器的按鍵不會送給遊戲。頭盔上的 2D 串流用和 VR 相同的兩個操作：兩支選單鍵 2 秒把控制器從雷射滑鼠變成 Xbox 手把，兩支搖桿 1.5 秒切換方向盤。有些賽車遊戲要先改遊戲自己的輸入設定（Assetto Corsa Competizione：選項 → 控制 → DRIVING CONTROL → 手柄）。工具列的 **?** 鍵會列出所有快捷鍵。

**VR 設定（2.0.122）：**
- **視線所在更清晰（注視點串流）**——關閉／小／中／大。每隻眼睛送兩張畫面：整個視野縮小的一張，加上跟著視線走、保留完整細節的一張（沒有眼動追蹤時固定在正前方）。預設「小」，頭盔最省電；選大一點，視線以外的地方也比較清楚，代價是流量、耗電和一點延遲。主機要 2.0.122。
- **幀率**——自動（網路不穩時降到半速，穩定後自己回來）、全速、半速、半速加補幀。
- **測試網路**——主機播放固定的片段，解析度一階一階往上，「保留家用 Wi-Fi」與「只用轉接器」各跑一輪，記住比較撐得住全速的那一種。主機上要放測試片段（`vr_netbench_clip`），安裝包沒有附。
- **傳送頭盔麥克風至電腦**——聲音會出現在主機的「Steam Streaming Microphone」裝置，在遊戲或語音軟體裡選它。
- **重新置中**——SteamVR 的重新置中（長按 Steam 鍵）也會重擺 VipleStream 的面板；在 VipleStream 介面裡長按選單鍵 1 秒。

**低電量（2.0.70 起）：** 串流時電量 ≤ 15% 且沒在充電，視野右上角會出現小電池圖示並嘟一聲。注意：高負載串流時 Frame 可能在電量遠高於 15% 時就斷電（見 [SteamOS#3040](https://github.com/ValveSoftware/SteamOS/issues/3040)），長時間遊玩建議準備行動電源。

要離開桌面或 VR 串流，按住 **A+X+扳機+握把**，會回到 VipleStream 介面。新場次開始時如果 SteamVR 的 Steam 選單蓋在上面，按 Steam 鍵關掉即可。

更多：[`docs/setup_guide.md`](docs/setup_guide.md)（設定與疑難排解）· [`docs/steam_frame_client.md`](docs/steam_frame_client.md)（頭盔用戶端）· [`docs/steamvr_driver.md`](docs/steamvr_driver.md)（主機端 VR）。

## 從外面連回家

- **Tailscale（建議）：** 主機和用戶端都裝 Tailscale。主機會公告自己的 Tailscale 位址，用戶端記下來當備用，在家和出門用同一組配對。連線退到 Tailscale 中繼（DERP）時，串流不中斷，有直連路徑時會自動換回直連。
- **中繼伺服器（不用 VPN）：** 在一台常開的機器跑 `tools/viplestream-relay/relay_server.py`（TLS 可交給 Cloudflare Tunnel 或 nginx）。用戶端經中繼找到主機，RTSP 握手走一條 WebSocket，影音與輸入再用 UDP 打洞直連。連線要提供預先共享的金鑰。
- **外網玩 VR：** 可以，但走中繼會增加延遲；進 VR 前用戶端會先量往返時間，太慢時會明確告訴你。

## 比上游多的功能

**Steam Frame／VR**
- 自帶 SteamVR driver 的 PC VR 串流；姿態預測、依延遲調整的節拍、自動 1／2 格緩衝、眼動注視點串流（每眼兩張畫面，大小可選）、網路不穩時自動半速、半速串流加補幀。
- 多連線：每條網路連線（家用 Wi-Fi、頭盔熱點到主機網卡、有線）各送一份完整串流，補包時機跟著實測往返時間走。
- 頭盔全 VR 介面、影片模式、Frame 手把以 Frame 手把身分出現（含手部骨架，2.0.68 起）、Xbox 手把模式（含方向盤模擬）、頭盔麥克風、在家用 Wi-Fi 與轉接器之間選擇的網路測試、低電量圖示。
- 與 Steam 內建串流共存：Steam Link 要用 SteamVR 時，主機會讓出來。

**網路**
- 免轉送連接埠的 NAT 穿越（STUN、中繼信令、RTSP 經 WebSocket、UDP 打洞、PSK 驗證）。
- 2D 串流的 MP-QUIC 多路徑，含故障轉移與路徑回歸；對方是上游版本時退回 ENet。
- 依延遲趨勢與丟包分類的自適應碼率；Tailscale 中繼自動恢復直連。

**畫面**
- 補幀：Windows（D3D11：通用運算、NVIDIA Optical Flow、DirectML／NCNN RIFE；Vulkan 繪製器）與 Android（Vulkan、零複製）。
- Android HDR；PyroWave 小波編碼（Windows，需手動選）。

**Steam 與主機**
- 自動匯入 Steam 遊戲庫，可依遊玩時間、最近遊玩排序；從用戶端切換主機的 Steam 帳號。
- 結束程式會正常關閉 Steam 遊戲；Steam Controller 原生 HID 轉發；各平台自動更新（主機、電腦、Android、Steam Frame）；串流中傳檔。
- 30 種介面語言，含繁體中文。

## 架構

```
┌──────────────────────┐   direct, Tailscale or relay   ┌──────────────────────┐
│  VipleStream client  │ ◄────────────────────────────► │  VipleStream-Server  │
│  PC / Android /      │   1. STUN → public address     │  Windows / Linux     │
│  Steam Frame         │   2. relay lookup              │                      │
│                      │   3. HTTP proxy /launch        │  + SteamVR driver    │
│                      │   4. RTSP over WebSocket       │    (Windows, VR)     │
│                      │   5. UDP hole punch / MP-QUIC  │                      │
└──────────────────────┘                                └──────────────────────┘
                                      ▲
                                      │ WebSocket (ws:// or wss://)
                              ┌───────┴────────┐
                              │  Relay server  │
                              │ relay_server.py│
                              └────────────────┘
```

## 安全

- **配對流程不變：** PIN 配對與用戶端憑證信任鏈跟上游相同。
- **中繼驗證：** 每條中繼連線都要提供 `HMAC-SHA256(psk, uuid)[:16]`；原始碼裡沒有任何金鑰或中繼位址。
- **Android 中繼 TLS：** 中繼的 WebSocket 不驗憑證（TLS 由反向代理處理）；遊戲串流本身仍是 Moonlight 的憑證綁定。
- **Steam 帳密：** 主機不會看到 Steam 密碼；切換帳號只執行 `steam.exe -login <帳號>`，靠 Steam 自己記住的登入。
- **熱點金鑰：** 頭盔經加密控制通道把熱點資訊交給主機，金鑰不會寫進任何 log。

## 版本摘要

| 版本 | 重點 |
|---|---|
| **2.0.175** | VR 串流裡的虛擬鍵盤（可拖動）；Steam Frame 2D 串流的 Xbox 手把與方向盤模式；VR 畫面每一兩秒輕微挪動的問題消失；周邊少花位元（桌球實測每條連線 32 Mbps、頭盔解碼 3.2 → 2.0 ms）；工具列的快捷鍵列表；整份程式碼審查逐項處理，未配對的 `/serverinfo` 只回配對欄位、Steam 帳號切換限 session 擁有者、影片分享預設只開一個資料夾、更新核對 sha256；QUIC 大畫面停頓已修。 |
| **2.0.122** | Steam Frame 注視點改成每眼兩張畫面（整個視野縮小的一張＋跟著視線、保留完整細節的一張），設定頁可選關閉／小／中／大，實測頭盔耗電從約 13 W 降到 10 W 以下；網路不穩時自動半速；頭盔設定頁「測試網路」；串流時擋掉 Wi-Fi 背景掃描；方向盤模擬；頭盔麥克風；各平台自動更新；2D 塞車時的碼率修正、相對滑鼠不怕掉包。 |
| **2.0.63** | 碼率依網路自動調整（延遲趨勢、丟包類型、以 QP 決定上限）；串流中 Tailscale 從中繼自動恢復直連；頭盔緩衝修好並自動在 1／2 格之間切換；大批程式碼審查修正；Android 最低改為 Android 9；Linux .deb 改依賴 Ayatana AppIndicator。 |
| **2.0.31** | Steam Frame 眼動注視點編碼；開場更快鎖定節拍；配戴驗過的 VR 設定改為預設；串流中新增的連線自動加入多連線；震動冗餘發送。 |
| **2.0.25** | 訊號差時 VR 快速降碼與回升；Frame 手把以 Frame 手把身分出現；正確的遊玩區與坐姿高度；與 Steam 內建串流共存。 |
| **2.0.22** | 結束程式會關閉 Steam 遊戲；多路徑穩定性修正；Android 翻譯補齊。 |
| **2.0.9 – 2.0.11** | 頭盔全 VR 介面；VR 手把結束串流；離開區網也能用 Tailscale 位址連主機；影片縮圖；30 種語言。 |
| **2.0.0 – 2.0.2** | Steam Frame 用戶端（VR、桌面、影片模式）與主機自帶的 SteamVR driver；頭盔安裝腳本。 |
| **1.x** | NAT 穿越、MP-QUIC 多路徑、補幀、Steam 遊戲庫匯入與帳號切換、Steam Controller 轉發、Linux 與 Android 版。 |

完整說明見 [Releases](https://github.com/finaltwinsen/VipleStream/releases) 與 `git log`。

## 授權

VipleStream 採用 **GPL-3.0**（[LICENSE](LICENSE)），與上游元件相同：

- [Sunshine](https://github.com/LizardByte/Sunshine) — GPL-3.0
- [Moonlight](https://github.com/moonlight-stream/moonlight-qt) — GPL-3.0
- [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c) — LGPL-3.0

`moonlight-common-c` 的部分維持 LGPL-3.0，其餘（server、Qt 用戶端、Android 用戶端、建置腳本、中繼伺服器）是 GPL-3.0。SteamVR driver 的手部骨架程式改寫自 Valve OpenVR SDK 範例（BSD-3-Clause）。
