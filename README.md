# VipleStream

A custom game streaming solution based on [Sunshine](https://github.com/LizardByte/Sunshine) (host — renamed to **VipleStream-Server** in this fork) and [Moonlight](https://github.com/moonlight-stream) (client — renamed to **VipleStream**), extended with built-in NAT traversal, frame interpolation (FRUC), and performance optimizations. Project homepage: <https://github.com/finaltwinsen/VipleStream>.

> **Current version:** 1.1.80

---

## Features

### NAT Traversal (Zero Hardware Cost)
Stream across the internet without port forwarding, UPnP, or a VPN.

- **STUN prober** — Detects public IP and NAT type at startup
- **Relay signaling server** — Lightweight Python WebSocket server, deployable behind Cloudflare Tunnel
- **HTTP proxy** — Fetches server info and launches apps through the relay
- **RTSP TCP tunnel** — Tunnels the RTSP handshake (OPTIONS → DESCRIBE → SETUP×3 → PLAY) through the relay WebSocket
- **UDP hole punch** — Direct peer-to-peer UDP for video/audio/control after RTSP succeeds
- **Android support** — Full relay stack ported to the Android client (RelayClient + RelayTcpTunnel)

### Frame Rate Up-Conversion (FRUC)
2× frame interpolation using NVIDIA Optical Flow SDK (RTX cards required).

- **Motion estimation** — HLSL compute shaders on D3D11 (quality / balanced / performance presets)
- **Warp rendering** — Per-pixel forward-warp with occlusion masking
- **Presets** — Quality, Balanced, Performance, Ultra-Performance; runtime-switchable
- **Adaptive bitrate** — Buffer-based target bitrate control

### Other Improvements
- Debug pairing tool (`scripts/debug_pair.ps1`) — ADB-based PIN-free pairing for development
- Adaptive bitrate logic
- Network status overlay (LAN / Relay / DERP)
- Traditional Chinese UI (`qml_zh_TW.ts`)

---

## Architecture

```
┌──────────────────────┐        relay / direct        ┌──────────────────────┐
│  Moonlight Client    │ ◄──────────────────────────► │  Sunshine Server     │
│  (PC / Android)      │                               │  (Windows)           │
│                      │   1. STUN → public IP         │                      │
│  RelayClient.java    │   2. relay lookup             │  stun.cpp            │
│  RelayTcpTunnel.java │   3. HTTP proxy /launch       │  relay.cpp           │
│  relaytcptunnel.cpp  │   4. TCP tunnel (RTSP)        │  rtsp.cpp            │
│  relaylookup.cpp     │   5. UDP hole punch           │  stream.cpp          │
└──────────────────────┘                               └──────────────────────┘
                                      ▲
                                      │ WebSocket (ws:// or wss://)
                              ┌───────┴───────┐
                              │  Relay Server  │
                              │  relay_server.py│
                              │  (Python 3.10+) │
                              └───────────────┘
```

---

## Repository Structure

```
VipleStream/
├── Sunshine/                  # Server (C++ / CMake)
│   └── src/
│       ├── relay.cpp/.h       # Relay signaling + TCP tunnel server-side
│       ├── stun.cpp/.h        # STUN prober (public IP detection)
│       ├── rtsp.cpp           # RTSP with last_session cache for tunnel reuse
│       └── ...
├── moonlight-qt/              # PC client (C++ / Qt6)
│   └── app/
│       ├── backend/
│       │   ├── relaylookup.cpp/.h       # Relay lookup + HTTP proxy
│       │   └── relaytcptunnel.cpp/.h    # RTSP TCP tunnel (native sockets)
│       └── streaming/video/ffmpeg-renderers/
│           ├── nvofruc.cpp/.h           # NVIDIA Optical Flow FRUC
│           └── d3d11va.cpp/.h           # D3D11 renderer integration
├── moonlight-android/         # Android client (Java / Gradle + NDK)
│   └── app/src/main/java/com/limelight/
│       ├── relay/
│       │   ├── RelayClient.java         # WebSocket lookup + HTTP proxy
│       │   └── RelayTcpTunnel.java      # RTSP TCP tunnel (Java threads)
│       └── nvstream/
│           └── NvConnection.java        # Relay launch + tunnel lifecycle
├── tools/
│   └── viplestream-relay/
│       └── relay_server.py    # Relay signaling server
├── scripts/
│   ├── debug_pair.ps1         # ADB-based debug pairing
│   ├── deploy_server.ps1      # Deploy Sunshine to local install
│   ├── deploy_client.ps1      # Deploy Moonlight to local install
│   └── benchmark/             # FRUC quality measurement tools
├── build_all.cmd              # One-command build (Sunshine + Moonlight)
├── build_sunshine.cmd         # Build Sunshine only
├── build_moonlight.cmd        # Build Moonlight-Qt only
├── build-config.template.cmd  # Copy → build-config.local.cmd, fill paths
└── version.json               # Single source of truth for version number
```

---

## Setup

### Prerequisites

**Sunshine (server):**
- Windows 10/11
- MSYS2 + MinGW-w64 (GCC 13+)
- CMake 3.25+
- Node.js 18+ (for web UI assets)

**Moonlight PC (client):**
- Windows 10/11
- Visual Studio 2022 Build Tools (MSVC)
- Qt 6.10+ (`msvc2022_64`)
- NVIDIA GPU with driver ≥ 522 (for FRUC)

**Moonlight Android:**
- Android Studio or Gradle 8+
- Android NDK r26+
- JDK 17+

**Relay server:**
- Python 3.10+
- No external dependencies (stdlib only)
- Any always-on host (VPS, Raspberry Pi, etc.)

### Setup from a fresh clone

```cmd
git clone https://github.com/finaltwinsen/VipleStream.git
cd VipleStream
```

1. Create your per-machine config (paths to MSVC, Qt, 7-Zip, MSYS2, etc.):
   ```cmd
   copy build-config.template.cmd build-config.local.cmd
   notepad build-config.local.cmd
   ```
   `build-config.local.cmd` is gitignored. Everything else (the build wrappers,
   the packaging script, version bumper) is tracked so you can `git pull` and
   immediately build with no extra setup on a new machine.

2. Build everything (server + client):
   ```cmd
   build_all.cmd
   ```
   Or build individually:
   ```cmd
   build_sunshine.cmd     :: server only
   build_moonlight.cmd    :: client only
   ```
   Outputs staged in `temp/sunshine/` and `temp/moonlight/`, zipped into
   `release/VipleStream-{Server|Client}-<version>.zip`.

3. Deploy locally (needs admin once per session for `C:\Program Files\...`):
   ```cmd
   scripts\deploy_server.ps1     :: Deploy Sunshine
   scripts\deploy_client.ps1     :: Deploy Moonlight
   ```
   Deploy scripts read `DEPLOY_SERVER` / `DEPLOY_CLIENT` from
   `build-config.local.cmd` (defaults point at the usual install paths).

### Build chain

```
build_all.cmd  ─ bumps version
   ├─ scripts/build_sunshine_inner.sh      (MSYS2 UCRT64 GCC build)
   ├─ scripts/build_moonlight_package.cmd  (canonical shader/DLL list + zip)
   └─ scripts/bump_version.cmd             (version.json → all build files)
```

The canonical Moonlight shader / DLL / windeployqt list lives in
`scripts/build_moonlight_package.cmd`, not in the root wrapper. If you add a
new shader, update the `for %%F in (…)` list there and every build path picks
it up.

### Relay Server

```bash
python tools/viplestream-relay/relay_server.py --port 9999 --psk <your-psk>
```

For HTTPS/WSS termination, use Cloudflare Tunnel or nginx as a reverse proxy in front of port 9999.

### Client Configuration

In Moonlight settings → **Relay**:
- **Relay URL**: `wss://your-relay-domain/` (or `ws://host:9999` for plain)
- **Relay PSK**: must match `--psk` on the server

The PSK is stored in app preferences (not hardcoded). It is used for HMAC-SHA256 authentication; the raw PSK is never transmitted.

---

## NAT Traversal Protocol

```
Client                    Relay                     Server
  |                         |                         |
  |── register(uuid,role) ──►|◄── register(uuid,role)─|
  |                         |                         |
  |── lookup(server_uuid) ──►|                         |
  |◄── {stun_ip, stun_port} ─|                         |
  |                         |                         |
  |── http_proxy(/launch) ──►│──── http_proxy ────────►|
  |◄──────── 200 OK ─────────│◄───── response ─────────|
  |                         |                         |
  |── tcp_tunnel_open ──────►|◄── tcp_tunnel_open ─────|
  |◄═══════ RTSP tunnel ════►│◄════ RTSP tunnel ════════|
  |    (OPTIONS/DESCRIBE/    |    (7 TCP connections)  |
  |     SETUP×3/PLAY)        |                         |
  |                         |                         |
  |◄══════════════ UDP hole punch (video/audio) ═══════►|
```

---

## Developing on a second machine

Everything you need to build + deploy is in the repo; only the machine-specific
paths live outside. On the new machine:

```cmd
git clone https://github.com/finaltwinsen/VipleStream.git
cd VipleStream
copy build-config.template.cmd build-config.local.cmd
:: adjust the paths in build-config.local.cmd to wherever this machine has
::   MSVC vcvars64.bat, Qt 6.10 msvc2022_64, MSYS2, 7-Zip, Windows SDK D3D
```

Then either:

- **Run the whole build**: `build_all.cmd` → produces both server + client zips.
- **Only client incremental**: `build_moonlight.cmd` — qmake + nmake + zip,
  uses the same `scripts/build_moonlight_package.cmd` so shader shipping is
  consistent with `build_all.cmd`.
- **Only server**: `build_sunshine.cmd` — MSYS2 UCRT64 cmake + ninja build.

After a build, the release zip in `release/` can be extracted directly on the
same machine or scp'd to another one; there's no installer, just a folder of
binaries.

---

## Security Notes

- **PSK authentication**: Every WebSocket connection must present `HMAC-SHA256(psk, uuid)[:16]`. Without the correct PSK, the relay rejects registration.
- **No credentials in source**: All PSK and relay URL values are stored in local config files or app preferences — never hardcoded.
- **Trust-all SSL (Android relay only)**: The relay WebSocket connection uses a trust-all `SSLContext` on Android because the relay server's TLS is terminated by a reverse proxy. The Moonlight ↔ Sunshine game streaming channel continues to use the original Moonlight certificate pinning.
- **Sunshine pairing unchanged**: The Moonlight ↔ Sunshine PIN pairing and client certificate trust chain are not modified.

---

## Version History (highlights)

| Version | Changes |
|---------|---------|
| 1.1.80 | Workspace sync: root build wrappers tracked, canonical packaging script |
| 1.1.79 | Fix FRUC crash on iGPU + 4K display (Intel UHD TDR); cap FRUC to stream res |
| 1.1.78 | Fix FRUC silently disabled — preset shader .fxc files were missing from zip |
| 1.1.73 | Revert WaitForSingleObjectEx removal (GPU sync); add M1–M5 diagnostic logs |
| 1.1.67 | Fix FRUC frame-late feedback loop — std −76 %, p95 −48 % for Quality preset |
| 1.1.66 | Route `/launch` + `/cancel` through relay HTTP proxy in relay-only mode |
| 1.1.65 | Android NAT traversal port (RelayClient + RelayTcpTunnel) |
| 1.1.64 | RTSP multi-TCP tunnel fix; Sunshine last_session cache |
| 1.1.63 | Relay server online without STUN; lookup error fix |
| 1.1.62 | Native socket I/O in relay tunnel (fixes RTSP timeout 10060) |
| 1.1.61 | STUN host:port parsing fix; relay thread idle timeout |
| 1.1.60 | Full NAT traversal (PC): relay lookup + HTTP proxy + RTSP TCP tunnel |
| 1.1.5x | FRUC presets (quality/balanced/performance/ultra); adaptive bitrate |
| 1.1.4x | NVIDIA Optical Flow FRUC implementation |
| 1.1.3x | Initial VipleStream fork; Traditional Chinese UI |

---

## License

This project is based on:
- [Sunshine](https://github.com/LizardByte/Sunshine) — GPL-3.0
- [Moonlight](https://github.com/moonlight-stream/moonlight-qt) — GPL-3.0
- [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c) — LGPL-3.0

All modifications and additions are released under the same licenses as their respective upstream components.
