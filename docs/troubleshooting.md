# VipleStream — Troubleshooting

Common failure modes + the actual fix. Symptom → cause → fix.

For setup from scratch see [`setup_guide.md`](./setup_guide.md).

---

## Pairing issues

### Client says "PIN mismatch" or "Pairing timed out"

- **Cause**: Server's TLS cert generation can take 5-10 sec on first
  pair. The 4-digit PIN expires after 60 sec.
- **Fix**: Retry. If repeated failure, check server's `sunshine.log` for
  `Certificate generation failed` / `OpenSSL` errors and reinstall the
  server.

### Client can't see server in mDNS discovery

- **Cause**: Multicast traffic is blocked on Windows Firewall (default
  for "Public" network profile) or on a managed switch.
- **Fix**: Either enter the server's IP manually in the client, or set
  the network profile to "Private" via Settings → Network. Sunshine
  doesn't strictly need mDNS — only for auto-discovery.

### `Failed to authenticate certificate` on connect

- **Cause**: Server cert was regenerated (e.g., reinstall) but client
  still has the old fingerprint cached.
- **Fix**: On the client, remove the paired server entry and re-pair.

---

## Stream quality / performance

### Frame stutter / pacer drops

Symptoms: Client overlay shows non-zero `pacerDroppedFrames` or
`networkDroppedFrames`.

- **Cause A — pacerDropped > 0**: Client renderer can't keep up with
  decoded frame rate. Usually GPU contention from another app.
- **Fix A**: Close GPU-intensive background apps (browsers with
  hardware-accelerated video, OBS, streaming software). On laptops,
  ensure power plan = High Performance.

- **Cause B — networkDropped > 0**: UDP packets lost between server
  and client.
- **Fix B**: First confirm with `ping <server-ip>` showing < 1 ms RTT
  + 0% loss on LAN. If LAN ping is fine but stream still drops:
    - Try a different codec (H.264 has smallest per-frame packet count
      and is most resilient to bursty drop)
    - Bump server FEC % via `sunshine.conf` `fec_percentage = 20`
      (default 10) for lossy LANs
    - On the client side, check the log for the line `Actual receive
      buffer size: N (requested: M)`. If `actual` is much smaller than
      `requested`, the OS clamped your SO_RCVBUF. Raise via:
        - Windows: `HKLM\System\CurrentControlSet\Services\AFD\Parameters\DefaultReceiveWindow` DWORD = `8388608` (8 MB), reboot
        - Linux: `sudo sysctl -w net.core.rmem_max=33554432`

### HEVC 1440p120 only delivers ~50 fps when using Vulkan + FRUC

This is **not** packet loss or a server-side encoder problem — verified
via pktmon on both ends (server NIC TX 0 loss, client NIC RX 0 loss,
2026-05-03 measurement). It's a client-side **SW HEVC decoder
throughput cap** in the VkFrucRenderer SW upload path.

- **Cause**: When you select Renderer = Vulkan + FRUC enabled, the FRUC
  ME/warp shaders need CPU-side frame access for staging upload, so HW
  decode (NVDEC/DXVA) is bypassed and libavcodec SW HEVC runs instead.
  At 1440p HEVC the SW decoder tops out at ~50 fps on most mobile CPUs.
  H.264 / AV1 SW decoders are faster and pass 1440p120; HW decode (the
  default D3D11 renderer's path) handles 1440p120 HEVC easily.
- **Self-diagnose**: Look for `[VIPLE-NET-WARN]` in the log — it fires
  at startup for the verified cap combo (SW HEVC + ≥1440p + ≥90fps),
  and again at runtime if `received fps < 75% × target` for 5s.
- **Fix (pick one)**:
    - Switch to default D3D11 renderer (Settings → Renderer). Uses
      NVDEC/DXVA HW decode, full 120fps.
    - Stay on Vulkan + FRUC, drop to 1080p120 (passes on all 3 codecs).
    - Stay on Vulkan + FRUC, switch codec to H.264 or AV1 (both pass
      1440p120 in SW decode).
    - Stay on Vulkan + FRUC + HEVC + 1440p, drop frame rate to 60fps.

See [`TODO.md`](./TODO.md) §J HEVC 1440p120 decoder-throughput cap for
the full diagnosis trail.

### Stream black-screens / freezes after 10-30 seconds

- **Cause**: Server-side display device manipulation (`dd_*` config) is
  trying to switch the host display to a mode it doesn't support, then
  reverting. On reverting, the stream chain reinitializes.
- **Fix**: Set `sunshine.conf` `dd_configuration_option = disabled` to
  skip display-mode switching entirely.

### Decoder reports `Test decode failed`

- **Cause**: Client-side codec is missing (FFmpeg DLL absent / wrong
  version) or the host is offering a codec the client doesn't support.
- **Fix**: Reinstall the client (the release zip bundles required
  FFmpeg DLLs). If you're a dev: check `temp/moonlight/avcodec-*.dll`
  / `swscale-*.dll` exist after `build_moonlight.cmd`.

---

## Vulkan / FRUC

### `[VKBE-VK_ERROR_DEVICE_LOST]` in Android log

- **Cause**: Adreno / Mali driver bug interacting with VK_KHR_video_decode
  or per-slot resource pattern. Usually NVIDIA 596.36 or older Adreno
  6xx drivers.
- **Fix**: Update GPU driver. If the device-lost recurs at the same
  frame number after driver update, file an issue with the
  `%TEMP%\VipleStream-aftermath-*.nv-gpudmp` dump attached.

### Desktop FRUC: `Path C cascade — falling back to no-FRUC`

- **Cause**: DirectML inference time exceeds 14 ms budget on this GPU.
  Common on A1000 / GTX 16-series / Iris Xe (low-end).
- **Fix**: Working as designed — VipleStream falls back to direct render
  without interpolation. To re-enable forcibly, set env
  `VIPLE_DIRECTML_FORCE=1` (will visibly stutter on slow GPUs).

### Android FRUC dual mode never engages on 60 fps stream

- **Cause**: The dual-mode entry threshold is `2 × input ≤ display ×
  1.40`. At 60 fps source on 60 Hz display, `120 > 84` — single mode is
  selected (FRUC effectively off). At 60 fps on 90 Hz display, `120 ≤
  126` — dual mode engages and you see interp frames.
- **Fix**: For dual-mode FRUC value, you need a panel with `display >
  input × 2 / 1.40`. Pixel 5 (90 Hz) qualifies; most laptops + budget
  Android phones don't. See [`TODO.md`](./TODO.md) §I.D for hardware
  table.

### `[VKBE-D21] init clear: cross-queue handoff OK ...` not appearing

- **Cause**: You're not on a Vulkan FRUC backend — using GLES backend or
  D3D11 fallback.
- **Fix**:
    - Android: `adb shell setprop debug.viplestream.vkprobe 1`, then
      restart the stream.
    - Desktop: Settings → "Renderer" → choose "RS_VULKAN".

---

## Server install / service

### `viplestream-svc.exe` won't start as a service

- **Cause**: Service binary path has spaces and isn't quoted in the
  `sc create` command, OR the service account lacks permission to read
  the install directory.
- **Fix**: Use `sc create VipleStreamServer binPath= "C:\Program Files\VipleStream-Server\tools\viplestream-svc.exe" start= auto`
  (note the space after `binPath=` is required by `sc.exe`).

### Web UI returns 401 even with correct password

- **Cause**: Web UI realm ("VipleStream-Server Web UI") changed in
  v1.3.309. Browser cached old realm credentials.
- **Fix**: Clear cached HTTP credentials in browser, OR use a fresh
  incognito/private window.

### Service starts but `sunshine.log` is empty

- **Cause**: Service is running as `LocalSystem` and the config dir is
  ACL-restricted to a different user.
- **Fix**: `icacls "C:\Program Files\VipleStream-Server\config" /grant
  "NT AUTHORITY\SYSTEM":(OI)(CI)F`. Restart the service.

---

## Audio

### No audio in stream

- **Cause**: Sunshine captures from the system default audio device. If
  the host is headless / has no sound device, capture returns silence.
- **Fix**: Either install a virtual audio loopback driver (VB-CABLE,
  Voicemeeter Banana), or set `sunshine.conf` `audio_sink = ` to a
  specific device name.

### Audio crackling / popping during stream

- **Cause**: Client-side audio buffer underflow. Common when client CPU
  is pinned by the decode thread.
- **Fix**: In client Settings → Audio, switch from "Stereo" to "5.1" if
  your output device supports it (forces a different SDL audio path with
  bigger buffers). Or raise audio buffer ms (Advanced settings).

---

## Logs

| Log | Default path |
|---|---|
| Server (Sunshine) | `C:\Program Files\VipleStream-Server\config\sunshine.log` |
| Windows client | `%TEMP%\VipleStream-*.log` (one per session, latest by mtime) |
| Android client | Available via `adb logcat -d`, filter on `com.piinsta`, `VKBE`, or `VIPLE-` tags |

For server-side packet flow tracing, the [VIPLE-NVENC-RATE] /
[VIPLE-BCAST-RATE] log lines show end-to-end fps + Mbps every 5 sec.
For client-side network reception stats, look for [VIPLE-NET] — emits
once per second with `received / decoded / networkDropped / total`.

If filing a bug, paste the relevant log slice (10-30 sec window around
the issue) — full logs can be 100 MB+ and aren't useful unless trimmed.
