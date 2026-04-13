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
import ssl
import sys
from typing import Dict, Optional, Set

logger = logging.getLogger("relay")

# ============================================================
# Registry: tracks connected peers and their endpoints
# ============================================================

class Peer:
    __slots__ = ("uuid", "role", "stun_ip", "stun_port", "nat_type",
                 "writer", "last_seen", "extra")

    def __init__(self, uuid: str, role: str, writer):
        self.uuid = uuid
        self.role = role  # "server" or "client"
        self.stun_ip = ""
        self.stun_port = 0
        self.nat_type = "unknown"
        self.writer = writer
        self.last_seen = time.time()
        self.extra = {}


class Registry:
    def __init__(self):
        self.peers: Dict[str, Peer] = {}  # uuid -> Peer
        self.connections: Set = set()      # active writer set

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
            del self.peers[uuid]
            logger.info(f"[UNREG] {uuid[:8]}.. disconnected")

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

async def ws_recv(reader) -> Optional[str]:
    """Read one WebSocket text frame. Returns None on close/error."""
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
    if opcode == 0x9:  # Ping → Pong
        return "__PING__"
    if opcode == 0x1:  # Text
        return payload.decode("utf-8", errors="replace")
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

# ============================================================
# Client handler
# ============================================================

async def handle_client(reader, writer, psk: str):
    addr = writer.get_extra_info("peername")
    logger.info(f"[CONN] New connection from {addr}")

    if not await ws_handshake(reader, writer):
        logger.warning(f"[CONN] {addr} WebSocket handshake failed")
        return
    logger.info(f"[CONN] {addr} WebSocket handshake OK")

    peer_uuid = None
    try:
        while True:
            msg = await asyncio.wait_for(ws_recv(reader), timeout=120)
            if msg is None:
                break
            if msg == "__PING__":
                writer.write(ws_pong())
                await writer.drain()
                continue

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
# Server main
# ============================================================

async def run_server(host: str, port: int, psk: str, certfile: str = "", keyfile: str = ""):
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

    logger.info("=" * 50)
    logger.info("  VipleStream Signaling Relay")
    logger.info("=" * 50)
    logger.info(f"  Listening: {addrs}")
    logger.info(f"  PSK auth:  {'enabled' if psk else 'disabled (open)'}")
    logger.info(f"  TLS:       {'enabled' if ssl_ctx else 'disabled'}")
    logger.info("=" * 50)

    async with server:
        await server.serve_forever()

def main():
    parser = argparse.ArgumentParser(description="VipleStream Signaling Relay Server")
    parser.add_argument("--host", default="0.0.0.0", help="Bind address (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=9999, help="Listen port (default: 9999)")
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

    try:
        asyncio.run(run_server(args.host, args.port, args.psk, certfile, keyfile))
    except KeyboardInterrupt:
        logger.info("Relay server stopped.")

if __name__ == "__main__":
    main()
