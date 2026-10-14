#!/usr/bin/env python3
"""Exercise protocol-version advertisements and persistent prejoin errors.

Requires a C++ compiler, Blood 1.21 assets and X11/Xvfb. ENet peers advertise
older/newer protocols without rebuilding the game; real clients handle all
version diagnostics and refusal to enter gameplay.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import time

from cinematics import capture_window, focus_window, free_port
from run import Failure, build_driver, require
from scoreboard import Game, expect


def wait_for(process, read, predicate, description, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = read()
        if predicate(text):
            return text
        require(process.poll() is None, f"process exited before {description}: {text[-2000:]}")
        time.sleep(0.05)
    raise Failure(f"timed out waiting for {description}: {text[-2000:]}")


def quit_before_map(game):
    focus_window(game)
    game.key(67, True)  # F10: normal quit dialog works before the first map.
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
    parser.add_argument("--skip-screenshots", action="store_true")
    args = parser.parse_args()
    for name in ("binary", "assets", "engine_data"):
        setattr(args, name, getattr(args, name).resolve())
    require(os.environ.get("DISPLAY"), "clients need DISPLAY (use an Xvfb wrapper)")
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="netblood-client-version-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print(f"Client-version evidence: {root}", flush=True)
    source_binary = args.binary
    args.binary = root / "nblood-tested"
    shutil.copy2(source_binary, args.binary)
    library = build_driver(root)
    peer = root / "version-peer"
    command = shlex.split(os.environ.get("CXX", "c++")) + ["-std=c++11", "-O1", "-g", "-Wall", "-Wextra", "-Werror"]
    command += shlex.split(os.environ.get("CPPFLAGS", "")) + shlex.split(os.environ.get("CXXFLAGS", ""))
    command += [f"-I{repository / 'source/build/include'}", f"-I{repository / 'source/blood/src'}",
                str(Path(__file__).with_name("content-handshake.cpp")),
                str(repository / "source/blood/src/netaddon.cpp"), "-pthread"]
    command += shlex.split(os.environ.get("LDFLAGS", "")) + ["-o", str(peer)]
    subprocess.run(command, check=True)
    result = dict(passed=False, source_binary=str(source_binary),
                  binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                  phases={}, screenshots={}, failures=[])
    games, peers = [], []

    try:
        server_port = free_port()
        server = Game("server", args, root, library, server_port, dedicated=True,
                      launch_arguments=["-dedicated", "-port", str(server_port), "-gametype", "bloodbath",
                                        "-maxplayers", "8", "-map", "E1M1"])
        games.append(server)
        server.wait(lambda text: "Dedicated server running:" in text and "NET server_tick=" in text,
                    "dedicated server", args.timeout)
        for version in (7, 8, 9):
            completed = subprocess.run([str(peer), "version-probe", str(server_port), str(version)],
                                       text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
            (root / f"probe-{version}.log").write_text(completed.stdout)
            require(completed.returncode == 0, f"protocol {version} probe failed: {completed.stdout}")
            require("advertisement=1" in completed.stdout and "required_protocol=8" in completed.stdout,
                    f"protocol {version}: missing required-version advertisement")
            if version != 8:
                require("rejected=1" in completed.stdout and "disconnected=1" in completed.stdout,
                        f"protocol {version}: mismatch not rejected/disconnected")
            result["phases"][f"server-client-protocol-{version}"] = completed.stdout.strip()
            expect(server, lambda rows: not rows, "no admission before a compatible Hello", args.timeout)
        require("NET joined slot=" not in server.text(), "version-only peer entered authoritative simulation")

        compatible = Game("compatible-client", args, root, library, server_port,
                          launch_arguments=["-client", "127.0.0.1", "-port", str(server_port), "-pname=Compatible"])
        games.append(compatible)
        compatible.wait(lambda text: "NET client map loaded " in text and "NET client slot=" in text,
                        "compatible client gameplay", args.timeout)
        require("NET server version required=8 client=8 build=" in compatible.text(),
                "compatible client did not receive the required-version/build advertisement")
        result["phases"]["compatible-client-joins"] = expect(
            compatible, lambda rows: len(rows) == 1 and rows[0]["name"] == "Compatible",
            "compatible client authoritative roster", args.timeout)
        compatible.stop()
        server.stop()
        print("PASS: server advertises protocol/build, rejects older/newer peers, and admits a current client", flush=True)

        for mode, required in (("advert-newer", 9), ("advert-older", 7), ("legacy-reject", 7)):
            fake_port = free_port()
            fake_log = root / f"{mode}-peer.log"
            with fake_log.open("w") as output:
                fake = subprocess.Popen([str(peer), "version-server", str(fake_port), mode],
                                        stdout=output, stderr=subprocess.STDOUT)
            peers.append(fake)
            wait_for(fake, lambda: fake_log.read_text(errors="replace"), lambda text: "listening=1" in text,
                     f"{mode} listening", args.timeout)
            client = Game(mode, args, root, library, fake_port)
            games.append(client)
            # Game.wait deliberately treats any rejection as a failed join;
            # this test expects rejection and instead reads the real error log.
            expected = f"NET version mismatch client=8 required={required} build="
            diagnostic = wait_for(client.process, client.text,
                                  lambda text: expected in text,
                                  f"{mode} actionable version mismatch", args.timeout)
            require(fake.wait(timeout=15) == 0, f"{mode}: peer failed: {fake_log.read_text()}")
            peer_text = fake_log.read_text()
            require("version_server_disconnected=1" in peer_text and "content_ready_sent=0" in peer_text
                    and "ready_sent=0" in peer_text and "commands_sent=0" in peer_text,
                    f"{mode}: incompatible connection reached admission/gameplay")
            if mode == "legacy-reject":
                require("NET rejected by server version=7:" in diagnostic,
                        "old server rejection was not recognized before strict gameplay decoding")
            if not args.skip_screenshots:
                result["screenshots"][mode] = capture_window(client, root / f"{mode}.png")
            # A disconnected prejoin screen must retain the actionable error,
            # rather than showing it only for the network event's single frame.
            time.sleep(3)
            require(client.process.poll() is None, f"{mode}: client closed instead of retaining its error")
            if not args.skip_screenshots:
                capture = capture_window(client, root / f"{mode}-persistent.png")
                from PIL import Image, ImageChops
                first = Image.open(root / f"{mode}.png").convert("RGB")
                later = Image.open(capture["path"]).convert("RGB")
                # Blood's nominal white font uses a light-gray palette entry,
                # below the cinematic helper's >200 pure-white threshold.
                bounds = (0, int(later.height * 0.4), later.width, int(later.height * 0.7))
                region = later.crop(bounds)
                pixels = region.get_flattened_data() if hasattr(region, "get_flattened_data") else region.getdata()
                require(sum(min(pixel) > 160 and max(pixel) - min(pixel) < 12 for pixel in pixels) > 100,
                        f"{mode}: error region has no visible light-gray text")
                require(ImageChops.difference(first.crop(bounds), region).getbbox() is None,
                        f"{mode}: error region changed after disconnect instead of remaining visible")
                result["screenshots"][mode + "-persistent"] = capture
            text = client.text()
            require(not any(marker in text for marker in
                            ("NET content verified", "NET client map loaded", "NET map download started", "NET client slot=")),
                    f"{mode}: incompatible client started content admission or gameplay")
            require(not (client.path / "net-content-mismatch.txt").exists(),
                    f"{mode}: version error incorrectly produced mod-asset installation advice")
            result["phases"][mode] = dict(peer=peer_text.strip(), log=str(client.path / "nblood.log"),
                                         required_protocol=required, error_persistence_seconds=3)
            quit_before_map(client)
            print(f"PASS: {mode} produces persistent update guidance without loading gameplay", flush=True)
        result["passed"] = True
    except Exception as error:
        result["failures"].append(f"{type(error).__name__}: {error}")
        print(f"FAIL: {type(error).__name__}: {error}", flush=True)
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
