# Moonlight Android — VipleStream Fork

This directory contains the VipleStream-modified Moonlight Android client.

**Upstream:** [moonlight-stream/moonlight-android](https://github.com/moonlight-stream/moonlight-android) — GPL-3.0

**VipleStream additions:**
- `app/src/main/java/com/limelight/relay/RelayClient.java` — WebSocket relay client (lookup, HTTP proxy, RFC 6455, HMAC-SHA256 PSK)
- `app/src/main/java/com/limelight/relay/RelayTcpTunnel.java` — RTSP TCP tunnel thread (multi-connection with reconnect protocol)
- `app/src/main/java/com/limelight/nvstream/NvConnection.java` — Relay launch path + tunnel lifecycle
- `app/src/main/java/com/limelight/nvstream/ConnectionContext.java` — relayUrl, relayPsk, serverUuid, stunAddress fields
- `app/src/main/java/com/limelight/nvstream/http/NvHTTP.java` — Parse StunEndpoint/StunNatType
- `app/src/main/java/com/limelight/computers/ComputerManagerService.java` — Relay fallback poll
- `app/src/main/java/com/limelight/preferences/PreferenceConfiguration.java` — relay_url, relay_psk
- `app/src/main/res/xml/preferences.xml` — Relay settings UI
- `app/src/main/jni/moonlight-core/moonlight-common-c/src/` — RtspAddrString cherry-pick
- `app/src/main/assets/shaders/` — FRUC compute shaders (OpenGL ES)

See the [root README](../README.md) for full project documentation and build instructions.
