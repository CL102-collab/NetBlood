#!/usr/bin/env python3
"""Compile a test-only provider and verify companion datagram IPC behavior.

This exercises the actual companion source but DOES NOT test a ZeroTier
connection. The provider implements SDK calls using native loopback sockets.
Never package the executable produced by this test.
"""
import argparse
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
from test_companion import Parent

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-source", required=True, type=Path, help="Pinned SDK source with include/ZeroTierSockets.h")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="zerotier-test-provider-") as temporary:
        root = Path(temporary)
        helper = root / "TEST-ONLY-companion"
        subprocess.run([args.cxx, "-std=c++17", "-pthread", "-O1", "-Wall", "-Wextra", "-Werror",
            "-I" + str(args.sdk_source.resolve() / "include"), "-I" + str(HERE.parent.parent / "source/build/include"),
            str(HERE / "companion.cpp"), str(HERE / "test_socket_provider.cpp"), "-o", str(helper)], check=True)
        parent = Parent(helper, root / "identity")
        try:
            deadline = time.monotonic() + 5
            while True:
                _, status = parent.receive(2, deadline=deadline)
                if status[0] == 2:
                    break
            assert status[1:5] == socket.inet_aton("127.0.0.1")

            def opened(request, payload=b"\x00\x00\x01"):
                parent.send(3, 1, request, payload)
                _, result = parent.receive(4, request)
                return struct.unpack("<HH", result)

            error, bound = opened(101)
            assert error == 0 and bound > 0
            assert opened(101) == (0, bound), "Open retry must preserve the same bound socket"
            assert opened(100)[0] != 0, "Reordered older Open cannot replace a live socket"
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as echo:
                echo.bind(("127.0.0.1", 0))
                echo.settimeout(2)
                address = socket.inet_aton("127.0.0.1") + struct.pack("<H", echo.getsockname()[1])
                body = b"binary\x00payload\xff" * 100
                parent.send(5, 1, 101, address + body)
                data, sender = echo.recvfrom(65536)
                assert data == body and sender == ("127.0.0.1", bound)
                echo.sendto(data, sender)
                fields, response = parent.receive(6, 101)
                assert fields[3] == 1 and response == address + body

                parent.send(5, 1, 100, address + b"stale generation")
                echo.settimeout(0.15)
                try:
                    echo.recvfrom(65536)
                except socket.timeout:
                    pass
                else:
                    raise AssertionError("Stale-generation send escaped onto the virtual network")

                parent.send(7, 1, 100)  # Stale close must not destroy current socket.
                assert opened(101) == (0, bound)
                # Oversize overlay packets cannot bypass the IPC datagram cap.
                echo.sendto(b"x" * 17000, ("127.0.0.1", bound))
                time.sleep(0.03)
                parent.send(5, 1, 101, address + b"after oversize")
                data, sender = echo.recvfrom(65536)
                assert data == b"after oversize"
                echo.sendto(data, sender)
                _, response = parent.receive(6, 101)
                assert response == address + b"after oversize"

                # A newer Open replaces a prior socket even if Close was lost.
                assert opened(102)[0] == 0
                assert opened(101)[0] != 0
                # A later session must not receive old-generation Send/Close.
                parent.send(7, 1, 101)
                assert opened(102)[0] == 0
        finally:
            parent.stop()
    print("PASS: test-only provider verified datagram bytes/ports, idempotent opens, socket generations and oversize drops")


if __name__ == "__main__":
    main()
