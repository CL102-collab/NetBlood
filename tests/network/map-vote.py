#!/usr/bin/env python3
"""Exercise map votes through real dedicated servers and graphical clients.

Requires the same Linux/SDL2 display and Blood 1.21 assets as run.py. Ballots
use their real 30-second deadlines; the test does not accelerate server time.
"""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import tempfile
import time

from run import Failure, Game, build_driver, inspect_demo, play_demo, records, require


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def joined(game, count=1, timeout=45):
    game.wait(lambda text: len(records(text, "NET client map loaded ")) >= count and
              len(records(text, "NET client slot=")) >= count,
              f"authoritative map {count}", timeout)


def status(game):
    offset = len(game.text())
    game.command_text("net_status")
    text = game.text()[offset:]
    rules, session = records(text, "NET rules "), records(text, "NET status ")
    require(rules and session, f"{game.name}: missing authoritative status")
    return {"rules": rules[-1], "session": session[-1],
            "players": {row["slot"]: row for row in records(text, "NET player ")},
            "team_scores": records(text, "NET team scores ")[-1:]}


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


def main():
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=repository / "nblood")
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--engine-data", type=Path, default=repository)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=45)
    focused = parser.add_mutually_exclusive_group()
    focused.add_argument("--listen-restart-only", action="store_true",
                         help="run only the focused co-op listen-server map restart check")
    focused.add_argument("--competitive-restart-only", action="store_true",
                         help="run only Bloodbath, teams and custom-map listen-server restart votes")
    args = parser.parse_args()
    for name in ("binary", "assets", "engine_data"):
        setattr(args, name, getattr(args, name).resolve())
    require(os.environ.get("DISPLAY"), "clients need DISPLAY (use an Xvfb wrapper)")
    require(args.binary.is_file() and args.assets.is_dir(), "supply a built executable and game assets")
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="netblood-map-vote-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print(f"Map-vote evidence: {root}", flush=True)
    source_binary = args.binary
    args.binary = root / "nblood-tested"
    shutil.copy2(source_binary, args.binary)
    library = build_driver(root)
    result = {"passed": False, "source_binary": str(source_binary),
              "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
              "listen_restart_only": args.listen_restart_only,
              "competitive_restart_only": args.competitive_restart_only,
              "phases": {}, "failures": []}
    games = []

    def start(name, port, dedicated=False, arguments=None, files=None):
        game = Game(name, args, root, library, port, dedicated=dedicated,
                    launch_arguments=arguments, initial_files=files)
        games.append(game)
        if dedicated:
            game.wait(lambda text: "Dedicated server running:" in text and "NET server_tick=" in text,
                      "dedicated server", args.timeout)
        else:
            joined(game, timeout=args.timeout)
        return game

    def call(game, command, eligible, required):
        offset = len(game.text())
        began = time.monotonic()
        game.command_text(command)
        game.wait(lambda text: any(row.get("active") == "1" for row in
                                   records(text[offset:], "NET vote active=")), "accepted vote", args.timeout)
        row = [row for row in records(game.text()[offset:], "NET vote active=") if row.get("active") == "1"][-1]
        require(row.get("eligible") == str(eligible) and row.get("required") == str(required),
                f"{game.name}: incorrect starting electorate/51 percent threshold: {row}")
        require(row.get("yes") == "1" and row.get("no") == "0", "caller did not receive exactly one automatic yes")
        return {"id": row["id"], "began": began, "initial": row}

    def finished(server, ballot, expected, clients, map_count=None):
        vote_id = ballot["id"]
        server.wait(lambda text: any(row.get("id") == vote_id for row in records(text, "NET vote result=")),
                    f"30-second vote {vote_id} completion", 40)
        rows = [row for row in records(server.text(), "NET vote result=") if row.get("id") == vote_id]
        require(len(rows) == 1, f"vote {vote_id} produced duplicate results")
        row = rows[0]
        require(row.get("result") == expected, f"vote {vote_id}: expected {expected}, got {row}")
        elapsed = time.monotonic() - ballot["began"]
        require(elapsed >= 29, f"vote {vote_id} finished early after {elapsed:.2f}s")
        # Every participant receives the result even when a successful vote
        # immediately starts another authoritative map generation.
        for client in clients:
            client.wait(lambda text: any(item.get("id") == vote_id and item.get("result") == expected
                                        for item in records(text, "NET vote result=")),
                        f"vote {vote_id} result", args.timeout)
            if map_count is not None:
                joined(client, map_count, args.timeout)
        return {"result": row, "elapsed_seconds": elapsed}

    def same_rules(before, after, allowed=("rotation_index", "episode", "level")):
        for key, value in before["rules"].items():
            if key not in allowed:
                require(after["rules"].get(key) == value, f"map vote changed server rule {key}")

    def listen_restart_phase():
        # Exercise a graphical host voting on its own authoritative state.
        # CLI -map registration has different standalone-map markers from
        # GUI hosting; the latter is covered by the production policy unit.
        listen = Game("listen-restart", args, root, library, free_port(), listen=True, launch_map="E1M1")
        games.append(listen)
        listen.wait(lambda text: any(int(row["server_tick"]) >= 30
                                    for row in records(text, "NET server_tick=")),
                    "listen server tick", args.timeout)
        before = status(listen)
        before_map = records(listen.text(), "NET map generation=")[-1]

        ballot = call(listen, "callvote restart", 1, 1)
        listen.wait(lambda text: any(row.get("id") == ballot["id"] and row.get("result") == "passed"
                                    for row in records(text, "NET vote result=")),
                    "listen-server restart vote completion", 40)
        elapsed = time.monotonic() - ballot["began"]
        require(elapsed >= 29, f"listen restart vote finished early after {elapsed:.2f}s")
        listen.wait(lambda text: int(records(text, "NET map generation=")[-1]["generation"]) >
                    int(before_map["generation"]), "listen-server restart generation", args.timeout)
        after = status(listen)
        after_map = records(listen.text(), "NET map generation=")[-1]
        same_rules(before, after, allowed=())
        require(int(after_map["generation"]) == int(before_map["generation"]) + 1 and
                after_map["crc"] == before_map["crc"],
                "listen restart must replace the generation exactly once while retaining the same map CRC")
        result["phases"]["listen-restart"] = {
            "elapsed_seconds": elapsed, "before": before, "after": after,
            "before_map": before_map, "after_map": after_map}
        listen.stop()
        print("PASS: listen-server restart preserves map slot, CRC and server rules", flush=True)

    def competitive_restart_phases():
        from cinematics import focus_window
        spec = importlib.util.spec_from_file_location("team_balance_fixture", Path(__file__).with_name("team-balance.py"))
        fixture = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(fixture)
        files = {"NBRST1.MAP": fixture.death_map(), "NBRST2.MAP": fixture.death_map(1),
                 "rotation.txt": b"NBRST1.MAP\nNBRST2.MAP\n"}

        for name, mode, dedicated, rotation in (
                ("bloodbath-restart", "bloodbath", True, False),
                ("teams-restart", "teams", True, True),
                ("listen-teams-restart", "teams", False, False)):
            port = free_port()
            if dedicated:
                options = ["-dedicated", "-port", str(port), "-gametype", mode, "-maxplayers", "2",
                           "-timelimit", "10", "-scorelimit", "25", "-autoaim", "off", "-weaponsstay", "on"]
                options += ["-maplist", "rotation.txt"] if rotation else ["-map", "NBRST1.MAP"]
                server = start(name + "-server", port, True, options, files)
                first = start(name + "-first", port, files=files)
                clients = [first]
            else:
                server = Game(name + "-host", args, root, library, port, listen=True, initial_files=files,
                              launch_arguments=["-server", "2", "-gametype", mode, "-port", str(port),
                                                "-map", "NBRST1.MAP"])
                games.append(server)
                server.wait(lambda text: any(int(row["server_tick"]) >= 30 for row in records(text, "NET server_tick=")),
                            "listen team server", args.timeout)
                first, clients = server, []
            if mode == "teams":
                clients.append(start(name + "-second", port, files=files))
            electorate = len(clients) + (not dedicated)
            if not dedicated:
                focus_window(server)
            before = status(server)
            before_map = records(server.text(), "NET map generation=")[-1]
            offset = len(first.text())
            first.command_text("vote_maps")
            first.wait(lambda text: bool(records(text[offset:], "NET vote options ")), "restart vote options", args.timeout)
            advertised = records(first.text()[offset:], "NET vote options ")[-1]
            expected_actions = 2 | (1 if rotation else 0) | (8 if mode == "teams" else 0)
            require(int(advertised["actions"]) == expected_actions, f"{name}: wrong available vote actions: {advertised}")
            first.wait(lambda text: "callvote restart:" in text[offset:] and
                       (mode != "teams" or "callvote shuffle:" in text[offset:]),
                       "listed restart and team-shuffle commands", args.timeout)
            malformed = rejection(first, "callvote restart NBRST2.MAP", "callvote")

            # Ordinary movement into the fixture pit establishes actual dead
            # state and a nonzero score for the restart to reset.
            focus_window(first)
            first.key(26, True)
            deadline = time.monotonic() + args.timeout
            try:
                while time.monotonic() < deadline:
                    dead = status(server)
                    if dead["players"]["0"]["health"] == "0":
                        break
                    time.sleep(0.1)
                else:
                    raise Failure(f"{name}: player did not fall into the fixture pit")
            finally:
                first.key(26, False)
            while time.monotonic() < deadline:
                dead = status(server)
                if dead["players"]["0"]["frags"] == "-1":
                    break
                time.sleep(0.1)
            else:
                raise Failure(f"{name}: fatal fall did not finalize its score")
            focus_window(first)
            ballot = call(first, "callvote restart", electorate, electorate)
            require(ballot["initial"]["action"] == "restart", "restart vote advertised the wrong action")
            if mode == "teams":
                second = clients[-1]
                focus_window(second)
                rejection(second, "callvote shuffle", "already")
                second.command_text("vote yes")
            if not dedicated:
                focus_window(server)
            require(status(server)["session"]["map_generation"] == before["session"]["map_generation"],
                    "restart occurred before the vote deadline")
            if dedicated:
                phase = finished(server, ballot, "passed", clients, map_count=2)
            else:
                server.wait(lambda text: any(row.get("id") == ballot["id"] and row.get("result") == "passed"
                                            for row in records(text, "NET vote result=")), "listen restart vote completion", 40)
                elapsed = time.monotonic() - ballot["began"]
                require(elapsed >= 29, "listen restart vote finished before its deadline")
                joined(clients[0], 2, args.timeout)
                phase = {"elapsed_seconds": elapsed}
            if not dedicated:
                focus_window(server)
            after = status(server)
            after_map = records(server.text(), "NET map generation=")[-1]
            same_rules(before, after, allowed=())
            require(int(after["session"]["map_generation"]) == int(before["session"]["map_generation"]) + 1 and
                    before_map["crc"] == after_map["crc"], f"{name}: restart must load the same map exactly once")
            expected_teams = {slot: row["team"] for slot, row in before["players"].items()}
            require({slot: row["team"] for slot, row in after["players"].items()} == expected_teams,
                    f"{name}: restart changed team assignments")
            require(all(row["frags"] == "0" and int(row["health"]) > 0 for row in after["players"].values()),
                    f"{name}: restart retained previous health/score state")
            require(all(after["players"][slot]["join_order"] == row["join_order"]
                        for slot, row in before["players"].items()), f"{name}: restart changed connection ages")
            require(not records(server.text(), "NET team shuffle "), f"{name}: restart ran team shuffle")
            if mode == "teams":
                require(after["team_scores"] and all(value == "0" for value in after["team_scores"][-1].values()),
                        "restart retained team scores")
            observed = {}
            for client in clients:
                focus_window(client)
                current = status(client)
                same_rules(before, current, allowed=())
                require(current["session"]["map_generation"] == after["session"]["map_generation"] and
                        {slot: row["team"] for slot, row in current["players"].items()} == expected_teams,
                        f"{client.name}: restarted generation or teams diverged")
                if mode == "teams":
                    require(all(int(row["palette"]) == 11 + int(row["team"]) for row in current["players"].values()),
                            "restart changed the authoritative team colors")
                observed[client.name] = current
            focus_window(first)
            phase.update(before=before, dead=dead, after=after, clients=observed, before_map=before_map,
                         after_map=after_map, options=advertised, malformed=malformed,
                         cooldown=rejection(first, "callvote restart", "wait"))
            result["phases"][name] = phase
            for game in clients + [server]:
                game.stop()
            print(f"PASS: {name} resets health/scores and preserves map, rules and teams", flush=True)

    def multiplayer_phases():
        port = free_port()
        server = start("rotation-server", port, True,
                       ["-dedicated", "-port", str(port), "-gametype", "bloodbath", "-maxplayers", "4",
                        "-timelimit", "10", "-scorelimit", "25", "-autoaim", "off", "-weaponsstay", "on",
                        "-maplist", "rotation.txt"], {"rotation.txt": b"E1M1\nE1M2\n"})
        first = start("client-1", port)
        second = start("client-2", port)
        third = start("client-3", port)
        clients = [first, second, third]
        initial = status(server)
        require(initial["rules"].get("mapvote") == "1", "map voting is not enabled by default")
        offset = len(first.text())
        first.command_text("vote_maps")
        first.wait(lambda text: len(records(text[offset:], "NET vote map ")) == 2, "rotation voting options")
        maps = records(first.text()[offset:], "NET vote map ")
        require([row["name"].upper().removesuffix(".MAP") for row in maps] == ["E1M1", "E1M2"],
                f"vote targets differ from configured rotation: {maps}")
        rejected = {}
        for label, command, diagnostic in (
                ("outside-rotation", "callvote map E1M3", "rotation"),
                ("competitive-next", "callvote next", "rotation"),
                ("gametype", "callvote gametype coop", "callvote"),
                ("extra-target", "callvote map E1M2 extra", "callvote"),
                ("no-ballot", "vote yes", "active")):
            rejected[label] = rejection(first, command, diagnostic)
        require(vote_status(first).get("active") == "0", "invalid request started a vote")
        result["phases"]["validation"] = {"status": initial, "maps": maps, "rejected": rejected}

        # One yes out of three is neither quorum nor a majority. A fourth
        # player joining after the call must not alter the denominator.
        ballot = call(first, "callvote map E1M2", 3, 2)
        rejection(second, "callvote map E1M1", "already")
        rejection(first, "vote no", "already")
        late = start("late-client", port)
        rejection(late, "vote yes", "joined after")
        state = vote_status(late)
        require(state.get("eligible") == "3" and state.get("yes") == "1", "late join changed electorate")
        late.stop()
        phase = finished(server, ballot, "failed", clients)
        phase["status"] = status(server)
        require(phase["status"]["session"]["map_generation"] == initial["session"]["map_generation"],
                "no-quorum vote changed map")
        phase["cooldown"] = rejection(first, "callvote map E1M2", "wait")
        result["phases"]["no-quorum"] = phase
        print("PASS: no quorum, fixed electorate, concurrent vote and caller cooldown", flush=True)

        ballot = call(second, "callvote map E1M2", 3, 2)
        first.command_text("vote no")
        third.command_text("vote no")
        rejection(third, "vote yes", "already")
        phase = finished(server, ballot, "failed", clients)
        require(phase["result"].get("yes") == "1" and phase["result"].get("no") == "2",
                "duplicate ballot changed yes/no totals")
        phase["status"] = status(server)
        require(phase["status"]["session"]["map_generation"] == initial["session"]["map_generation"],
                "majority-no vote changed map")
        result["phases"]["majority-no"] = phase
        print("PASS: participation alone cannot pass a vote; ballot cannot be changed", flush=True)

        ballot = call(third, "callvote map e1m2.map", 3, 2)
        first.command_text("vote yes")
        second.command_text("vote no")
        active = vote_status(first)
        require(active.get("active") == "1" and active.get("yes") == "2", "majority did not remain open for 30 seconds")
        require(status(server)["session"]["map_generation"] == initial["session"]["map_generation"],
                "majority caused an early map change")
        time.sleep(5)
        first.command_text("net_record voted-map.nbpd")
        phase = finished(server, ballot, "passed", clients, map_count=2)
        after = status(server)
        require(int(after["session"]["map_generation"]) == int(initial["session"]["map_generation"]) + 1 and
                after["rules"].get("rotation_index") == "1", "passed vote did not select the rotation target once")
        same_rules(initial, after)
        for client in clients:
            observed = status(client)
            same_rules(initial, observed)
            require(observed["session"]["map_generation"] == after["session"]["map_generation"] and
                    observed["rules"]["rotation_index"] == "1", f"{client.name}: map generation/rules diverged")
        phase.update(server=after, caller_cooldown=rejection(third, "callvote map E1M1", "wait"),
                     earlier_caller_cooldown=rejection(first, "callvote map E1M1", "wait"))
        first.command_text("net_stoprecord")
        demo = inspect_demo(first.path / "voted-map.nbpd")
        recorded_votes = [row for row in demo["packets"] if row["type"] == 17]
        recorded_options = [row for row in demo["packets"] if row["type"] == 18]
        require(recorded_votes and recorded_votes[0]["sequence"] == 0 and
                recorded_votes[0]["vote_state"] == 1 and recorded_votes[0]["vote_yes"] == 2 and
                recorded_votes[0]["vote_no"] == 1 and recorded_votes[0]["vote_eligible"] == 3 and
                0 < recorded_votes[0]["vote_remaining_ms"] < 26000,
                "recording begun during vote lacks the synthetic active-vote baseline")
        require(recorded_options and recorded_options[0]["sequence"] == 0 and
                recorded_options[0]["vote_enabled"] == 1 and recorded_options[0]["vote_actions"] == 3 and
                recorded_options[0]["vote_map_count"] == 2,
                "recording begun during vote lacks the synthetic map-options baseline")
        require(any(row["vote_state"] == 2 and row["vote_id"] == int(ballot["id"]) for row in recorded_votes),
                "packet recording omitted the passed vote result")
        require(len({row["generation"] for row in demo["packets"] if row["type"] == 2}) == 2,
                "packet recording omitted the voted map transition")
        result["phases"]["majority-yes"] = phase
        print("PASS: majority changes only the map; cooldown survives map transitions", flush=True)
        for game in clients + [server]:
            game.stop()
        result["vote_demo"] = demo
        result["vote_demo_playback"] = play_demo("vote-playback", demo, args, root, library, games)
        playback = games[-1]
        active_votes = records(playback.text(), "NET vote active=")
        require(any(row.get("active") == "1" and row.get("yes") == "2" and row.get("no") == "1"
                    for row in active_votes), "playback did not restore the mid-vote state")
        require(any(row.get("result") == "passed" and row.get("id") == ballot["id"]
                    for row in records(playback.text(), "NET vote result=")), "playback omitted the passed vote result")
        require(len(records(playback.text(), "NET client map loaded ")) >= 2,
                "playback omitted the voted map transition")
        print("PASS: mid-vote recording restores vote/options and replays voted map transition", flush=True)

        port = free_port()
        coop = start("coop-server", port, True,
                     ["-dedicated", "-port", str(port), "-gametype", "coop", "-maxplayers", "2",
                      "-episode", "1", "-difficulty", "3", "-losekeys", "off", "-autoaim", "on"])
        coop_first = start("coop-first", port)
        coop_second = start("coop-second", port)
        coop_clients = [coop_first, coop_second]
        before = status(coop)
        rejection(coop_first, "callvote map E1M2", "Co-op")
        ballot = call(coop_first, "callvote restart", 2, 2)
        coop_second.command_text("vote yes")
        phase = finished(coop, ballot, "passed", coop_clients, map_count=2)
        after = status(coop)
        same_rules(before, after, allowed=())
        require(int(after["session"]["map_generation"]) == int(before["session"]["map_generation"]) + 1,
                "co-op restart did not replace the map generation")
        phase["server"] = after
        result["phases"]["coop-restart"] = phase
        ballot = call(coop_second, "callvote next", 2, 2)
        coop_first.command_text("vote yes")
        phase = finished(coop, ballot, "passed", coop_clients, map_count=3)
        after = status(coop)
        same_rules(before, after)
        require(after["rules"].get("episode") == "1" and after["rules"].get("level") == "2",
                "co-op next vote did not follow the episode's normal exit")
        for client in coop_clients:
            observed = status(client)
            same_rules(before, observed)
            require(observed["session"]["map_generation"] == after["session"]["map_generation"] and
                    observed["rules"].get("level") == "2", "co-op map generation/rules diverged")
        phase["server"] = after
        result["phases"]["coop-next"] = phase
        print("PASS: co-op restart and next preserve server rules", flush=True)
        for game in coop_clients + [coop]:
            game.stop()

        for name, options, diagnostic in (
                ("disabled", ["-mapvote", "off", "-maplist", "rotation.txt"], "disabled"),
                ("no-rotation", ["-map", "E1M1"], "rotation")):
            port = free_port()
            host = start(name + "-server", port, True,
                         ["-dedicated", "-port", str(port), "-gametype", "bloodbath"] + options,
                         {"rotation.txt": b"E1M1\nE1M2\n"})
            client = start(name + "-client", port)
            before = status(host)
            diagnostic_text = rejection(client, "callvote map E1M2", diagnostic)
            require(vote_status(client).get("active") == "0", f"{name}: invalid vote became active")
            require(status(host)["session"]["map_generation"] == before["session"]["map_generation"],
                    f"{name}: rejected vote changed map")
            result["phases"][name] = {"server": before, "rejection": diagnostic_text}
            client.stop()
            host.stop()
    try:
        if args.competitive_restart_only:
            competitive_restart_phases()
        else:
            if not args.listen_restart_only:
                multiplayer_phases()
                competitive_restart_phases()
            listen_restart_phase()
        result["passed"] = True
    except (Failure, OSError, subprocess.SubprocessError) as error:
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
