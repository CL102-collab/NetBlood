#!/usr/bin/env python3
"""Exercise dedicated CLI policies, authoritative rules and timed map rotation.

Requires a built game, stock Blood 1.21 assets and an existing X11 display.
Actual game processes perform admission, snapshots, timing and map changes.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import socket
import subprocess
import tempfile
import time

from run import Failure, Game, build_driver, records, require


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def wait_raw(game, predicate, description, timeout=45):
    # Rejection is an expected result in the player-cap case.
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate(game.text()):
            return game.text()
        require(game.process.poll() is None, f"{game.name} exited before {description}")
        time.sleep(0.05)
    raise Failure(f"{game.name}: timed out waiting for {description}")


def quit_before_map(game):
    game.key(67, True)  # F10, then ordinary quit confirmation.
    time.sleep(0.25)
    game.key(67, False)
    game.key(28, True)  # Y.
    game.process.wait(timeout=10)
    game.stop()


def joined(game, count=1, timeout=45):
    return game.wait(lambda text: len(records(text, "NET client map loaded ")) >= count and
                     len(records(text, "NET client slot=")) >= count,
                     f"authoritative map {count}", timeout)


def status(game, expected=None):
    offset = len(game.text())
    game.command_text("net_status")
    text = game.text()[offset:]
    rule_rows, status_rows = records(text, "NET rules "), records(text, "NET status ")
    require(rule_rows and status_rows, f"{game.name}: status omitted authoritative rules")
    rules = rule_rows[-1]
    for name, value in (expected or {}).items():
        require(rules.get(name) == str(value), f"{game.name}: {name} expected {value}, got {rules.get(name)}")
    return {"rules": rules, "session": status_rows[-1], "players": records(text, "NET player ")}


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
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="netblood-server-settings-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print(f"Server-settings evidence: {root}", flush=True)
    source_binary = args.binary
    args.binary = root / "nblood-tested"
    shutil.copy2(source_binary, args.binary)
    library = build_driver(root)
    peer = root / "content-handshake-peer"
    compile_command = shlex.split(os.environ.get("CXX", "c++")) + ["-std=c++11", "-O1", "-g", "-Wall", "-Wextra", "-Werror"]
    compile_command += shlex.split(os.environ.get("CPPFLAGS", "")) + shlex.split(os.environ.get("CXXFLAGS", ""))
    compile_command += [f"-I{repository / 'source/build/include'}", f"-I{repository / 'source/blood/src'}",
                        str(Path(__file__).with_name("content-handshake.cpp")), "-pthread"]
    compile_command += shlex.split(os.environ.get("LDFLAGS", "")) + ["-o", str(peer)]
    subprocess.run(compile_command, check=True)
    result = {"passed": False, "source_binary": str(source_binary),
              "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
              "invalid_startup": {}, "phases": {}, "failures": []}
    games, peers = [], []

    def start(name, port, dedicated=False, arguments=None, files=None):
        game = Game(name, args, root, library, port, dedicated=dedicated,
                    launch_arguments=arguments, initial_files=files)
        games.append(game)
        return game

    def invalid(name, arguments, needle, files=None):
        directory = root / ("invalid-" + name)
        directory.mkdir()
        for filename, contents in (files or {}).items():
            (directory / filename).write_text(contents)
        command = [str(args.binary), "-usecwd", "-nosetup", "-noautoload", "-quick", "-nodemo",
                   f"-j={args.assets}", f"-j={args.engine_data}"] + arguments
        environment = os.environ.copy()
        for variable in ("DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY"):
            environment.pop(variable, None)
        environment.update(SDL_VIDEODRIVER="network-test-invalid-video-driver",
                           SDL_AUDIODRIVER="network-test-invalid-audio-driver")
        completed = subprocess.run(command, cwd=directory, env=environment, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True, timeout=args.timeout)
        (directory / "console.log").write_text(completed.stdout)
        log = directory / "nblood.log"
        text = completed.stdout + (log.read_text(errors="replace") if log.exists() else "")
        require(completed.returncode != 0, f"{name}: invalid startup succeeded")
        diagnostics = [line for line in text.splitlines() if re.search(r"\b(?:ERR|ERROR|FATAL)\||\.cpp\(\d+\):", line)]
        require(any(needle.lower() in line.lower() for line in diagnostics), f"{name}: error diagnostic did not mention {needle}")
        require("NET server listening" not in text, f"{name}: invalid startup opened a server")
        result["invalid_startup"][name] = {"command": command, "exit_code": completed.returncode,
                                            "diagnostic": diagnostics, "log": str(directory / "console.log")}

    def profile_probe(server, server_port, expected_aim, expected_skill, name):
        inspected = subprocess.run([str(peer), "probe", str(server_port), "inspect"], text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)
        require(inspected.returncode == 0, f"{name}: could not inspect content fingerprint: {inspected.stdout}")
        fingerprint = int(re.search(r"fingerprint=(\d+)", inspected.stdout).group(1))
        trigger = root / (name + ".release")
        log = root / (name + ".log")
        with log.open("w") as output:
            process = subprocess.Popen([str(peer), "probe", str(server_port), "profile-hold", str(fingerprint), str(trigger)],
                                       stdout=output, stderr=subprocess.STDOUT)
        peers.append(process)
        deadline = time.monotonic() + args.timeout
        while "profile_ready=1" not in log.read_text():
            require(process.poll() is None, f"{name}: profile peer exited: {log.read_text()}")
            require(time.monotonic() < deadline, f"{name}: profile admission timed out")
            time.sleep(0.05)
        slot = int(re.search(r"slot=(\d+)", log.read_text()).group(1))
        snapshot = status(server)
        player = [row for row in snapshot["players"] if int(row["slot"]) == slot]
        require(len(player) == 1 and player[0].get("autoaim") == str(expected_aim),
                f"{name}: peer's requested aim=3 bypassed server aim={expected_aim}")
        require(player[0].get("skill") == str(expected_skill),
                f"{name}: peer's requested skill=0 bypassed server difficulty={expected_skill}")
        disconnect_count = server.text().count(f"NET disconnected slot={slot}")
        trigger.write_text("release\n")
        require(process.wait(timeout=10) == 0, f"{name}: profile peer failed: {log.read_text()}")
        result["phases"][name] = {"status": snapshot, "peer": log.read_text()}
        server.wait(lambda text: text.count(f"NET disconnected slot={slot}") > disconnect_count,
                    "spoofed-profile peer disconnect", args.timeout)

    try:
        for flag, values in (("port", ("0", "65536", "12junk")),
                             ("maxplayers", ("0", "9", "2x")),
                             ("timelimit", ("-1", "10081", "1.5")),
                             ("scorelimit", ("-1", "2147483648", "ten")),
                             ("difficulty", ("-1", "5", "2x")),
                             ("episode", ("0", "8", "1x")),
                             ("autoaim", ("maybe",)), ("weaponsstay", ("2",)), ("losekeys", ("yes",)),
                             ("mapvote", ("maybe",)),
                             ("gametype", ("invalid",))):
            for index, value in enumerate(values):
                invalid(f"{flag}-{index}", ["-dedicated", "-" + flag, value], "game type" if flag == "gametype" else flag)
        for flag in ("server", "port", "gametype", "maxplayers", "timelimit", "scorelimit", "autoaim", "weaponsstay", "maplist", "episode", "difficulty", "losekeys", "mapvote"):
            invalid("missing-" + flag, ["-dedicated", "-" + flag], "game type" if flag == "gametype" else flag)
        for flag, value in (("episode", "1"), ("difficulty", "2"), ("losekeys", "off")):
            invalid("coop-only-" + flag, ["-dedicated", "-gametype", "bloodbath", "-" + flag, value], flag)
        invalid("maplist-path", ["-dedicated", "-maplist", "../rotation.txt"], "maplist")
        invalid("maplist-extension", ["-dedicated", "-maplist", "rotation.cfg"], "maplist")
        invalid("maplist-missing", ["-dedicated", "-maplist", "missing.txt"], "missing.txt")
        invalid("maplist-empty", ["-dedicated", "-maplist", "rotation.txt"], "rotation",
                {"rotation.txt": "\n# Comment\n; Another comment\n  \n"})
        invalid("maplist-missing-map", ["-dedicated", "-maplist", "rotation.txt"], "NO_SUCH_MAP_4591",
                {"rotation.txt": "E1M1\nNO_SUCH_MAP_4591\n"})
        invalid("map-maplist-conflict", ["-dedicated", "-map", "E1M1", "-maplist", "rotation.txt"], "map",
                {"rotation.txt": "E1M1\nE1M2\n"})
        invalid("episode-maplist-conflict", ["-dedicated", "-episode", "1", "-maplist", "rotation.txt"], "episode",
                {"rotation.txt": "E1M1\nE1M2\n"})
        invalid("episode-map-conflict", ["-dedicated", "-episode", "1", "-map", "E1M1"], "episode")
        invalid("player-cap-conflict", ["-dedicated", "-server", "2", "-maxplayers", "3"], "maxplayers")
        invalid("coop-scorelimit", ["-dedicated", "-gametype", "coop", "-scorelimit", "5"], "scorelimit")
        invalid("non-dedicated-rule", ["-maxplayers", "2"], "dedicated")
        invalid("non-dedicated-mapvote", ["-mapvote", "off"], "dedicated")

        port = free_port()
        expected = {"gametype": 2, "timelimit": 1, "scorelimit": 25, "maxplayers": 2,
                    "autoaim": 0, "weaponsstay": 1, "rotation_count": 2}
        server_started = time.monotonic()
        server = start("rotation-server", port, True,
                       ["-dedicated", "-port", str(port), "-gametype", "bloodbath", "-maxplayers", "2",
                        "-timelimit", "1", "-scorelimit", "25", "-autoaim", "off", "-weaponsstay", "on",
                        "-maplist", "rotation.txt"],
                       {"rotation.txt": b"\xef\xbb\xbf# Real server rotation\r\n\r\n E1M1 \r\n; Comment between maps\r\nE1M2 # inline comment\r\n"})
        server.wait(lambda text: "Dedicated server running:" in text and "NET server_tick=" in text,
                    "configured empty server", args.timeout)
        result["phases"]["empty-server"] = status(server, expected)
        require(f"UDP port={port} dedicated=1 maxplayers=2" in server.text(), "server did not bind configured port/cap")
        profile_probe(server, port, 0, 2, "spoof-autoaim-off")

        first = start("client-1", port)
        joined(first, timeout=args.timeout)
        result["phases"]["first-client"] = status(first, expected)
        time.sleep(1)
        second = start("late-client", port)
        joined(second, timeout=args.timeout)
        result["phases"]["late-client"] = status(second, expected)
        before = status(server, expected)
        require(before["session"]["players"] == "2", "server did not admit exactly two clients")
        require(all(row.get("autoaim") == "0" for row in before["players"]), "off rule did not reach player profiles")
        first.command_text("map E1M2\nchangelevel 1 2\nrestartmap\ncl_autoaim 3\ncl_autoaim")
        require(first.text().count("Only the server can change the map.") >= 3, "client map commands were not denied")
        require("cl_autoaim: controlled by server" in first.text(), "client autoaim override was not denied")
        after = status(first, expected)
        require(after["session"]["map_generation"] == before["session"]["map_generation"], "client command changed map generation")
        result["phases"]["denied-client-changes"] = after

        excess = start("excess-client", port)
        wait_raw(excess, lambda text: "NET rejected: Server full" in text, "full-server rejection", args.timeout)
        require("NET client map loaded" not in excess.text(), "full server admitted extra player")
        quit_before_map(excess)

        first_map_time = time.monotonic()
        first_generation = int(before["session"]["map_generation"])
        # This is intentionally a real minute: the production dedicated loop,
        # round-limit check and asynchronous hot-join transition all run.
        server.wait(lambda text: any(int(row["generation"]) > first_generation for row in records(text, "NET map generation=")),
                    "one-minute automatic rotation", 80)
        joined(first, count=2, timeout=args.timeout)
        joined(second, count=2, timeout=args.timeout)
        finished = records(server.text(), "NET dedicated map finished:")
        require(finished and finished[-1].get("reason") == "timelimit" and finished[-1].get("elapsed_ticks") == "1800",
                "rotation was not caused by the configured one-minute limit")
        require(time.monotonic() - server_started >= 55, "one-minute game limit ran substantially faster than wall time")
        result["phases"]["timed-rotation"] = {"wait_seconds": time.monotonic() - first_map_time,
                                                  "completion": finished[-1],
                                                  "server": status(server, expected),
                                                  "first": status(first, expected), "second": status(second, expected)}
        require(result["phases"]["timed-rotation"]["server"]["rules"]["rotation_index"] == "1",
                "time limit did not advance to second rotation entry")
        next_map = records(server.text(), "NET map generation=")[-1]
        for game, phase in ((first, "first"), (second, "second")):
            welcome = records(game.text(), "NET welcome ")[-1]
            rules = result["phases"]["timed-rotation"][phase]["rules"]
            require(welcome["map_crc"] == next_map["crc"] and welcome["map_generation"] == next_map["generation"] and
                    rules["episode"] == "1" and rules["level"] == "2",
                    f"{game.name}: rotation did not install the server's second map")
        roundtrips = records(first.text(), "NET snapshot roundtrip ")
        require(len(roundtrips) >= 30 and all(row.get("equal") == "1" for row in roundtrips),
                "configured game rules did not round-trip through installed snapshots")
        result["exact_snapshot_roundtrips"] = len(roundtrips)
        first.stop()
        second.stop()
        server.stop()

        port = free_port()
        coop_expected = {"gametype": 1, "timelimit": 0, "scorelimit": 0, "maxplayers": 1, "autoaim": 1,
                         "weaponsstay": 0, "difficulty": 4, "losekeys": 0, "episode": 2, "level": 1}
        coop = start("coop-server", port, True,
                     ["-dedicated", "-port", str(port), "-gametype", "coop", "-maxplayers", "1",
                      "-episode", "2", "-difficulty", "4", "-losekeys", "off", "-autoaim", "on",
                      "-weaponsstay", "off", "-timelimit", "0", "-scorelimit", "0"])
        coop.wait(lambda text: "Dedicated server running:" in text and "NET server_tick=" in text,
                  "configured co-op episode", args.timeout)
        result["phases"]["coop-server"] = status(coop, coop_expected)
        profile_probe(coop, port, 1, 4, "spoof-autoaim-on")
        coop_client = start("coop-client", port)
        joined(coop_client, timeout=args.timeout)
        result["phases"]["coop-client"] = status(coop_client, coop_expected)
        coop_client.command_text("cl_autoaim 0\ncl_autoaim")
        require("cl_autoaim: controlled by server" in coop_client.text(), "client disabled forced autoaim")
        result["phases"]["coop-client-after-override"] = status(coop_client, coop_expected)
        coop.command_text("restartmap")
        joined(coop_client, count=2, timeout=args.timeout)
        result["phases"]["coop-server-restart"] = {"server": status(coop, coop_expected),
                                                     "client": status(coop_client, coop_expected)}
        coop_client.stop()
        coop.stop()
        port = free_port()
        teams = start("teams-server", port, True,
                      ["-dedicated", "-server", "1", "-port", str(port), "-gametype", "teams", "-scorelimit", "20"])
        teams.wait(lambda text: "Dedicated server running:" in text and "NET server_tick=" in text,
                   "configured teams server", args.timeout)
        result["phases"]["teams-server"] = status(teams, {"gametype": 3, "maxplayers": 1, "scorelimit": 20,
                                                            "timelimit": 0, "autoaim": 1, "weaponsstay": 1})
        teams.stop()
        result["passed"] = True
    except (Failure, OSError, subprocess.SubprocessError) as error:
        result["failures"].append(str(error))
        print(f"FAIL: {error}", flush=True)
    finally:
        for peer_process in peers:
            if peer_process.poll() is None:
                peer_process.terminate()
                try:
                    peer_process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    peer_process.kill()
                    peer_process.wait(timeout=5)
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
