# WS UDP Tunnel — Stage Milestone (2026-04-19)

Checkpoint before parking the WS-carrier tunnel exploration. Captures
what landed, what didn't, and the residual issues so the next pass
(or the Android port, or a different transport) starts with context.

## TL;DR

End-to-end **tunneling works**: video + audio + ENet control all ride
a relay-mediated carrier (UDP preferred, WebSocket binary-frame
fallback) when direct UDP between client and server is blocked.
Signaling, /launch, /cancel and RTSP handshake all go through the
relay's HTTP + TCP tunnel paths. Fresh tunnel streams produce a
decodable picture.

What it still **can't** do well: sustain low latency under motion
bursts when the WS carrier is backing onto a Cloudflare Tunnel path.
The Cloudflare edge + TCP ACK clocking is a poor fit for bursty
low-latency video, and no amount of client-side queue management
fully hides that. This is a transport-level limit, not a bug we can
iterate out of with more drop gates.

## What works

- **Relay signaling plane** (`tools/viplestream-relay/relay_server.py`):
  peer registration, UUID-keyed lookup, STUN endpoint exchange,
  http_proxy, tcp_tunnel, udp_tunnel_allocate with role a/b, idle
  flow reaper, per-flow stats reporter. Pure stdlib asyncio, no
  external deps. TLS-terminated at Cloudflare Tunnel in production.
- **UDP tunnel datagram plane**: HMAC-SHA256[:16] header with
  `VP` magic + flow_id + src/dst port, TOFU-pinned per side, cross-
  mode forwarding (UDP↔WS) in the relay.
- **WS binary-frame fallback carrier** (`Sunshine/src/tunnel_session.cpp`
  + `moonlight-qt/app/backend/relayudptunnel.cpp`): when a peer's
  UDP to relay is blocked, the same datagrams ride WS binary frames
  over the existing signaling TCP. Zero config switchover.
- **ENet loopback bridge** for control-stream tunneling: shim socket
  on the server bound to 127.0.0.1:ephemeral, learns the client's
  ENet port from the first packet, relays replies back.
- **Ephemeral-port proxies + LiOverrideUdpPorts**
  (`moonlight-common-c`): the video/audio/control ports inside the
  tunnel are OS-assigned, sidestepping Hyper-V / winnat stealth
  reservations on 47998–48000. RTSP rewriting didn't work because
  Sunshine uses corever=1 encrypted RTSP; the override API runs
  post-decrypt in common-c and does.
- **Pre-launch /cancel via relay http_proxy** recovers from stuck
  sessions (forced-exit leaves `proc::proc.running() > 0` otherwise;
  next /launch returns 400 "An app is already running").
- **Partial-WS-frame bug fix** (v1.1.127): removing SO_SNDTIMEO on
  the Sunshine relay socket and looping on short sends dropped the
  server-side binary tunnel drop rate from 24% peak → ~0%.
  SO_SNDTIMEO was the underlying reason for most prior reconnect
  churn — timed-out sends left truncated WS frames on the wire that
  desynced the parser.

## What doesn't

### 1. Low-latency video over Cloudflare Tunnel WS

At 1.5 Mbps cap + 32 KB backlog drop gate the picture stays alive,
but continuous mouse motion still visibly inflates latency (1–3 s
range). Static scenes: fine. Motion: degraded.

This is fundamental. The Cloudflare Tunnel carries WS as TLS-over-
TCP. A motion-burst of video hits the Cloudflare edge's sustainable
rate briefly, the TCP queue grows, every queued byte is visible
latency, ACK clocking slows the sender just enough to unstall — by
which point a second's worth of frames are late. Dropping at the
backlog gate keeps things alive but doesn't eliminate the queue.

**What would help**:
- Replace the WS carrier with a genuinely UDP-reachable relay
  endpoint (i.e. relay hosted somewhere that isn't fronted by
  Cloudflare Tunnel, since CF Tunnel doesn't proxy UDP). The WS
  fallback stays for cases where the client's ISP blocks UDP
  altogether, but UDP should be the common case.
- Or swap TLS-over-TCP for a QUIC carrier on the signaling + binary
  plane — loss tolerance without HoL blocking.
- Or accept the bitrate ceiling: at 720p30, ~800 kbps with a good
  encoder is probably the honest sustainable rate on this path.

### 2. Stale-session recovery isn't fully clean

Pre-launch /cancel reliably clears `proc::proc.running()` and
terminates RTSP sessions. But a handful of times after a forced
exit, the second /launch still failed or the next connect stalled
waiting for video that never arrived. The surviving state is likely
somewhere in the Sunshine encoder / display-config path (display
revert happens on session teardown, and if the teardown was cut
short by the client's WS carrier freezing, it may be half-done).

### 3. Client crash under extreme packet loss

One reproducible access violation (0xC0000005) at
`0x7FFAD773C0D5` inside an unnamed 2.5 MB module during a
"Waiting for IDR frame" storm. No PDBs shipped with the release
build so we couldn't resolve it; the timing makes the decoder / the
ffmpeg wrapper the obvious suspects. Lowering bitrate and tightening
the drop gate (v1.1.129) keeps the decoder queue shallower and so
far hasn't reproduced the crash, but the underlying bug is latent —
build with PDBs next time we need to debug it.

## Iteration log (what moved the needle, what didn't)

| Round | Change | Effect |
|------:|--------|--------|
| R1 | TCP_NODELAY both ends | Needed. Latency floor dropped visibly. |
| R2 | Backlog drop gates (client + relay) | Kept runaway latency from permanently wedging the socket. |
| R3 | 4 MiB SO_SNDBUF on client | **Reverted.** Hid backlog from bytesToWrite(), gates never fired, 20 s+ accumulated latency. |
| R4 | Per-flow stats reporter in relay + drop counters on Sunshine | No behavior change but revealed ~48% server-side drops at 5 Mbps — confirmed Cloudflare path couldn't sustain it. |
| R5 | 2 ms SO_SNDTIMEO | **Harmful.** Caused false reconnect storms because text control frames also hit the timeout. |
| R5.5 | 10 ms SO_SNDTIMEO | Fewer false reconnects, but still wrong (see R6). |
| R6 (v1.1.127) | Remove SO_SNDTIMEO, loop on short sends, try-lock binary | **Big win.** Drop rate 24% peak → 0%. Killed the reconnect churn. |
| R7 (v1.1.128) | Pre-launch /cancel via relay proxy | Fixes stuck "an app is already running" after forced exit. |
| R8 (v1.1.129) | 1.5 Mbps cap, 32 KB backlog | Picture no longer freezes on motion. Latency still spikes. |

## Residual known issues

- Motion-induced latency inflation over the WS carrier (transport-
  level — see above).
- Forced-exit recovery is mostly clean but not always.
- Decoder crash under sustained packet loss (needs a PDB build to
  root-cause).

## Where the state lives

- Server: `Sunshine/src/relay.cpp`, `Sunshine/src/tunnel_session.cpp`,
  tunnel hook-in at `Sunshine/src/stream.cpp` (session::join,
  maybe_start_tunnel).
- Client: `moonlight-qt/app/backend/relayudptunnel.cpp`,
  `moonlight-qt/app/streaming/session.cpp` (tryRelayLaunch, tunnel
  setup, bitrate cap).
- Wire format: `Sunshine/src/udp_tunnel.h` (shared header, also
  vendored into the client).
- common-c tunnel-port overrides:
  `moonlight-qt/moonlight-common-c/moonlight-common-c/src/Connection.c`
  (LiOverrideUdpPorts).
- Relay: `tools/viplestream-relay/relay_server.py` +
  README / systemd unit.

## Picking this back up

The cleanest next step is to stop trying to squeeze low-latency
video through Cloudflare-fronted TCP. Either:

1. Host a second relay endpoint somewhere UDP-reachable (not behind
   Cloudflare Tunnel) and prefer that for the datagram plane. Keep
   the CF-fronted endpoint for signaling + as a last-resort WS
   carrier.
2. Try QUIC for the binary-frame plane. Keeps the TLS+single-port
   ergonomics of the WS carrier but decouples frames from HoL
   blocking.

Both decouple the *signaling* story (Cloudflare Tunnel works great
for that) from the *media* story (Cloudflare Tunnel does not).
