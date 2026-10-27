#!/usr/bin/env python3
"""Exercise the actual companion SDK startup and authenticated IPC lifecycle."""
import argparse
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

HEADER = struct.Struct("<4sBBH16sIHH")


class Parent:
    def __init__(self, helper, identity):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(("127.0.0.1", 0))
        self.socket.settimeout(2)
        self.token = os.urandom(16)
        self.peer = None
        environment = os.environ.copy()
        environment["NETBLOOD_ZT_IPC_TOKEN"] = self.token.hex()
        self.process = subprocess.Popen([str(helper), "--ipc-port", str(self.socket.getsockname()[1]),
            "--token-env", "NETBLOOD_ZT_IPC_TOKEN", "--network", "123456789abcdef0", "--identity", str(identity)],
            env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    def packet(self, kind, sock=0, request=0, payload=b"", token=None):
        return HEADER.pack(b"ZTIP", 1, kind, sock, token or self.token, request, len(payload), 0) + payload

    def send(self, kind, sock=0, request=0, payload=b"", token=None):
        self.socket.sendto(self.packet(kind, sock, request, payload, token), self.peer)

    def receive(self, kind=None, request=None, deadline=None):
        deadline = deadline or time.monotonic() + 5
        while time.monotonic() < deadline:
            try:
                packet, peer = self.socket.recvfrom(65536)
            except socket.timeout:
                continue
            fields = HEADER.unpack(packet[:HEADER.size])
            assert fields[0:2] == (b"ZTIP", 1)
            assert fields[4] == self.token and fields[6] == len(packet) - HEADER.size and fields[7] == 0
            assert peer[0] == "127.0.0.1"
            if self.peer is None:
                self.peer = peer
            assert peer == self.peer
            if (kind is None or fields[2] == kind) and (request is None or fields[5] == request):
                return fields, packet[HEADER.size:]
        raise AssertionError("Expected authenticated companion reply was not received")

    def stop(self):
        if self.process.poll() is None and self.peer:
            self.send(8)
        try:
            code = self.process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            self.process.kill()
            raise AssertionError("Companion failed to stop")
        assert code == 0, self.process.stderr.read().decode(errors="replace")
        self.socket.close()
        self.process.stderr.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--helper", required=True, type=Path)
    args = parser.parse_args()
    helper = args.helper.resolve()
    subprocess.run([str(helper), "--help"], check=True)
    assert subprocess.run([str(helper), "--ipc-port", "-1"], capture_output=True).returncode == 2
    with tempfile.TemporaryDirectory(prefix="zerotier-companion-test-") as temporary:
        identity = Path(temporary) / "identity"
        parent = Parent(helper, identity)
        try:
            _, status = parent.receive(2)
            assert len(status) >= 5 and status[0] <= 4
            assert parent.process.poll() is None
            # Neither a wrong token from the right endpoint nor a correct
            # token from a different endpoint may control the companion.
            parent.send(8, token=b"\x00" * 16)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as intruder:
                intruder.sendto(parent.packet(8), parent.peer)
            time.sleep(0.05)
            assert parent.process.poll() is None
            parent.send(1)
            parent.receive(2)
            # Malformed socket request produces a bounded correlated failure.
            parent.send(3, 1, 55, b"bad-open")
            fields, payload = parent.receive(4, 55)
            assert fields[3] == 1 and len(payload) == 4 and struct.unpack("<HH", payload)[0] != 0
            duplicate = Parent(helper, identity)
            try:
                _, status = duplicate.receive(2)
                assert status[0] == 4 and b"already in use" in status[5:]
            finally:
                duplicate.stop()
            # The first process remains available after rejecting duplicate use.
            parent.send(1)
            parent.receive(2)
        finally:
            parent.stop()
        # Loss of the parent must not leave a background node running forever.
        orphan = Parent(helper, identity)
        orphan.receive(2)
        code = orphan.process.wait(timeout=24)
        assert code == 0
        orphan.socket.close()
        orphan.process.stderr.close()
    print("PASS: real SDK IPC authentication, request errors, identity locking, stop and 15-second orphan cleanup")


if __name__ == "__main__":
    main()
