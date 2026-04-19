#!/usr/bin/env python3
"""End-to-end test for the relay UDP tunnel.

Flow:
    1. Start relay_server in a child asyncio task.
    2. Peer A and Peer B both connect via WS, register, exchange PSK auth.
    3. Peer A sends `udp_tunnel_allocate` for target=B.
    4. Both peers receive `udp_tunnel_allocated` with the same flow_id + token,
       role="a" for A, role="b" for B.
    5. Peer A sends a UDP datagram (with valid HMAC) to the relay's UDP port.
       → relay forwards it to B. B verifies HMAC and sees the payload.
    6. Same in reverse.

Run: python test_udp_tunnel.py
"""
import asyncio
import base64
import hashlib
import hmac
import json
import os
import struct
import sys
import time
import uuid
from typing import Optional, Tuple

import relay_server as rs

WS_PORT = 19999
UDP_PORT = 19998
PSK = "test-psk"
HOST = "127.0.0.1"

MAGIC = b"VP"
HEADER_LEN = 24


def build_datagram(token: bytes, flow_id: int, src_port: int,
                   dst_port: int, payload: bytes) -> bytes:
    header_no_mac = MAGIC + struct.pack("!HHH", flow_id, src_port, dst_port)
    mac = hmac.new(token, header_no_mac + payload, hashlib.sha256).digest()[:16]
    return header_no_mac + mac + payload


def parse_datagram(token: bytes, data: bytes) -> Optional[Tuple[int, int, int, bytes]]:
    if len(data) < HEADER_LEN or data[0:2] != MAGIC:
        return None
    flow_id, src_port, dst_port = struct.unpack("!HHH", data[2:8])
    recv_mac = data[8:24]
    payload = data[24:]
    expected = hmac.new(token, data[0:8] + payload, hashlib.sha256).digest()[:16]
    if not hmac.compare_digest(expected, recv_mac):
        return None
    return flow_id, src_port, dst_port, payload


# ---------------------------------------------------------------------------
# Minimal WebSocket client (text frames only)
# ---------------------------------------------------------------------------

async def ws_connect(host: str, port: int):
    reader, writer = await asyncio.open_connection(host, port)
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        f"GET / HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        f"Upgrade: websocket\r\n"
        f"Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n"
        f"\r\n"
    )
    writer.write(req.encode())
    await writer.drain()
    # Read response headers until blank line
    while True:
        line = await reader.readline()
        if line in (b"\r\n", b""):
            break
    return reader, writer


async def ws_send(writer, msg: str):
    payload = msg.encode("utf-8")
    n = len(payload)
    mask = os.urandom(4)
    # FIN=1, opcode=1 (text)
    header = bytes([0x81])
    if n < 126:
        header += bytes([0x80 | n])
    elif n < 65536:
        header += bytes([0x80 | 126]) + struct.pack("!H", n)
    else:
        header += bytes([0x80 | 127]) + struct.pack("!Q", n)
    header += mask
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    writer.write(header + masked)
    await writer.drain()


async def ws_recv_text(reader) -> Optional[str]:
    b = await reader.readexactly(2)
    opcode = b[0] & 0x0F
    masked = bool(b[1] & 0x80)
    length = b[1] & 0x7F
    if length == 126:
        length = struct.unpack("!H", await reader.readexactly(2))[0]
    elif length == 127:
        length = struct.unpack("!Q", await reader.readexactly(8))[0]
    if masked:
        mk = await reader.readexactly(4)
    else:
        mk = None
    payload = await reader.readexactly(length)
    if masked and mk:
        payload = bytes(b ^ mk[i % 4] for i, b in enumerate(payload))
    if opcode == 1:
        return payload.decode("utf-8")
    return None


def psk_hash(psk: str, uid: str) -> str:
    return hmac.new(psk.encode(), uid.encode(), hashlib.sha256).hexdigest()[:16]


# ---------------------------------------------------------------------------
# Test harness
# ---------------------------------------------------------------------------

class UdpCollector(asyncio.DatagramProtocol):
    """Peer-side UDP endpoint: collects datagrams destined for this peer."""

    def __init__(self, token: bytes):
        self.token = token
        self.recv_queue: asyncio.Queue = asyncio.Queue()
        self.transport: Optional[asyncio.DatagramTransport] = None

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        parsed = parse_datagram(self.token, data)
        if parsed is not None:
            self.recv_queue.put_nowait(parsed)


async def run_test() -> int:
    # Configure relay
    rs.UDP_TUNNEL_PORT = UDP_PORT
    rs.UDP_TUNNEL_ADVERTISE_HOST = HOST

    # Start relay in-process
    server_task = asyncio.create_task(
        rs.run_server(HOST, WS_PORT, PSK, udp_port=UDP_PORT))
    await asyncio.sleep(0.3)  # let it bind

    # ------ Connect two WS peers ------
    a_uuid = str(uuid.uuid4())
    b_uuid = str(uuid.uuid4())

    ar, aw = await ws_connect(HOST, WS_PORT)
    br, bw = await ws_connect(HOST, WS_PORT)

    await ws_send(aw, json.dumps({"type": "register", "uuid": a_uuid,
                                  "role": "client", "psk_hash": psk_hash(PSK, a_uuid)}))
    assert json.loads(await ws_recv_text(ar))["type"] == "registered"

    await ws_send(bw, json.dumps({"type": "register", "uuid": b_uuid,
                                  "role": "server", "psk_hash": psk_hash(PSK, b_uuid)}))
    assert json.loads(await ws_recv_text(br))["type"] == "registered"

    print(f"[TEST] A={a_uuid[:8]}.. B={b_uuid[:8]}.. both registered")

    # ------ Allocate tunnel ------
    await ws_send(aw, json.dumps({"type": "udp_tunnel_allocate",
                                  "target_uuid": b_uuid}))

    alloc_a = json.loads(await ws_recv_text(ar))
    alloc_b = json.loads(await ws_recv_text(br))
    assert alloc_a["type"] == "udp_tunnel_allocated"
    assert alloc_b["type"] == "udp_tunnel_allocated"
    assert alloc_a["flow_id"] == alloc_b["flow_id"]
    assert alloc_a["token"] == alloc_b["token"]
    assert alloc_a["role"] == "a"
    assert alloc_b["role"] == "b"
    flow_id = alloc_a["flow_id"]
    token = bytes.fromhex(alloc_a["token"])
    relay_udp = (alloc_a["relay_udp_host"], alloc_a["relay_udp_port"])
    print(f"[TEST] flow_id={flow_id} token={token.hex()[:16]}... relay_udp={relay_udp}")

    # ------ Spin up UDP collectors at both ends ------
    loop = asyncio.get_event_loop()
    a_transport, a_proto = await loop.create_datagram_endpoint(
        lambda: UdpCollector(token), local_addr=(HOST, 0))
    b_transport, b_proto = await loop.create_datagram_endpoint(
        lambda: UdpCollector(token), local_addr=(HOST, 0))

    # ------ Probe: both peers send an empty datagram so relay learns their
    # UDP address. In production this is the periodic NAT-keepalive. First
    # probes may not be forwarded (peer on other side not yet pinned) — that
    # is expected. After both probes the relay has both endpoints pinned.
    probe = build_datagram(token, flow_id, 0, 0, b"")
    a_transport.sendto(probe, relay_udp)
    b_transport.sendto(probe, relay_udp)
    await asyncio.sleep(0.2)
    # Drain any probe that looped back (addr already pinned → forwarded).
    while not a_proto.recv_queue.empty():
        a_proto.recv_queue.get_nowait()
    while not b_proto.recv_queue.empty():
        b_proto.recv_queue.get_nowait()
    print("[TEST] probes sent, both sides pinned on relay")

    # ------ A → B ------
    pkt_ab = build_datagram(token, flow_id, src_port=47999, dst_port=47999,
                            payload=b"HELLO-from-A")
    a_transport.sendto(pkt_ab, relay_udp)

    got_b = await asyncio.wait_for(b_proto.recv_queue.get(), timeout=2.0)
    assert got_b == (flow_id, 47999, 47999, b"HELLO-from-A"), got_b
    print(f"[TEST] [OK] A→B forwarded: {got_b[3]}")

    # ------ B → A (reply) ------
    pkt_ba = build_datagram(token, flow_id, src_port=48000, dst_port=48000,
                            payload=b"REPLY-from-B-longer-payload-1234567890")
    b_transport.sendto(pkt_ba, relay_udp)

    got_a = await asyncio.wait_for(a_proto.recv_queue.get(), timeout=2.0)
    assert got_a == (flow_id, 48000, 48000, b"REPLY-from-B-longer-payload-1234567890"), got_a
    print(f"[TEST] [OK] B→A forwarded: {got_a[3][:20]}...")

    # ------ Spoof attempt: wrong HMAC ------
    bad_token = os.urandom(16)
    bad_pkt = build_datagram(bad_token, flow_id, 47999, 47999, b"SPOOF")
    a_transport.sendto(bad_pkt, relay_udp)
    try:
        await asyncio.wait_for(b_proto.recv_queue.get(), timeout=0.5)
        print("[TEST] [FAIL] Spoof was delivered (bad HMAC should be dropped)")
        return 1
    except asyncio.TimeoutError:
        print("[TEST] [OK] Bad-HMAC spoof was dropped")

    # ------ Close flow ------
    await ws_send(aw, json.dumps({"type": "udp_tunnel_close", "flow_id": flow_id}))
    # B should receive udp_tunnel_closed
    closed_notice = json.loads(await ws_recv_text(br))
    assert closed_notice["type"] == "udp_tunnel_closed", closed_notice
    assert closed_notice["flow_id"] == flow_id
    print("[TEST] [OK] Close flow: B received udp_tunnel_closed notice")

    # After close, previously valid packets should be dropped
    pkt_after_close = build_datagram(token, flow_id, 47999, 47999, b"POSTCLOSE")
    a_transport.sendto(pkt_after_close, relay_udp)
    try:
        await asyncio.wait_for(b_proto.recv_queue.get(), timeout=0.5)
        print("[TEST] [FAIL] Packet after close was forwarded")
        return 1
    except asyncio.TimeoutError:
        print("[TEST] [OK] Packet after close was dropped")

    # ------ Cleanup ------
    a_transport.close()
    b_transport.close()
    aw.close()
    bw.close()
    server_task.cancel()
    try:
        await server_task
    except (asyncio.CancelledError, Exception):
        pass

    print("[TEST] [DONE] All UDP tunnel checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(run_test()))
