# VipleStream

A custom game streaming solution based on [Sunshine](https://github.com/LizardByte/Sunshine) and [Moonlight](https://github.com/moonlight-stream), extended with built-in NAT traversal, frame interpolation (FRUC), and performance optimizations.

> **Current version:** 1.1.65

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

### Build

1. Copy `build-config.template.cmd` → `build-config.local.cmd` and set your local paths.

2. Build everything:
   ```cmd
   build_all.cmd
   ```
   Outputs are placed in `temp/sunshine/` and `temp/moonlight/`.

3. Deploy:
   ```cmd
   scripts\deploy_server.ps1    # Deploy Sunshine
   scripts\deploy_client.ps1    # Deploy Moonlight
   ```

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

## Security Notes

- **PSK authentication**: Every WebSocket connection must present `HMAC-SHA256(psk, uuid)[:16]`. Without the correct PSK, the relay rejects registration.
- **No credentials in source**: All PSK and relay URL values are stored in local config files or app preferences — never hardcoded.
- **Trust-all SSL (Android relay only)**: The relay WebSocket connection uses a trust-all `SSLContext` on Android because the relay server's TLS is terminated by a reverse proxy. The Moonlight ↔ Sunshine game streaming channel continues to use the original Moonlight certificate pinning.
- **Sunshine pairing unchanged**: The Moonlight ↔ Sunshine PIN pairing and client certificate trust chain are not modified.

---

## Version History (highlights)

| Version | Changes |
|---------|---------|
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
