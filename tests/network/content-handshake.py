#!/usr/bin/env python3
"""Exercise production content admission and rejection with real ENet peers.

Requires an existing X11 display, the built game, C++ compiler and stock assets.
The companion peer deliberately sends protocol faults; the actual game handles
all admission decisions, map transitions, disconnects and visible diagnostics.
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


def port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def wait_output(process, log, needle, timeout=45):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = log.read_text(errors="replace")
        if needle in text:
            return text
        require(process.poll() is None, f"peer exited before {needle}: {text}")
        time.sleep(0.05)
    raise Failure(f"peer timed out waiting for {needle}: {log}")


def quit_before_map(game):
    game.key(67, True)  # F10
    time.sleep(0.25)
    game.key(67, False)
    game.key(28, True)  # Y
    game.process.wait(timeout=10)
    game.stop()


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
    require(os.environ.get("DISPLAY"), "clients need DISPLAY (use an external Xvfb wrapper)")
    require(args.binary.is_file() and os.access(args.binary, os.X_OK), "game executable missing")
    require(args.assets.is_dir(), "game assets missing")
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="netblood-content-handshake-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print(f"Content-handshake evidence: {root}", flush=True)
    library = build_driver(root)
    peer = root / "content-handshake-peer"
    command = shlex.split(os.environ.get("CXX", "c++")) + ["-std=c++11", "-O1", "-g", "-Wall", "-Wextra", "-Werror"]
    command += shlex.split(os.environ.get("CPPFLAGS", "")) + shlex.split(os.environ.get("CXXFLAGS", ""))
    command += [f"-I{repository / 'source/build/include'}", f"-I{repository / 'source/blood/src'}",
                str(Path(__file__).with_suffix(".cpp")), "-pthread"]
    command += shlex.split(os.environ.get("LDFLAGS", "")) + ["-o", str(peer)]
    subprocess.run(command, check=True)
    source_binary = args.binary
    args.binary = root / "nblood-tested"
    shutil.copy2(source_binary, args.binary)
    result = {"passed": False, "source_binary": str(source_binary),
              "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(), "cases": {}, "failures": []}
    games, peers = [], []

    def probe(mode, server_port, fingerprint=0):
        completed = subprocess.run([str(peer), "probe", str(server_port), mode, str(fingerprint)],
                                   text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
        (root / f"probe-{mode}.log").write_text(completed.stdout)
        require(completed.returncode == 0, f"{mode} probe failed: {completed.stdout}")
        result["cases"][mode] = completed.stdout.strip()
        return completed.stdout

    try:
        server_port = port()
        server = Game("server", args, root, library, server_port, dedicated=True)
        games.append(server)
        server.wait(lambda text: "Dedicated server running:" in text and
                    any(int(row["server_tick"]) >= 30 for row in records(text, "NET server_tick=")),
                    "dedicated server", args.timeout)
        inspected = probe("inspect", server_port)
        fingerprint = int(re.search(r"fingerprint=(\d+)", inspected).group(1))
        probe("invalid-ready", server_port, fingerprint)
        probe("wrong-ready", server_port, fingerprint)
        probe("legacy", server_port, fingerprint)
        trigger = root / "map-changed.trigger"
        race_log = root / "probe-race.log"
        with race_log.open("w") as output:
            race = subprocess.Popen([str(peer), "probe", str(server_port), "race", str(fingerprint), str(trigger)],
                                    stdout=output, stderr=subprocess.STDOUT)
        peers.append(race)
        before = wait_output(race, race_log, "repeated_hello_suppressed=1", args.timeout)
        generation = int(re.search(r"offered_generation=(\d+)", before).group(1))
        server.command_text("map E1M2")
        server.wait(lambda text: any(int(row["generation"]) > generation for row in records(text, "NET map generation=")),
                    "map changed while client verifies files", args.timeout)
        trigger.write_text("ready\n")
        require(race.wait(timeout=15) == 0, f"map-generation race failed: {race_log.read_text()}")
        result["cases"]["race"] = race_log.read_text().strip()
        require("NET joined slot=" not in server.text(), "unready probe entered authoritative simulation")
        server.stop()

        # Keep separate peer processes and UDP ports so no case inherits state.
        for mode in ("bad-count", "truncated", "no-manifest", "old-reject"):
            fake_port = port()
            fake_log = root / f"fake-server-{mode}.log"
            with fake_log.open("w") as output:
                fake = subprocess.Popen([str(peer), "server", str(fake_port), mode],
                                        stdout=output, stderr=subprocess.STDOUT)
            peers.append(fake)
            wait_output(fake, fake_log, "listening=1", args.timeout)
            client = Game(f"client-{mode}", args, root, library, fake_port)
            games.append(client)
            if mode in ("bad-count", "truncated"):
                expected = "invalid asset manifest"
            elif mode == "no-manifest":
                expected = "did not provide an asset manifest"
            else:
                expected = "Update the server and client to compatible versions with mod asset checking."
            # Read directly: the general Game.wait intentionally fails on Reject.
            deadline = time.monotonic() + args.timeout
            while expected not in client.text():
                require(client.process.poll() is None, f"{mode}: game exited before diagnostic")
                require(time.monotonic() < deadline, f"{mode}: missing '{expected}' in {client.path}")
                time.sleep(0.05)
            require(fake.wait(timeout=15) == 0, f"{mode}: fake peer failed: {fake_log.read_text()}")
            text = client.text()
            require("NET content verified" not in text and "NET client map loaded" not in text and
                    "NET map download started" not in text and "NET client slot=" not in text,
                    f"{mode}: client approved content or started gameplay")
            report = client.path / "net-content-mismatch.txt"
            require(report.is_file() and expected in report.read_text(), f"{mode}: diagnostic was not saved")
            result["cases"][mode] = {"peer": fake_log.read_text().strip(), "report": str(report)}
            quit_before_map(client)
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
        result["processes"] = [{"name": game.name, "exit_code": game.process.returncode,
                                "forced": game.forced} for game in games]
        if any(item["exit_code"] != 0 or item["forced"] for item in result["processes"]):
            result["passed"] = False
            result["failures"].append("game process exited uncleanly")
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
