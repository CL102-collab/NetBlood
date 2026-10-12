#!/usr/bin/env python3
"""Verify server-assigned names with real clients, pending joins and slot reuse.

Requires Blood 1.21 assets and X11/Xvfb. Two ordinary UDP relays delay content
admission without changing packets, so pending names must reserve their suffix.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import tempfile

from cinematics import free_port
from relay import Relay
from run import build_driver, require
from scoreboard import Game, expect


def main():
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=repository / "nblood")
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--engine-data", type=Path, default=repository)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=45)
    args = parser.parse_args()
    for name in ("binary", "assets", "engine_data"):
        setattr(args, name, getattr(args, name).resolve())
    require(os.environ.get("DISPLAY"), "clients need DISPLAY (use an Xvfb wrapper)")
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="netblood-player-names-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print(f"Player-name evidence: {root}", flush=True)
    source_binary = args.binary
    args.binary = root / "nblood-tested"
    shutil.copy2(source_binary, args.binary)
    library = build_driver(root)
    result = dict(passed=False, source_binary=str(source_binary),
                  binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                  phases={}, failures=[])
    games, relays = [], []

    def client(name, port, player_name, wait=True):
        game = Game(name, args, root, library, port,
                    launch_arguments=["-client", "127.0.0.1", "-port", str(port), f"-pname={player_name}"])
        games.append(game)
        if wait:
            joined(game)
        return game

    def joined(game):
        game.wait(lambda text: "NET client map loaded " in text and "NET client slot=" in text,
                  "first authoritative snapshot", args.timeout)

    def roster(game, names):
        return expect(game, lambda rows: sorted(row["name"] for row in rows) == sorted(names),
                      f"names {names!r}", args.timeout)

    def pending_client(name, port):
        relay = Relay(("127.0.0.1", port)).start()
        relays.append(relay)
        # Drop ContentReady, including reliable retransmissions, while allowing
        # Hello, the manifest and transport acknowledgements through unchanged.
        relay.block_client_payload = struct.pack("<III", 0x314e424e, 7, 15)
        game = client(name, relay.port, "", wait=False)
        game.wait(lambda text: "NET content verified" in text
                  and relay.counters["client_to_server_filtered"] > 0,
                  "verified content awaiting admission", args.timeout)
        require("NET welcome " not in game.text(), f"{name}: admission was not held")
        return game, relay

    try:
        port = free_port()
        server = Game("server", args, root, library, port, dedicated=True,
                      launch_arguments=["-dedicated", "-port", str(port), "-gametype", "bloodbath",
                                        "-maxplayers", "8", "-map", "E1M1"])
        games.append(server)
        server.wait(lambda text: "Dedicated server running:" in text and "NET server_tick=" in text,
                    "dedicated server", args.timeout)
        zero = client("client-1", port, "cAlEb00")
        one = client("client-2", port, "CALEB01")
        original = ["cAlEb00", "CALEB01"]
        result["phases"]["explicit-names-preserved"] = roster(server, original)

        two, relay_two = pending_client("pending-2", port)
        three, relay_three = pending_client("pending-3", port)
        result["phases"]["two-pending-joins"] = dict(
            admitted=roster(server, original),
            filtered=[relay_two.counters["client_to_server_filtered"],
                      relay_three.counters["client_to_server_filtered"]])
        require("NET welcome " not in two.text() and "NET welcome " not in three.text(),
                "pending clients were admitted before the relays released them")
        relay_three.block_client_payload = None
        joined(three)
        result["phases"]["pending-name-reserved"] = roster(server, original + ["Caleb03"])
        roster(three, original + ["Caleb03"])
        relay_two.block_client_payload = None
        joined(two)
        full = original + ["Caleb02", "Caleb03"]
        result["phases"]["case-insensitive-collisions"] = roster(server, full)
        for game in (zero, one, two, three):
            roster(game, full)
        print("PASS: empty names avoid active and pending names, including mixed-case collisions", flush=True)

        zero.stop()
        roster(server, full[1:])
        replacement = client("replacement", port, "")
        reused = ["Caleb00", "CALEB01", "Caleb02", "Caleb03"]
        result["phases"]["lowest-hole-reused"] = roster(server, reused)
        roster(replacement, reused)
        roster(three, reused)
        server.command_text("map E1M2")
        server.wait(lambda text: "NET map generation=2 " in text, "second map generation", args.timeout)
        replacement.wait(lambda text: "map_generation=2 " in text,
                         "client received the second map generation", args.timeout)
        result["phases"]["map-preserves-names"] = roster(server, reused)
        roster(replacement, reused)
        print("PASS: departure frees the lowest suffix; assigned names survive a map change", flush=True)
        for game in reversed(games):
            game.stop()

        listen_port = free_port()
        host = Game("listen-host", args, root, library, listen_port,
                    launch_arguments=["-server", "8", "-port", str(listen_port),
                                      "-gametype", "bloodbath", "-map", "E1M1", "-pname="])
        games.append(host)
        host.wait(lambda text: "NET server_tick=" in text, "listen server", args.timeout)
        result["phases"]["empty-listen-host"] = roster(host, ["Caleb00"])
        guest = client("listen-guest", listen_port, "")
        result["phases"]["listen-host-reserved"] = roster(host, ["Caleb00", "Caleb01"])
        roster(guest, ["Caleb00", "Caleb01"])
        print("PASS: an empty listen-host name gets Caleb00 and reserves it for joining clients", flush=True)
        result["passed"] = True
    except Exception as error:
        result["failures"].append(f"{type(error).__name__}: {error}")
        print(f"FAIL: {type(error).__name__}: {error}", flush=True)
    finally:
        for relay in relays:
            relay.block_client_payload = None
        for game in reversed(games):
            game.stop()
        for relay in relays:
            relay.close()
        result["games"] = {game.name: game.result() for game in games}
        result["relays"] = [relay.report() for relay in relays]
        for game in games:
            if game.process.returncode != 0 or game.forced:
                result["failures"].append(f"{game.name}: unclean exit {game.process.returncode}")
            if re.search(r"runtime error:|AddressSanitizer|snapshot roundtrip.*equal=0|NET snapshot invalid",
                         game.text() + game.console()):
                result["failures"].append(f"{game.name}: runtime/snapshot error")
            if game.dedicated and "NETWORK_TEST_DEVICES windows=0 audio=0" not in game.console():
                result["failures"].append(f"{game.name}: missing headless device audit")
        result["passed"] = result["passed"] and not result["failures"]
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(dict(passed=result["passed"], failures=result["failures"], evidence=str(root))), flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
