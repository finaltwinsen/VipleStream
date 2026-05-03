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
