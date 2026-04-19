# VipleStream Signaling Relay

Lightweight WebSocket signaling + UDP-tunnel relay that brokers NAT traversal
between a VipleStream Sunshine server and a Moonlight client.

## Contents

- `relay_server.py` — the relay process itself (pure stdlib asyncio, no external
  dependencies). Handles peer registration, UUID lookup, STUN endpoint exchange,
  HTTP proxying, TCP tunnel forwarding, and UDP tunnel allocation + forwarding
  with a WebSocket-binary-frame fallback.
- `test_udp_tunnel.py` — spins up a relay + two fake peers and verifies the
  full UDP tunnel datagram plane end-to-end.
- `test_ws_fallback.py` — same shape, but forces one peer onto the WS binary
  carrier to exercise the UDP↔WS cross-mode path.

## Running

```bash
# open relay, no auth, bind everything to the default ports
python3 relay_server.py

# typical production invocation behind Cloudflare Tunnel
python3 relay_server.py \
    --host 0.0.0.0 \
    --port 9999 \
    --udp-port 9998 \
    --udp-advertise-host sunshine.example.com \
    --psk "$RELAY_PSK"
```

Ports:

- `9999/tcp` — WebSocket signaling + WS-binary tunnel fallback. This is the
  only port that needs to be reachable from both peers; everything else can
  be stealth.
- `9998/udp` — direct UDP tunnel datagram plane. Optional: if UDP is blocked
  between a peer and the relay, that peer falls back to the WS binary carrier
  on 9999 automatically.

When fronted by Cloudflare Tunnel, `--udp-advertise-host` should point to the
public hostname peers should try for direct UDP before falling back to WS —
Cloudflare Tunnel does not proxy UDP, so in that deployment the UDP leg is
typically unreachable and peers will auto-fall-back to WS.

## Protocol

See the docstring at the top of `relay_server.py` for the on-wire message
shapes and the UDP-tunnel datagram header format.

## Tests

```bash
python3 test_udp_tunnel.py
python3 test_ws_fallback.py
```

Both scripts run a relay in-process, drive two clients through a full
allocate-and-forward exchange, and exit 0 on success.
