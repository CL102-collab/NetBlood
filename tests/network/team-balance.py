#!/usr/bin/env python3
"""Check authoritative team balancing with real UDP clients and packet playback.

Uses ordinary joins, disconnects and operator commands. Requires an X11 display
and Blood 1.21 assets, like run.py; no game-state injection or accelerated clock.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import zlib

from run import Failure, Game, build_driver, inspect_demo, play_demo, records, require, snapshot_hashes


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def status(game):
    offset = len(game.text())
    game.command_text("net_status")
    text = game.text()[offset:]
    sessions = records(text, "NET status ")
    require(sessions, f"{game.name}: missing network status")
    return {"session": sessions[-1],
            "players": {row["slot"]: row for row in records(text, "NET player ")},
            "scores": records(text, "NET team scores ")[-1:]}


def teams_of(state):
    return {int(slot): int(row["team"]) for slot, row in state["players"].items()}


def expect_teams(game, expected, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        state = status(game)
        if teams_of(state) == expected:
            for row in state["players"].values():
                require(int(row["palette"]) == 11 + int(row["team"]),
                        f"{game.name}: player color does not match authoritative team: {row}")
            return state
        time.sleep(0.2)
    raise Failure(f"{game.name}: expected teams {expected}, last state {state}")

def death_map(revision=0):
    """Return a legal BLM6 map with 8 MP starts, safe platform and deep east pit.

    Map origin/angle: (-2048, -7168, 0), facing +X (angle 0).
    Sector 0 platform: x=-8192..0, y=-8192..8192, floor z=0.
    Sector 1 pit: x=0..32768, same Y range, floor z=262144.
    Ceiling z=-32768 in both sectors; their shared edge is a clear portal.
    All 8 MP starts are x=-2048 and spaced 2048 units along Y.
    No monsters, pickups, triggers or extended sector/wall effects are present.
    """
    def put16(data, offset, value):
        struct.pack_into("<H", data, offset, value & 0xffff)

    def put32(data, offset, value):
        struct.pack_into("<I", data, offset, value & 0xffffffff)

    # Signature, 37-byte map header and one 16-bit sky offset (skybits=0).
    data = bytearray(6 + 37 + 2)
    data[:4] = b"BLM\x1a"
    put16(data, 4, 0x600)
    put32(data, 6, -2048)
    put32(data, 10, -7168)
    put32(data, 6 + 0x1b, revision)  # Distinct CRC for the second map generation.
    put16(data, 6 + 0x1f, 2)
    put16(data, 6 + 0x21, 8)
    put16(data, 6 + 0x23, 8)

    for sector_index, floor_z in enumerate((0, 262144)):
        sector = bytearray(40)
        put16(sector, 0, sector_index * 4)
        put16(sector, 2, 4)
        put32(sector, 4, -32768)
        put32(sector, 8, floor_z)
        put16(sector, 16, 0)  # Ceiling tile.
        put16(sector, 24, 0)  # Floor tile.
        put16(sector, 38, -1)  # No XSECTOR.
        data += sector

    vertices = ((-8192, -8192), (0, -8192), (0, 8192), (-8192, 8192),
                (0, -8192), (32768, -8192), (32768, 8192), (0, 8192))
    for index, (x, y) in enumerate(vertices):
        wall = bytearray(32)
        put32(wall, 0, x)
        put32(wall, 4, y)
        put16(wall, 8, (index // 4) * 4 + ((index + 1) % 4))
        put16(wall, 10, {1: 7, 7: 1}.get(index, -1))
        put16(wall, 12, {1: 1, 7: 0}.get(index, -1))
        put16(wall, 18, -1)  # No overlay; unmasked portal/walls.
        wall[22:24] = bytes((8, 8))
        put16(wall, 30, -1)  # No XWALL.
        data += wall

    for index in range(8):
        sprite = bytearray(44)
        put32(sprite, 0, -2048)
        put32(sprite, 4, -7168 + index * 2048)
        sprite[20:22] = bytes((64, 64))
        put16(sprite, 26, 0)  # kStatDecoration; warpInit consumes start markers.
        put16(sprite, 30, -1)
        put16(sprite, 38, 2)  # kMarkerMPStart.
        put16(sprite, 42, index + 1)  # Any positive value requests an XSPRITE.
        data += sprite
        extended = bytearray(56)
        put16(extended, 0, index)  # 14-bit reference; overwritten during loading.
        put16(extended, 16, index)  # Signed 16-bit XSPRITE data1 = start index.
        put16(extended, 30, -1)  # target sprite reference.
        put16(extended, 46, -1)  # burnSource sprite reference.
        data += extended

    return bytes(data) + struct.pack("<I", zlib.crc32(data))


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
    require(args.binary.is_file() and args.assets.is_dir(), "supply a built executable and game assets")
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="netblood-team-balance-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print(f"Team-balance evidence: {root}", flush=True)
    source_binary = args.binary
    args.binary = root / "nblood-tested"
    shutil.copy2(source_binary, args.binary)
    library = build_driver(root)
    games = []
    fixtures = {"NBBAL1.MAP": death_map(), "NBBAL2.MAP": death_map(1)}
    result = {"passed": False, "source_binary": str(source_binary),
              "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
              "phases": {}, "failures": []}

    def server(name, mode):
        port = free_port()
        game = Game(name, args, root, library, port, dedicated=True,
                    launch_arguments=["-dedicated", "-port", str(port), "-gametype", mode,
                                      "-maxplayers", "4", "-map", "NBBAL1.MAP" if mode == "teams" else "E1M1"],
                    initial_files=fixtures if mode == "teams" else None)
        games.append(game)
        game.wait(lambda text: "Dedicated server running:" in text and "NET server_tick=" in text,
                  "dedicated server", args.timeout)
        return game, port

    def client(name, port, expected_slot):
        game = Game(name, args, root, library, port, initial_files=fixtures)
        games.append(game)
        game.wait(lambda text: "NET client map loaded " in text and "NET client slot=" in text,
                  "authoritative client snapshot", args.timeout)
        require(records(game.text(), "NET welcome ")[-1]["slot"] == str(expected_slot),
                f"{name}: did not receive expected slot {expected_slot}")
        game.command_text('bind H "gamefunc_Open"')
        return game

    def balance(host, count, slot, old_team, new_team):
        host.wait(lambda text: len(records(text, "NET team balance ")) >= count,
                  f"team transfer {count}", args.timeout)
        changes = records(host.text(), "NET team balance ")
        require(len(changes) == count, f"unexpected additional team transfers: {changes}")
        row = changes[-1]
        require((row["slot"], row["from"], row["to"]) ==
                (str(slot), str(old_team), str(new_team)), f"wrong player/team transferred: {row}")
        return row

    def synchronized(host, clients, expected):
        state = expect_teams(host, expected, args.timeout)
        observed = {game.name: expect_teams(game, expected, args.timeout) for game in clients}
        return state, observed

    def unchanged(host, transfers, expected, seconds=1):
        time.sleep(seconds)
        state = expect_teams(host, expected, args.timeout)
        require(len(records(host.text(), "NET team balance ")) == transfers,
                "alive or no-longer-needed candidate was transferred")
        return state

    def die(host, game, slot):
        before = status(host)
        player = before["players"][str(slot)]
        require(int(player["health"]) > 0, f"slot {slot} must be alive before walking into the pit")
        deadline = time.monotonic() + args.timeout
        game.key(26, True)  # W: all ordinary spawn markers face the lethal drop.
        try:
            while time.monotonic() < deadline:
                current = status(host)
                if int(current["players"][str(slot)]["health"]) == 0:
                    break
                time.sleep(0.1)
            else:
                raise Failure(f"slot {slot} did not die after walking into the fixture pit")
        finally:
            game.key(26, False)
        # A hard landing can first enter Blood's revivable kneeling animation.
        # Wait for its ordinary final-death callback, without pressing Open.
        while time.monotonic() < deadline:
            current = status(host)
            row = current["players"][str(slot)]
            if int(row["health"]) == 0 and int(row["frags"]) == int(player["frags"]) - 1:
                require(row["join_order"] == player["join_order"], "death reset connection age")
                original_team = "blue" if player["team"] == "0" else "red"
                require(before["scores"] and current["scores"], "missing team scores around death")
                expected_scores = {team: str(int(score) - (team == original_team))
                                   for team, score in before["scores"][0].items()}
                require(current["scores"][0] == expected_scores,
                        "fatal fall must subtract exactly one point from the original team before transfer")
                return {"before": before, "dead": current}
            time.sleep(0.1)
        raise Failure(f"slot {slot} did not finish its natural death animation")

    def respawn(host, game, slot, team):
        before = status(host)
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            game.key(11, True)  # H is bound to the ordinary Open/respawn action.
            time.sleep(0.15)
            game.key(11, False)
            time.sleep(0.15)
            current = status(host)
            row = current["players"][str(slot)]
            if int(row["health"]) > 0:
                require(int(row["team"]) == team and int(row["palette"]) == 11 + team,
                        "respawn did not use the transferred team/color")
                require(row["join_order"] == before["players"][str(slot)]["join_order"],
                        "respawn reset session join age")
                require(current["scores"] == before["scores"] and
                        row["frags"] == before["players"][str(slot)]["frags"],
                        "respawn reset or changed team/personal scores")
                return current
        raise Failure(f"slot {slot} did not respawn through Open")

    try:
        host, port = server("teams-server", "teams")
        oldest = client("oldest-slot-0", port, 0)
        second = client("original-slot-1", port, 1)
        recorder = client("client-1", port, 2)  # Enables exact snapshot verification.
        fourth = client("original-slot-3", port, 3)
        initial, _ = synchronized(host, [oldest, second, recorder, fourth], {0: 0, 1: 1, 2: 0, 3: 1})
        orders = [int(initial["players"][str(slot)]["join_order"]) for slot in range(4)]
        require(0 < orders[0] < orders[1] < orders[2] < orders[3], "join chronology is not increasing")
        recorder.command_text("net_record team-balance.nbpd")
        second.stop()
        unchanged(host, 0, {0: 0, 2: 0, 3: 1})
        fourth.stop()
        pending = unchanged(host, 0, {0: 0, 2: 0})
        require(all(int(row["health"]) > 0 for row in pending["players"].values()),
                "initial imbalance did not have two live players")
        older_death = die(host, oldest, 0)
        older_dead = unchanged(host, 0, {0: 0, 2: 0})
        require(int(older_dead["players"]["2"]["health"]) > 0,
                "newest candidate should remain alive while the older player is dead")
        result["phases"]["wait-for-newest-death"] = {
            "initial": initial, "pending": pending, "older_death": older_death, "after": older_dead}
        print("PASS: living newest player stays put; older teammate's death is not substituted", flush=True)

        reused = client("cancel-by-join-slot-1", port, 1)
        cancelled, _ = synchronized(host, [oldest, recorder, reused], {0: 0, 1: 1, 2: 0})
        candidate_death = die(host, recorder, 2)
        after_cancel = unchanged(host, 0, {0: 0, 1: 1, 2: 0})
        respawn(host, oldest, 0, 0)
        respawn(host, recorder, 2, 0)
        result["phases"]["join-cancels-pending"] = {
            "joined": cancelled, "candidate_death": candidate_death, "after": after_cancel}
        print("PASS: joining the smaller team cancels the transfer before the candidate dies", flush=True)

        reused.stop()
        before_map = unchanged(host, 0, {0: 0, 2: 0})
        host.command_text("map NBBAL2.MAP")
        for game in (oldest, recorder):
            game.wait(lambda text: len(records(text, "NET client map loaded ")) >= 2,
                      "second map load", args.timeout)
        after_map, _ = synchronized(host, [oldest, recorder], {0: 0, 2: 0})
        require(int(after_map["session"]["map_generation"]) == int(before_map["session"]["map_generation"]) + 1,
                "map change did not advance exactly one generation")
        for slot in before_map["players"]:
            require(before_map["players"][slot]["join_order"] == after_map["players"][slot]["join_order"],
                    "map change reset connection age")
        unchanged(host, 0, {0: 0, 2: 0})
        newest_death = die(host, recorder, 2)
        first_transfer = balance(host, 1, 2, 0, 1)
        after, observed = synchronized(host, [oldest, recorder], {0: 0, 2: 1})
        require(after["players"]["2"]["health"] == "0", "transfer respawned the selected player")
        require(after["scores"] == newest_death["dead"]["scores"] and
                after["players"]["2"]["frags"] == newest_death["dead"]["players"]["2"]["frags"],
                "team transfer changed already-finalized death scores")
        require(first_transfer["join_order"] == str(orders[2]), "map change lost newest player age")
        respawned = respawn(host, recorder, 2, 1)
        result["phases"]["map-and-deferred-transfer"] = {
            "before_map": before_map, "after_map": after_map, "death": newest_death,
            "transfer": first_transfer, "after": after, "clients": observed, "respawned": respawned}
        print("PASS: pending imbalance survives map change; newest transfers while dead before respawn", flush=True)

        one = client("disconnect-cancel-slot-1", port, 1)
        three = client("disconnect-cancel-slot-3", port, 3)
        join_pending = unchanged(host, 1, {0: 0, 1: 1, 2: 1, 3: 1})
        one.stop()  # 1:3 becomes 1:2, cancelling newest slot 3's pending move.
        death_after_leave = die(host, three, 3)
        cancelled = unchanged(host, 1, {0: 0, 2: 1, 3: 1})
        respawn(host, three, 3, 1)
        three.stop()
        result["phases"]["disconnect-cancels-pending"] = {
            "pending": join_pending, "death": death_after_leave, "after": cancelled}
        print("PASS: departure restoring a one-player difference cancels the pending transfer", flush=True)

        newest_low = client("newest-reused-slot-1", port, 1)
        lower_slot, _ = synchronized(host, [oldest, recorder, newest_low], {0: 0, 1: 1, 2: 1})
        require(int(lower_slot["players"]["1"]["join_order"]) > int(lower_slot["players"]["2"]["join_order"]),
                "reused lower slot inherited its departed occupant's age")
        oldest.stop()
        unchanged(host, 1, {1: 1, 2: 1})
        lower_death = die(host, newest_low, 1)
        second_transfer = balance(host, 2, 1, 1, 0)
        final, observed = synchronized(host, [recorder, newest_low], {1: 0, 2: 1})
        require(final["players"]["1"]["health"] == "0", "lower-slot transfer respawned player")
        require(final["scores"] == lower_death["dead"]["scores"] and
                final["players"]["1"]["frags"] == lower_death["dead"]["players"]["1"]["frags"],
                "lower-slot transfer changed already-finalized death scores")
        respawned = respawn(host, newest_low, 1, 0)
        result["phases"]["newest-reused-slot"] = {
            "before": lower_slot, "death": lower_death, "transfer": second_transfer,
            "after": final, "clients": observed, "respawned": respawned}
        print("PASS: newest reused low slot transfers on death and respawns on its new team", flush=True)

        time.sleep(2)
        recorder.wait(lambda text: text.count("team to balance teams") >= 2,
                      "both team-change announcements", args.timeout)
        recorder.command_text("net_stoprecord\nnet_status")
        installed = records(recorder.text(), "NET snapshot roundtrip ")
        require(len(installed) >= 30 and all(row.get("equal") == "1" for row in installed),
                "client snapshot installation failed exact authoritative-byte verification")
        result["exact_snapshot_roundtrips"] = len(installed)
        host_hashes, client_hashes = snapshot_hashes(host.text(), True), snapshot_hashes(recorder.text())
        shared = sorted(set(host_hashes) & set(client_hashes))
        require(len(shared) >= 2 and all(host_hashes[key] == client_hashes[key] for key in shared),
                "server/client authoritative snapshot hashes differ")
        result["snapshot_comparisons"] = [dict(generation=key[0], snapshot=key[1], tick=key[2],
                                                hash=client_hashes[key]) for key in shared]
        demo = inspect_demo(recorder.path / "team-balance.nbpd")
        require(len({row["generation"] for row in demo["packets"] if row["type"] == 2}) == 2,
                "recording did not include both maps")
        result["packet_demo"] = demo
        recorder.stop()
        newest_low.stop()
        host.stop()
        replay = Game("team-packet-playback", args, root, library, playback=True, initial_files=fixtures)
        games.append(replay)
        replay.wait(lambda text: "> This map" in text, "offline replay map", args.timeout)
        time.sleep(1)
        result["packet_playback"] = play_demo("team-packet-playback", demo, args, root, library, games, existing=replay)
        replay_players = {row["slot"]: row for row in records(replay.text(), "NET player ")}
        require({int(slot): int(row["team"]) for slot, row in replay_players.items()} == {1: 0, 2: 1},
                "offline packet playback did not retain the final authoritative teams")
        require(replay.text().count("team to balance teams") >= 2,
                "packet demo did not replay the team-transfer announcements")
        result["packet_playback"]["players"] = replay_players
        print("PASS: exact client snapshots and two-map packet demo retain death-delayed transfers", flush=True)

        for mode in ("bloodbath", "coop"):
            other, other_port = server(mode + "-server", mode)
            clients = [client(f"{mode}-client-{slot}", other_port, slot) for slot in range(3)]
            before = status(other)
            clients[1].stop()
            other.wait(lambda text: records(text, "NET server_tick=")[-1]["players"] == "2",
                       "middle player disconnect", args.timeout)
            after = status(other)
            require(not records(other.text(), "NET team balance "), f"{mode} incorrectly ran team balancing")
            require(set(after["players"]) == {"0", "2"} and
                    all(after["players"][slot]["team"] == before["players"][slot]["team"]
                        for slot in after["players"]), f"{mode} changed player team identity")
            result["phases"][mode + "-unchanged"] = {"before": before, "after": after}
            for game in clients:
                game.stop()
            other.stop()
        print("PASS: Bloodbath and co-op keep their existing player identities", flush=True)
        result["passed"] = True
    except (Failure, OSError, subprocess.SubprocessError, KeyError, ValueError) as error:
        result["failures"].append(str(error))
        print(f"FAIL: {error}", flush=True)
    finally:
        for game in reversed(games):
            game.stop()
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
