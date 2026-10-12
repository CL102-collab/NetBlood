#!/usr/bin/env python3
"""Exercise scoreboard metadata, F-key binding and packet demos with real clients.

Requires an X11 display and Blood 1.21 assets. The minute boundary uses real
elapsed time; scores change through an ordinary fatal fall in a generated map.
"""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import tempfile
import time
import zlib

from cinematics import capture_window, focus_window, free_port
from relay import Relay
from run import Failure, Game as BaseGame, build_driver, inspect_demo, play_demo, records, require


class Game(BaseGame):
    def command_text(self, commands, timeout=10):
        # Background frame throttling can consume key-up and the next key-down
        # in one iteration, preventing an ordinary norepeat console binding
        # from firing again. Give the actual window focus before each command.
        if not self.dedicated and self.process.poll() is None:
            focus_window(self)
        return super().command_text(commands, timeout)


def scoreboard(game):
    offset = len(game.text())
    game.command_text("net_status")
    text = game.text()[offset:]
    rows = []
    for line in text.splitlines():
        if "NET scoreboard slot=" not in line:
            continue
        values = dict(re.findall(r"(\w+)=([^\s,]+)", line))
        name = re.search(r' name="(.*)"', line)
        require(name, f"{game.name}: scoreboard name missing: {line}")
        values["name"] = name[1]
        rows.append(values)
    return rows


def expect(game, predicate, description, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        rows = scoreboard(game)
        if predicate(rows):
            return rows
        time.sleep(0.2)
    raise Failure(f"{game.name}: {description}; last scoreboard={rows}")


def wire_rows(payload):
    """Read the independently encoded scoreboard payload from a packet demo."""
    require(payload, "empty scoreboard payload")
    count, offset, rows = payload[0], 1, []
    require(count <= 8, "too many recorded scoreboard rows")
    for _ in range(count):
        require(offset + 24 <= len(payload), "truncated scoreboard row")
        slot, team, generation, ping, minutes, score, lives, downed, giveup = struct.unpack_from("<BBIIIiiBB", payload, offset)
        offset += 24
        require(-1 <= lives <= 255, "invalid scoreboard life count")
        require(downed in (0, 1) and giveup <= 90 and (lives > 0 if downed else giveup == 0),
                "invalid scoreboard downed state")
        strings = []
        for maximum in (15, 2):
            require(offset + 2 <= len(payload), "truncated scoreboard string length")
            length = struct.unpack_from("<H", payload, offset)[0]
            offset += 2
            require(length <= maximum and offset + length <= len(payload), "truncated scoreboard string")
            strings.append(payload[offset:offset + length].decode("latin1"))
            offset += length
        rows.append(dict(slot=str(slot), team=str(team), generation=str(generation), ping_ms=str(ping),
                         minutes=str(minutes), score=str(score), lives=str(lives), downed=str(downed), giveup=str(giveup),
                         name=strings[0], country=strings[1] or "--"))
    require(offset == len(payload), "trailing scoreboard bytes")
    return rows


def demo_scoreboards(path):
    data = path.read_bytes()
    size = struct.unpack_from("<I", data, 12)[0]
    offset, found = 20 + size, []
    while offset < len(data):
        marker, length, elapsed, tick, _ = struct.unpack_from("<IIQII", data, offset)
        offset += 24
        body = data[offset:offset + length]
        offset += length
        if marker == 0x31444e45:
            break
        if struct.unpack_from("<I", body, 8)[0] == 21:
            found.append(dict(elapsed_ticks=elapsed, tick=tick, rows=wire_rows(body[28:])))
    require(found, "packet demo omitted all scoreboard metadata")
    return found


def synthetic_flags(source, target):
    """Make a clearly synthetic display fixture; never infer countries on loopback."""
    data = source.read_bytes()
    size = struct.unpack_from("<I", data, 12)[0]
    output, offset = bytearray(data[:20 + size]), 20 + size
    while offset < len(data):
        marker, length, elapsed, tick, _ = struct.unpack_from("<IIQII", data, offset)
        offset += 24
        body = bytearray(data[offset:offset + length])
        offset += length
        if marker != 0x31444e45 and struct.unpack_from("<I", body, 8)[0] == 21:
            rows = wire_rows(body[28:])
            payload = bytearray([len(rows)])
            for row in rows:
                payload += struct.pack("<BBIIIiiBB", *[int(row[key]) for key in
                                                     ("slot", "team", "generation", "ping_ms", "minutes", "score", "lives", "downed", "giveup")])
                for text in (row["name"], ("HU", "US", "JP", "GB")[int(row["slot"]) % 4]):
                    encoded = text.encode("latin1")
                    payload += struct.pack("<H", len(encoded)) + encoded
            body = body[:28] + payload
            struct.pack_into("<I", body, 24, len(payload))
        header = struct.pack("<IIQI", marker, len(body), elapsed, tick)
        output += header + struct.pack("<I", zlib.crc32(body, zlib.crc32(header))) + body
    target.write_bytes(output)


def main():
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=repository / "nblood")
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--engine-data", type=Path, default=repository)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--renderer", type=int, choices=(0, 3, 4, 5), default=0)
    parser.add_argument("--quick", action="store_true", help="omit the real 60-second minute-boundary check")
    parser.add_argument("--skip-screenshots", action="store_true")
    args = parser.parse_args()
    for name in ("binary", "assets", "engine_data"):
        setattr(args, name, getattr(args, name).resolve())
    require(os.environ.get("DISPLAY"), "clients need DISPLAY (use an Xvfb wrapper)")
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="netblood-scoreboard-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print(f"Scoreboard evidence: {root}", flush=True)
    source_binary = args.binary
    args.binary = root / "nblood-tested"
    shutil.copy2(source_binary, args.binary)
    library = build_driver(root)
    spec = importlib.util.spec_from_file_location("team_balance_fixture", Path(__file__).with_name("team-balance.py"))
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    files = {"NBSCORE1.MAP": fixture.death_map(), "NBSCORE2.MAP": fixture.death_map(1)}
    result = dict(passed=False, source_binary=str(source_binary),
                  binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                  renderer=args.renderer, quick=args.quick, phases={}, failures=[])
    games, relays = [], []

    def client(name, port, player_name, custom=False, free_f7=False):
        initial = dict(files)
        if custom:
            initial["network_settings.cfg"] = b'bind F11 norepeat "echo SCOREBOARD_CUSTOM_F11"\n'
        if free_f7:
            initial["network_settings.cfg"] = initial.get("network_settings.cfg", b"") + b"unbind F7\n"
        game = Game(name, args, root, library, port,
                    launch_arguments=["-client", "127.0.0.1", "-port", str(port), "-pname", player_name],
                    initial_files=initial)
        games.append(game)
        game.wait(lambda text: "NET client map loaded " in text and "NET client slot=" in text,
                  "first authoritative snapshot", args.timeout)
        return game

    def binding(game, key):
        offset = len(game.text())
        game.command_text("bind " + key)
        return game.text()[offset:]

    def picture(game, name, key=68):
        focus_window(game)
        game.key(key, True)  # F11 by default; hold, do not toggle.
        try:
            time.sleep(0.3)
            return capture_window(game, root / f"{name}.png")
        finally:
            game.key(key, False)

    try:
        port = free_port()
        server = Game("server", args, root, library, port, dedicated=True, initial_files=files,
                      launch_arguments=["-dedicated", "-port", str(port), "-gametype", "bloodbath",
                                        "-maxplayers", "4", "-map", "NBSCORE1.MAP"])
        games.append(server)
        server.wait(lambda text: "Dedicated server running:" in text and "NET server_tick=" in text,
                    "dedicated server", args.timeout)
        first = client("client-1", port, "Alice")
        joined_at = time.monotonic()
        relay = Relay(("127.0.0.1", port), delay_ms=90, seed=41).start()
        relays.append(relay)
        second = client("client-2", relay.port, "Caleb_Score", custom=True)
        initial = expect(first, lambda rows: len(rows) == 2, "two-player scoreboard")
        require([row["name"] for row in initial] == ["Alice", "Caleb_Score"], "server-assigned names missing")
        require(all(row["country"] == "--" and row["team"] == "255" for row in initial),
                "loopback must have unknown countries, non-team games no team")
        require(all(row["minutes"] == "0" and row["score"] == "0" for row in initial), "new player ages/scores not zero")
        result["phases"]["join"] = initial
        require('"gamefunc_Show_Scoreboard"' in binding(first, "F11"), "fresh configuration did not choose free F11")
        focus_window(second)
        offset = len(second.text())
        second.key(68, True)
        second.wait(lambda text: "SCOREBOARD_CUSTOM_F11" in text[offset:], "preserved custom F11")
        second.key(68, False)
        second.command_text('bind G "gamefunc_Show_Scoreboard"')
        result["phases"]["custom_f11"] = True

        # Move into the map's deep pit; ordinary final death costs one point.
        focus_window(first)
        first.key(26, True)
        deadline = time.monotonic() + args.timeout
        try:
            while time.monotonic() < deadline:
                rows = scoreboard(server)
                if rows and int(rows[0]["score"]) == -1:
                    break
                time.sleep(0.1)
            else:
                raise Failure("ordinary fall did not produce a -1 scoreboard score")
        finally:
            first.key(26, False)
        scored = expect(second, lambda rows: len(rows) == 2 and rows[0]["score"] == "-1", "remote signed score update")
        result["phases"]["score"] = scored
        if not args.quick:
            remaining = max(0, 62 - (time.monotonic() - joined_at))
            print(f"Waiting {remaining:.1f}s for the real connection-age minute boundary", flush=True)
            until = time.monotonic() + remaining
            while time.monotonic() < until:
                time.sleep(min(5, max(0, until - time.monotonic())))
            aged = expect(first, lambda rows: len(rows) == 2 and int(rows[0]["minutes"]) >= 1,
                          "elapsed connection minute")
            result["phases"]["minute_boundary"] = aged
        delayed = expect(first, lambda rows: len(rows) == 2 and int(rows[1]["ping_ms"]) >= 120,
                         "server-measured delayed peer ping", args.timeout)
        result["phases"]["server_ping"] = delayed
        print("PASS: authoritative names, signed score, delayed-peer RTT and connection age", flush=True)
        if not args.skip_screenshots:
            result["scoreboard"] = picture(first, "scoreboard")
            result["remapped_scoreboard"] = picture(second, "scoreboard-custom-key", key=10)
            time.sleep(0.25)
            result["released"] = capture_window(first, root / "scoreboard-released.png")

        first.command_text("net_record scoreboard.nbpd")
        late = client("late-client", port, "Duke_New", custom=True, free_f7=True)
        require('"gamefunc_Show_Scoreboard"' in binding(late, "F7"), "migration did not use the available F7")
        require("SCOREBOARD_CUSTOM_F11" in binding(late, "F11"), "F7 migration overwrote custom F11")
        result["phases"]["free_f7"] = True
        joined = expect(first, lambda rows: len(rows) == 3, "late join roster")
        require(joined[2]["minutes"] == "0" and joined[2]["name"] == "Duke_New", "late join reused another age/name")
        old_generation = joined[2]["generation"]
        late.stop()
        left = expect(first, lambda rows: len(rows) == 2, "departed player removal")
        replacement = client("replacement-client", port, "New_Connection")
        replaced = expect(first, lambda rows: len(rows) == 3 and rows[2]["name"] == "New_Connection", "slot reuse")
        require(replaced[2]["generation"] != old_generation and replaced[2]["minutes"] == "0",
                "slot reuse inherited previous identity or elapsed time")
        result["phases"]["roster"] = dict(joined=joined, left=left, reused=replaced)
        count = len(records(first.text(), "NET client map loaded "))
        age_before = int(replaced[0]["minutes"])
        server.command_text("map NBSCORE2")
        first.wait(lambda text: len(records(text, "NET client map loaded ")) > count, "second map", args.timeout)
        after_map = expect(first, lambda rows: len(rows) == 3 and rows[0]["score"] == "0", "post-map scoreboard")
        require(int(after_map[0]["minutes"]) >= age_before and
                [row["generation"] for row in after_map] == [row["generation"] for row in replaced],
                "map transition reset connection age or identity")
        result["phases"]["map"] = after_map
        time.sleep(1.2)
        first.command_text("net_stoprecord")
        demo = inspect_demo(first.path / "scoreboard.nbpd")
        metadata = demo_scoreboards(first.path / "scoreboard.nbpd")
        require(any(len(item["rows"]) == 2 for item in metadata) and any(len(item["rows"]) == 3 for item in metadata),
                "packet demo lost roster changes")
        result["demo"] = dict(**demo, scoreboards=metadata)
        final_expected = metadata[-1]["rows"]
        first.command_text("unbind F11")
        for game in (replacement, second, first, server):
            game.stop()
        relay.close()
        # Reuse the actual config saved at shutdown, including migration marker.
        persisted = dict(files)
        persisted["persisted.cfg"] = first.path / "network.cfg"
        persisted["persisted_cvars.cfg"] = first.path / "network_cvars.cfg"
        unbound = Game("deliberate-unbinding", args, root, library, playback=True, initial_files=persisted,
                       launch_arguments=["-cfg", "persisted.cfg", "-map", "NBSCORE1"])
        games.append(unbound)
        unbound.wait(lambda text: "> This map" in text, "saved configuration startup", args.timeout)
        require("F11 is unbound" in binding(unbound, "F11"), "restart rebound an intentionally unbound scoreboard")
        unbound.stop()
        result["phases"]["deliberate_unbinding_persists"] = True
        captured_files = dict(files)
        captured_files["network_settings.cfg"] = (
            b'bind F7 "gamefunc_Show_Console"\nunbind F7\n'
            b'bind F11 norepeat "echo SCOREBOARD_CUSTOM_F11"\n')
        captured = Game("console-captured-f7", args, root, library, playback=True,
                        initial_files=captured_files, launch_map="NBSCORE1")
        games.append(captured)
        captured.wait(lambda text: "> This map" in text, "console-capture configuration startup", args.timeout)
        require("F7 is unbound" in binding(captured, "F7"), "migration reused the separately captured console key")
        require("SCOREBOARD_CUSTOM_F11" in binding(captured, "F11"), "console-key exclusion overwrote custom F11")
        captured.stop()
        result["phases"]["console_capture_preserved"] = True
        replay = Game("replay", args, root, library, playback=True, initial_files=files)
        games.append(replay)
        replay.wait(lambda text: "> This map" in text, "offline replay startup", args.timeout)
        result["replay"] = play_demo("replay", demo, args, root, library, games, existing=replay)
        replay_text = replay.text()
        last_status = replay_text.rsplit("NET status ", 1)[-1]
        actual = []
        for line in last_status.splitlines():
            if "NET scoreboard slot=" in line:
                row = dict(re.findall(r"(\w+)=([^\s,]+)", line))
                row["name"] = re.search(r' name="(.*)"', line)[1]
                actual.append(row)
        require(actual == final_expected, f"packet playback scoreboard differs: expected {final_expected}, actual {actual}")
        result["replay"]["scoreboard"] = actual
        print("PASS: late join, departure, slot reuse, map age continuity and exact packet-demo scoreboard replay", flush=True)
        if not args.skip_screenshots:
            # These countries are explicitly synthetic fixture values. The live
            # local clients above correctly show unknown-country placeholders.
            synthetic = root / "synthetic-country-flags.nbpd"
            synthetic_flags(first.path / "scoreboard.nbpd", synthetic)
            visual = Game("synthetic-flags-replay", args, root, library, playback=True, initial_files=files)
            games.append(visual)
            visual.wait(lambda text: "> This map" in text, "synthetic playback startup", args.timeout)
            visual.command_text(f'net_playback "{synthetic}"')
            duration = max(row["elapsed_ticks"] for row in demo["packets"]) / 30
            time.sleep(duration + 1)
            result["synthetic_flags_screenshot"] = picture(visual, "synthetic-country-flags")
            result["synthetic_flags_screenshot"]["description"] = "Display-only fixture: HU/US/JP countries substituted into recorded metadata"
            visual.stop()
        result["passed"] = True
    except Exception as error:
        result["failures"].append(f"{type(error).__name__}: {error}")
    finally:
        for game in reversed(games):
            game.stop()
        for relay in relays:
            relay.close()
        result["games"] = {game.name: game.result() for game in games}
        for game in games:
            if game.process.returncode != 0 or game.forced:
                result["failures"].append(f"{game.name}: unclean exit {game.process.returncode}")
            if re.search(r"runtime error:|AddressSanitizer|snapshot roundtrip.*equal=0|NET snapshot invalid", game.text() + game.console()):
                result["failures"].append(f"{game.name}: runtime/snapshot error")
        result["passed"] = result["passed"] and not result["failures"]
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(dict(passed=result["passed"], failures=result["failures"], evidence=str(root))), flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
