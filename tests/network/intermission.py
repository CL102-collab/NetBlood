#!/usr/bin/env python3
"""Exercise final results, a real 15-second pause and map rotation with clients.

Needs Blood 1.21 assets and X11/Xvfb. A generated legal map and ordinary
movement/pitchfork attacks produce a winning frag; the time-limit case uses a
real minute. No accelerated clocks, cheats or game-state injection are used.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import time
import zlib

from cinematics import focus_window, free_port
from relay import Relay
from run import Failure, Game as BaseGame, build_driver, inspect_demo, records, require
from scoreboard import wire_rows


class Game(BaseGame):
    def command_text(self, commands, timeout=10):
        if not self.dedicated and self.process.poll() is None:
            focus_window(self)
        return super().command_text(commands, timeout)


def arena_map(revision=0):
    """Flat empty arena with eight separated east-facing Bloodbath starts."""
    def u16(data, offset, value):
        struct.pack_into("<H", data, offset, value & 0xffff)

    def u32(data, offset, value):
        struct.pack_into("<I", data, offset, value & 0xffffffff)

    data = bytearray(45)
    data[:4] = b"BLM\x1a"
    u16(data, 4, 0x600)
    u32(data, 6, -7168)
    u32(data, 6 + 0x1b, revision)
    for offset, value in ((0x1f, 1), (0x21, 4), (0x23, 8)):
        u16(data, 6 + offset, value)
    sector = bytearray(40)
    u16(sector, 2, 4)
    u32(sector, 4, -32768)
    u16(sector, 38, -1)
    data += sector
    for index, (x, y) in enumerate(((-16384, -8192), (16384, -8192), (16384, 8192), (-16384, 8192))):
        wall = bytearray(32)
        u32(wall, 0, x)
        u32(wall, 4, y)
        u16(wall, 8, (index + 1) % 4)
        for offset in (10, 12, 18, 30):
            u16(wall, offset, -1)
        wall[22:24] = bytes((8, 8))
        data += wall
    for index in range(8):
        sprite = bytearray(44)
        u32(sprite, 0, -7168 + 2048 * index)
        sprite[20:22] = bytes((64, 64))
        u16(sprite, 30, -1)
        u16(sprite, 38, 2)
        u16(sprite, 42, index + 1)
        data += sprite
        extended = bytearray(56)
        u16(extended, 0, index)
        u16(extended, 16, index)
        u16(extended, 30, -1)
        u16(extended, 46, -1)
        data += extended
    return bytes(data) + struct.pack("<I", zlib.crc32(data))


def status(game):
    offset = len(game.text())
    game.command_text("net_status")
    text = game.text()[offset:]
    found = {name: records(text, prefix) for name, prefix in (
        ("session", "NET status "), ("rules", "NET rules "),
        ("intermission", "NET intermission active="), ("world", "NET world "))}
    require(found["session"] and found["rules"] and found["intermission"],
            f"{game.name}: status omitted session, rules or intermission state")
    state = {name: rows[-1] if rows else None for name, rows in found.items()}
    state["players"] = {row["slot"]: row for row in records(text, "NET player ")}
    state["scoreboard"] = records(text, "NET scoreboard ")
    return state


def expect(game, predicate, description, timeout=45):
    deadline, state = time.monotonic() + timeout, None
    while time.monotonic() < deadline:
        state = status(game)
        if predicate(state):
            return state
        time.sleep(0.05)
    raise Failure(f"{game.name}: timed out waiting for {description}; last state={state}")


def press(game, scan, seconds=0.2):
    focus_window(game)
    game.key(scan, True)
    try:
        # A cold or slow rendering frame can outlast the requested hold and
        # otherwise drain key-down and key-up in the same event-loop iteration.
        game.input_barrier()
        time.sleep(seconds)
    finally:
        game.key(scan, False)
        game.input_barrier()
    time.sleep(0.15)


def frozen_players(state, slots):
    fields = ("x", "y", "z", "health", "frags", "weapon", "ammo_sum", "armor_sum", "weapons_mask")
    return {str(slot): {field: state["players"][str(slot)][field] for field in fields} for slot in slots}


def demo_intermissions(path):
    data = path.read_bytes()
    offset, found = 20 + struct.unpack_from("<I", data, 12)[0], []
    while offset < len(data):
        marker, length, elapsed, tick, _ = struct.unpack_from("<IIQII", data, offset)
        offset += 24
        body = data[offset:offset + length]
        offset += length
        if marker == 0x31444e45:
            break
        if struct.unpack_from("<I", body, 8)[0] == 23:  # netwire::Type::Intermission.
            active, remaining = struct.unpack_from("<BI", body, 28)
            require(active in (0, 1) and remaining <= 15000, "invalid recorded intermission")
            found.append({"elapsed_ticks": elapsed, "tick": tick, "active": active,
                          "remaining_ms": remaining, "rows": wire_rows(body[33:])})
    require(found, "packet demo omitted all intermission metadata")
    return found


def main():
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=repository / "nblood")
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--engine-data", type=Path, default=repository)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--renderer", type=int, choices=(0, 3, 5), default=3)
    parser.add_argument("--second-renderer", type=int, choices=(0, 3, 5), default=5)
    parser.add_argument("--skip-screenshots", action="store_true")
    parser.add_argument("--skip-time-limit", action="store_true", help="omit the real-minute limit/wrap check")
    args = parser.parse_args()
    for name in ("binary", "assets", "engine_data"):
        setattr(args, name, getattr(args, name).resolve())
    require(os.environ.get("DISPLAY"), "clients need DISPLAY (use an Xvfb wrapper)")
    require(args.binary.is_file() and args.assets.is_dir(), "supply a built executable and game assets")
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="netblood-intermission-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print(f"Intermission evidence: {root}", flush=True)
    source_binary = args.binary
    args.binary = root / "nblood-tested"
    shutil.copy2(source_binary, args.binary)
    library = build_driver(root)
    fixtures = {"NBEND1.MAP": arena_map(), "NBEND2.MAP": arena_map(1)}
    result = {"passed": False, "source_binary": str(source_binary),
              "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
              "phases": {}, "screenshots": {}, "failures": []}
    games, relays, pictures = [], [], []

    def client(name, port, player_name, renderer, wait=True):
        game = Game(name, args, root, library, port, renderer=renderer, initial_files=fixtures,
                    launch_arguments=["-client", "127.0.0.1", "-port", str(port), "-pname", player_name])
        games.append(game)
        return joined(game) if wait else game

    def joined(game, configure_controls=True):
        game.wait(lambda text: "NET client map loaded " in text and "NET client slot=" in text,
                  "first authoritative snapshot", args.timeout)
        if configure_controls:
            game.command_text('bind S "gamefunc_Move_Backward"\nbind X norepeat "gamefunc_Turn_Around"\n'
                              'bind H "gamefunc_Open"')
        return game, records(game.text(), "NET welcome ")[-1]["slot"]

    def picture(game, name):
        if not args.skip_screenshots:
            path = root / (name + ".png")
            window = focus_window(game)
            # Wait for ordinary event/render iterations after exposing this
            # window; otherwise a slow frame can still show the previous
            # overlapping client's pixels on an uncomposited Xvfb display.
            game.input_barrier()
            subprocess.run(["import", "-window", hex(window), str(path)], check=True)
            pictures.append((game.name, name, path))
            result["screenshots"][name] = {"path": str(path)}

    def check_pictures():
        # Image analysis is deliberately outside the real 15-second deadline.
        # The captured pixels and all readability assertions remain identical.
        if not pictures:
            return
        from PIL import Image
        for game_name, name, path in pictures:
            image = Image.open(path).convert("RGB")
            pixels = list(image.get_flattened_data() if hasattr(image, "get_flattened_data") else image.getdata())
            captured = {"path": str(path), "width": image.width, "height": image.height,
                        "black_fraction": sum(max(pixel) < 12 for pixel in pixels) / len(pixels),
                        "white_pixels": sum(min(pixel) > 200 and max(pixel)-min(pixel) < 12 for pixel in pixels),
                        "colored_pixels": sum(max(pixel)-min(pixel) > 40 for pixel in pixels)}
            # Fatal damage must not leave the whole results display washed in
            # a frozen red flash. Both the central board and bottom countdown
            # should retain their light, readable lettering.
            for label, crop in (("scoreboard", (image.width // 40, image.height // 3,
                                                image.width * 39 // 40, image.height * 3 // 5)),
                                ("countdown", (image.width // 8, image.height - image.height // 12,
                                               image.width * 7 // 8, image.height - 3))):
                region = image.crop(crop)
                pixels = region.get_flattened_data() if hasattr(region, "get_flattened_data") else region.getdata()
                captured[label + "_light_pixels"] = sum(min(pixel) > 130 and max(pixel) - min(pixel) < 65 for pixel in pixels)
                require(captured[label + "_light_pixels"] > 100,
                        f"{game_name}: {label} has no readable light text (stale damage tint?)")
            result["screenshots"][name] = captured

    def complete_pause(server, clients, initial, observed_at, expected_index):
        generation = int(initial["session"]["map_generation"])
        initial_remaining = int(initial["intermission"]["remaining_ms"])
        server.wait(lambda text: any(int(row["generation"]) > generation for row in records(text, "NET map generation=")),
                    "intermission deadline map transition", 20)
        elapsed = time.monotonic() - observed_at + (15000 - initial_remaining) / 1000
        require(14.8 <= elapsed <= 19, f"map transition did not respect the real 15-second pause: {elapsed:.3f}s")
        text = server.text()
        started = re.findall(r"^\s*([\d.]+)s .*NET intermission started ", text, re.MULTILINE)
        advanced = re.findall(rf"^\s*([\d.]+)s .*NET map generation={generation + 1}\b", text, re.MULTILINE)
        require(started and advanced, "server omitted intermission/transition timing evidence")
        server_elapsed = float(advanced[-1]) - float(started[-1])
        require(14.99 <= server_elapsed <= 19, f"server log shows premature or delayed transition: {server_elapsed:.4f}s")
        for game in clients:
            expect(game, lambda state: int(state["session"]["map_generation"]) == generation + 1 and
                   state["intermission"]["active"] == "0", "new map without final scoreboard", args.timeout)
        final = status(server)
        require(int(final["session"]["map_generation"]) == generation + 1 and
                final["rules"]["rotation_index"] == str(expected_index), "rotation did not advance exactly one entry")
        require(all(row["score"] == "0" for row in final["scoreboard"]), "new map retained completed-match scores")
        time.sleep(0.5)
        require(status(server)["session"]["map_generation"] == str(generation + 1), "rotation advanced twice")
        return {"duration_seconds": elapsed, "server_log_duration_seconds": server_elapsed,
                "before": initial, "after": final}

    try:
        port = free_port()
        server = Game("server", args, root, library, port, dedicated=True,
                      initial_files={**fixtures, "rotation.txt": b"NBEND1.MAP\nNBEND2.MAP\n"},
                      launch_arguments=["-dedicated", "-port", str(port), "-gametype", "bloodbath",
                                        "-maxplayers", "3", "-scorelimit", "1", "-timelimit", "1",
                                        "-maplist", "rotation.txt"])
        games.append(server)
        server.wait(lambda text: "Dedicated server running:" in text and "NET server_tick=" in text,
                    "dedicated Bloodbath server", args.timeout)
        first, first_slot = client("client-1", port, "Winner", args.renderer)
        second, second_slot = client("client-2", port, "Opponent", args.second_renderer)
        initial = status(server)
        require(all(int(row["health"]) > 0 for row in initial["players"].values()), "fixture players must start alive")
        require(all(row["score"] == "0" for row in initial["scoreboard"]), "fixture caused a spawn frag")
        # Every marker faces east. Move the first player along the common Y=0
        # row until ordinary collision/pitchfork range, then face the opponent.
        delta = int(initial["players"][second_slot]["x"]) - int(initial["players"][first_slot]["x"])
        if abs(delta) < 1500:
            # Bloodbath chooses random starts and can initially overlap two
            # living players. Separate them before establishing attack range.
            focus_window(first)
            first.key(22, True)
            try:
                expect(server, lambda state: int(state["players"][second_slot]["x"]) -
                       int(state["players"][first_slot]["x"]) > 1800, "separated random spawn positions", 10)
            finally:
                first.key(22, False)
            time.sleep(0.4)
            positioned = status(server)
            delta = int(positioned["players"][second_slot]["x"]) - int(positioned["players"][first_slot]["x"])
        if delta < 0:
            press(first, 27)  # X: TurnAround.
        focus_window(first)
        first.key(26, True)
        try:
            ready = expect(server, lambda state: abs(int(state["players"][second_slot]["x"]) -
                           int(state["players"][first_slot]["x"])) < 700,
                           "ordinary movement into pitchfork range", 20)
        finally:
            first.key(26, False)
        time.sleep(0.4)
        # Initialize the late client's engine/renderer before the real 15-second
        # pause. Delay ContentReady (including retransmits), leaving Hello,
        # content verification and transport acknowledgements unmodified. Its
        # actual admission and first authoritative snapshot still happen during
        # intermission, without cold startup consuming the observation window.
        late_relay = Relay(("127.0.0.1", port)).start()
        relays.append(late_relay)
        late_relay.block_client_payload = struct.pack("<III", 0x314e424e, int(initial["session"]["protocol"]), 15)
        late = client("late-client", late_relay.port, "LateJoin", args.renderer, wait=False)
        late.wait(lambda text: "NET content verified" in text
                  and late_relay.counters["client_to_server_filtered"] > 0,
                  "verified content awaiting intermission admission", args.timeout)
        require("NET welcome " not in late.text(), "late client was admitted before the intermission")
        first.command_text("net_record intermission.nbpd")
        focus_window(first)
        # Hits push the victim away; continue ordinary forward movement so
        # subsequent pitchfork strikes remain in range.
        first.key(26, True)
        first.key(13, True)
        try:
            ended = expect(server, lambda state: state["intermission"]["active"] == "1", "real winning pitchfork frag", 20)
            ended_at = time.monotonic()
        finally:
            first.key(13, False)
            first.key(26, False)
        require(ended["players"][first_slot]["frags"] == "1" and int(ended["players"][second_slot]["health"]) <= 0,
                "score-limit pause did not include the winning kill")
        require(ended["intermission"]["level_ticks"] == status(server)["intermission"]["level_ticks"],
                "level simulation advanced during the intermission")
        final_rows = ended["scoreboard"]
        require(len(final_rows) == 2 and any(row["slot"] == first_slot and row["score"] == "1" for row in final_rows),
                "final scoreboard omitted the actual winning score")
        require("NET welcome " not in late.text(), "late client bypassed its admission hold")
        # Release admission at the beginning of the pause, so the reliable
        # ContentReady retransmission cannot time out while screenshots and
        # input checks run. Joining remains asynchronous until checked below.
        late_relay.block_client_payload = None
        for game in (first, second):
            installed = expect(game, lambda state: state["intermission"]["active"] == "1", "automatic final results", 5)
            require(installed["scoreboard"] == final_rows, "client final scoreboard differs from server's frozen results")
        picture(first, "intermission-polymost" if args.renderer == 3 else "intermission-first")
        picture(second, "intermission-vulkan" if args.second_renderer == 5 else "intermission-second")
        before = status(server)
        focus_window(first)
        first.key(26, True)
        first.key(13, True)
        time.sleep(0.6)
        first.key(26, False)
        first.key(13, False)
        press(second, 11, 0.5)  # A dead player must not respawn during results.
        denied = status(server)
        require(frozen_players(before, (first_slot, second_slot)) == frozen_players(denied, (first_slot, second_slot)),
                "gameplay input changed completed-match world state")
        require(int(denied["session"]["tick"]) > int(before["session"]["tick"]) and
                denied["intermission"]["level_ticks"] == before["intermission"]["level_ticks"],
                "transport/demo clock must advance while world time is frozen")
        require(denied["scoreboard"] == final_rows, "final metadata changed during the pause")
        late, late_slot = joined(late, configure_controls=False)
        late_state = status(late)
        require(late_state["intermission"]["active"] == "1" and 0 < int(late_state["intermission"]["remaining_ms"]) <
                int(ended["intermission"]["remaining_ms"]), "late join restarted or omitted the intermission countdown")
        require(late_state["scoreboard"] == final_rows, "late join did not receive the original final scoreboard")
        require(late_slot not in [row["slot"] for row in final_rows], "late join appeared in historical match results")
        # Observe the disconnect before waiting for renderer/process shutdown;
        # that cleanup can otherwise consume the final part of the real pause.
        second.command_text("net_disconnect")
        departed = expect(server, lambda state: second_slot not in state["players"], "losing player departure", 5)
        require(departed["intermission"]["active"] == "1" and
                departed["session"]["map_generation"] == ended["session"]["map_generation"],
                "test could not observe the departure before the real intermission deadline")
        require(departed["scoreboard"] == final_rows, "departure removed a player from historical final results")
        result["phases"]["winning-frag"] = {"initial": initial, "ready": ready, "ended": ended,
                                              "denied_input": denied, "late_join": late_state, "departure": departed}
        result["phases"]["score-limit-rotation"] = complete_pause(server, (first, late), ended, ended_at, 1)
        first.command_text("net_stoprecord")
        # Gameplay console binds are disabled on the disconnected screen. Use
        # the ordinary F10/Y quit dialog, outside the intermission deadline.
        press(second, 67)
        second.key(28, True)
        second.process.wait(timeout=10)
        second.stop()
        demo = inspect_demo(first.path / "intermission.nbpd")
        demo_states = demo_intermissions(first.path / "intermission.nbpd")
        active = [row for row in demo_states if row["active"]]
        require(active and active[0]["remaining_ms"] >= 14000 and active[-1]["remaining_ms"] < 1500,
                "recording omitted the beginning or end of the countdown")
        require(all(row["rows"] == active[0]["rows"] for row in active), "recorded final results changed during the pause")
        require(active[-1]["elapsed_ticks"] - active[0]["elapsed_ticks"] >= 400,
                "packet demo collapsed the real pause")
        demo["intermissions"] = demo_states
        result["demo"] = demo
        print("PASS: actual winning score, frozen input/world, late join and one real 15-second transition", flush=True)

        if not args.skip_time_limit:
            timed = expect(server, lambda state: state["intermission"]["active"] == "1", "real one-minute time limit", 70)
            timed_at = time.monotonic()
            require(timed["intermission"]["level_ticks"] == "1800", "time limit did not stop world time at one minute")
            require(all(row["score"] == "0" for row in timed["scoreboard"]), "unexpected frag in timed round")
            result["phases"]["time-limit-wrap"] = complete_pause(server, (first, late), timed, timed_at, 0)
            print("PASS: real time limit pauses and wraps the rotation after 15 seconds", flush=True)

        for game in (first, second, late, server):
            game.stop()
        replay = Game("replay", args, root, library, playback=True, initial_files=fixtures)
        games.append(replay)
        replay.wait(lambda text: "Level ready: map=" in text, "offline replay setup", args.timeout)
        time.sleep(0.5)
        replay.command_text(f'net_playback "{demo["path"]}"')
        replay_pause = expect(replay, lambda state: state["intermission"]["active"] == "1", "recorded intermission", args.timeout)
        require(any(row["score"] == "1" for row in replay_pause["scoreboard"]), "replay lost the winning final score")
        picture(replay, "intermission-replay")
        last_snapshot = [row for row in demo["packets"] if row["type"] == 5][-1]
        final = expect(replay, lambda state: int(state["session"]["snapshot"]) == last_snapshot["snapshot"] and
                       state["session"].get("snapshot_hash") == last_snapshot["hash"], "exact final packet-demo snapshot", 25)
        require(final["intermission"]["active"] == "0", "replay retained results after its map transition")
        result["phases"]["replay"] = {"pause": replay_pause, "final": final, "expected": last_snapshot}
        check_pictures()
        print("PASS: offline packet demo preserves final results/countdown and exact final world snapshot", flush=True)
        result["passed"] = True
    except (Failure, OSError, subprocess.SubprocessError) as error:
        result["failures"].append(str(error))
        print(f"FAIL: {error}", flush=True)
    finally:
        for relay in relays:
            relay.block_client_payload = None
        for game in reversed(games):
            game.stop()
        for relay in relays:
            relay.close()
        result["relays"] = [relay.report() for relay in relays]
        result["games"] = {game.name: game.result() for game in games}
        for game in games:
            if game.process.returncode != 0 or game.forced:
                result["failures"].append(f"{game.name}: unclean exit {game.process.returncode}")
            if re.search(r"NET (?:apply snapshot|capture failed|invalid bootstrap|snapshot verification failed)|AddressSanitizer|runtime error:|Segmentation fault",
                         game.text() + game.console()):
                result["failures"].append(f"{game.name}: runtime/snapshot diagnostic")
            if game.dedicated and "NETWORK_TEST_DEVICES windows=0 audio=0" not in game.console():
                result["failures"].append(f"{game.name}: missing headless device audit")
        result["passed"] = result["passed"] and not result["failures"]
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("PASS" if result["passed"] else "FAIL", flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
