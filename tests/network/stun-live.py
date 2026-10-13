#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise STUN discovery and advertised endpoints with real game processes.

Requires Blood 1.21 assets and X11/Xvfb. Every host disables public STUN unless
explicitly pointed at the local fixture. The fixture speaks RFC 5389 Binding
over real UDP and can report a synthetic external mapping; no internet, relay,
rendezvous infrastructure, firewall changes or Discord accounts are required.
"""

import argparse
import hashlib
import importlib.util
import ipaddress
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

from cinematics import focus_window, free_port
from run import Failure, build_driver, inspect_demo, play_demo, records, require
from scoreboard import Game


spec = importlib.util.spec_from_file_location("discord_presence_fixture", Path(__file__).with_name("discord-presence.py"))
discord = importlib.util.module_from_spec(spec)
spec.loader.exec_module(discord)
COOKIE = 0x2112A442


class FakeStun:
    """A local Binding responder; optional mapped address models NAT output."""

    def __init__(self, mapped=None, respond=True):
        self.mapped, self.respond = mapped, respond
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(("127.0.0.1", 0))
        self.socket.settimeout(0.1)
        self.port = self.socket.getsockname()[1]
        self.requests, self.errors = [], []
        self.stopped = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        try:
            while not self.stopped.is_set():
                try:
                    data, source = self.socket.recvfrom(2048)
                except socket.timeout:
                    continue
                require(len(data) >= 20, "truncated STUN Binding request")
                kind, size, cookie, transaction = struct.unpack_from("!HHI12s", data)
                require(kind == 1 and cookie == COOKIE and size % 4 == 0 and size == len(data) - 20,
                        "invalid STUN Binding request header")
                require(any(transaction), "empty STUN transaction ID")
                self.requests.append(dict(source=list(source), transaction=transaction.hex(), bytes=len(data)))
                if not self.respond:
                    continue
                mapped = self.mapped or source
                host = ipaddress.ip_address(mapped[0])
                mask = struct.pack("!I", COOKIE) + transaction
                value = struct.pack("!BBH", 0, 1 if host.version == 4 else 2, mapped[1] ^ (COOKIE >> 16))
                value += bytes(byte ^ mask[index] for index, byte in enumerate(host.packed))
                attribute = struct.pack("!HH", 0x0020, len(value)) + value
                response = struct.pack("!HHI12s", 0x0101, len(attribute), COOKIE, transaction) + attribute
                self.socket.sendto(response, source)
        except Exception as error:
            if not self.stopped.is_set():
                self.errors.append(f"{type(error).__name__}: {error}")

    def close(self):
        self.stopped.set()
        self.thread.join(timeout=2)
        self.socket.close()


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
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="netblood-stun-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print(f"STUN game evidence: {root}", flush=True)
    original = args.binary
    args.binary = root / "nblood-tested"
    shutil.copy2(original, args.binary)
    library = build_driver(root)
    dns_library = root / "stun-dns-driver.so"
    subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
        str(Path(__file__).with_name("stun-dns-driver.c")), "-o", str(dns_library), "-ldl", "-pthread"], check=True)
    result = dict(passed=False, binary=str(original),
                  binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                  phases={}, failures=[])
    games, peers, fixtures = [], {}, []

    def launch(name, arguments, dedicated=False, presence=False, extra="", preload=None):
        peer = discord.FakeDiscord(root / (name + "-discord"))
        peers[name] = peer
        settings = discord.settings(extra) if presence else extra.encode()
        environment = dict(peer.environment)
        if preload:
            environment["LD_PRELOAD"] = str(preload) + (":" + os.environ["LD_PRELOAD"] if os.environ.get("LD_PRELOAD") else "")
        with discord.environment(environment):
            game = Game(name, args, root, library, dedicated=dedicated,
                        launch_arguments=["-stunserver", "off"] + arguments,
                        initial_files={"network_settings.cfg": settings})
        games.append(game)
        return game, peer

    def dedicated(name, options, extra="", preload=None):
        port = free_port()
        game, _ = launch(name, ["-dedicated", "-port", str(port), "-gametype", "bloodbath",
                               "-maxplayers", "3", "-map", "E1M1"] + options, dedicated=True, extra=extra, preload=preload)
        game.wait(lambda text: "Dedicated server running:" in text and "NET server_tick=" in text,
                  "dedicated server", args.timeout)
        return game, port

    def fixture(**kwargs):
        server = FakeStun(**kwargs)
        fixtures.append(server)
        return server

    def status(game):
        game.command_text("net_stun_status")
        return game.text()

    try:
        invalid = []
        for index, value in enumerate(("127.0.0.1:0", "127.0.0.1:65536", "netblood://user@example.test:3478")):
            directory = root / f"invalid-stun-{index}"
            directory.mkdir()
            env = dict(os.environ, SDL_VIDEODRIVER="network-test-invalid-video-driver",
                       SDL_AUDIODRIVER="network-test-invalid-audio-driver")
            for variable in ("DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY"):
                env.pop(variable, None)
            completed = subprocess.run([str(args.binary), "-usecwd", "-nosetup", "-noautoload",
                "-quick", "-nodemo", "-dedicated", f"-j={args.assets}", f"-j={args.engine_data}",
                "-stunserver", value], cwd=directory, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=args.timeout)
            (directory / "console.log").write_text(completed.stdout)
            log = directory / "nblood.log"
            text = completed.stdout + (log.read_text(errors="replace") if log.exists() else "")
            require(completed.returncode != 0, f"invalid STUN endpoint unexpectedly started a game: {value}")
            diagnostics = [line for line in text.splitlines()
                           if re.search(r"\b(?:ERR|ERROR|FATAL)\||\.cpp\(\d+\):", line)]
            require(any("-stunserver" in line for line in diagnostics),
                    f"invalid STUN endpoint had no useful option diagnostic: {diagnostics}")
            invalid.append(dict(value=value, diagnostics=diagnostics))
        result["phases"]["invalid-startup-diagnostics"] = invalid

        echo = fixture()
        host, port = dedicated("stun-server", ["-stunserver", f"netblood://127.0.0.1:{echo.port}"])
        host.wait(lambda text: "STUN mapping discovered" in text, "same-socket STUN discovery", args.timeout)
        text = status(host)
        require(echo.requests and all(row["source"][1] == port for row in echo.requests),
                "STUN queried a temporary socket instead of the actual bound game socket")
        require(f"127.0.0.1:{port}" in text and "reachability" in text.lower(),
                "STUN status omitted the observed endpoint or its reachability limitation")
        client, _ = launch("direct-client", ["-client", "127.0.0.1", "-port", str(port)])
        discord.joined(client, args.timeout)
        result["phases"]["same-game-socket"] = dict(game_port=port, requests=list(echo.requests))
        client.stop()
        host.stop()
        print("PASS: URI-prefixed STUN endpoint, same bound game socket and normal direct connection", flush=True)

        mapped = fixture(mapped=("203.0.113.17", 41123))
        port = free_port()
        host, host_peer = launch("mapped-stun-host", ["-server", "3", "-port", str(port),
            "-gametype", "bloodbath", "-map", "E1M1", "-stunserver", f"127.0.0.1:{mapped.port}"], presence=True)
        host.wait(lambda text: "NET server_tick=" in text, "graphical listen host", args.timeout)
        host.wait(lambda text: "STUN mapping discovered" in text, "synthetic external STUN mapping", args.timeout)
        text = status(host)
        require("203.0.113.17:41123" in text, "server did not display its discovered public endpoint")
        require(mapped.requests and all(row["source"][1] == port for row in mapped.requests),
                "mapped endpoint discovery used a different UDP socket")
        lan, peer = launch("client-1", ["-client", "127.0.0.1", "-port", str(port)], presence=True)
        discord.joined(lan, args.timeout)
        public_uri = "netblood://203.0.113.17:41123"
        advertised = peer.activity(lambda value: discord.secret(value) == public_uri,
            "server's discovered public endpoint on a LAN client", args.timeout)
        hosted = host_peer.activity(lambda value: discord.secret(value) == public_uri and
            value.get("party", {}).get("size") == [2, 3], "listen host's discovered public endpoint", args.timeout)
        require(advertised.get("party", {}).get("size") == [2, 3], "advertised server capacity is incorrect")
        require(hosted["party"]["id"] == advertised["party"]["id"], "host and LAN client disagree about party identity")
        mark = peer.mark()
        host.command_text("map E1M2")
        lan.wait(lambda text: len(records(text, "NET client map loaded ")) >= 2,
                 "authoritative map change", args.timeout)
        changed = peer.activity(lambda value: discord.secret(value) == public_uri and
            "Wrong Side" in value.get("details", ""), "public endpoint after map change", args.timeout, mark)
        require(changed["party"]["id"] == advertised["party"]["id"], "map change replaced server party identity")
        lan.command_text("net_record stun.nbpd")
        time.sleep(3)
        lan.command_text("net_stoprecord")
        demo_path = lan.path / "stun.nbpd"
        demo = inspect_demo(demo_path)
        require(public_uri.encode() not in demo_path.read_bytes(), "live public invitation leaked into packet recording")
        replay = play_demo("stun-playback", demo, args, root, library, games, existing=lan)
        result["phases"]["public-advertisement-map-and-demo"] = dict(activity=advertised, host_activity=hosted,
            changed_activity=changed, requests=list(mapped.requests), demo=demo, replay=replay)
        host.stop()
        print("PASS: LAN client advertises the discovered endpoint; map changes and packet replay remain valid", flush=True)

        silent = fixture(respond=False)
        host, port = dedicated("timeout-server", ["-stunserver", f"127.0.0.1:{silent.port}"])
        client, _ = launch("timeout-direct-client", ["-client", "127.0.0.1", "-port", str(port)])
        discord.joined(client, args.timeout)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            text = status(host)
            if re.search(r"STUN[^\n]*(?:timed out|timeout|did not answer)", text, re.IGNORECASE):
                break
            time.sleep(1)
        else:
            raise Failure("unavailable STUN server produced no meaningful timeout diagnostic")
        require(silent.requests, "timeout fixture received no actual STUN requests")
        offset = len(client.text())
        client.command_text("net_status")
        states = records(client.text()[offset:], "NET status ")
        require(states and int(states[-1].get("snapshot", "0")) > 30,
                "direct connection stopped receiving snapshots while STUN was unavailable")
        result["phases"]["timeout-direct-connection"] = dict(status=states[-1], requests=list(silent.requests))
        client.stop()
        host.stop()

        # Only this reserved fixture hostname is intercepted. It fails once,
        # then resolves locally after the production thirty-second DNS retry.
        # Neither resolution reaches a public DNS server.
        dns = fixture()
        host, port = dedicated("dns-retry-server", ["-stunserver", f"stun-fixture.invalid:{dns.port}"],
                               preload=dns_library)
        host.wait(lambda text: "STUN DNS FIXTURE: first lookup failed locally" in host.console(),
                  "initial local DNS failure", args.timeout)
        require(not dns.requests, "STUN sent a request before the fixture hostname resolved")
        client, _ = launch("dns-retry-client", ["-client", "127.0.0.1", "-port", str(port)])
        discord.joined(client, args.timeout)
        host.wait(lambda text: "STUN DNS FIXTURE: retry resolved to loopback" in host.console() and
                  "STUN mapping discovered" in text, "automatic DNS retry and recovered discovery", args.timeout)
        require(dns.requests and all(row["source"][1] == port for row in dns.requests),
                "recovered STUN lookup did not use the original game socket")
        offset = len(client.text())
        client.command_text("net_status")
        states = records(client.text()[offset:], "NET status ")
        require(states and int(states[-1].get("snapshot", "0")) > 100,
                "LAN gameplay stalled while DNS discovery retried")
        result["phases"]["dns-failure-and-recovery"] = dict(status=states[-1], requests=list(dns.requests))
        client.stop()
        host.stop()

        # The real connection timer must stop a prejoin screen from waiting
        # forever and retain actionable guidance about restrictive NAT.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as blackhole:
            blackhole.bind(("127.0.0.1", 0))
            client, _ = launch("no-server-reply", ["-client", "127.0.0.1",
                               "-port", str(blackhole.getsockname()[1])])
            client.wait(lambda text: "NET connection failed: No reply from the server" in text,
                        "bounded initial UDP connection timeout", 25)
            text = client.text()
            diagnostic = next(line for line in text.splitlines() if "NET connection failed:" in line)
            require(all(word in diagnostic for word in ("UDP port", "STUN", "NAT/CGNAT", "dedicated server")),
                    f"initial connection failure omitted useful next steps: {diagnostic}")
            started = float(re.search(r"([0-9.]+)s INFO\| NET connecting UDP", text).group(1))
            ended = float(re.search(r"([0-9.]+)s ERROR\| NET connection failed:", text).group(1))
            require(14 <= ended - started <= 20, f"initial connection timeout was not fifteen seconds: {ended - started}")
            require("NET client map loaded" not in text, "blackholed server unexpectedly entered gameplay")
            result["phases"]["initial-server-timeout"] = dict(diagnostic=diagnostic, seconds=ended - started)
            focus_window(client)
            client.key(67, True)  # F10 opens the normal quit dialog before a map.
            time.sleep(0.25)
            client.key(67, False)
            client.key(28, True)  # Y.
            client.process.wait(timeout=10)
            client.stop()

        for label, options in (("off", ["-stunserver", "off"]), ("empty", ["-stunserver="])):
            disabled = fixture()
            host, port = dedicated(f"disabled-{label}-server", options,
                extra=f'net_stun_server "127.0.0.1:{disabled.port}"\n')
            client, _ = launch(f"disabled-{label}-client", ["-client", "127.0.0.1", "-port", str(port)])
            discord.joined(client, args.timeout)
            text = status(host)
            require("NET STUN status=disabled" in text and not disabled.requests,
                    f"explicit {label} STUN CLI did not override saved configuration")
            result["phases"][f"disabled-{label}-command-line-override"] = True
            client.stop()
            host.stop()
        print("PASS: STUN timeout does not interrupt LAN gameplay; explicit opt-out overrides saved configuration", flush=True)
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
        for index, server in enumerate(fixtures):
            server.close()
            if server.errors:
                result["failures"].append(f"STUN fixture {index}: {server.errors}")
        result["passed"] = result["passed"] and not result["failures"]
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(dict(passed=result["passed"], failures=result["failures"], evidence=str(root))), flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
