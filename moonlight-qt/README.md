# VipleStream (PC) — moonlight-qt fork

This directory contains the PC / desktop client build of VipleStream,
a fork of Moonlight's Qt client.

**Project:** [finaltwinsen/VipleStream](https://github.com/finaltwinsen/VipleStream)

**Upstream (GPL-3.0 attribution):** [moonlight-stream/moonlight-qt](https://github.com/moonlight-stream/moonlight-qt)

**VipleStream additions:**
- `app/backend/relaylookup.cpp/.h` — Relay server lookup + HTTP proxy
- `app/backend/relaytcptunnel.cpp/.h` — RTSP TCP tunnel via relay (native sockets)
- `app/streaming/video/ffmpeg-renderers/nvofruc.cpp/.h` — NVIDIA Optical Flow 2× FRUC
- `app/streaming/video/ffmpeg-renderers/genericfruc.cpp/.h` — Generic FRUC fallback
- `app/shaders/` — Motion estimation + warp compute shaders (Quality / Balanced / Performance)
- `app/settings/streamingpreferences.cpp/.h` — relay_url, relay_psk, FRUC preset settings
- `app/streaming/session.cpp` — STUN IP / relay fallback for stream connect
- `moonlight-common-c/src/Connection.c` — RtspAddrString global (RTSP host override)
- `moonlight-common-c/src/RtspConnection.c` — Use RtspAddrString for tunnel connect

See the [root README](../README.md) for full project documentation and build instructions.
