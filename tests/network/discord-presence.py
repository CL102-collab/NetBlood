#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise Discord presence and invite joins against a private fake local IPC.

Requires Blood 1.21 assets and X11/Xvfb; no Discord installation, account, token
or real Application ID is used. The fake desktop Discord speaks the actual IPC
framing, while clients join a real native dedicated server over UDP/IPv6.
All HOME/XDG registration and configuration are isolated from the user's files.
"""

import argparse
import ctypes
from contextlib import contextmanager
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time

from cinematics import free_port
from run import Failure, build_driver, records, require
from scoreboard import Game, expect


APPLICATION_ID = "123456789012345678"
FULL_LOOPBACK = "0000:0000:0000:0000:0000:0000:0000:0001"


@contextmanager
def environment(values):
    old = {key: os.environ.get(key) for key in values}
    os.environ.update(values)
    try:
        yield
    finally:
        for key, value in old.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


class FakeDiscord:
    """A local Discord peer, not a replacement for the game's RPC transport."""

    def __init__(self, base):
        self.temporary = tempfile.TemporaryDirectory(prefix="nb-discord-")
        self.runtime = Path(self.temporary.name)
        os.chmod(self.runtime, 0o700)
        self.environment = dict(HOME=str(base / "home"), XDG_RUNTIME_DIR=str(self.runtime),
                                XDG_CONFIG_HOME=str(base / "config"),
                                XDG_DATA_HOME=str(base / "data"), TMPDIR=str(self.runtime))
        for key in ("HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME"):
            Path(self.environment[key]).mkdir(parents=True)
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.bind(str(self.runtime / "discord-ipc-0"))
        self.socket.listen(4)
        self.socket.settimeout(0.1)
        self.lock = threading.Lock()
        self.send_lock = threading.Lock()
        self.stopped = threading.Event()
        self.messages = []
        self.errors = []
        self.connection = None
        self.connections = 0
        self.disconnections = 0
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def mark(self):
        with self.lock:
            return len(self.messages)

    def rows(self, after=0):
        with self.lock:
            return list(self.messages[after:])

    def send(self, opcode, value):
        payload = json.dumps(value, separators=(",", ":")).encode()
        frame = struct.pack("<II", opcode, len(payload)) + payload
        with self.send_lock:
            require(self.connection is not None, "Discord IPC is not connected")
            self.connection.sendall(frame)

    def dispatch(self, event, data):
        self.send(1, dict(cmd="DISPATCH", evt=event, data=data))

    def join(self, secret):
        self.dispatch("ACTIVITY_JOIN", dict(secret=secret))

    def run(self):
        try:
            while not self.stopped.is_set():
                try:
                    connection, _ = self.socket.accept()
                except socket.timeout:
                    continue
                connection.settimeout(0.1)
                with self.send_lock:
                    self.connection = connection
                    self.connections += 1
                buffer = bytearray()
                try:
                    while not self.stopped.is_set():
                        try:
                            data = connection.recv(16384)
                        except socket.timeout:
                            continue
                        if not data:
                            break
                        buffer += data
                        while len(buffer) >= 8:
                            opcode, length = struct.unpack_from("<II", buffer)
                            require(length <= 65536, f"unbounded Discord frame: {length}")
                            if len(buffer) < length + 8:
                                break
                            raw = bytes(buffer[8:8 + length])
                            del buffer[:8 + length]
                            value = json.loads(raw.decode())
                            with self.lock:
                                self.messages.append(dict(opcode=opcode, value=value,
                                                          received=time.monotonic()))
                            if opcode == 0:
                                require(value.get("v") == 1, "incorrect Discord IPC version")
                                require(value.get("client_id") == APPLICATION_ID,
                                        "incorrect Discord Application ID")
                                self.dispatch("READY", dict(v=1, config={},
                                                            user=dict(id="100000000000000001", username="Fixture")))
                            elif opcode == 1 and value.get("nonce"):
                                # Discord responses carry the matching command and
                                # nonce; only DISPATCH messages are unsolicited events.
                                self.send(1, dict(cmd=value["cmd"], nonce=value["nonce"],
                                                  evt=None, data=value.get("args", {})))
                except (BrokenPipeError, ConnectionResetError):
                    # Shutdown may send a final clear and close immediately;
                    # Discord's acknowledgement is not required for that close.
                    pass
                finally:
                    with self.send_lock:
                        self.connection = None
                        self.disconnections += 1
                    connection.close()
        except Exception as error:
            if not self.stopped.is_set():
                self.errors.append(f"{type(error).__name__}: {error}")

    def wait(self, predicate, description, timeout=45, after=0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            require(not self.errors, f"fake Discord failed: {self.errors}")
            for row in self.rows(after):
                if predicate(row):
                    return row
            time.sleep(0.05)
        raise Failure(f"timed out waiting for Discord {description}; last messages={self.rows()[-5:]}")

    def activity(self, predicate, description, timeout=45, after=0):
        row = self.wait(lambda row: row["opcode"] == 1 and
                        row["value"].get("cmd") == "SET_ACTIVITY" and
                        predicate(row["value"].get("args", {}).get("activity")),
                        description, timeout, after)
        return row["value"]["args"].get("activity")

    def close(self):
        self.stopped.set()
        self.thread.join(timeout=2)
        self.socket.close()
        self.temporary.cleanup()


def settings(extra=""):
    return (f'discord_appid "{APPLICATION_ID}"\ndiscord_enabled 1\n' + extra).encode()


def joined(game, timeout, after=0):
    game.wait(lambda text: "NET client map loaded " in text[after:] and "NET client slot=" in text[after:],
              "first authoritative snapshot", timeout)


def secret(activity):
    return activity.get("secrets", {}).get("join") if activity else None


class DesktopGame(Game):
    """Adopt a real GIO-launched portable installation, with no game CLI flags."""

    def __init__(self, name, directory, library, peer, uri):
        self.name, self.path = name, directory
        self.fifo = self.path / "input.fifo"
        os.mkfifo(self.fifo)
        self.events = os.open(self.fifo, os.O_RDWR | os.O_NONBLOCK)
        self.command_serial = 0
        self.dedicated = self.forced = False
        self.log = (directory / "console.log").open("w")
        env = dict(os.environ, **peer.environment, NETWORK_TEST_EVENTS=str(self.fifo),
                   SDL_AUDIODRIVER="dummy", LD_PRELOAD=str(library))
        if os.environ.get("LD_PRELOAD"):
            env["LD_PRELOAD"] += ":" + os.environ["LD_PRELOAD"]
        self.command = ["gio", "open", uri]
        dispatched = subprocess.run(self.command, cwd="/", env=env, stdout=self.log,
                                    stderr=subprocess.STDOUT, timeout=15)
        require(dispatched.returncode == 0, "GIO rejected registered Discord launch handler")
        executable = directory / "nblood"
        deadline, pid = time.monotonic() + 10, None
        while pid is None and time.monotonic() < deadline:
            for candidate in Path("/proc").glob("[0-9]*/exe"):
                try:
                    if candidate.resolve() == executable:
                        pid = int(candidate.parent.name)
                        break
                except (FileNotFoundError, PermissionError):
                    pass
            if pid is None:
                time.sleep(0.05)
        require(pid is not None, "Discord desktop scheme did not launch the game")
        spec = importlib.util.spec_from_file_location("uri_join", Path(__file__).with_name("uri-join.py"))
        uri_join = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(uri_join)
        self.process = uri_join.AdoptedProcess(pid)


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
    require(shutil.which("gio") and shutil.which("xdg-mime"), "install gio and xdg-utils")
    require(ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0) == 0,
            "could not enable Linux child subreaper for desktop launch")
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="netblood-discord-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print(f"Discord presence evidence: {root}", flush=True)
    source_binary = args.binary
    args.binary = root / "nblood-tested"
    shutil.copy2(source_binary, args.binary)
    library = build_driver(root)
    result = dict(passed=False, source_binary=str(source_binary),
                  binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                  phases={}, failures=[])
    games, peers = [], {}

    def launch(name, arguments, dedicated=False, configure=True, extra=""):
        peer = FakeDiscord(root / (name + "-discord"))
        peers[name] = peer
        initial = {"network_settings.cfg": settings(extra)} if configure else {}
        with environment(peer.environment):
            game = Game(name, args, root, library, dedicated=dedicated,
                        launch_arguments=arguments, initial_files=initial)
        games.append(game)
        return game, peer

    try:
        port = free_port()
        server, dedicated_peer = launch("server", ["-dedicated", "-port", str(port),
                                      "-gametype", "bloodbath", "-maxplayers", "2", "-map", "E1M1"],
                                      dedicated=True)
        server.wait(lambda text: "Dedicated server running:" in text and "NET server_tick=" in text,
                    "dedicated server", args.timeout)
        client, peer = launch("client-1", ["-map", "E1M1", "-pname", "DiscordAlice"])
        client.wait(lambda text: "> This map" in text, "offline initial map", args.timeout)
        offline = peer.activity(lambda activity: activity and "Single" in activity.get("details", ""),
                                "single-player activity", args.timeout)
        require(not secret(offline), "offline activity offered a multiplayer join")
        started = offline.get("timestamps", {}).get("start")
        require(isinstance(started, int) and abs(started - time.time()) < 120,
                f"activity has no credible elapsed-play timestamp: {offline}")
        peer.wait(lambda row: row["value"].get("cmd") == "SUBSCRIBE" and
                  row["value"].get("evt") == "ACTIVITY_JOIN", "join subscription", args.timeout)
        result["phases"]["offline"] = offline

        # The longest complete IPv6 text is accepted through Discord callbacks,
        # not just by the parser unit test or the normal command-line URI path.
        offset, mark = len(client.text()), peer.mark()
        uri = f"netblood://[{FULL_LOOPBACK}]:{port}"
        peer.join(uri)
        joined(client, args.timeout)
        require(f"{FULL_LOOPBACK}:{port}" in client.text()[offset:] or
                f"[{FULL_LOOPBACK}]:{port}" in client.text()[offset:],
                "Discord join truncated or replaced the requested IPv6 address")
        activity = peer.activity(lambda activity: secret(activity) is not None and
                                 "Bloodbath" in activity.get("details", ""),
                                 "joined Bloodbath activity", args.timeout, mark)
        require("Cradle" in activity.get("details", ""), f"wrong authoritative map: {activity}")
        require(str(port) in activity.get("state", "") and ":" in activity.get("state", ""),
                f"server endpoint missing from visible state: {activity}")
        require(secret(activity) in (uri, f"netblood://[::1]:{port}"),
                f"join secret lost its server destination: {activity}")
        require(activity.get("party", {}).get("size") == [1, 2],
                f"incorrect authoritative party occupancy/capacity: {activity}")
        require(activity.get("timestamps", {}).get("start") == started,
                "elapsed-play time restarted after joining a server")
        party_id = activity["party"].get("id")
        require(party_id, "joinable activity has no Discord party ID")
        result["phases"]["ipv6-join"] = activity
        print("PASS: actual Discord callback joins full IPv6; authoritative map/mode/address/party published", flush=True)

        mark = peer.mark()
        guest, guest_peer = launch("guest", ["-client", "127.0.0.1", "-port", str(port),
                                             "-pname", "DiscordBob"], configure=False)
        joined(guest, args.timeout)
        expect(server, lambda rows: len(rows) == 2, "second authoritative player", args.timeout)
        two = peer.activity(lambda value: value and value.get("party", {}).get("size") == [2, 2],
                            "updated two-player party", args.timeout, mark)
        require(two["party"].get("id") == party_id, "party ID changed for a new player")
        require(two.get("timestamps", {}).get("start") == started, "party update reset elapsed play")
        require(not secret(two), "full server still offers a Discord join")
        result["phases"]["party-update"] = two

        mark = peer.mark()
        guest.stop()
        available = peer.activity(lambda value: value and value.get("party", {}).get("size") == [1, 2]
                                  and secret(value), "join restored after a player leaves", args.timeout, mark)
        result["phases"]["full-server-invite-removed-and-restored"] = available

        mark = peer.mark()
        server.command_text("map E1M2")
        server.wait(lambda text: "NET map generation=2 " in text, "second map generation", args.timeout)
        changed = peer.activity(lambda value: value and "Wrong Side" in value.get("details", "") and
                                secret(value), "authoritative map change", args.timeout, mark)
        require(changed.get("timestamps", {}).get("start") == started, "map change reset elapsed play")
        require(changed["party"].get("id") == party_id, "map change replaced server party identity")
        result["phases"]["map-change"] = changed
        require(dedicated_peer.connections == 0, "dedicated server connected to desktop Discord")
        require(guest_peer.connections == 0, "unconfigured application contacted Discord")
        result["phases"]["dedicated-and-unconfigured-no-ipc"] = True
        print("PASS: player count/map changes keep elapsed time; dedicated/unconfigured games never use IPC", flush=True)

        # Packet demo metadata contains authoritative maps/modes but must never
        # retain the live session's public join address or invite secret.
        client.command_text("net_record discord.nbpd")
        time.sleep(3)
        client.command_text("net_stoprecord")
        mark = peer.mark()
        client.command_text("net_disconnect\nmap E1M1")
        disconnected = peer.activity(lambda value: value is not None and not secret(value) and
                                     not value.get("party"), "cleared multiplayer invite after disconnect",
                                     args.timeout, mark)
        result["phases"]["disconnect"] = disconnected
        mark = peer.mark()
        client.command_text("net_playback discord.nbpd")
        playback = peer.activity(lambda value: value and "demo" in json.dumps(value).lower() and
                                 not secret(value) and not value.get("party"),
                                 "non-joinable packet demo", args.timeout, mark)
        result["phases"]["packet-demo"] = playback
        client.command_text("net_disconnect\nmap E1M1")

        offset = len(client.text())
        before = sorted(str(path.relative_to(root)) for path in root.rglob("*.dem"))
        for malformed in ("netblood://127.0.0.1:0", "netblood://example.org/path",
                          'netblood://127.0.0.1:23513 -r INJECTED.dem',
                          'netblood://127.0.0.1:23513\";quit', "https://example.org/"):
            peer.join(malformed)
            time.sleep(0.2)
        time.sleep(1)
        require("NET connecting UDP" not in client.text()[offset:], "malformed Discord secret started connection")
        require(before == sorted(str(path.relative_to(root)) for path in root.rglob("*.dem")),
                "malformed Discord secret created a file")
        result["phases"]["malformed-secrets-rejected"] = True

        mark = peer.mark()
        client.command_text("discord_enabled 0")
        peer.activity(lambda value: value is None, "clear activity on opt-out", args.timeout, mark)
        deadline = time.monotonic() + args.timeout
        while peer.connection is not None and time.monotonic() < deadline:
            time.sleep(0.05)
        require(peer.connection is None, "disabled integration left Discord IPC open")
        connections = peer.connections
        time.sleep(2)
        require(peer.connections == connections, "disabled integration reconnected to Discord")
        result["phases"]["opt-out-clears-and-closes"] = True
        print("PASS: disconnect/demos remove invites; malformed secrets ignored; opt-out clears and closes IPC", flush=True)

        # Create a realistic portable installation. Initial activation registers
        # this exact executable; accepting an invite while it is closed must
        # start it from an unrelated desktop working directory, then receive
        # the actual destination through IPC (not from the launch URI).
        installation = root / "Portable Discord Game"
        installation.mkdir()
        executable = installation / "nblood"
        shutil.copy2(args.binary, executable)
        for source in args.assets.iterdir():
            if source.is_file():
                (installation / source.name).symlink_to(source)
        (installation / "nblood.pk3").symlink_to(args.engine_data / "nblood.pk3")
        (installation / "user_profiles_disabled").touch()
        (installation / "nblood.cfg").write_text(
            "[Setup]\nForceSetup = 0\nNoAutoLoad = 1\n\n[Screen Setup]\n"
            "ScreenMode = 0\nScreenBPP = 8\nRenderer = 0\nScreenWidth = 640\nScreenHeight = 480\n")
        (installation / "autoexec.cfg").write_text(
            'r_maxfps 60\nbind I norepeat "exec command.cfg"\n')
        tested_binary, args.binary = args.binary, executable
        try:
            warm, portable_peer = launch("portable-registration", ["-map", "E1M1"])
        finally:
            args.binary = tested_binary
        warm.wait(lambda text: "DISCORD launch handler registered application=" in text,
                  "automatic Discord desktop registration", args.timeout)
        warm.wait(lambda text: "> This map" in text, "portable registration game map", args.timeout)
        portable_peer.activity(lambda value: value is not None, "portable registration activity", args.timeout)
        with environment(portable_peer.environment):
            handler = subprocess.check_output(["xdg-mime", "query", "default",
                         f"x-scheme-handler/discord-{APPLICATION_ID}"], text=True).strip()
        require(handler == f"netblood-discord-{APPLICATION_ID}.desktop", f"wrong registered Discord handler: {handler}")
        warm.stop()
        mark = portable_peer.mark()
        cold = DesktopGame("desktop-invite", installation, library, portable_peer,
                           f"discord-{APPLICATION_ID}://invite?ignored=netblood://127.0.0.1:1")
        games.append(cold)
        portable_peer.activity(lambda value: value and value.get("details") == "In the main menu",
                               "cold-launched game ready for invitation", args.timeout, mark)
        require("NET connecting UDP" not in cold.text(), "Discord launch URI became a server destination")
        mark = portable_peer.mark()
        portable_peer.join(uri)
        joined(cold, args.timeout)
        invited = portable_peer.activity(lambda value: secret(value) and
                                         "Bloodbath" in value.get("details", ""),
                                         "cold invite joined live server", args.timeout, mark)
        result["phases"]["desktop-cold-invite"] = invited
        print("PASS: registered Discord scheme starts a closed portable game and its queued invite joins UDP server", flush=True)

        # A listen host knows its reachable DNS address only through the
        # explicit setting. Clients publish resolved numeric addresses; both
        # forms must still identify the same Discord party/server session.
        listen_port = free_port()
        listen_uri = f"netblood://localhost:{listen_port}"
        host, host_peer = launch("listen-host", ["-server", "3", "-port", str(listen_port),
                                                 "-gametype", "bloodbath", "-map", "E1M1",
                                                 "-pname", "DiscordHost"],
                                 extra=f'discord_server_address "{listen_uri}"\n')
        host.wait(lambda text: "NET server_tick=" in text, "graphical listen host", args.timeout)
        hosted = host_peer.activity(lambda value: secret(value) == listen_uri,
                                    "configured listen-host invite", args.timeout)
        require(hosted.get("state") == f"localhost:{listen_port}",
                f"listen host lost its explicitly advertised address: {hosted}")
        require(hosted.get("party", {}).get("size") == [1, 3],
                f"incorrect listen-host party occupancy/capacity: {hosted}")
        host_mark, client_mark = host_peer.mark(), portable_peer.mark()
        offset = len(cold.text())
        portable_peer.join(secret(hosted))
        joined(cold, args.timeout, offset)
        expect(host, lambda rows: len(rows) == 2, "invited client on the listen host", args.timeout)
        host_party = host_peer.activity(lambda value: value and value.get("party", {}).get("size") == [2, 3],
                                       "two-player listen-host presence", args.timeout, host_mark)
        client_party = portable_peer.activity(lambda value: value and value.get("party", {}).get("size") == [2, 3]
                                             and str(listen_port) in value.get("state", ""),
                                             "invited client's listen-server presence", args.timeout, client_mark)
        require(host_party["party"]["id"] == client_party["party"]["id"],
                f"DNS host and numeric client disagree about party identity: {host_party}; {client_party}")
        require(secret(host_party) == listen_uri and secret(client_party)
                and "localhost" not in secret(client_party),
                "listen-host override did not remain independent from the client's actual numeric endpoint")
        require(secret(client_party) in (f"netblood://127.0.0.1:{listen_port}",
                                        f"netblood://[::1]:{listen_port}"),
                f"client invite lost its resolved listen-server endpoint: {client_party}")
        require(client_party["party"]["id"] != invited["party"]["id"],
                "switching servers retained the previous server's party identity")
        result["phases"]["listen-host-dns-client-numeric-party"] = dict(host=host_party, client=client_party)
        print("PASS: listen-host DNS invite joins its server; host and numeric-address client share one party", flush=True)
        # The normal in-game quit binding is unavailable on the disconnected
        # screen. Quit this client before shutting down its listen host.
        cold.stop()
        result["passed"] = True
    except Exception as error:
        result["failures"].append(f"{type(error).__name__}: {error}")
        print(f"FAIL: {type(error).__name__}: {error}", flush=True)
    finally:
        for game in reversed(games):
            game.stop()
        result["games"] = {game.name: game.result() for game in games}
        for name, peer in peers.items():
            (root / f"{name}-ipc.json").write_text(json.dumps(peer.rows(), indent=2) + "\n")
            if peer.errors:
                result["failures"].append(f"{name}: fake Discord errors: {peer.errors}")
            peer.close()
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
