#!/usr/bin/env python3
"""Exercise team-shuffle votes with real UDP clients and 30-second ballots.

Requires Blood 1.21 assets and X11/Xvfb. Ordinary movement into a generated
map's pit establishes a nonzero score before the shuffle; no cheats, synthetic
gameplay packets or accelerated clocks are used.
"""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

from cinematics import focus_window, free_port
from intermission import Game, expect, status
from run import Failure, build_driver, inspect_demo, play_demo, records, require, snapshot_hashes


def joined(game, count=1, timeout=45):
    game.wait(lambda text: len(records(text, "NET client map loaded ")) >= count and
              len(records(text, "NET client slot=")) >= count,
              f"authoritative map {count}", timeout)


def vote_status(game):
    offset = len(game.text())
    game.command_text("vote_status")
    game.wait(lambda text: bool(records(text[offset:], "NET vote active=")), "vote status")
    return records(game.text()[offset:], "NET vote active=")[-1]


def rejection(game, command, diagnostic):
    offset = len(game.text())
    game.command_text(command)
    game.wait(lambda text: diagnostic.lower() in text[offset:].lower(),
              f"{command}: rejection mentioning {diagnostic}")
    return game.text()[offset:]


def teams(state):
    return {int(slot): int(row["team"]) for slot, row in state["players"].items()}


def check_teams(state, expected=None):
    found = teams(state)
    require(found and set(found.values()) <= {0, 1}, "invalid authoritative team assignment")
    require(abs(sum(team == 0 for team in found.values()) - sum(team == 1 for team in found.values())) <= 1,
            f"shuffle left unbalanced teams: {found}")
    if expected is not None:
        require(found == expected, f"team assignment mismatch: expected {expected}, got {found}")
    for row in state["players"].values():
        require(int(row["palette"]) == 11 + int(row["team"]),
                f"player color differs from authoritative team: {row}")
    return found


def same_rules(before, after, allowed=()):
    for key, value in before["rules"].items():
        if key not in allowed:
            require(after["rules"].get(key) == value, f"shuffle changed server rule {key}")


def main():
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=repository / "nblood")
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--engine-data", type=Path, default=repository)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--renderer", type=int, choices=(0, 3, 5), default=0)
    parser.add_argument("--skip-intermission", action="store_true", help="omit the real-minute time-limit case")
    args = parser.parse_args()
    for name in ("binary", "assets", "engine_data"):
        setattr(args, name, getattr(args, name).resolve())
    require(os.environ.get("DISPLAY"), "clients need DISPLAY (use an Xvfb wrapper)")
    require(args.binary.is_file() and args.assets.is_dir(), "supply a built executable and game assets")
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="netblood-team-shuffle-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print(f"Team-shuffle evidence: {root}", flush=True)
    source_binary = args.binary
    args.binary = root / "nblood-tested"
    shutil.copy2(source_binary, args.binary)
    library = build_driver(root)
    spec = importlib.util.spec_from_file_location("team_balance_fixture", Path(__file__).with_name("team-balance.py"))
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    files = {"NBSHUF1.MAP": fixture.death_map(), "NBSHUF2.MAP": fixture.death_map(1)}
    games = []
    result = {"passed": False, "source_binary": str(source_binary),
              "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
              "renderer": args.renderer, "phases": {}, "failures": []}

    def host(name, mode="teams", rotation=True, extra=()):
        port = free_port()
        arguments = ["-dedicated", "-port", str(port), "-gametype", mode, "-maxplayers", "4",
                     "-autoaim", "off", "-weaponsstay", "permanent"]
        arguments += ["-maplist", "rotation.txt"] if rotation else ["-map", "NBSHUF1.MAP"]
        arguments += list(extra)
        game = Game(name, args, root, library, port, dedicated=True, launch_arguments=arguments,
                    initial_files={**files, "rotation.txt": b"NBSHUF1.MAP\nNBSHUF2.MAP\n"})
        games.append(game)
        game.wait(lambda text: "Dedicated server running:" in text and "NET server_tick=" in text,
                  "dedicated server", args.timeout)
        return game, port

    def client(name, port, expected_slot):
        game = Game(name, args, root, library, port, initial_files=files,
                    launch_arguments=["-client", "127.0.0.1", "-port", str(port), "-pname", name])
        games.append(game)
        joined(game, timeout=args.timeout)
        require(records(game.text(), "NET welcome ")[-1]["slot"] == str(expected_slot),
                f"{name}: wrong expected player slot")
        return game

    def call(game, command, eligible, required):
        offset, began = len(game.text()), time.monotonic()
        game.command_text(command)
        game.wait(lambda text: any(row.get("active") == "1" for row in
                                   records(text[offset:], "NET vote active=")), "accepted vote", args.timeout)
        row = [row for row in records(game.text()[offset:], "NET vote active=") if row.get("active") == "1"][-1]
        require(row.get("eligible") == str(eligible) and row.get("required") == str(required),
                f"incorrect electorate/51 percent threshold: {row}")
        require(row.get("yes") == "1" and row.get("no") == "0", "caller must receive exactly one automatic yes")
        return {"id": row["id"], "began": began, "initial": row}

    def finish(server, ballot, expected, clients):
        server.wait(lambda text: any(row.get("id") == ballot["id"] for row in records(text, "NET vote result=")),
                    f"30-second vote {ballot['id']} completion", 40)
        rows = [row for row in records(server.text(), "NET vote result=") if row.get("id") == ballot["id"]]
        require(len(rows) == 1 and rows[0].get("result") == expected,
                f"vote {ballot['id']} expected {expected}, got {rows}")
        elapsed = time.monotonic() - ballot["began"]
        require(elapsed >= 29, f"vote {ballot['id']} ended early after {elapsed:.3f}s")
        for game in clients:
            game.wait(lambda text: any(row.get("id") == ballot["id"] and row.get("result") == expected
                                      for row in records(text, "NET vote result=")), "vote result", args.timeout)
        return {"result": rows[0], "elapsed_seconds": elapsed}

    try:
        server, port = host("rotation-server", extra=("-scorelimit", "25", "-timelimit", "10"))
        first, second, third = [client(name, port, slot) for slot, name in
                                enumerate(("client-1", "client-2", "client-3"))]
        clients = [first, second, third]
        initial = status(server)
        require(initial["rules"]["mapvote"] == "1", "voting must be enabled by default")
        check_teams(initial)
        first.command_text("vote_maps")
        first.wait(lambda text: any(int(row["actions"]) & 8 for row in records(text, "NET vote options ")),
                   "advertised shuffle option", args.timeout)
        malformed = rejection(first, "callvote shuffle extra", "callvote")

        # All vote types share the active ballot, fixed electorate and cooldown.
        ballot = call(first, "callvote shuffle", 3, 2)
        require(ballot["initial"]["action"] == "shuffle", "wrong action advertised for shuffle vote")
        concurrent_map = rejection(second, "callvote map NBSHUF2.MAP", "already")
        concurrent_shuffle = rejection(second, "callvote shuffle", "already")
        late = client("late-voter", port, 3)
        late_rejection = rejection(late, "vote yes", "joined after")
        require(vote_status(late)["eligible"] == "3", "late join changed shuffle electorate")
        late.stop()
        failed = finish(server, ballot, "failed", clients)
        after_failed = status(server)
        require(after_failed["session"]["map_generation"] == initial["session"]["map_generation"] and
                teams(after_failed) == teams(initial), "failed shuffle changed map or teams")
        failed.update(before=initial, after=after_failed, malformed=malformed,
                      concurrent_map=concurrent_map, concurrent_shuffle=concurrent_shuffle,
                      late_rejection=late_rejection,
                      shared_cooldown=rejection(first, "callvote map NBSHUF2.MAP", "wait"))
        result["phases"]["failed-fixed-electorate"] = failed
        print("PASS: failed shuffle preserves teams; map and shuffle share voting guards", flush=True)

        # Kill slot 0 using normal forward movement, so a successful restart
        # must clear real death/score state as well as reassigning teams.
        focus_window(first)
        first.key(26, True)
        try:
            expect(server, lambda state: int(state["players"]["0"]["health"]) == 0,
                   "ordinary fatal fall", args.timeout)
        finally:
            first.key(26, False)
        dead = expect(server, lambda state: int(state["players"]["0"]["frags"]) == -1,
                      "finalized fall score", args.timeout)
        before_map = records(server.text(), "NET map generation=")[-1]
        ballot = call(second, "callvote shuffle", 3, 2)
        first.command_text("vote yes")
        third.command_text("vote no")
        rejection(third, "vote yes", "already")
        require(vote_status(first)["active"] == "1", "majority ended the shuffle before its deadline")
        require(status(server)["session"]["map_generation"] == dead["session"]["map_generation"],
                "shuffle reset the level before its deadline")
        newcomer = client("shuffle-newcomer", port, 3)
        clients.append(newcomer)
        rejection(newcomer, "vote yes", "joined after")
        require(vote_status(newcomer)["eligible"] == "3", "late join changed the successful vote's threshold")
        before_shuffle = status(server)
        # A short recording tail keeps offline playback quick, while still
        # checking the synthetic mid-vote state and the transition itself.
        remaining = max(0, 24 - (time.monotonic() - ballot["began"]))
        time.sleep(remaining)
        first.command_text("net_record team-shuffle.nbpd")
        passed = finish(server, ballot, "passed", clients)
        for game in clients:
            joined(game, 2, args.timeout)
        after = status(server)
        assignment = check_teams(after)
        require(assignment != teams(before_shuffle), "successful shuffle retained identical team assignments")
        require(any((teams(before_shuffle)[a] == teams(before_shuffle)[b]) != (assignment[a] == assignment[b])
                    for a in assignment for b in assignment), "shuffle only reversed team colors")
        require(int(after["session"]["map_generation"]) == int(dead["session"]["map_generation"]) + 1,
                "successful shuffle did not restart exactly once")
        require(records(server.text(), "NET map generation=")[-1]["crc"] == before_map["crc"],
                "shuffle changed the current map")
        same_rules(dead, after)
        require(all(int(row["health"]) > 0 and row["frags"] == "0" for row in after["players"].values()),
                "shuffle restart did not restore life and reset player scores")
        require(all(value == "0" for value in records(server.text(), "NET team scores ")[-1].values()),
                "shuffle restart retained the previous team score")
        require(all(after["players"][slot]["join_order"] == row["join_order"]
                    for slot, row in before_shuffle["players"].items()), "shuffle reset connection ages")
        observed = {}
        for game in clients:
            current = expect(game, lambda state: teams(state) == assignment, "shuffled client roster", args.timeout)
            check_teams(current, assignment)
            same_rules(dead, current)
            require(current["session"]["map_generation"] == after["session"]["map_generation"],
                    f"{game.name}: shuffle map generation diverged")
            observed[game.name] = current
        passed.update(before=before_shuffle, dead=dead, after=after, clients=observed,
                      cooldown=rejection(second, "callvote map NBSHUF2.MAP", "wait"))
        result["phases"]["passed-shuffle"] = passed
        print("PASS: accepted shuffle resets the same map with balanced, synchronized teams", flush=True)

        time.sleep(1)
        first.command_text("net_stoprecord\nnet_status")
        demo = inspect_demo(first.path / "team-shuffle.nbpd")
        # A subsequent map vote exercises the shared ballot in the opposite
        # direction and must keep the newly assigned teams on the next map.
        map_ballot = call(third, "callvote map NBSHUF2.MAP", 4, 3)
        rejection(second, "callvote shuffle", "already")
        first.command_text("vote yes")
        second.command_text("vote no")
        newcomer.command_text("vote yes")
        next_phase = finish(server, map_ballot, "passed", clients)
        for game in clients:
            joined(game, 3, args.timeout)
        advanced = status(server)
        check_teams(advanced, assignment)
        require(all(advanced["players"][slot]["join_order"] == row["join_order"]
                    for slot, row in after["players"].items()), "next map reset connection ages")
        same_rules(after, advanced, allowed=("rotation_index", "episode", "level"))
        require(advanced["rules"]["rotation_index"] == "1", "normal map change lost rotation bookkeeping")
        for game in clients:
            current = expect(game, lambda state: teams(state) == assignment and
                             state["session"]["map_generation"] == advanced["session"]["map_generation"],
                             "retained shuffled teams on next map", args.timeout)
            check_teams(current, assignment)
        next_phase.update(after=advanced, shared_cooldown=rejection(third, "callvote shuffle", "wait"))
        result["phases"]["next-map"] = next_phase
        vote_packets = [row for row in demo["packets"] if row["type"] == 17]
        option_packets = [row for row in demo["packets"] if row["type"] == 18]
        require(vote_packets and vote_packets[0]["sequence"] == 0 and vote_packets[0]["vote_state"] == 1 and
                vote_packets[0]["vote_action"] == 4 and vote_packets[0]["vote_yes"] == 2,
                "mid-shuffle recording lacks active vote baseline")
        require(option_packets and option_packets[0]["sequence"] == 0 and option_packets[0]["vote_actions"] & 8,
                "recording omitted advertised team-shuffle option")
        require(any(row["vote_state"] == 2 and row["vote_action"] == 4 for row in vote_packets),
                "recording omitted successful team-shuffle result")
        require(len({row["generation"] for row in demo["packets"] if row["type"] == 2}) == 2,
                "recording omitted shuffle restart")
        verified = records(first.text(), "NET snapshot roundtrip ")
        require(verified and all(row.get("equal") == "1" for row in verified),
                "client snapshot roundtrip did not verify")
        authoritative, received = snapshot_hashes(server.text(), True), snapshot_hashes(first.text())
        shared = set(authoritative) & set(received)
        require(shared and all(authoritative[key] == received[key] for key in shared),
                "client did not retain authoritative snapshot hashes")
        result["snapshot_checks"] = {"shared": len(shared), "roundtrips": len(verified)}
        result["packet_demo"] = demo
        for game in clients + [server]:
            game.stop()
        replay = Game("shuffle-playback", args, root, library, playback=True, initial_files=files)
        games.append(replay)
        replay.wait(lambda text: "> This map" in text, "offline replay map", args.timeout)
        time.sleep(1)
        result["playback"] = play_demo("shuffle-playback", demo, args, root, library, games, existing=replay)
        replay_players = {int(row["slot"]): int(row["team"]) for row in records(replay.text(), "NET player ")}
        require(replay_players == assignment, "packet playback lost shuffled team assignments")
        require(any(row.get("action") == "shuffle" and row.get("active") == "1"
                    for row in records(replay.text(), "NET vote active=")), "playback omitted active shuffle vote")
        print("PASS: map progression and exact packet playback retain shuffled teams and vote state", flush=True)

        # Shuffle availability is independent of rotation, while its common
        # enable flag and team-mode restriction remain authoritative.
        for label, mode, options, diagnostic in (
                ("disabled", "teams", ("-mapvote", "off"), "disabled"),
                ("bloodbath", "bloodbath", (), "team"),
                ("coop", "coop", (), "team")):
            other, other_port = host(label + "-server", mode, rotation=False, extra=options)
            one = client(label + "-client", other_port, 0)
            before = status(other)
            rejected = rejection(one, "callvote shuffle", diagnostic)
            require(vote_status(one)["active"] == "0", f"{label}: rejected shuffle became active")
            require(status(other)["session"]["map_generation"] == before["session"]["map_generation"],
                    f"{label}: rejected shuffle changed the map")
            result["phases"][label] = {"before": before, "rejection": rejected}
            one.stop()
            other.stop()
        no_rotation, no_rotation_port = host("no-rotation-server", rotation=False)
        one = client("no-rotation-first", no_rotation_port, 0)
        alone = rejection(one, "callvote shuffle", "two")
        require(vote_status(one)["active"] == "0", "one-player shuffle was accepted")
        two = client("no-rotation-second", no_rotation_port, 1)
        before = status(no_rotation)
        ballot = call(one, "callvote shuffle", 2, 2)
        two.command_text("vote yes")
        phase = finish(no_rotation, ballot, "passed", [one, two])
        joined(one, 2, args.timeout)
        joined(two, 2, args.timeout)
        after = status(no_rotation)
        same_rules(before, after)
        assignment = check_teams(after)
        require(int(after["session"]["map_generation"]) == int(before["session"]["map_generation"]) + 1,
                "shuffle without rotation did not restart current map")
        phase.update(before=before, after=after, alone_rejection=alone)
        result["phases"]["no-rotation"] = phase
        for game in (one, two, no_rotation):
            game.stop()
        print("PASS: team-only/disable guards and successful shuffle without a rotation", flush=True)

        listen_port = free_port()
        listen = Game("listen-host", args, root, library, listen_port, listen=True, initial_files=files,
                      launch_arguments=["-server", "4", "-gametype", "teams", "-port", str(listen_port),
                                        "-map", "NBSHUF1.MAP"])
        games.append(listen)
        listen.wait(lambda text: any(int(row["server_tick"]) >= 30 for row in records(text, "NET server_tick=")),
                    "listen host simulation", args.timeout)
        remote = client("listen-client", listen_port, 1)
        before = status(listen)
        before_map = records(listen.text(), "NET map generation=")[-1]
        ballot = call(listen, "callvote shuffle", 2, 2)
        remote.command_text("vote yes")
        listen.wait(lambda text: any(row.get("id") == ballot["id"] and row.get("result") == "passed"
                                    for row in records(text, "NET vote result=")),
                    "listen-host successful shuffle", 40)
        elapsed = time.monotonic() - ballot["began"]
        require(elapsed >= 29, "listen-host shuffle finished before its deadline")
        joined(remote, 2, args.timeout)
        after = status(listen)
        after_map = records(listen.text(), "NET map generation=")[-1]
        same_rules(before, after)
        require(int(after["session"]["map_generation"]) == int(before["session"]["map_generation"]) + 1 and
                after_map["crc"] == before_map["crc"], "listen-host shuffle changed or reregistered the custom map")
        assignment = check_teams(after)
        require(assignment != teams(before), "listen-host shuffle retained the original teams")
        observed = expect(remote, lambda state: teams(state) == assignment, "listen shuffle client teams", args.timeout)
        check_teams(observed, assignment)
        same_rules(before, observed)
        result["phases"]["listen-custom-map"] = {"before": before, "after": after, "client": observed,
                                                 "before_map": before_map, "after_map": after_map,
                                                 "elapsed_seconds": elapsed}
        remote.stop()
        listen.stop()
        print("PASS: graphical listen host shuffles and restarts its custom map once", flush=True)

        if not args.skip_intermission:
            ending, ending_port = host("intermission-server", extra=("-timelimit", "1"))
            one = client("intermission-client", ending_port, 0)
            ended = expect(ending, lambda state: state["intermission"]["active"] == "1",
                           "real one-minute time limit", 70)
            rejection_text = rejection(one, "callvote shuffle", "match")
            current = status(ending)
            require(current["intermission"]["active"] == "1" and
                    current["session"]["map_generation"] == ended["session"]["map_generation"] and
                    vote_status(one)["active"] == "0", "shuffle interrupted end-of-match results")
            result["phases"]["intermission"] = {"before": ended, "after": current, "rejection": rejection_text}
            one.stop()
            ending.stop()
            print("PASS: shuffle cannot interrupt the end-of-match pause", flush=True)
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
