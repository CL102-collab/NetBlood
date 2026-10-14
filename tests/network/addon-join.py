#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise automatic addon activation through real UDP, URI and Discord joins.

Requires Blood 1.21 assets, the built executable and an X11 display. Fixtures
are generated maps, INI/DEF files and arbitrary resources; no commercial addon
or copyrighted asset is copied into the downloadable package. The ordinary
game performs discovery, transfer, verification, restart and reconnection.
"""

import argparse
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import time
import zipfile

from cinematics import focus_window, free_port
from password import tap, type_text
from relay import Relay
from run import Failure, build_driver, require
from scoreboard import Game as BaseGame


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


uri_join = module("addon_uri_join", "uri-join.py")
discord = module("addon_discord_presence", "discord-presence.py")


class RestartLog:
    """Console output survives exec; the game's per-process log can be replaced."""

    def text(self):
        return self.console()

    def wait(self, predicate, description, timeout=60):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            text = self.text()
            if predicate(text):
                return text
            require(self.process.poll() is None,
                    f"{self.name}: exited before {description}; see {self.path}")
            time.sleep(0.05)
        raise Failure(f"{self.name}: timed out waiting for {description}; see {self.path}")


class Game(RestartLog, BaseGame):
    pass


class UriGame(RestartLog, uri_join.UriGame):
    pass


def fixtures():
    fixture = module("addon_map_fixture", "coop-lives-fixture.py")
    archive = io.BytesIO()
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_STORED) as package:
        entry = zipfile.ZipInfo("NBPACK.DAT", date_time=(2020, 1, 1, 0, 0, 0))
        package.writestr(entry, b"Archive resource loaded through the selected addon.\n")
        # Resource archives must not become a source of local console commands.
        # Both normal and cold-URI config filenames are covered; the harness's
        # real local autoexec must still provide its ordinary input binding.
        for name in ("autoexec.cfg", "nblood_cvars.cfg", "nblood_settings.cfg",
                     "network_cvars.cfg", "network_settings.cfg"):
            entry = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
            package.writestr(entry, b"echo ADDON_ARCHIVE_SCRIPT_MUST_NOT_RUN\n")
    return {
        "CUSTOM.INI": (b"[General]\nTitle=Network Addon Fixture\n\n"
                       b"[Episode1]\nTitle=Addon Episode\nMap1=NBADDON\n\n"
                       b"[NBADDON]\nTitle=Automatic Addon Test\nEndingA=1\nEndingB=1\n"),
        "NBADDON.MAP": fixture.lives_map(),
        "addon.def": (b'include "definitions/resources.def"\n'),
        "definitions/resources.def": (b"rffdefineid NBLOOSE DAT 62001 SYSTEM\n"
                                      b"rffdefineid NBPACK DAT 62002 SYSTEM\n"),
        "NBLOOSE.DAT": bytes(range(256)) * 256,
        "addon.pk3": archive.getvalue(),
        # A default-prefix ART override also tests leaving the addon: retaining
        # its injected search path would silently change stock tile metadata.
        "TILES018.ART": struct.pack("<4I2HI", 1, 1, 6000, 6000, 1, 1, 0) + b"\x40",
    }


def inside(directory, files):
    return {str(Path(directory) / name): contents for name, contents in files.items()}


def cache_files(game):
    root = game.path / ".nblood-addon-cache"
    return {str(path.relative_to(game.path)): path.read_bytes()
            for path in root.rglob("*") if path.is_file()} if root.is_dir() else {}


def verify_package(game, expected):
    """Read installed bytes independently of transport and fingerprint logging."""
    candidates = list((game.path / ".nblood-addon-cache").glob("*/.netblood-profile"))
    matches = []
    for marker in candidates:
        directory = marker.parent
        if all((directory / name).is_file() and (directory / name).read_bytes() == data
               for name, data in expected.items()):
            matches.append(directory)
    require(len(matches) == 1, f"{game.name}: missing byte-exact complete addon cache: {candidates}")
    return matches[0]


def starts(game):
    return game.text().count("Started at ")


def download_started(game):
    return "NET addon download started" in game.text()


def quit_before_map(game):
    # The content-error overlay leaves the normal F10 route available. Escape
    # would open the main menu first, where that global shortcut is inactive.
    game.key(67, True)
    time.sleep(0.25)
    game.key(67, False)
    game.key(28, True)
    game.process.wait(timeout=10)
    game.stop()


def capture_notice(path):
    """Capture startup without focusing SDL's not-yet-mapped temporary window."""
    from PIL import Image
    subprocess.run(["import", "-window", "root", str(path)], check=True, timeout=10,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    image = Image.open(path).convert("RGB")
    pixels = list(image.get_flattened_data() if hasattr(image, "get_flattened_data") else image.getdata())
    return dict(path=str(path), width=image.width, height=image.height,
                black_fraction=sum(max(pixel) < 12 for pixel in pixels) / len(pixels),
                white_pixels=sum(min(pixel) > 160 and max(pixel) - min(pixel) < 12 for pixel in pixels))


def main():
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=repository / "nblood")
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--engine-data", type=Path, default=repository)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=75)
    args = parser.parse_args()
    for name in ("binary", "assets", "engine_data"):
        setattr(args, name, getattr(args, name).resolve())
    require(os.environ.get("DISPLAY"), "clients require DISPLAY (use an Xvfb wrapper)")
    require(args.binary.is_file() and os.access(args.binary, os.X_OK), "game executable missing")
    require(args.assets.is_dir(), "Blood 1.21 assets missing")
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="netblood-addon-join-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print(f"Addon join evidence: {root}", flush=True)
    source_binary = args.binary
    args.binary = root / "nblood-tested"
    shutil.copy2(source_binary, args.binary)
    library = build_driver(root)
    files = fixtures()
    result = dict(passed=False, source_binary=str(source_binary),
                  binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                  fixtures={name: dict(size=len(data), md5=hashlib.md5(data).hexdigest())
                            for name, data in files.items()}, phases={}, failures=[])
    games, relays, peers = [], [], []
    environment = os.environ.copy()
    for name in ("HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME"):
        directory = root / name.lower()
        directory.mkdir()
        environment[name] = str(directory)

    def server(name, password=None, addon=True):
        port = free_port()
        options = ["-dedicated", "-port", str(port), "-gametype", "coop", "-episode", "1"]
        initial = {}
        if addon:
            initial = inside("server-addon", files)
            options += ["-game_dir=server-addon", "-ini", "CUSTOM.INI",
                        "-h", "addon.def", "-g", "addon.pk3"]
        if password is not None:
            options += ["-password", password]
        game = Game(name, args, root, library, port, dedicated=True,
                    initial_files=initial, launch_arguments=options)
        games.append(game)
        game.wait(lambda text: "Dedicated server running:" in text and "NET server_tick=" in text,
                  "dedicated server", args.timeout)
        return game, port

    def client(name, port, initial=None):
        game = Game(name, args, root, library, port, initial_files=initial)
        games.append(game)
        return game

    def joined(game, restart=True, after=0, expected_starts=None):
        game.wait(lambda text: "NET client map loaded " in text[after:] and "NET client slot=" in text[after:],
                  "authoritative gameplay after addon activation", args.timeout)
        text = game.text()
        require("NET content verified" in text, f"{game.name}: joined without asset validation")
        expected_starts = expected_starts if expected_starts is not None else (2 if restart else 1)
        require(starts(game) == expected_starts,
                f"{game.name}: expected {expected_starts} starts, got {starts(game)}")
        if restart:
            require(re.search(r"(?i)(?:addon|mod).{0,160}restart|restart.{0,160}(?:addon|mod)", text),
                    f"{game.name}: did not notify the player about addon restart")
        require("ADDON_ARCHIVE_SCRIPT_MUST_NOT_RUN" not in text,
                f"{game.name}: addon archive executed a local configuration script")
        # A surviving original PID establishes actual exec/reconnect, not an
        # unrelated process left behind by a launcher that happened to exit 0.
        require(game.process.poll() is None, f"{game.name}: original process did not survive restart")
        game.command_text("net_status")
        require("NET status role=client" in game.text(), f"{game.name}: autoexec console binding was not preserved")

    def phase(name, data):
        result["phases"][name] = data
        print(f"PASS: {name}", flush=True)

    try:
        host, port = server("server")
        relay = Relay(("127.0.0.1", port), loss=0.04, delay_ms=20, jitter_ms=5, reorder=0.05, seed=83).start()
        relays.append(relay)
        missing = UriGame("cold uri install", args, root, library, environment,
                          f"netblood://127.0.0.1:{relay.port}")
        games.append(missing)
        original_autoexec = (missing.path / "autoexec.cfg").read_bytes()
        missing.wait(lambda text: "NET addon restart required" in text,
                     "automatic restart notification", args.timeout)
        notice = None
        notice_deadline = time.monotonic() + args.timeout
        while notice is None:
            require(starts(missing) == 1 and missing.process.poll() is None and
                    time.monotonic() < notice_deadline,
                    "automatic restart notification was not visibly displayed before restarting")
            picture = capture_notice(root / "restart-notice.png")
            if picture["black_fraction"] > 0.85 and picture["white_pixels"] > 100:
                notice = picture
            if notice is None:
                time.sleep(0.05)
        missing.wait(lambda text: text.count("Started at ") == 2,
                     "automatic exec before reconnect", args.timeout)
        # The ordinary impairment relay pins one source port. Exec closes that
        # socket; allow the restarted process's new source to use the same
        # public endpoint, without changing any game packet or acknowledgement.
        relay.client = None
        joined(missing)
        require(download_started(missing) and "NET addon download complete" in missing.text(),
                "missing addon did not complete a real transfer")
        package = verify_package(missing, files)
        require((missing.path / "autoexec.cfg").read_bytes() == original_autoexec,
                "activation overwrote the user's autoexec.cfg")
        require("NET map download started" not in missing.text(),
                "activated package failed to expose its included map")
        saved_cache = cache_files(missing)
        phase("cold-uri-download-restart", dict(cache=str(package), starts=starts(missing), notice=notice))
        missing.stop()

        installed_files = inside("addons/My Existing Addon", files)
        installed = client("installed-addon", port, installed_files)
        joined(installed)
        require(not download_started(installed), "matching installed addon downloaded again")
        require(all((installed.path / name).read_bytes() == data for name, data in installed_files.items()),
                "autoactivation modified installed addon files")
        phase("installed-addon-reused", dict(starts=starts(installed), transferred=False))
        installed.stop()

        corrupt_files = dict(saved_cache)
        corrupt_name = next(name for name in corrupt_files if name.endswith("/NBLOOSE.DAT"))
        corrupt = bytearray(corrupt_files[corrupt_name])
        corrupt[len(corrupt) // 2] ^= 0x80
        corrupt_files[corrupt_name] = bytes(corrupt)
        repaired = client("corrupted-cache", port, corrupt_files)
        joined(repaired)
        require(download_started(repaired), "corrupted cached addon was accepted without repair")
        package = verify_package(repaired, files)
        phase("corrupted-cache-repaired", dict(cache=str(package)))
        repaired.stop()

        wrong = dict(installed_files)
        wrong_ini = "addons/My Existing Addon/CUSTOM.INI"
        wrong[wrong_ini] = files["CUSTOM.INI"].replace(b"Automatic Addon Test", b"Different Addon Test")
        mismatch = client("different-installed-ini", port, wrong)
        joined(mismatch)
        require(download_started(mismatch), "different installed INI was treated as the requested addon")
        verify_package(mismatch, files)
        require((mismatch.path / wrong_ini).read_bytes() == wrong[wrong_ini], "download overwrote installed INI")
        phase("ini-mismatch-isolated", dict(original_preserved=True))
        mismatch.stop()

        private, private_port = server("private-server", password="addon test 123")
        password_client = client("password-client", private_port)
        password_client.wait(lambda text: "NET rejected:" in text and "password" in text.lower(),
                             "in-game password prompt", args.timeout)
        time.sleep(0.3)
        require(not download_started(password_client) and not cache_files(password_client),
                "unauthenticated client received addon files")
        focus_window(password_client)
        type_text(password_client, "addon test 123")
        tap(password_client, 40)  # Actual password menu submission.
        joined(password_client)
        verify_package(password_client, files)
        require(password_client.text().count("NET rejected:") == 1,
                "automatic restart lost an accepted password and prompted again")
        phase("password-before-transfer-survives-restart", dict(rejections=1))
        password_client.stop()
        private.stop()

        vanilla, vanilla_port = server("vanilla-server", addon=False)
        peer = discord.FakeDiscord(root / "discord")
        peers.append(peer)
        with discord.environment(peer.environment):
            invited = Game("discord-client", args, root, library,
                           launch_arguments=["-map", "E1M1"],
                           initial_files={"network_settings.cfg": discord.settings()})
        games.append(invited)
        invited.wait(lambda text: "> This map" in text, "offline Discord client", args.timeout)
        peer.wait(lambda row: row["value"].get("cmd") == "SUBSCRIBE" and
                  row["value"].get("evt") == "ACTIVITY_JOIN", "join subscription", args.timeout)
        peer.join(f"netblood://127.0.0.1:{port}")
        joined(invited)
        require(download_started(invited), "Discord callback did not obtain the required addon")
        activity = peer.activity(lambda value: value and "Automatic Addon Test" in value.get("details", "") and
                                 discord.secret(value), "activated addon presence", args.timeout)
        require(peer.connections >= 2, "Discord IPC did not reconnect after automatic restart")
        verify_package(invited, files)
        phase("discord-download-restart", dict(activity=activity, ipc_connections=peer.connections))

        # Switching away from an activated addon must remove its injected
        # resource paths and selections. Otherwise its loose resources can
        # shadow stock content after the next automatic restart.
        offset, mark = len(invited.text()), peer.mark()
        peer.join(f"netblood://127.0.0.1:{vanilla_port}")
        joined(invited, after=offset, expected_starts=3)
        stock_activity = peer.activity(lambda value: value and "Cradle" in value.get("details", "") and
                                       discord.secret(value), "stock presence after leaving addon", args.timeout, mark)
        require(peer.connections >= 3, "Discord IPC did not reconnect after switching back to stock resources")
        require("NET content verified" in invited.text()[offset:] and
                "NET addon download started" not in invited.text()[offset:],
                "switching back to installed stock resources did not verify without downloading")
        phase("discord-addon-to-stock", dict(activity=stock_activity, starts=starts(invited)))
        invited.stop()

        blocked = client("blocked-cache", port, {".nblood-addon-cache": b"cache directory blocked\n"})
        blocked.wait(lambda text: re.search(r"(?i)addon[^\n]*(?:failed|cannot|error)", text),
                     "actionable addon cache error", args.timeout)
        require(starts(blocked) == 1 and "NET client map loaded " not in blocked.text(),
                "cache write failure restarted or entered a level")
        require((blocked.path / ".nblood-addon-cache").read_bytes() == b"cache directory blocked\n",
                "addon download overwrote cache obstacle")
        phase("cache-failure-no-restart", dict(starts=starts(blocked)))
        quit_before_map(blocked)

        unchanged = client("vanilla-client", vanilla_port)
        joined(unchanged, restart=False)
        require(not download_started(unchanged) and not cache_files(unchanged), "vanilla join downloaded an addon")
        phase("vanilla-join-unchanged", dict(starts=starts(unchanged)))
        unchanged.stop()
        vanilla.stop()
        host.stop()
        result["passed"] = True
    except (Failure, OSError, subprocess.SubprocessError) as error:
        result["failures"].append(str(error))
        print(f"FAIL: {error}", flush=True)
    finally:
        for game in reversed(games):
            game.stop()
        for relay in relays:
            relay.close()
        for peer in peers:
            peer.close()
            result["failures"].extend(peer.errors)
        for game in games:
            if game.process.returncode != 0 or game.forced:
                result["failures"].append(f"{game.name}: unclean exit {game.process.returncode}")
            if re.search(r"AddressSanitizer|runtime error:|Segmentation fault|NET snapshot invalid", game.text()):
                result["failures"].append(f"{game.name}: runtime/snapshot error")
            if game.dedicated and "NETWORK_TEST_DEVICES windows=0 audio=0" not in game.console():
                result["failures"].append(f"{game.name}: missing headless device audit")
        result["passed"] = result["passed"] and not result["failures"]
        result["games"] = {game.name: dict(game.result(), starts=starts(game)) for game in games}
        result["relays"] = [relay.report() for relay in relays]
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(dict(passed=result["passed"], failures=result["failures"], evidence=str(root))), flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
