# VipleStream

A self-hosted game-streaming stack — a fork of [Sunshine](https://github.com/LizardByte/Sunshine) (host) and [Moonlight](https://github.com/moonlight-stream) (clients) with built-in NAT traversal, AI frame interpolation, Steam library auto-import, and a Traditional Chinese UI. Wire-protocol-compatible with vanilla Sunshine / Moonlight so VipleStream and upstream installs interoperate.

> **Current version:** 1.5.108 — see [Releases](https://github.com/finaltwinsen/VipleStream/releases) for downloads.

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
