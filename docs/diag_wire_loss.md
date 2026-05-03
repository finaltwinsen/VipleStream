# Wire-loss diagnostics — pktmon SOP

If you suspect a streaming session is losing UDP video packets between
host and client, **capture both endpoints with pktmon first** before
tuning FEC %, SO_RCVBUF, NIC offload, or any other knob. If neither
endpoint reports missing packets, the loss is downstream of the NIC
(client OS UDP buffer / FEC reassembly / decoder back-pressure) and
those knobs do nothing. (See [`TODO.md`](./TODO.md) §J HEVC 1440p120
decoder-throughput cap for the worked example.)

`[VIPLE-NET] networkDropped` on the client is **not** packet loss — it's
a frame-number gap counter that fires whether the cause is wire loss,
FEC reassembly eviction, or decoder back-pressure. To rule wire loss in
or out, you need pktmon traces from both endpoints.

## Workflow

### 1. Find the host's physical NIC component id (one-time)

On the streaming host, in an Administrator PowerShell:

```powershell
pktmon list | findstr -i "Realtek Intel Killer"
```

Pick the line with your physical 1G/2.5G/10G NIC adapter (not the
virtual / Hyper-V / Tailscale / loopback ones). The id is the leading
number on the matching row. Without `--comp <id>` pktmon captures at
every NDIS layer (~5 layers in Windows 11) and you get 5× duplicate
packets in the trace — `analyze_wire_loss.py` handles dedup, but it's
cleaner to bind to the physical NIC.

### 2. Start capture on the host

```powershell
pktmon stop
pktmon filter remove
pktmon filter add 'video' -p 47998 --ip <CLIENT_LAN_IP>
pktmon start --capture --comp <NIC_ID> --pkt-size 96 `
             --file-name C:\temp\host_nic.etl
```

`-p 47998` is the Sunshine VIDEO_STREAM_PORT (= `sunshine.port` 47989 +
9 offset). `--pkt-size 96` keeps the capture small while preserving the
12-byte RTP header + a few payload bytes — enough for sequence-number
analysis.

### 3. Start capture on the client

In an **Administrator** PowerShell on the client:

```powershell
pktmon stop
pktmon filter remove
pktmon filter add 'video_rx' -p 47998 --ip <SERVER_LAN_IP>
pktmon start --capture --pkt-size 96 `
             --file-name C:\temp\client_rx.etl
```

(No `--comp` here — client NIC component id varies per machine and the
analyzer dedupes the multi-layer copies anyway.)

### 4. Run the streaming session for ~10–15 seconds

Pick a stable workload (no I-frame storms) — the analysis assumes a
roughly steady packet rate.

### 5. Stop both captures and convert to pcapng

```powershell
pktmon stop
pktmon etl2pcap C:\temp\host_nic.etl   --out C:\temp\host_nic.pcapng
pktmon etl2pcap C:\temp\client_rx.etl  --out C:\temp\client_rx.pcapng
```

### 6. Analyze each pcap

Run a tshark filter that extracts the RTP-style sequence number from
each video packet (first 2 bytes of the UDP payload starting at offset
2, big-endian — matches Sunshine / moonlight-common-c rtp.c layout),
dedupes multi-layer copies, and reports missing packets in the observed
sequence range. A reference Python implementation lives in the project
working directory under `scripts/diag/analyze_wire_loss.py` (not
tracked in git — copy / adapt locally). The shape of its output is:

```
File:                C:\temp\host_nic.pcapng
Filter:              ip.src == <SERVER> and udp.srcport == 47998
Raw packets:         13,525
Unique seqs:         13,525
Dup factor:          1.00x  (NDIS-layer copies; 1.0 = pktmon was started with --comp <id>)
Time window:         6.632s
Real pps:            2039.5
Seq range:           [0, 13524]  span = 13524
Expected packets:    13,525
Missing packets:     0
Loss rate:           0.000%
```

## Reading the results

| Output | What it means |
|---|---|
| `Dup factor: 1.00x` | Capture was bound to the physical NIC (`--comp` set) |
| `Dup factor: ~5x` | Multi-NDIS-layer capture; analyzer dedupes automatically |
| `Missing packets: 0` | Wire is clean at this point. Loss is downstream. |
| `Missing packets: > 0` | Real wire loss. Check NIC driver / cable / switch. |
| `Real pps:` matches expectation | At 120 fps × N pkts/frame; 1080p120 ≈ 1700 pps, 1440p120 ≈ 2700 pps |

If both endpoints report 0 missing, the loss is at one of:

- **Client OS UDP recv buffer**: check the client log for the
  `Actual receive buffer size: N (requested: M)` line. If `N` is much
  smaller than `M`, raise `DefaultReceiveWindow` in the registry (see
  [`troubleshooting.md`](./troubleshooting.md) "Frame stutter / pacer
  drops").
- **moonlight-common-c FEC reassembly**: if decoder back-pressure stalls
  RTP draining, partially-formed frames age out and you see frame gaps
  even with 0 packet loss. Look for `decodeMeanMs >> per-frame budget`
  (e.g. `> 8.3ms` for a 120 fps target).
- **Client decoder throughput**: see [`TODO.md`](./TODO.md) §J HEVC
  1440p120 decoder-throughput cap. SW HEVC 1440p caps ~50fps on most
  mobile CPUs. The `[VIPLE-NET-WARN]` log line fires automatically when
  the verified cap combo is detected (SW HEVC + ≥1440p + ≥90fps).

## Why concrete capture scripts aren't in the repo

Personal defaults (LAN IPs, NIC component ids, output paths) belong on
each developer's machine, not in a shared repo. Keep your local copies
in a working directory like `scripts/diag/` (gitignored) and rebuild
them from this SOP if you switch machines.
