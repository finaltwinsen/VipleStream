# VipleStream

A self-hosted game-streaming stack — a fork of [Sunshine](https://github.com/LizardByte/Sunshine) (host) and [Moonlight](https://github.com/moonlight-stream) (clients) with built-in NAT traversal, AI frame interpolation, Steam library auto-import, and a Traditional Chinese UI. Wire-protocol-compatible with vanilla Sunshine / Moonlight so VipleStream and upstream installs interoperate.

> **Current version:** 1.2.123 — see [Releases](https://github.com/finaltwinsen/VipleStream/releases) for downloads.

Project home: <https://github.com/finaltwinsen/VipleStream>

---

## Components

| Component | Replaces | Binary | Install path (Windows) |
|---|---|---|---|
| **VipleStream-Server** | Sunshine | `viplestream-server.exe` + `viplestream-svc.exe` | `C:\Program Files\VipleStream-Server\` |
| **VipleStream** (PC client) | Moonlight-Qt | `VipleStream.exe` | `C:\Program Files\Moonlight Game Streaming\` |
| **VipleStream Android** | Moonlight-Android | application id `com.piinsta` | sideload-only APK |
| **viplestream-relay** | — | `relay_server.py` | run on any always-on host |

The wire protocol on `_nvstream._tcp` mDNS, `/serverinfo` `/launch` `/applist`, RTSP, and the client cert chain are unchanged from upstream. A VipleStream client can connect to a vanilla Sunshine host, and a vanilla Moonlight client can connect to a VipleStream-Server host — VipleStream-only features (Steam profile dropdown, FRUC backends, etc.) are quietly hidden when the peer doesn't advertise the `<VipleStreamProtocol>` capability marker.

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

### Frame Rate Up-Conversion (FRUC)
2× frame interpolation, three independent backends with auto-cascade selection on PC client.

| Backend | Algorithm | GPU requirement | Notes |
|---|---|---|---|
| **Generic** | HLSL motion-warp shaders | any D3D11 GPU | default; ~1-2 ms / frame |
| **NvOFFRUC** | NVIDIA Optical Flow SDK | RTX 20+ series, driver ≥ 522 | hardware accelerated |
| **DirectML** | RIFE 4.25-lite ONNX, fp32 | strong GPU (RTX 30/40, RX 6000+) | auto-cascade probes 720p→540p→480p→360p at init and picks the highest the GPU can sustain; falls back to Generic if no resolution fits the frame budget |

DirectML diagnostics:
- `VIPLE_DIRECTML_DEBUG=1` — enables D3D12 debug layer + DML validation
- `VIPLE_DML_RES=540|720|1080|native` — overrides tensor resolution
- `[VIPLE-FRUC-DML]` log lines print per-stage EMA timings every 120 frames

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

### Other
- Traditional Chinese UI translation (`qml_zh_TW.ts`) on PC client; Android pulls system locale
- Web UI English + Traditional Chinese strings rebranded to VipleStream
- Editorial-style PcView / AppView design with IBM Plex Mono + Space Grotesk + lime accent (`#D4FF3A`)
- `/serverinfo` advertises `<VipleStreamProtocol>` capability marker so clients can hide VipleStream-only features when connected to vanilla Sunshine
- Adaptive bitrate, network-status overlay (LAN / Relay / DERP), debug pairing tool (ADB-based PIN-free)

---

## Architecture

```
┌──────────────────────┐        relay or direct       ┌──────────────────────┐
│  VipleStream client  │ ◄──────────────────────────► │  VipleStream-Server  │
│  (PC / Android)      │                              │  (Windows)           │
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

## Repository layout

```
VipleStream/
├── Sunshine/                          # Server (C++ / CMake / MinGW UCRT64)
│   ├── src/
│   │   ├── nvhttp.cpp                 # GameStream HTTP + steam profile/switch endpoints
│   │   ├── relay.cpp / stun.cpp       # NAT traversal server-side
│   │   ├── rtsp.cpp                   # RTSP with last_session cache for tunnel reuse
│   │   ├── platform/windows/
│   │   │   └── steam_scanner.{cpp,h}  # Steam library scanner (loginusers/libraryfolders/appmanifest)
│   │   └── tools/viple_splash.cpp     # launch splash overlay (avoids "launching..." desktop leak)
│   └── tools/sunshinesvc.cpp          # service helper (spawns viplestream-server.exe in user session)
├── moonlight-qt/                      # PC client (C++ / Qt6 / MSVC)
│   └── app/
│       ├── backend/
│       │   ├── nvhttp.cpp             # HTTP client + steam profile/switch endpoints
│       │   ├── nvcomputer.cpp         # adds isVipleStreamPeer flag
│       │   ├── relaylookup.cpp        # relay lookup + HTTP proxy
│       │   └── relaytcptunnel.cpp     # RTSP TCP tunnel (native sockets)
│       ├── gui/
│       │   ├── AppView.qml            # apps grid + Steam profile dropdown + busy overlay
│       │   └── appmodel.cpp           # /steamprofiles fetch + async switch poll loop
│       └── streaming/video/ffmpeg-renderers/
│           ├── nvofruc.cpp            # NVIDIA Optical Flow FRUC backend
│           ├── directmlfruc.cpp       # DirectML RIFE backend with auto-cascade
│           └── d3d11va.cpp            # D3D11 renderer integration
├── moonlight-android/                 # Android client (Java / Gradle + NDK)
│   └── app/src/main/java/com/limelight/
│       ├── nvstream/http/
│       │   ├── NvHTTP.java            # HTTP client + steam endpoints
│       │   ├── SteamProfile.java      # profile data class
│       │   └── SteamSwitchStatus.java # async-task status data class
│       ├── relay/                     # NAT traversal Java port
│       └── AppView.java               # apps grid + Spinner + SpinnerDialog
├── tools/viplestream-relay/
│   └── relay_server.py                # PSK-authenticated WebSocket relay
├── scripts/
│   ├── version.ps1                    # propagate version.json to all subprojects
│   ├── build_moonlight_package.cmd    # canonical shader/DLL/windeployqt list
│   ├── deploy_client.ps1              # local install for moonlight-qt
│   └── benchmark/                     # FRUC quality + latency harness
├── build_all.cmd                      # one-command build (server + Qt client; auto-bumps patch)
├── build_sunshine.cmd                 # server only
├── build_moonlight.cmd                # Qt client only
├── build_android.cmd                  # Android APK only (debug-signed)
├── build-config.template.cmd          # copy → build-config.local.cmd, fill paths
└── version.json                       # single source of truth for version number
```

---

## Build

> **Rule:** always go through the build scripts. They handle version propagation, qmake regeneration, shader manifests, windeployqt, PDB collection, CPack zipping, and APK signing — all things that quietly break when invoked piecewise. See [`docs/building.md`](docs/building.md) for full details.

### Prerequisites

| Build target | Requires |
|---|---|
| Server (`build_sunshine.cmd`) | MSYS2 + MinGW UCRT64 (GCC 13+), CMake 3.25+, Node 18+ (web UI assets) |
| Qt client (`build_moonlight.cmd`) | Visual Studio 2022 Build Tools (MSVC), Qt 6.10+ (`msvc2022_64`), 7-Zip |
| Android (`build_android.cmd`) | Android Studio or Gradle 8+, Android NDK r26+, JDK 17+ |
| Relay server | Python 3.10+ (stdlib only) |

### First-time setup

```cmd
git clone https://github.com/finaltwinsen/VipleStream.git
cd VipleStream
copy build-config.template.cmd build-config.local.cmd
notepad build-config.local.cmd
```

`build-config.local.cmd` is gitignored — it holds `QT_DIR`, `VCVARS`, `MSYS2`, `SEVENZIP`, etc. Without it the build scripts fail fast with `[ERROR] build-config.local.cmd not found`.

### Common operations

| Goal | Command |
|---|---|
| Build server + Qt client (auto-bump patch) | `build_all.cmd` |
| Build server only | `build_sunshine.cmd` |
| Build Qt client only | `build_moonlight.cmd` |
| Build Android APK only (no bump) | `build_android.cmd` |
| Sync `version.json` to all subprojects (no bump, no build) | `pwsh scripts\version.ps1 propagate` |
| Show current version | `pwsh scripts\version.ps1 get` |
| Deploy a fresh Qt client to local install | `scripts\deploy_client_now.cmd` |

Outputs land in `release/`:

```
release/VipleStream-Server-1.2.123.zip   (~33 MB)
release/VipleStream-Client-1.2.123.zip   (~106 MB)
release/VipleStream-Android-1.2.123.apk  (~6 MB)
```

### Server deploy (Windows host)

`temp/deploy_sunshine.ps1` (or your own variant) handles the host-side deploy: copies the zip contents to `C:\Program Files\VipleStream-Server\`, registers the `VipleStreamServer` service, force-kills any straggler `viplestream-server.exe` (which holds an open handle on its own .exe, blocking `Copy-Item`), and migrates a legacy `C:\Program Files\Sunshine\` install if present.

### Relay server

```bash
python tools/viplestream-relay/relay_server.py --port 9999 --psk <your-psk>
```

For HTTPS/WSS termination, run behind Cloudflare Tunnel or nginx. Then in the client's **Settings → Relay**:

- **Relay URL:** `wss://your-relay-domain/`
- **Relay PSK:** must match `--psk` on the server

The PSK is HMAC-only — the raw value is never transmitted.

---

## NAT traversal protocol

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
  |◄══════════════ UDP hole-punch (video/audio) ═══════►|
```

---

## Steam switch protocol (v1.2.119+)

```
Client                                Server
  |                                     |
  |── GET /steamprofiles ──────────────►| (returns <profile> list + current_user, fresh-read)
  |◄──────── 200 + XML ──────────────────|
  |                                     |
  |── GET /steamswitch?account=X ──────►| (creates SteamSwitchTask, spawns worker thread)
  |◄────── 202 + task_id ────────────────| (returns within ~50 ms)
  |                                     |
  |    (worker thread runs:             |
  |     count_steam_processes →         |
  |     -shutdown + poll →              |
  |     kill_steam stragglers →         |
  |     -login <account> →              |
  |     poll ActiveUser till target)    |
  |                                     |
  |── GET /steamswitch/status?id=X ────►| (every ~1 s)
  |◄──── 200 + state + message ──────────|
  |        ...                          |
  |── GET /steamswitch/status?id=X ────►|
  |◄──── 200 + state=done ───────────────| (or state=error)
```

States: `starting` → `shutting_down` → `logging_in` → `done` | `error` | `already_active`. Tasks are GC'd 60 s after reaching a terminal state.

---

## Working on a second machine

Everything needed to build + deploy is in the repo; only machine-specific paths live outside.

```cmd
git clone https://github.com/finaltwinsen/VipleStream.git
cd VipleStream
copy build-config.template.cmd build-config.local.cmd
:: edit paths: MSVC vcvars64.bat, Qt 6.10 msvc2022_64, MSYS2, 7-Zip, Windows SDK D3D
build_all.cmd
```

The release zips in `release/` can be extracted directly on the same machine or `scp`'d to another — no installer, just a folder of binaries.

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
| **1.2.123** | Android Steam-switch dropdown reaches feature parity with PC client; fix OkHttp `addPathSegment` URL-encoding `/` to `%2F` (use `addPathSegments` plural) |
| **1.2.119** | `/steamswitch` rewritten async — server returns 202+task_id immediately, client polls `/steamswitch/status`; eliminates spurious "host disconnected" during the 9 s Steam restart |
| **1.2.118** | Fix root cause of permanently empty `current_user`: `CreateProcessAsUserW` doesn't load the user profile, so HKCU points at `.DEFAULT`; switched to walking `HKEY_USERS` subhives |
| **1.2.117** | Steam switch force-kills straggler `steam.exe` + `steamwebhelper.exe`; detects Steam Guard 2FA prompts |
| **1.2.108–116** | Steam profile dropdown UI on PC client (Qt) — fetch / populate / async switch / busy overlay |
| **1.2.93–96** | VipleStream rebrand: Sunshine→VipleStream-Server (`viplestream-server.exe` + `viplestream-svc.exe`), Moonlight-Qt→VipleStream (`VipleStream.exe`), Android `applicationId com.piinsta`; firewall rules / mDNS instance name / NVAPI profile / Linux `.desktop` ID all updated; `<VipleStreamProtocol>` capability marker added |
| **1.2.86–91** | DirectML auto-cascade probe (720p→540p→480p→360p) + Generic fallback; default backend set to Generic |
| **1.2.82–86** | DirectML reset-race fix: fence-gated allocator reset, multi-ring slots, submitFrame 50 ms → 3 ms |
| **1.2.67** | Steam library auto-import (Phase 2): host scans Steam install + injects games into `/applist` |
| **1.2.59–64** | DirectML RIFE backend ships real model; Option C interop optimization; `VIPLE_DML_RES` env var |
| **1.1.60–80** | Full NAT traversal (PC + Android): relay lookup, HTTP proxy, RTSP TCP tunnel, UDP hole-punch |
| **1.1.40s–50s** | NVIDIA Optical Flow FRUC; presets (quality/balanced/performance/ultra); adaptive bitrate |
| **1.1.30s** | Initial VipleStream fork; Traditional Chinese UI |

Full per-version notes: `git log --oneline`.

---

## License

VipleStream is licensed under **GPL-3.0** ([LICENSE](LICENSE)) — the same license as its upstream components, as required for derivative works:

- [Sunshine](https://github.com/LizardByte/Sunshine) — GPL-3.0
- [Moonlight](https://github.com/moonlight-stream/moonlight-qt) — GPL-3.0
- [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c) — LGPL-3.0

The `moonlight-common-c` portions remain under LGPL-3.0; everything else (server, Qt client, Android client, build scripts, relay server) is GPL-3.0. Per GPL-3.0 you may run, study, share, and modify the software, including for commercial use, provided derivative works are distributed under the same terms with source code available.
