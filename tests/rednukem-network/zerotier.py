#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run actual graphical Rednukem games over the bundled-companion API.

The companion alone is a test double using a lossy loopback UDP bus. ENet,
password admission, hot join, mod download, private exec/reconnect, current-map
download and authoritative gameplay are production code. This check cannot
establish real ZeroTier NAT traversal or controller admission.
"""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import tempfile
import time

from content import ContentGame
from fixtures import room_map
from run import ROOT, Failure, build_driver, generate, poll_status, records, require

NETWORK = "1234567890abcdef"
PASSWORD = "crypt key"


@contextmanager
def environment(values):
    previous = {name: os.environ.get(name) for name in values}
    os.environ.update({name: str(value) for name, value in values.items()})
    try:
        yield
    finally:
        for name, value in previous.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value


class Game(ContentGame):
    def text(self):
        # The private addon handoff execs the same PID and replaces rednukem.log;
        # inherited stdout preserves evidence from both actual starts.
        return self.console()

    def wait(self, predicate, description, timeout=90):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            text = self.text()
            if predicate(text):
                return text
            require(self.process.poll() is None,
                    f"{self.name}: exited before {description}; see {self.path}")
            time.sleep(.05)
        raise Failure(f"{self.name}: timed out waiting for {description}; see {self.path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "rednukem")
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--renderer", type=int, choices=(0, 3, 5), default=3)
    args = parser.parse_args()
    args.assets = args.assets.resolve()
    args.grp = next(path for path in args.assets.iterdir() if path.name.lower() == "duke3d.grp")
    source = args.binary.resolve()
    require(os.environ.get("DISPLAY"), "graphical overlay checks require DISPLAY (use Xvfb)")
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="rednukem-zerotier-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    args.binary = root / "rednukem-tested"
    shutil.copy2(source, args.binary)
    helper = root / "netblood-zerotier"
    shutil.copy2(ROOT / "tests/network/zerotier-service-helper.py", helper)
    helper.chmod(0o700)
    generate(root / "fixture")
    library = build_driver(root)
    addon_con = (root / "fixture/fixture.con").read_bytes().replace(
        b"NETWORK ROOM ONE", b"DOWNLOADED OVERLAY ROOM ONE")
    files = {"addon.con": addon_con,
             "payload.bin": bytes((index * 13 + index // 17) & 255 for index in range(300123))}
    addon = b"KenSilverman" + struct.pack("<I", len(files))
    addon += b"".join(struct.pack("<12sI", name.encode(), len(data)) for name, data in files.items())
    addon += b"".join(files.values())
    addon_path = root / "server-only-addon.grp"
    addon_path.write_bytes(addon)
    map_bytes = room_map(True)
    alias = "zt-only.map"
    require(not any((path / alias).exists() for path in (args.assets, root / "fixture")),
            "server-only map is already available on the shared search path")
    games, all_games = [], []
    result = dict(passed=False, failures=[], phases={}, binary=str(source),
                  binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                  network="Only SDK companion replaced by lossy loopback UDP; no external controller")
    node = 0
    host_address = "10.9.2.1"

    def launch(name, host=False, password=PASSWORD):
        nonlocal node
        node += 1
        address = f"10.9.2.{node}"
        profile = root / (name + "-profile")
        values = {"NETBLOOD_ZT_TEST_ROOT": root / "overlay-bus",
                  "NETBLOOD_ZT_TEST_ADDRESS": address,
                  "HOME": profile / "home", "XDG_CONFIG_HOME": profile / "config",
                  "XDG_DATA_HOME": profile / "data", "XDG_CACHE_HOME": profile / "cache"}
        for name_ in ("HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"):
            Path(values[name_]).mkdir(parents=True)
        cli = ["-zerotier", NETWORK]
        cli += ["-server", "8", "-gametype", "coop", "-v1", "-l1", "-s1"] if host else ["-client", host_address]
        extra = ["-g", str(addon_path), "-x", "addon.con"] if host else []
        with environment(values):
            game = Game(name, args, root, library, 23513, password=password,
                        extra_arguments=extra, launch_arguments=cli)
        game.overlay_address = address
        games.append(game)
        all_games.append(game)
        return game

    def stop(game):
        game.stop()
        games.remove(game)

    print(f"Rednukem ZeroTier gameplay evidence: {root}", flush=True)
    try:
        host = launch("listen-host", host=True)
        host_address = host.overlay_address
        host.wait(lambda text: "NET server role=listen" in text, "graphical overlay host", args.timeout)
        alone = poll_status(host, lambda row: row["players"] == "1" and int(row["tick"]) >= 30,
                            "host simulation before friends join", args.timeout)
        require((root / "overlay-bus/fake-zt-10-9-2-1.json").is_file(),
                "graphical host did not start its overlay companion")
        result["phases"]["host-alone"] = alone

        for label, supplied, reason in (("missing-password", "", "Server password required"),
                                        ("wrong-password", "wrong key", "Incorrect server password")):
            rejected = launch(label, password=supplied)
            rejected.wait(lambda text: reason in text, reason, args.timeout)
            require(not any(marker in rejected.text() for marker in
                            ("Server addon differs", "Downloading map ", "Downloaded ", "NET content ready ")),
                    f"{label}: server transferred content before password admission")
            result["phases"][label] = dict(reason=reason, content_sent=False)
            rejected.process.terminate()
            rejected.process.wait(timeout=10)
            stop(rejected)

        client = launch("addon-client")
        pid = client.process.pid
        client.wait(lambda text: text.count("Started at ") >= 2 and "NET content ready " in text,
                    "addon download, private exec and overlay reconnect", args.timeout)
        state = poll_status(client, lambda row: row["role"] == "client" and row["state"] == "playing",
                            "authoritative snapshots after addon restart", args.timeout)
        require(client.process.pid == pid and client.text().count("Started at ") == 2,
                "addon did not exec the original process exactly once")
        require("Server addon verified. Restarting Rednukem" in client.text(), "missing addon was not activated")
        require("E1L1: DOWNLOADED OVERLAY ROOM ONE" in client.text(), "downloaded CON was not loaded")
        cache = list((client.path / ".rednukem-addon-cache").glob("*/server-only-addon.grp"))
        require(len(cache) == 1 and cache[0].read_bytes() == addon,
                "downloaded addon differs from the server archive")
        require(PASSWORD not in client.text(), "server password appeared in the game log")
        result["phases"]["addon-hotjoin-restart"] = dict(
            pid=pid, starts=2, status=state, bytes=len(addon),
            sha256=hashlib.sha256(addon).hexdigest(), cache=str(cache[0].relative_to(client.path)))
        print("PASS: graphical overlay authentication, hot join and addon exec/reconnect", flush=True)

        # The map is neither in the common fixture path nor the typed addon.
        # Only the server receives it; the already-running overlay handles the
        # transfer while its authoritative world continues to advance.
        (host.path / alias).write_bytes(map_bytes)
        generation = int(host.status()["map_generation"])
        host.command_text("map " + alias)
        mapped = poll_status(client, lambda row: row["state"] == "playing" and
                             int(row["map_generation"]) > generation,
                             "server-only user map download", args.timeout)
        maps = list((client.path / ".rednukem-addon-cache").glob("*/" + alias))
        require(len(maps) == 1 and maps[0].read_bytes() == map_bytes,
                "downloaded user map differs from server bytes")
        require(not (client.path / alias).exists(), "map download escaped its private cache")
        require("Downloading map " in client.text(), "missing map did not use the download path")
        players = records(client.text(), "NET player ")
        slot = mapped["local"]
        before = next(row for row in reversed(players) if row["slot"] == slot)
        client.key(26, True)
        time.sleep(1)
        client.key(26, False)
        client.input_barrier()
        time.sleep(.3)
        client.status()
        after = next(row for row in reversed(records(client.text(), "NET player ")) if row["slot"] == slot)
        require(any(before[key] != after[key] for key in ("x", "y", "z")),
                "overlay player did not move after map transfer")
        require(host.status()["players"] == "2", "listen host lost its player roster during transfers")
        result["phases"]["missing-map-gameplay"] = dict(
            status=mapped, bytes=len(map_bytes), sha256=hashlib.sha256(map_bytes).hexdigest(),
            cache=str(maps[0].relative_to(client.path)), before=before, after=after)
        print("PASS: current user-map download and authoritative overlay movement", flush=True)
        stop(client)
        stop(host)
        result["passed"] = True
    except Exception as error:
        result["failures"].append(str(error))
        raise
    finally:
        for game in reversed(games):
            game.stop()
        result["games"] = [{"name": game.name, "command": game.command,
                            "exit_code": game.process.poll(), "log": str(game.path / "console.log")}
                           for game in all_games]
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
