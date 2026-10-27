#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check actual NBlood listen-host hot join and downloads over the overlay API.

The only test double is the separately launched SDK companion: its generic UDP
sockets use a lossy loopback bus rather than contacting a ZeroTier controller.
The game, password admission, ENet, map/mod transfers, restart and snapshots are
production code. This test does not establish real ZeroTier NAT traversal.

Requires a built NBlood, Blood 1.21 assets, SDL2 headers and an X11 display.
"""
import argparse
from contextlib import contextmanager
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import tempfile
import time

from run import Failure, build_driver, records, require
from scoreboard import Game as BaseGame
from password import wait_rejection, tap, type_text
from cinematics import capture_window, focus_window

ROOT = Path(__file__).resolve().parents[2]
NETWORK = "1234567890abcdef"
PORT = 23513


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


addon = module("zerotier_addon_fixtures", "addon-join.py")
map_download = module("zerotier_map_cache", "map-download.py")
fixture = module("zerotier_map_fixture", "coop-lives-fixture.py")


class Game(BaseGame):
    def text(self):
        # Automatic addon exec replaces nblood.log; the inherited stdout handle
        # preserves both startup and reconnect evidence across the same PID.
        return self.console()

    def wait(self, predicate, description, timeout=60):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            text = self.text()
            if predicate(text):
                return text
            require(self.process.poll() is None,
                    f"{self.name}: exited before {description}; see {self.path}")
            time.sleep(.05)
        raise Failure(f"{self.name}: timed out waiting for {description}; see {self.path}")


@contextmanager
def environment(values):
    old = {key: os.environ.get(key) for key in values}
    os.environ.update({key: str(value) for key, value in values.items()})
    try:
        yield
    finally:
        for key, value in old.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "nblood")
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--engine-data", type=Path, default=ROOT)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=75)
    parser.add_argument("--browser-ui", action="store_true",
                        help="run only populated/full browser and protected menu-join checks")
    args = parser.parse_args()
    for name in ("binary", "assets", "engine_data"):
        setattr(args, name, getattr(args, name).resolve())
    require(os.environ.get("DISPLAY"), "graphical overlay tests require DISPLAY (use Xvfb)")
    require(args.binary.is_file() and args.assets.is_dir(), "build game and supply Blood 1.21 assets")
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="netblood-zerotier-gameplay-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print(f"ZeroTier gameplay evidence: {root}", flush=True)
    source_binary = args.binary
    args.binary = root / "nblood-tested"
    shutil.copy2(source_binary, args.binary)
    # Copy rather than symlink: companion lookup follows the real executable.
    helper = root / "netblood-zerotier"
    shutil.copy2(Path(__file__).with_name("zerotier-service-helper.py"), helper)
    helper.chmod(0o700)
    library = build_driver(root)
    result = dict(passed=False, binary=str(source_binary),
                  binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                  network="SDK companion test double over lossy loopback UDP; no external controller",
                  phases={}, failures=[])
    games, all_games = [], []
    node = 0

    def launch(name, host, files=None, options=None, password=None, browser=False, max_players=8):
        nonlocal node
        node += 1
        address = f"10.9.1.{node}"
        profile = root / (name + "-profile")
        values = {"NETBLOOD_ZT_TEST_ROOT": root / "overlay-bus",
                  "NETBLOOD_ZT_TEST_ADDRESS": address,
                  "HOME": profile / "home", "XDG_DATA_HOME": profile / "data",
                  "XDG_CONFIG_HOME": profile / "config", "XDG_CACHE_HOME": profile / "cache"}
        for key in ("HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME"):
            Path(values[key]).mkdir(parents=True)
        cli = ["-zerotier", NETWORK]
        if not browser:
            cli += ["-server", str(max_players), "-gametype", "coop"] if host else ["-client", host_address]
        if password is not None:
            cli += ["-password", password]
        cli += options or []
        with environment(values):
            game = Game(name, args, root, library, PORT, initial_files=files, launch_arguments=cli)
        games.append(game)
        all_games.append(game)
        game.overlay_address = address
        return game

    def started(host):
        host.wait(lambda text: "NET server listening UDP" in text and
                  any(int(row["server_tick"]) >= 30 for row in records(text, "NET server_tick=")),
                  "running graphical overlay host", args.timeout)
        require("dedicated=0" in host.text(), f"{host.name}: did not use a graphical listen host")
        require("ZeroTier" in host.text(), f"{host.name}: no overlay startup evidence")

    def joined(client):
        client.wait(lambda text: "NET client map loaded " in text and "NET client slot=" in text,
                    "overlay hot-join snapshots", args.timeout)
        require("NET content verified" in client.text(), f"{client.name}: content not validated")
        require(client.process.poll() is None, f"{client.name}: exited after joining")

    def stop(game):
        game.stop()
        games.remove(game)

    try:
        if args.browser_ui:
            map_bytes = fixture.lives_map()
            alias = "ZT_BROWSER.MAP"
            host_address = "10.9.1.1"
            host = launch("browser-host", True, files={alias: map_bytes},
                          options=["-map", alias], password="crypt key", max_players=2)
            host_address = host.overlay_address
            started(host)
            filler = launch("browser-filler", False, password="crypt key")
            joined(filler)
            browser = launch("client-1", False, browser=True)
            browser.wait(lambda text: "Waiting for network players!" in text,
                         "browser client title menu", args.timeout)
            time.sleep(.7)
            focus_window(browser)
            for scan in (81, 40, 81, 40, 40):
                tap(browser, scan)  # Multiplayer -> ZeroTier -> Browse Games.
            time.sleep(2.5)  # Initial asynchronous network membership and query.
            full = capture_window(browser, root / "browser-full.png")
            connected_before = host.text().count("NET connected slot=")
            tap(browser, 40)  # Full hosts remain listed, with local feedback.
            time.sleep(.3)
            feedback = capture_window(browser, root / "browser-full-feedback.png")
            require(host.text().count("NET connected slot=") == connected_before,
                    "selecting a full browser row attempted gameplay admission")
            require("NET client slot=" not in browser.text(),
                    "full browser host unexpectedly admitted another player")
            stop(filler)
            host.wait(lambda text: "NET disconnected slot=1 players=1" in text,
                      "browser host slot becoming available", args.timeout)
            focus_window(browser)
            tap(browser, 62)  # F5 refresh.
            time.sleep(2)
            available = capture_window(browser, root / "browser-available.png")
            tap(browser, 40)  # Protected selection opens the password form.
            time.sleep(.2)
            password_form = capture_window(browser, root / "browser-password.png")
            type_text(browser, "crypt key")
            tap(browser, 40)  # Submit through ordinary menu editing/callback.
            joined(browser)
            installed = map_download.cached_copy(browser, map_bytes)
            require("NET map download complete" in browser.text(),
                    "browser-selected missing map was not transferred")
            browser.command_text("net_status")
            require("NET status role=client" in browser.text(), "browser selection did not reach gameplay")
            result["phases"]["populated-browser"] = dict(
                screenshots=dict(full=full, full_feedback=feedback, available=available, password=password_form),
                expected=dict(map="ZT_BROWSER", mode="Co-op", full_players="2/2", available_players="1/2",
                              protection="LOCKED", latency="MS"),
                protected_menu_join=True, map_cache=str(installed.relative_to(browser.path)))
            stop(browser)
            stop(host)
            result["passed"] = True
            print("PASS: full browser row, live capacity update, protected menu join and downloaded map", flush=True)
            return
        map_bytes = fixture.lives_map()
        alias = "ZT_ONLY.MAP"
        require(not any((directory / alias).exists() for directory in (args.assets, args.engine_data)),
                "server-only map alias is present on shared search path")
        host_address = "10.9.1.1"
        host = launch("map-host", True, files={alias: map_bytes},
                      options=["-map", alias], password="crypt key")
        host_address = host.overlay_address
        started(host)
        # These clients arrive after the host's authoritative world is ticking.
        for label, password, reason in (("missing-password", None, "Server password required"),
                                        ("wrong-password", "wrong key", "Incorrect server password")):
            rejected = launch(label, False, password=password)
            wait_rejection(rejected, reason, args.timeout)
            text = rejected.text()
            require(not any(marker in text for marker in
                            ("NET content verified", "NET map download started", "NET addon download started", "NET client slot=")),
                    f"{label}: received content/game state before authentication")
            result["phases"][label] = dict(reason=reason, content_sent=False)
            # Rejected clients have no live gameplay console binding.
            rejected.process.terminate()
            rejected.process.wait(timeout=10)
            stop(rejected)
        client = launch("client-1", False, password="crypt key")
        require(not (client.path / alias).exists(), "joining client already has server map")
        joined(client)
        require("NET map download started" in client.text() and "NET map download complete" in client.text(),
                "missing user map was not transferred over the overlay")
        installed = map_download.cached_copy(client, map_bytes)
        require(not (client.path / alias).exists(), "download escaped the private map cache")
        client.command_text("net_status")
        require("NET status role=client" in client.text(), "client cannot use ordinary gameplay console")
        host.command_text("net_status")
        require("NET status role=server" in host.text(), "graphical host stopped while client downloaded")
        roundtrips = records(client.text(), "NET snapshot roundtrip ")
        require(roundtrips and all(row["equal"] == "1" for row in roundtrips),
                "overlay snapshots did not install exact canonical state")
        result["phases"]["listen-hotjoin-map"] = dict(
            cache=str(installed.relative_to(client.path)), bytes=len(map_bytes),
            sha256=hashlib.sha256(installed.read_bytes()).hexdigest(),
            exact_snapshot_roundtrips=len(roundtrips), password_protected=True)
        print("PASS: graphical password admission and late user-map download", flush=True)
        stop(client)
        stop(host)

        # Advertise a real typed addon profile. The unmodified client downloads,
        # activates it, execs itself, restarts its companion and rejoins overlay.
        files = addon.fixtures()
        host = launch("addon-host", True, files=addon.inside("server-addon", files),
                      options=["-game_dir=server-addon", "-ini", "CUSTOM.INI", "-h", "addon.def", "-g", "addon.pk3"],
                      password="crypt key")
        host_address = host.overlay_address
        started(host)
        client = launch("addon-client", False, password="crypt key")
        pid = client.process.pid
        client.wait(lambda text: text.count("Started at ") >= 2,
                    "automatic overlay addon activation restart", args.timeout)
        joined(client)
        text = client.text()
        require("NET addon download started" in text and "NET addon download complete" in text,
                "missing addon was not downloaded")
        require(text.count("Started at ") == 2 and client.process.pid == pid,
                "addon activation did not restart the original game exactly once")
        require(text.count("NET ZeroTier:") >= 2,
                "automatic restart did not reconnect the overlay service")
        require("ADDON_ARCHIVE_SCRIPT_MUST_NOT_RUN" not in text,
                "downloaded addon executed a console startup script")
        directory = addon.verify_package(client, files)
        client.command_text("net_status")
        result["phases"]["addon-overlay-restart"] = dict(
            process_id=pid, starts=2, files=len(files),
            installed=str(directory.relative_to(client.path)),
            original_process_alive=client.process.poll() is None)
        print("PASS: missing addon download, automatic activation and overlay reconnect", flush=True)
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


if __name__ == "__main__":
    main()
