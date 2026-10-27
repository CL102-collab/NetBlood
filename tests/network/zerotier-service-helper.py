#!/usr/bin/env python3
"""Test double for the standalone UDP companion, not a ZeroTier SDK emulator.

The production game-side service, IPC, discovery and ENet run unchanged. This
process replaces only the external SDK network with a lossy loopback datagram
bus so those layers can be checked without a controller or Internet UDP access.
"""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import select
import socket
import struct
import time

HEADER = struct.Struct("<4sBBH16sIHH")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ipc-port", type=int, required=True)
    parser.add_argument("--token-env", required=True)
    parser.add_argument("--network", required=True)
    parser.add_argument("--identity", type=Path, required=True)
    args = parser.parse_args()
    token = bytes.fromhex(os.environ.pop(args.token_env))
    assert len(token) == 16
    root = Path(os.environ.get("NETBLOOD_ZT_TEST_ROOT", args.identity.parent))
    root.mkdir(parents=True, exist_ok=True)
    args.identity.mkdir(parents=True, exist_ok=True)
    suffix = {"host": 1, "client": 2, "redhost": 3}.get(args.identity.name, 4)
    address = ipaddress.IPv4Address(os.environ.get("NETBLOOD_ZT_TEST_ADDRESS", f"10.7.0.{suffix}")).packed
    channel = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    channel.bind(("127.0.0.1", 0))
    channel.setblocking(False)
    parent = ("127.0.0.1", args.ipc_port)
    registry = root / ("fake-zt-" + str(ipaddress.IPv4Address(address)).replace(".", "-") + ".json")
    registry.write_text(json.dumps({"address": address.hex(), "port": channel.getsockname()[1]}))
    sockets = {}
    generations = {}
    dropped_close = False
    heartbeat = time.monotonic()
    last_status = 0
    count = 0

    def send(kind, payload=b"", logical=0, request=0, credential=token):
        channel.sendto(HEADER.pack(b"ZTIP", 1, kind, logical, credential, request, len(payload), 0) + payload, parent)

    try:
        while time.monotonic() - heartbeat < 15:
            now = time.monotonic()
            if now - last_status > .25:
                # A wrong bearer token must never establish/replace a session.
                send(2, bytes([4]) + address + b"forged status", credential=b"\xff" * 16)
                send(2, bytes([2]) + address + b"Test datagram network ready")
                last_status = now
            if not select.select([channel], [], [], .005)[0]:
                continue
            packet, sender = channel.recvfrom(65536)
            if sender != parent:
                if packet[:4] != b"BUSD" or len(packet) < 12:
                    continue
                source = packet[4:8]
                source_port, target_port = struct.unpack("<HH", packet[8:12])
                for logical, entry in list(sockets.items()):
                    generation, bound, _ = entry
                    if bound == target_port:
                        send(6, source + struct.pack("<H", source_port) + packet[12:], logical, generation)
                continue
            if len(packet) < HEADER.size:
                continue
            magic, version, kind, logical, credential, request, size, reserved = HEADER.unpack(packet[:HEADER.size])
            if magic != b"ZTIP" or version != 1 or credential != token or reserved or len(packet) != HEADER.size + size:
                continue
            heartbeat = now
            body = packet[HEADER.size:]
            if kind == 8:
                return
            if kind == 3:
                if len(body) != 3 or not logical:
                    continue
                port, flags = struct.unpack("<HB", body)
                if not port:
                    port = 40000 + logical
                old = sockets.get(logical)
                previous = generations.get(logical)
                newer = previous is None or 0 < ((request - previous) & 0xffffffff) < 0x80000000
                error = 0 if newer or old == (request, port, flags) else 1
                if not error:
                    sockets[logical] = (request, port, flags)
                    generations[logical] = request
                send(4, struct.pack("<HH", error, port), logical, request)
            elif kind == 7:
                if not dropped_close:
                    dropped_close = True
                    continue  # Newer Open must recover even when Close is lost.
                if sockets.get(logical, (None,))[0] == request:
                    del sockets[logical]
            elif kind == 5:
                entry = sockets.get(logical)
                if not entry or entry[0] != request or len(body) < 6:
                    continue
                target, port = body[:4], struct.unpack("<H", body[4:6])[0]
                count += 1
                if count % 7 == 0:
                    continue  # Real ENet must recover fragmented traffic from loss.
                wire = b"BUSD" + address + struct.pack("<HH", entry[1], port) + body[6:]
                for path in root.glob("fake-zt-*.json"):
                    try:
                        peer = json.loads(path.read_text())
                    except (OSError, ValueError):
                        continue
                    if target == b"\xff" * 4 or target.hex() == peer["address"]:
                        channel.sendto(wire, ("127.0.0.1", peer["port"]))
    finally:
        registry.unlink(missing_ok=True)
        channel.close()

if __name__ == "__main__":
    main()
