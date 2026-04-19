#!/usr/bin/env python3
"""
VipleStream Signaling Relay Server
===================================
Lightweight WebSocket signaling server for NAT traversal.
Clients (Sunshine/Moonlight) register with their UUID and exchange STUN endpoints.

NO streaming data passes through this server — only signaling messages (< 1KB each).

Usage:
    python relay_server.py                          # Default port 9999, no auth
    python relay_server.py --port 9999 --psk mykey  # With PSK authentication
    python relay_server.py --tls cert.pem key.pem   # With TLS

Protocol:
    1. Client connects via WebSocket to ws://relay:9999/
    2. Sends REGISTER: {"type":"register","uuid":"...","role":"server|client","psk_hash":"..."}
    3. Server sends ENDPOINT_UPDATE: {"type":"endpoint","uuid":"...","stun_ip":"...","stun_port":N}
    4. Client sends LOOKUP: {"type":"lookup","target_uuid":"..."}
    5. Relay responds with target's last known endpoint (if registered)
    6. Both sides do hole-punching directly (no relay of actual data)

Requires: Python 3.7+ (uses asyncio). Optional: websockets (pip install websockets)
Falls back to built-in asyncio if websockets not available.
"""

import asyncio
import json
import hashlib
import hmac
import time
import argparse
import logging
import socket
import ssl
import sys
from typing import Dict, Optional, Set

logger = logging.getLogger("relay")

# ============================================================
# Registry: tracks connected peers and their endpoints
# ============================================================

class Peer:
    __slots__ = ("uuid", "role", "stun_ip", "stun_port", "nat_type",
                 "writer", "last_seen", "extra", "udp_flows")

    def __init__(self, uuid: str, role: str, writer):
        self.uuid = uuid
        self.role = role  # "server" or "client"
        self.stun_ip = ""
        self.stun_port = 0
        self.nat_type = "unknown"
        self.writer = writer
        self.last_seen = time.time()
        self.extra = {}
        self.udp_flows: Set[int] = set()  # flow_ids owned by this peer (for cleanup)


class UdpFlow:
    """A relay-mediated tunnel between two peers.

    Each side can reach the relay via either direct UDP (fastest) or a WebSocket
    binary frame fallback (when UDP cannot reach the relay — e.g. ISP blocks
    inbound UDP). An endpoint is a tuple:
        ("udp", (ip, port))   — peer sent a valid datagram from this addr
        ("ws",  writer)       — peer used a WS binary frame, routed via this WS
    Endpoints are pinned on first-valid-arrival. Subsequent traffic from a
    pinned endpoint is forwarded to the other side's pinned endpoint (which
    may be a different mode — i.e. UDP↔WS cross-mode routing is supported).

    Stats track mode combination and byte counters for diagnostic UI.
    """
    __slots__ = ("flow_id", "peer_a_uuid", "peer_b_uuid",
                 "endpoint_a", "endpoint_b", "token",
                 "bin_sent", "bin_dropped",
                 "created_at", "last_active",
                 "bytes_a_to_b", "bytes_b_to_a",
                 "pkts_a_to_b", "pkts_b_to_a")

    def __init__(self, flow_id: int, peer_a_uuid: str, peer_b_uuid: str, token: bytes):
        self.flow_id = flow_id
        self.peer_a_uuid = peer_a_uuid
        self.peer_b_uuid = peer_b_uuid
        self.endpoint_a = None  # ("udp", addr) or ("ws", writer)
        self.endpoint_b = None
        self.token = token  # 16-byte random, shared by both peers
        self.created_at = time.time()
        self.last_active = self.created_at
        self.bytes_a_to_b = 0
        self.bytes_b_to_a = 0
        self.pkts_a_to_b = 0
        self.pkts_b_to_a = 0
        self.bin_sent = 0      # WS binary frames successfully written
        self.bin_dropped = 0   # WS binary frames dropped due to backlog

    @property
    def mode_a(self) -> str:
        return self.endpoint_a[0] if self.endpoint_a else "-"

    @property
    def mode_b(self) -> str:
        return self.endpoint_b[0] if self.endpoint_b else "-"

    @property
    def mode_label(self) -> str:
        return f"{self.mode_a}<->{self.mode_b}"

    def stats_dict(self) -> dict:
        return {
            "flow_id": self.flow_id,
            "peer_a": self.peer_a_uuid[:8] + "..",
            "peer_b": self.peer_b_uuid[:8] + "..",
            "mode_a": self.mode_a,
            "mode_b": self.mode_b,
            "mode_label": self.mode_label,
            "bytes_a_to_b": self.bytes_a_to_b,
            "bytes_b_to_a": self.bytes_b_to_a,
            "pkts_a_to_b": self.pkts_a_to_b,
            "pkts_b_to_a": self.pkts_b_to_a,
            "age_s": int(time.time() - self.created_at),
            "idle_s": int(time.time() - self.last_active),
        }


class Registry:
    def __init__(self):
        self.peers: Dict[str, Peer] = {}  # uuid -> Peer
        self.connections: Set = set()      # active writer set
        self.flows: Dict[int, UdpFlow] = {}  # flow_id -> UdpFlow
        self._next_flow_id = 1  # 0 reserved

    def register(self, peer: Peer):
        old = self.peers.get(peer.uuid)
        if old and old.writer != peer.writer:
            # Close old connection if UUID re-registers
            try:
                old.writer.close()
            except Exception:
                pass
        self.peers[peer.uuid] = peer
        self.connections.add(peer.writer)
        logger.info(f"[REG] {peer.role} {peer.uuid[:8]}.. registered")

    def unregister(self, writer):
        self.connections.discard(writer)
        to_remove = [uuid for uuid, p in self.peers.items() if p.writer == writer]
        for uuid in to_remove:
            # Tear down any flows owned by this peer
            peer = self.peers[uuid]
            for flow_id in list(peer.udp_flows):
                self._close_flow(flow_id)
            del self.peers[uuid]
            logger.info(f"[UNREG] {uuid[:8]}.. disconnected")

    def allocate_flow(self, peer_a_uuid: str, peer_b_uuid: str) -> UdpFlow:
        """Allocate a new UDP tunnel flow between two peers. Returns the flow."""
        # Find a free flow_id (wrap around, skip 0)
        for _ in range(65535):
            fid = self._next_flow_id
            self._next_flow_id = (self._next_flow_id + 1) & 0xFFFF
            if self._next_flow_id == 0:
                self._next_flow_id = 1
            if fid not in self.flows:
                break
        else:
            raise RuntimeError("No free UDP flow_id (65535 already in use)")

        token = os.urandom(16)
        flow = UdpFlow(fid, peer_a_uuid, peer_b_uuid, token)
        self.flows[fid] = flow
        for uuid in (peer_a_uuid, peer_b_uuid):
            p = self.peers.get(uuid)
            if p:
                p.udp_flows.add(fid)
        logger.info(f"[UDP-TUN] alloc flow={fid} {peer_a_uuid[:8]}..<->{peer_b_uuid[:8]}..")
        return flow

    def _close_flow(self, flow_id: int):
        flow = self.flows.pop(flow_id, None)
        if not flow:
            return
        for uuid in (flow.peer_a_uuid, flow.peer_b_uuid):
            p = self.peers.get(uuid)
            if p:
                p.udp_flows.discard(flow_id)
        logger.info(f"[UDP-TUN] closed flow={flow_id}")

    def close_flow(self, flow_id: int):
        self._close_flow(flow_id)

    def lookup(self, uuid: str) -> Optional[Peer]:
        return self.peers.get(uuid)

    def update_endpoint(self, uuid: str, stun_ip: str, stun_port: int, nat_type: str = ""):
        peer = self.peers.get(uuid)
        if peer:
            peer.stun_ip = stun_ip
            peer.stun_port = stun_port
            if nat_type:
                peer.nat_type = nat_type
            peer.last_seen = time.time()

    @property
    def stats(self):
        servers = sum(1 for p in self.peers.values() if p.role == "server")
        clients = sum(1 for p in self.peers.values() if p.role == "client")
        return {"servers": servers, "clients": clients, "total": len(self.peers)}


registry = Registry()

# ============================================================
# Authentication
# ============================================================

def verify_psk(psk: str, uuid: str, provided_hash: str) -> bool:
    """Verify PSK: client sends HMAC-SHA256(psk, uuid)[:16] as hex."""
    if not psk:
        return True  # No PSK configured = open relay
    expected = hmac.new(psk.encode(), uuid.encode(), hashlib.sha256).hexdigest()[:16]
    return hmac.compare_digest(expected, provided_hash)

# ============================================================
# Protocol handler (pure asyncio TCP + simple WebSocket frame)
# ============================================================

# Minimal WebSocket implementation (no external dependencies)
# Supports: text frames, ping/pong, close

import struct
import base64
import os

WS_MAGIC = b"258EAFA5-E914-47DA-95CA-5AB5AA86BE98"

def ws_accept_key(key: str) -> str:
    h = hashlib.sha1(key.encode() + WS_MAGIC).digest()
    return base64.b64encode(h).decode()

async def ws_handshake(reader, writer) -> bool:
    """Perform WebSocket upgrade handshake."""
    data = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), timeout=10)
    headers = {}
    for line in data.decode().split("\r\n"):
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()

    ws_key = headers.get("sec-websocket-key", "")
    if not ws_key:
        writer.close()
        return False

    accept = ws_accept_key(ws_key)
    response = (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {accept}\r\n"
        "\r\n"
    )
    writer.write(response.encode())
    await writer.drain()
    return True

async def ws_recv_frame(reader) -> Optional[tuple]:
    """Read one WebSocket frame. Returns (opcode, payload_bytes) or None.

    Handled opcodes (payload meaning varies):
        0x1 Text   — payload is UTF-8 bytes
        0x2 Binary — payload is raw bytes
        0x8 Close  — returns None (end of stream)
        0x9 Ping   — handled here, returns (0x9, b"") for caller to pong
        0xA Pong   — ignored (caller sees 0xA, can no-op)
    """
    try:
        b0 = await reader.readexactly(2)
    except (asyncio.IncompleteReadError, ConnectionError):
        return None

    opcode = b0[0] & 0x0F
    masked = bool(b0[1] & 0x80)
    length = b0[1] & 0x7F

    if length == 126:
        length = struct.unpack("!H", await reader.readexactly(2))[0]
    elif length == 127:
        length = struct.unpack("!Q", await reader.readexactly(8))[0]

    if masked:
        mask = await reader.readexactly(4)
    else:
        mask = None

    payload = await reader.readexactly(length)

    if masked and mask:
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))

    if opcode == 0x8:  # Close
        return None
    return (opcode, payload)


# Backward-compat: ws_recv returns str for text frames, "__PING__" for ping,
# None on close/unsupported. Used by existing JSON-signaling callers.
async def ws_recv(reader) -> Optional[str]:
    frame = await ws_recv_frame(reader)
    if frame is None:
        return None
    opcode, payload = frame
    if opcode == 0x9:  # Ping
        return "__PING__"
    if opcode == 0x1:  # Text
        return payload.decode("utf-8", errors="replace")
    # Binary / Pong / other → caller of this legacy helper ignores it
    return None

def ws_frame(data: str) -> bytes:
    """Encode a WebSocket text frame (server→client, unmasked)."""
    payload = data.encode("utf-8")
    frame = bytearray()
    frame.append(0x81)  # FIN + text opcode
    length = len(payload)
    if length < 126:
        frame.append(length)
    elif length < 65536:
        frame.append(126)
        frame.extend(struct.pack("!H", length))
    else:
        frame.append(127)
        frame.extend(struct.pack("!Q", length))
    frame.extend(payload)
    return bytes(frame)

def ws_pong() -> bytes:
    return bytes([0x8A, 0x00])  # FIN + pong, no payload

async def ws_send(writer, data: str):
    writer.write(ws_frame(data))
    await writer.drain()


def ws_frame_binary(data: bytes) -> bytes:
    """Encode a WebSocket binary frame (server→client, unmasked)."""
    frame = bytearray()
    frame.append(0x82)  # FIN + binary opcode
    length = len(data)
    if length < 126:
        frame.append(length)
    elif length < 65536:
        frame.append(126)
        frame.extend(struct.pack("!H", length))
    else:
        frame.append(127)
        frame.extend(struct.pack("!Q", length))
    frame.extend(data)
    return bytes(frame)


async def ws_send_binary(writer, data: bytes):
    """Send a WebSocket binary frame carrying a tunnel datagram."""
    writer.write(ws_frame_binary(data))
    await writer.drain()

# ============================================================
# Client handler
# ============================================================

async def handle_client(reader, writer, psk: str):
    addr = writer.get_extra_info("peername")
    logger.info(f"[CONN] New connection from {addr}")

    # Disable Nagle. The tunneled WS carrier ships per-packet RTP
    # through this connection; batching inflates latency by hundreds
    # of ms. Has to happen before the handshake so the first binary
    # frames aren't stuck.
    sock = writer.get_extra_info("socket")
    if sock is not None:
        try:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError as e:
            logger.debug(f"[CONN] {addr} TCP_NODELAY set failed: {e}")

    if not await ws_handshake(reader, writer):
        logger.warning(f"[CONN] {addr} WebSocket handshake failed")
        return
    logger.info(f"[CONN] {addr} WebSocket handshake OK")

    peer_uuid = None
    try:
        while True:
            frame = await asyncio.wait_for(ws_recv_frame(reader), timeout=120)
            if frame is None:
                break
            opcode, payload = frame

            if opcode == 0x9:  # Ping
                writer.write(ws_pong())
                await writer.drain()
                continue
            if opcode == 0xA:  # Pong — ignore
                continue
            if opcode == 0x2:  # Binary — WS-mode tunnel datagram
                # Peer is sending a tunnel packet over the WS control connection
                # because direct UDP to the relay is blocked.
                if not peer_uuid:
                    # Binary frames before register are rejected.
                    continue
                flow = _verify_tunnel_packet(payload)
                if flow is None:
                    continue
                _pin_and_forward(flow, payload,
                                 sender_endpoint=("ws", writer),
                                 sender_uuid=peer_uuid)
                continue
            if opcode != 0x1:  # Not Text — unknown/unsupported
                continue

            # Text frame — JSON signaling (existing flow)
            msg = payload.decode("utf-8", errors="replace")
            try:
                data = json.loads(msg)
            except json.JSONDecodeError:
                logger.warning(f"[RECV] {addr} invalid JSON: {msg[:100]}")
                continue

            msg_type = data.get("type", "")
            logger.info(f"[RECV] {addr} type={msg_type} {json.dumps(data, ensure_ascii=False)}")

            if msg_type == "register":
                uuid = data.get("uuid", "")
                role = data.get("role", "client")
                psk_hash = data.get("psk_hash", "")

                if not uuid:
                    await ws_send(writer, json.dumps({"type": "error", "msg": "missing uuid"}))
                    continue

                if not verify_psk(psk, uuid, psk_hash):
                    await ws_send(writer, json.dumps({"type": "error", "msg": "auth failed"}))
                    logger.warning(f"[AUTH] Failed for {uuid[:8]}.. from {addr}")
                    break

                peer = Peer(uuid, role, writer)
                registry.register(peer)
                peer_uuid = uuid

                resp = {"type": "registered", "uuid": uuid, "stats": registry.stats}
                await ws_send(writer, json.dumps(resp))
                logger.info(f"[SEND] {addr} -> {json.dumps(resp)}")

            elif msg_type == "endpoint":
                uuid = data.get("uuid", peer_uuid or "")
                stun_ip = data.get("stun_ip", "")
                stun_port = data.get("stun_port", 0)
                nat_type = data.get("nat_type", "")

                if uuid:
                    registry.update_endpoint(uuid, stun_ip, stun_port, nat_type)
                    logger.info(f"[ENDPOINT] {uuid[:8]}.. = {stun_ip}:{stun_port} ({nat_type})")

            elif msg_type == "lookup":
                target_uuid = data.get("target_uuid", "")
                target = registry.lookup(target_uuid)
                logger.info(f"[LOOKUP] {addr} looking for {target_uuid[:8]}.. -> "
                            f"{'FOUND (stun=' + target.stun_ip + ')' if target and target.stun_ip else 'FOUND (no stun)' if target else 'NOT FOUND'}")

                if target:
                    # Return result even without STUN endpoint — the client
                    # can still use http_proxy and tcp_tunnel through the relay
                    resp = {
                        "type": "lookup_result",
                        "target_uuid": target_uuid,
                        "stun_ip": target.stun_ip or "",
                        "stun_port": target.stun_port or 0,
                        "nat_type": target.nat_type or "unknown",
                        "role": target.role,
                        "age_s": int(time.time() - target.last_seen),
                        "online": True
                    }
                    await ws_send(writer, json.dumps(resp))
                    logger.info(f"[SEND] {addr} -> {target.stun_ip or '(no stun)'}:{target.stun_port} ({target.nat_type})")
                else:
                    resp = {
                        "type": "lookup_result",
                        "target_uuid": target_uuid,
                        "stun_ip": "",
                        "stun_port": 0,
                        "error": "not_found"
                    }
                    await ws_send(writer, json.dumps(resp))
                    logger.info(f"[SEND] {addr} -> not_found (registered: {list(registry.peers.keys())})")

            elif msg_type == "http_proxy":
                # Client wants to send an HTTP request to a server via relay
                target_uuid = data.get("target_uuid", "")
                request_id = data.get("request_id", "")
                target = registry.lookup(target_uuid)

                if target and target.writer:
                    logger.info(f"[PROXY] {addr} -> {target_uuid[:8]}.. {data.get('method','GET')} {data.get('path','/')}")
                    # Forward the proxy request to the target server
                    data["type"] = "http_proxy_request"  # rename for server side
                    data["from_uuid"] = peer_uuid or ""
                    try:
                        await ws_send(target.writer, json.dumps(data))
                    except Exception as e:
                        logger.warning(f"[PROXY] Forward failed: {e}")
                        await ws_send(writer, json.dumps({
                            "type": "http_proxy_response",
                            "request_id": request_id,
                            "status": 502,
                            "body": "relay forward failed"
                        }))
                else:
                    logger.warning(f"[PROXY] Target {target_uuid[:8]}.. not connected")
                    await ws_send(writer, json.dumps({
                        "type": "http_proxy_response",
                        "request_id": request_id,
                        "status": 504,
                        "body": "server not connected to relay"
                    }))

            elif msg_type == "http_proxy_response":
                # Server responding to a proxy request — route back to the requesting client
                from_uuid = data.get("from_uuid", "")
                request_id = data.get("request_id", "")
                requester = registry.lookup(from_uuid)
                if requester and requester.writer:
                    logger.info(f"[PROXY] {addr} -> {from_uuid[:8]}.. response ({data.get('status', '?')})")
                    await ws_send(requester.writer, json.dumps(data))
                else:
                    logger.warning(f"[PROXY] Can't route response to {from_uuid[:8]}.. (not connected)")

            elif msg_type in ("tcp_tunnel_open", "tcp_tunnel_data", "tcp_tunnel_closed",
                              "tcp_tunnel_reconnect", "tcp_tunnel_close_all"):
                # TCP tunnel: bidirectional forwarding between client and server
                target_uuid = data.get("target_uuid", "")
                from_uuid = data.get("from_uuid", "")

                if target_uuid:
                    # Has target: Client → Server (or explicit routing)
                    target = registry.lookup(target_uuid)
                    if target and target.writer:
                        data["from_uuid"] = peer_uuid or ""
                        try:
                            await ws_send(target.writer, json.dumps(data))
                            if msg_type == "tcp_tunnel_open":
                                logger.info(f"[TUNNEL] {addr} -> {target_uuid[:8]}.. open port {data.get('target_port')}")
                            elif msg_type == "tcp_tunnel_reconnect":
                                logger.info(f"[TUNNEL] {addr} -> {target_uuid[:8]}.. reconnect port {data.get('target_port')}")
                        except Exception as e:
                            logger.warning(f"[TUNNEL] Forward failed: {e}")
                elif from_uuid:
                    # No target but has from_uuid: Server → Client (response)
                    dest = registry.lookup(from_uuid)
                    if dest and dest.writer:
                        try:
                            await ws_send(dest.writer, json.dumps(data))
                            if msg_type == "tcp_tunnel_data":
                                logger.debug(f"[TUNNEL] {addr} -> {from_uuid[:8]}.. data response")
                        except Exception as e:
                            logger.warning(f"[TUNNEL] Response forward failed: {e}")
                    else:
                        logger.warning(f"[TUNNEL] Can't route to {from_uuid[:8]}.. (not found)")

            elif msg_type == "udp_tunnel_allocate":
                # Requester asks the relay to mint a UDP tunnel flow between
                # itself and target_uuid. Both peers receive `udp_tunnel_allocated`
                # with the same flow_id/token but a "role" marker ("a" or "b").
                target_uuid = data.get("target_uuid", "")
                request_id = data.get("request_id", "")
                if not peer_uuid:
                    await ws_send(writer, json.dumps({
                        "type": "udp_tunnel_error",
                        "msg": "not registered"}))
                    continue
                target = registry.lookup(target_uuid)
                if not target:
                    await ws_send(writer, json.dumps({
                        "type": "udp_tunnel_error",
                        "target_uuid": target_uuid,
                        "msg": "target not online"}))
                    continue
                try:
                    flow = registry.allocate_flow(peer_uuid, target_uuid)
                except RuntimeError as e:
                    await ws_send(writer, json.dumps({
                        "type": "udp_tunnel_error",
                        "msg": str(e)}))
                    continue

                base = {
                    "type": "udp_tunnel_allocated",
                    "flow_id": flow.flow_id,
                    "token": flow.token.hex(),
                    "relay_udp_port": UDP_TUNNEL_PORT,
                    "relay_udp_host": UDP_TUNNEL_ADVERTISE_HOST,
                }
                # Tell requester it's side "a" (and echo their request_id so
                # the response-matching logic in the client wrapper works),
                # target it's side "b" (no request_id — they didn't ask).
                requester_msg = dict(base, role="a", remote_uuid=target_uuid,
                                     request_id=request_id)
                target_msg    = dict(base, role="b", remote_uuid=peer_uuid)
                await ws_send(writer, json.dumps(requester_msg))
                try:
                    await ws_send(target.writer, json.dumps(target_msg))
                except Exception as e:
                    logger.warning(f"[UDP-TUN] Notify target failed: {e}")
                    registry.close_flow(flow.flow_id)
                    continue
                logger.info(f"[UDP-TUN] allocated flow={flow.flow_id} "
                            f"{peer_uuid[:8]}..(a)<->{target_uuid[:8]}..(b)")

            elif msg_type == "udp_tunnel_stats":
                # Query: return stats for flows this peer is part of, or all
                # if no flow_ids filter. Useful for UI connection-type display.
                wanted = data.get("flow_ids")  # optional list[int]
                if peer_uuid is None:
                    await ws_send(writer, json.dumps({
                        "type": "udp_tunnel_stats_result",
                        "flows": []}))
                    continue
                flows = []
                for fid, fl in registry.flows.items():
                    if peer_uuid not in (fl.peer_a_uuid, fl.peer_b_uuid):
                        continue
                    if wanted is not None and fid not in wanted:
                        continue
                    s = fl.stats_dict()
                    # Tell THIS peer which side they are (a/b) so they can
                    # pick the right byte counters for local display.
                    s["my_side"] = "a" if peer_uuid == fl.peer_a_uuid else "b"
                    flows.append(s)
                await ws_send(writer, json.dumps({
                    "type": "udp_tunnel_stats_result",
                    "flows": flows}))

            elif msg_type == "udp_tunnel_close":
                flow_id = int(data.get("flow_id", 0))
                flow = registry.flows.get(flow_id)
                if flow and peer_uuid in (flow.peer_a_uuid, flow.peer_b_uuid):
                    registry.close_flow(flow_id)
                    # Notify the other peer so it can tear down its tunnel endpoint
                    other_uuid = flow.peer_b_uuid if peer_uuid == flow.peer_a_uuid else flow.peer_a_uuid
                    other = registry.lookup(other_uuid)
                    if other:
                        try:
                            await ws_send(other.writer, json.dumps({
                                "type": "udp_tunnel_closed",
                                "flow_id": flow_id,
                                "reason": "peer_closed"}))
                        except Exception:
                            pass

            elif msg_type == "ping":
                if peer_uuid:
                    peer = registry.lookup(peer_uuid)
                    if peer:
                        peer.last_seen = time.time()
                await ws_send(writer, json.dumps({"type": "pong"}))
                logger.debug(f"[PING] {addr} ({peer_uuid[:8] if peer_uuid else '?'}..)")

    except asyncio.TimeoutError:
        logger.info(f"[TIMEOUT] {addr} (idle 120s)")
    except (ConnectionError, OSError) as e:
        logger.info(f"[ERROR] {addr} {type(e).__name__}: {e}")
    finally:
        uuid_str = peer_uuid[:8] + ".." if peer_uuid else "unknown"
        registry.unregister(writer)
        try:
            writer.close()
        except Exception:
            pass
        logger.info(f"[DISC] {addr} (uuid={uuid_str}, remaining peers={registry.stats})")

# ============================================================
# UDP tunnel relay (datagram plane)
# ============================================================
#
# Packet format (network byte order):
#     [0:2]  magic 'VP' (0x56, 0x50)
#     [2:4]  flow_id (uint16)
#     [4:6]  src_port (uint16) — opaque to relay, interpreted by peers
#     [6:8]  dst_port (uint16) — opaque to relay, interpreted by peers
#     [8:24] HMAC-SHA256(token, magic||flow_id||src_port||dst_port||payload)[:16]
#     [24:]  payload (UDP datagram)
#
# Both peers share the same 16-byte token (handed out via WS allocate). The
# relay verifies HMAC, pins sender address on first-valid-arrival (side a/b),
# and forwards the packet as-is (token is symmetric → receiver verifies too).
# UDP_TUNNEL_PORT / UDP_TUNNEL_ADVERTISE_HOST are set by main() before run.

UDP_TUNNEL_MAGIC = b"VP"
UDP_TUNNEL_HEADER_LEN = 24  # magic(2) + flow_id(2) + src/dst port(2+2) + hmac(16)
UDP_TUNNEL_PORT: int = 9998
UDP_TUNNEL_ADVERTISE_HOST: str = ""  # what we tell peers to connect to


class UdpRelayProtocol(asyncio.DatagramProtocol):
    """Relay UDP datagrams between two pinned peer endpoints, keyed by flow_id.

    Also handles cross-mode routing: if the sender is on UDP but the other
    peer is pinned as WS, the datagram is forwarded as a WS binary frame.
    """

    def __init__(self):
        self.transport: Optional[asyncio.DatagramTransport] = None

    def connection_made(self, transport):
        self.transport = transport
        global UDP_TRANSPORT_REF
        UDP_TRANSPORT_REF = transport
        sockname = transport.get_extra_info("sockname")
        logger.info(f"[UDP-TUN] relay UDP datagram plane listening on {sockname}")

    def datagram_received(self, data: bytes, addr):
        flow = _verify_tunnel_packet(data)
        if flow is None:
            return
        _pin_and_forward(flow, data, sender_endpoint=("udp", addr),
                         sender_uuid=None)  # identify sender by endpoint match


# Shared helpers — used by both UDP plane and WS binary-frame path
# --------------------------------------------------------------------------
UDP_TRANSPORT_REF: Optional[asyncio.DatagramTransport] = None


def _verify_tunnel_packet(data: bytes) -> Optional["UdpFlow"]:
    """Parse + HMAC-verify a tunnel packet. Returns the flow or None."""
    if len(data) < UDP_TUNNEL_HEADER_LEN or data[0:2] != UDP_TUNNEL_MAGIC:
        return None
    flow_id = struct.unpack("!H", data[2:4])[0]
    flow = registry.flows.get(flow_id)
    if not flow:
        return None
    recv_hmac = data[8:24]
    payload = data[24:]
    expected = hmac.new(flow.token, data[0:8] + payload, hashlib.sha256).digest()[:16]
    if not hmac.compare_digest(expected, recv_hmac):
        logger.debug(f"[TUN] bad HMAC flow={flow_id}")
        return None
    return flow


def _pin_and_forward(flow: "UdpFlow", data: bytes,
                     sender_endpoint: tuple, sender_uuid: Optional[str]):
    """Pin the sender's endpoint to side a/b (if not yet), then forward the
    packet to the other side. Supports cross-mode (UDP<->WS) forwarding.

    sender_endpoint: ("udp", (ip, port)) or ("ws", writer)
    sender_uuid:     required for WS mode; None for UDP (identified by addr)
    """
    # 1. Determine which side the sender belongs to.
    def _matches(ep, sender):
        if ep is None or sender is None:
            return False
        return ep[0] == sender[0] and ep[1] == sender[1]

    side = None
    if flow.endpoint_a is not None and _matches(flow.endpoint_a, sender_endpoint):
        side = "a"
    elif flow.endpoint_b is not None and _matches(flow.endpoint_b, sender_endpoint):
        side = "b"
    else:
        # Not pinned yet. Decide which side this sender belongs to using uuid
        # (for WS mode) or first-free-slot (for UDP — addr pinning is TOFU).
        if sender_uuid is not None:
            # WS mode — uuid tells us unambiguously which side.
            if sender_uuid == flow.peer_a_uuid:
                side = "a"
            elif sender_uuid == flow.peer_b_uuid:
                side = "b"
            else:
                return  # not a peer of this flow
        else:
            # UDP mode — TOFU pin into first free slot.
            if flow.endpoint_a is None:
                side = "a"
            elif flow.endpoint_b is None:
                side = "b"
            else:
                return  # both sides pinned and sender unknown

        if side == "a":
            flow.endpoint_a = sender_endpoint
            logger.info(f"[TUN] flow={flow.flow_id} pinned a={sender_endpoint[0]}({sender_endpoint[1] if sender_endpoint[0]=='udp' else 'ws'})")
        else:
            flow.endpoint_b = sender_endpoint
            logger.info(f"[TUN] flow={flow.flow_id} pinned b={sender_endpoint[0]}({sender_endpoint[1] if sender_endpoint[0]=='udp' else 'ws'})")

    flow.last_active = time.time()

    # 2. Forward to the other side.
    other = flow.endpoint_b if side == "a" else flow.endpoint_a
    if other is None:
        # The other side hasn't sent anything yet, so it's not pinned.
        # If the other peer is known WS-registered (typical: Sunshine
        # keeps a persistent WS up and the client just allocated against
        # its uuid), auto-pin endpoint to that writer so we don't
        # deadlock waiting for a packet that can only arrive AFTER this
        # one is delivered. UDP-pin still requires TOFU from an actual
        # datagram since we don't know the source addr in advance.
        other_uuid = flow.peer_b_uuid if side == "a" else flow.peer_a_uuid
        other_peer = registry.lookup(other_uuid) if other_uuid else None
        if other_peer is not None and other_peer.writer is not None:
            pinned = ("ws", other_peer.writer)
            if side == "a":
                flow.endpoint_b = pinned
            else:
                flow.endpoint_a = pinned
            logger.info(f"[TUN] flow={flow.flow_id} auto-pinned "
                        f"{'b' if side == 'a' else 'a'}=ws (via registry)")
            other = pinned
        else:
            return
    kind, val = other
    size = len(data)
    if kind == "udp":
        if UDP_TRANSPORT_REF is not None:
            try:
                UDP_TRANSPORT_REF.sendto(data, val)
                _account(flow, side, size)
            except Exception as e:
                logger.debug(f"[TUN] UDP forward error flow={flow.flow_id}: {e}")
    elif kind == "ws":
        # val is the peer's WS writer. We defer the actual write to the next
        # event-loop iteration via call_soon. Calling val.write() directly
        # from within a datagram_received callback on Windows SelectorEventLoop
        # has been observed not to propagate bytes to the peer until the loop
        # processes other I/O — deferring via call_soon makes it reliable.
        try:
            loop = asyncio.get_running_loop()
        except RuntimeError:
            logger.warning(f"[TUN] no running loop flow={flow.flow_id}")
            return
        frame = ws_frame_binary(data)

        def _do_write(_val=val, _frame=frame, _flow=flow, _side=side, _size=size):
            try:
                # Backpressure: asyncio's StreamWriter.write() has no
                # flow control — without this check it will buffer
                # data indefinitely if the peer TCP isn't draining,
                # trading memory for latency (we saw multi-second
                # visible lag on 5 Mbps video bursts). Drop the frame
                # once the transport-level write buffer is already
                # behind by more than 128 KB. RTP / audio / ENet all
                # handle loss better than they handle head-of-line
                # blocking.
                transport = _val.transport
                if transport is not None:
                    buf_size = transport.get_write_buffer_size()
                    if buf_size > 128 * 1024:
                        _flow.bin_dropped += 1
                        return
                _val.write(_frame)
                _flow.bin_sent += 1
                _account(_flow, _side, _size)
            except Exception as e:
                logger.debug(f"[TUN] WS write error flow={_flow.flow_id}: {e}")

        loop.call_soon(_do_write)


def _account(flow: "UdpFlow", side: str, size: int):
    if side == "a":
        flow.bytes_a_to_b += size
        flow.pkts_a_to_b += 1
    else:
        flow.bytes_b_to_a += size
        flow.pkts_b_to_a += 1


async def _ws_forward_safe(writer, data: bytes, flow: "UdpFlow", side: str, size: int):
    try:
        await ws_send_binary(writer, data)
        _account(flow, side, size)
    except Exception as e:
        logger.debug(f"[TUN] WS forward error flow={flow.flow_id}: {e}")


async def _udp_flow_reaper(idle_timeout: float = 300.0, interval: float = 30.0):
    """Reap UDP flows that have been idle for too long."""
    while True:
        await asyncio.sleep(interval)
        now = time.time()
        for fid in [fid for fid, f in registry.flows.items()
                    if now - f.last_active > idle_timeout]:
            logger.info(f"[UDP-TUN] reap idle flow={fid}")
            registry.close_flow(fid)


async def _flow_stats_reporter(interval: float = 10.0):
    """Periodically report per-flow WS forward stats so we can see
    whether backpressure drops are actually firing during a stream."""
    last_sent = {}
    last_dropped = {}
    while True:
        await asyncio.sleep(interval)
        for fid, f in list(registry.flows.items()):
            sent = f.bin_sent
            dropped = f.bin_dropped
            d_sent = sent - last_sent.get(fid, 0)
            d_drop = dropped - last_dropped.get(fid, 0)
            last_sent[fid] = sent
            last_dropped[fid] = dropped
            if d_sent == 0 and d_drop == 0:
                continue
            total = d_sent + d_drop
            pct = (d_drop * 100 // total) if total > 0 else 0
            logger.info(
                f"[TUN/STATS] flow={fid} fwd sent={d_sent} "
                f"dropped={d_drop} ({pct}%) "
                f"cum sent={sent} drop={dropped}"
            )


# ============================================================
# Server main
# ============================================================

async def run_server(host: str, port: int, psk: str,
                     certfile: str = "", keyfile: str = "",
                     udp_port: int = 9998):
    ssl_ctx = None
    if certfile and keyfile:
        ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ssl_ctx.load_cert_chain(certfile, keyfile)
        logger.info(f"TLS enabled: {certfile}")

    async def client_cb(reader, writer):
        try:
            await handle_client(reader, writer, psk)
        except Exception as e:
            logger.error(f"Handler error: {e}")

    server = await asyncio.start_server(client_cb, host, port, ssl=ssl_ctx)
    addrs = ", ".join(str(s.getsockname()) for s in server.sockets)

    # UDP datagram plane (tunnel data).
    loop = asyncio.get_running_loop()
    udp_transport, _ = await loop.create_datagram_endpoint(
        lambda: UdpRelayProtocol(),
        local_addr=(host, udp_port),
    )
    udp_sockname = udp_transport.get_extra_info("sockname")

    # Idle flow reaper
    reaper_task = asyncio.create_task(_udp_flow_reaper())
    stats_task = asyncio.create_task(_flow_stats_reporter())

    logger.info("=" * 50)
    logger.info("  VipleStream Signaling Relay")
    logger.info("=" * 50)
    logger.info(f"  WS listening:  {addrs}")
    logger.info(f"  UDP tunnel:    {udp_sockname}  (advertise={UDP_TUNNEL_ADVERTISE_HOST or '<dynamic>'})")
    logger.info(f"  PSK auth:      {'enabled' if psk else 'disabled (open)'}")
    logger.info(f"  TLS:           {'enabled' if ssl_ctx else 'disabled'}")
    logger.info("=" * 50)

    try:
        async with server:
            await server.serve_forever()
    finally:
        reaper_task.cancel()
        udp_transport.close()

def main():
    # Windows asyncio's default ProactorEventLoop has known limitations with
    # datagram_received for UDP sockets. Force the selector loop so UDP
    # tunnel forwarding works on Windows dev/test hosts. No-op on Linux.
    if sys.platform == "win32":
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())

    parser = argparse.ArgumentParser(description="VipleStream Signaling Relay Server")
    parser.add_argument("--host", default="0.0.0.0", help="Bind address (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=9999, help="WS listen port (default: 9999)")
    parser.add_argument("--udp-port", type=int, default=9998,
                        help="UDP tunnel listen port (default: 9998)")
    parser.add_argument("--udp-advertise-host", default="",
                        help="Hostname/IP to advertise to peers for UDP tunnel "
                             "(default: empty = peer uses the WS host)")
    parser.add_argument("--psk", default="", help="Pre-shared key for authentication (empty=open)")
    parser.add_argument("--tls", nargs=2, metavar=("CERT", "KEY"), help="TLS cert and key files")
    parser.add_argument("--verbose", action="store_true", help="Enable debug logging")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%H:%M:%S"
    )

    certfile, keyfile = (args.tls[0], args.tls[1]) if args.tls else ("", "")

    # Export UDP tunnel config so handle_client can include it in allocated messages.
    global UDP_TUNNEL_PORT, UDP_TUNNEL_ADVERTISE_HOST
    UDP_TUNNEL_PORT = args.udp_port
    UDP_TUNNEL_ADVERTISE_HOST = args.udp_advertise_host

    try:
        asyncio.run(run_server(args.host, args.port, args.psk, certfile, keyfile,
                               udp_port=args.udp_port))
    except KeyboardInterrupt:
        logger.info("Relay server stopped.")

if __name__ == "__main__":
    main()
