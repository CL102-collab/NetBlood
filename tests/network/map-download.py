#!/usr/bin/env python3
"""Exercise automatic server-only map downloads through real UDP sessions.

Uses the ordinary executable, SDL input, and isolated working directories. No
map files are placed on a client's search path before its initial connection.
Blood 1.21 game assets, two valid external maps, and an X11 display are required.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

from relay import Relay
from run import Failure, Game, build_driver, inspect_demo, play_demo, records, require


def cached_copy(game, contents):
    """Identify the installed file by its complete bytes, independently of logs."""
    matches = [path for path in game.path.rglob("*")
               if path.is_file() and path.stat().st_size == len(contents) and path.read_bytes() == contents]
    require(len(matches) == 1, f"{game.name}: expected exactly one byte-exact cached map, found {matches}")
    return matches[0]


def joined(game, previous=0, timeout=60):
    return game.wait(lambda text: len(records(text, "NET client map loaded ")) > previous and
                     len(records(text, "NET client slot=")) > previous,
                     f"downloaded map {previous+1} and authoritative state", timeout)


def quit_before_map(game):
    # Console binds are processed only after gameplay starts. F10 and the quit
    # confirmation use the ordinary menu key path even while joining or after
    # a missing-map error. SDL_QUIT itself is intentionally mapped to Escape.
    game.key(67, True)  # SDL_SCANCODE_F10
    time.sleep(0.25)
    game.key(67, False)
    game.key(28, True)  # SDL_SCANCODE_Y
    game.process.wait(timeout=10)
    game.stop()


def main():
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=repository / "nblood")
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--engine-data", type=Path, default=repository)
    parser.add_argument("--first-map", type=Path, help="default: ASSETS/CP01.MAP")
    parser.add_argument("--second-map", type=Path, help="default: ASSETS/CP02.MAP")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()
    for name in ("binary", "assets", "engine_data"):
        setattr(args, name, getattr(args, name).resolve())
    require(os.environ.get("DISPLAY"), "clients need DISPLAY (use an external Xvfb wrapper)")
    require(args.binary.is_file() and os.access(args.binary, os.X_OK) and args.assets.is_dir(),
            "build executable and supply game assets first")
    source_maps = [(args.first_map or args.assets / "CP01.MAP").resolve(),
                   (args.second_map or args.assets / "CP02.MAP").resolve()]
    require(all(path.is_file() for path in source_maps), "supply two valid external maps")
    map_bytes = [path.read_bytes() for path in source_maps]
    require(map_bytes[0] != map_bytes[1], "the two fixtures must be different maps")
    # The unique server-only names cannot resolve to stock maps in BLOOD.RFF.
    aliases = [f"NBDL_{os.getpid()}_{number}.MAP" for number in (1, 2)]
    require(all(not (directory / alias).exists() for directory in (args.assets, args.engine_data)
                for alias in aliases), "fixture aliases already exist in a shared search path")
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="netblood-map-download-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print(f"Map-download evidence: {root}", flush=True)
    library = build_driver(root)
    source_binary = args.binary
    args.binary = root / "nblood-tested"
    shutil.copy2(source_binary, args.binary)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    games, relays = [], []
    result = {"passed": False, "source_binary": str(source_binary),
              "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(), "failures": [],
              "fixtures": [dict(source=str(path), server_name=alias, bytes=len(contents),
                                sha256=hashlib.sha256(contents).hexdigest())
                           for path, alias, contents in zip(source_maps, aliases, map_bytes)]}
    try:
        # The first descriptor is an absolute server path. Even though these
        # test processes share a machine, the client must treat that path as an
        # opaque name and receive bytes through UDP, never read the server cwd.
        first_descriptor = str(root / "server" / aliases[0])
        result["absolute_server_map"] = first_descriptor
        server = Game("server", args, root, library, port, dedicated=True,
                      initial_files=dict(zip(aliases, source_maps)), launch_map=first_descriptor)
        games.append(server)
        server.wait(lambda text: "Dedicated server running:" in text and
                    any(int(row["server_tick"]) >= 30 and row["players"] == "0"
                        for row in records(text, "NET server_tick=")), "empty custom-map server", args.timeout)
        relay = Relay(("127.0.0.1", port), loss=0.08, delay_ms=35, jitter_ms=10, reorder=0.1, seed=68).start()
        relays.append(relay)
        first = Game("client-1", args, root, library, relay.port)
        games.append(first)
        require(all(not (first.path / alias).exists() for alias in aliases), "initial client already has server map")
        joined(first, timeout=args.timeout)
        require("NET map download started" in first.text() and "NET map download complete" in first.text(),
                "missing map did not complete an actual transfer")
        first_cache = cached_copy(first, map_bytes[0])
        first.command_text("net_record downloaded-maps.nbpd\nnet_status")
        time.sleep(1)
        server.command_text(f"map {aliases[1]}")
        joined(first, previous=1, timeout=args.timeout)
        require(first.text().count("NET map download complete") == 2,
                "map transition did not transfer the second server-only map")
        second_cache = cached_copy(first, map_bytes[1])
        time.sleep(2)
        first.command_text("net_stoprecord\nnet_status\nscreenshot")
        roundtrips = records(first.text(), "NET snapshot roundtrip ")
        require(len(roundtrips) >= 10 and all(row["equal"] == "1" for row in roundtrips),
                "downloaded maps did not install exact authoritative snapshots")
        result["exact_installed_snapshot_roundtrips"] = len(roundtrips)
        require(all(not (first.path / alias).exists() for alias in aliases),
                "download created a server-named file outside its private cache")
        cache_files = {str(path.relative_to(first.path)): path for path in (first_cache, second_cache)}
        result["installed_cache"] = list(cache_files)
        demo = inspect_demo(first.path / "downloaded-maps.nbpd")
        require(len({row["generation"] for row in demo["packets"]}) == 2,
                "recording does not contain both downloaded maps")
        require(all(row["type"] not in (12, 13) for row in demo["packets"]),
                "packet demo contains transfer requests or map chunks instead of using the validated cache")
        result["packet_demo"] = demo
        # Replay both downloaded maps offline, including the previous generation.
        result["packet_demo_playback"] = play_demo("download-replay", demo, args, root, library, games, existing=first)
        # A new process receives only the verified cache, never the original
        # filenames. This also tests persisted cache identity across sessions.
        reused = Game("cache-rejoin", args, root, library, port, initial_files=cache_files)
        games.append(reused)
        joined(reused, timeout=args.timeout)
        reused.command_text("net_status")
        require("NET map cache hit" in reused.text() and "NET map download started" not in reused.text(),
                "rejoining client downloaded an already verified cached map")
        require(cached_copy(reused, map_bytes[1]).relative_to(reused.path) == second_cache.relative_to(first.path),
                "rejoining client did not retain its verified cached map")
        reused.stop()
        # Damaged cache bytes must never reach the fatal game map loader. The
        # client must request and install a fresh verified copy instead.
        corrupt = bytearray(map_bytes[1])
        corrupt[len(corrupt)//2] ^= 0x80
        damaged_files = dict(cache_files)
        damaged_files[str(second_cache.relative_to(first.path))] = bytes(corrupt)
        repaired = Game("corrupt-cache-rejoin", args, root, library, port, initial_files=damaged_files)
        games.append(repaired)
        joined(repaired, timeout=args.timeout)
        require("NET map download complete" in repaired.text(), "corrupt cache did not trigger a fresh download")
        result["repaired_cache"] = str(cached_copy(repaired, map_bytes[1]).relative_to(repaired.path))
        repaired.command_text("net_status")
        repaired.stop()
        installed = Game("installed-map-client", args, root, library, port,
                         initial_files={aliases[1]: source_maps[1]})
        games.append(installed)
        joined(installed, timeout=args.timeout)
        require("NET map download started" not in installed.text(), "client downloaded its installed matching map")
        require((installed.path / aliases[1]).read_bytes() == map_bytes[1], "installed map was modified")
        installed.command_text("net_status")
        installed.stop()
        mismatched = Game("different-installed-map", args, root, library, port,
                          initial_files={aliases[1]: source_maps[0]})
        games.append(mismatched)
        joined(mismatched, timeout=args.timeout)
        require("NET map download complete" in mismatched.text(), "different installed map did not trigger a download")
        require((mismatched.path / aliases[1]).read_bytes() == map_bytes[0],
                "download overwrote a different installed map")
        cached_copy(mismatched, map_bytes[1])
        mismatched.stop()
        missing_replay = Game("uncached-offline-replay", args, root, library, playback=True)
        games.append(missing_replay)
        missing_replay.wait(lambda text: "> This map" in text, "offline setup", args.timeout)
        time.sleep(1)
        missing_replay.command_text(f'net_playback "{demo["path"]}"')
        missing_replay.wait(lambda text: "NET map download failed: Map is unavailable for offline playback" in text,
                            "missing offline map reported", args.timeout)
        require("NET client slot=" not in missing_replay.text() and "NET client map loaded " not in missing_replay.text() and
                "NET map download started" not in missing_replay.text(),
                "offline playback tried networking or applied a world without its map")
        quit_before_map(missing_replay)
        slow_relay = Relay(("127.0.0.1", port), delay_ms=250, seed=69).start()
        relays.append(slow_relay)
        cancelled = Game("cancelled-download", args, root, library, slow_relay.port)
        games.append(cancelled)
        cancelled.wait(lambda text: "NET map download started" in text, "download to cancel", args.timeout)
        require("NET map download complete" not in cancelled.text(), "cancellation happened after transfer completed")
        quit_before_map(cancelled)
        require("NET client map loaded " not in cancelled.text(), "cancelled transfer loaded an incomplete map")
        require(not list((cancelled.path / ".nblood-map-cache").glob("*")),
                "cancelled transfer left published or temporary map files")
        after_cancel = Game("join-after-cancellation", args, root, library, port)
        games.append(after_cancel)
        joined(after_cancel, timeout=args.timeout)
        cached_copy(after_cancel, map_bytes[1])
        after_cancel.stop()
        # A regular file at the directory name reliably fails on any Unix user,
        # including root (a chmod-only test would not establish write failure).
        blocked = Game("blocked-cache-client", args, root, library, port,
                       initial_files={".nblood-map-cache": b"cache directory blocked by test\n"})
        games.append(blocked)
        blocked.wait(lambda text: "NET map download failed" in text, "cache failure reported", args.timeout)
        require("NET client slot=" not in blocked.text() and "NET client map loaded " not in blocked.text(),
                "client played before its missing map was verified")
        require((blocked.path / ".nblood-map-cache").read_bytes() == b"cache directory blocked by test\n",
                "download overwrote the cache-path obstacle")
        quit_before_map(blocked)
        server.stop()
        require("NETWORK_TEST_DEVICES windows=0 audio=0" in server.console(), "dedicated server opened a device")
        impairment = relay.report()["counters"]
        require(impairment.get("server_to_client_lost", 0) > 0 and
                impairment.get("server_to_client_reordered", 0) > 0, "download was not impaired on the wire")
        for game in games:
            require(game.process.returncode == 0 and not game.forced, f"{game.name} did not shut down cleanly")
            require(not re.search(r"NET (?:apply snapshot|capture failed|invalid bootstrap|local map CRC mismatch|snapshot verification failed)|AddressSanitizer|runtime error:|Segmentation fault",
                                  game.text()+game.console()), f"{game.name} reported a runtime/snapshot error")
        result["passed"] = True
    except (Failure, OSError, subprocess.SubprocessError) as error:
        result["failures"].append(str(error))
    finally:
        for game in reversed(games):
            game.stop()
        for relay in relays:
            relay.close()
        result["games"] = {game.name: dict(game.result(),
                                          downloads=records(game.text(), "NET map download complete"),
                                          cache_hits=records(game.text(), "NET map cache hit"),
                                          local_matches=records(game.text(), "NET map local match"))
                           for game in games}
        result["relays"] = [relay.report() for relay in relays]
        (root / "result.json").write_text(json.dumps(result, indent=2)+"\n")
    print(("PASS" if result["passed"] else "FAIL") + f": {root / 'result.json'}", flush=True)
    for failure in result["failures"]:
        print(failure, file=sys.stderr)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
