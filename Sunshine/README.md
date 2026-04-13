# Sunshine — VipleStream Fork

This directory contains the VipleStream-modified Sunshine server.

**Upstream:** [LizardByte/Sunshine](https://github.com/LizardByte/Sunshine) — GPL-3.0

**VipleStream additions:**
- `src/relay.cpp/.h` — WebSocket relay signaling + RTSP TCP tunnel server
- `src/stun.cpp/.h` — STUN prober for public IP / NAT type detection
- `src/rtsp.cpp` — `last_session` cache for multi-TCP relay tunnel
- `src/config.cpp/.h` — `relay_url`, `relay_psk`, `stun_server` settings
- `src/nvhttp.cpp` — expose `StunEndpoint`/`StunNatType` in `/serverinfo`
- `src/video.cpp` — adaptive bitrate logic
- `src_assets/.../Network.vue` — relay + STUN settings in web UI

See the [root README](../README.md) for full project documentation and build instructions.
