#!/usr/bin/env python3
"""Run a real dedicated server, delayed joins, impaired UDP and packet playback.

Linux/SDL2 test: clients require an existing DISPLAY, and Blood 1.21 assets must
be supplied. The dedicated process gets no DISPLAY and invalid video/audio
drivers; an SDL interposer rejects any attempt to create a window/audio device.
All player input goes through SDL and ordinary console commands.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

from relay import Relay


class Failure(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise Failure(message)


def records(text, prefix):
    return [dict(re.findall(r"(\w+)=([^\s,]+)", row))
            for row in text.splitlines() if prefix in row]


def snapshot_hashes(text, server=False):
    generation = None
    hashes = {}
    for line in text.splitlines():
        values = dict(re.findall(r"(\w+)=([^\s,]+)", line))
        if server and "NET map generation=" in line:
            generation = int(values["generation"])
        elif not server and "NET welcome " in line:
            generation = int(values["map_generation"])
        if ("NET server snapshot=" if server else "NET client slot=") in line and "hash" in values:
            hashes[(generation, int(values["snapshot"]), int(values["tick"]))] = values["hash"]
    return hashes


class Game:
    def __init__(self, name, args, root, library, port=None, dedicated=False, playback=None, renderer=None, listen=False,
                 initial_files=None, launch_map=None):
        self.name = name
        self.path = root / name
        self.path.mkdir()
        for relative, source in (initial_files or {}).items():
            target = self.path / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            if isinstance(source, bytes):
                target.write_bytes(source)
            else:
                shutil.copy2(source, target)
        self.fifo = self.path / "input.fifo"
        os.mkfifo(self.fifo)
        self.events = os.open(self.fifo, os.O_RDWR | os.O_NONBLOCK)
        self.command_serial = 0
        if renderer is None and not dedicated:
            renderer = getattr(args, "renderer", 0) or None
        (self.path / "network.cfg").write_text(
            "[Setup]\nForceSetup = 0\nNoAutoLoad = 1\n\n"
            "[Screen Setup]\nScreenMode = 0\n"
            f"ScreenBPP = {32 if renderer is not None else 8}\n"
            f"Renderer = {renderer if renderer is not None else 0}\n"
            "ScreenWidth = 640\nScreenHeight = 480\n"
        )
        (self.path / "autoexec.cfg").write_text(
            'r_maxfps 60\nnet_lagometer 1\n'
            'bind I norepeat "exec command.cfg"\n'
            'bind W "gamefunc_Move_Forward"\n'
            'bind J "gamefunc_Weapon_Fire"\n'
        )
        environment = os.environ.copy()
        environment.update(SDL_AUDIODRIVER="dummy", NETWORK_TEST_EVENTS=str(self.fifo),
                           LD_PRELOAD=str(library) + (":"+environment["LD_PRELOAD"] if environment.get("LD_PRELOAD") else ""))
        if name == "client-1":
            environment["NBLOOD_NET_VERIFY_SNAPSHOTS"] = "1"
        command = [str(args.binary), "-usecwd", "-nosetup", "-noautoload", "-quick", "-nodemo",
                   "-cfg", "network.cfg", f"-j={args.assets}", f"-j={args.engine_data}"]
        if dedicated:
            for variable in ("DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY"):
                environment.pop(variable, None)
            environment.update(SDL_VIDEODRIVER="network-test-invalid-video-driver",
                               SDL_AUDIODRIVER="network-test-invalid-audio-driver", NETWORK_TEST_HEADLESS="1")
            command += ["-dedicated", "-server", "8", "-port", str(port), "-gametype", "coop"]
            if launch_map:
                command += [f"-map={launch_map}"]
        elif playback:
            command += [f"-map={launch_map or 'E1M1'}"]
        elif listen:
            command += ["-server", "8", "-port", str(port), "-gametype", "coop", f"-map={launch_map or 'E1M1'}"]
        else:
            command += ["-client", "127.0.0.1", "-port", str(port)]
        self.command = command
        self.log = (self.path / "console.log").open("w")
        self.process = subprocess.Popen(command, cwd=self.path, env=environment,
                                        stdin=subprocess.PIPE, stdout=self.log, stderr=subprocess.STDOUT)
        self.dedicated = dedicated
        self.forced = False

    def text(self):
        log = self.path / "nblood.log"
        return log.read_text(errors="replace") if log.exists() else ""

    def console(self):
        return (self.path / "console.log").read_text(errors="replace")

    def wait(self, predicate, description, timeout=45):
        deadline = time.monotonic()+timeout
        while time.monotonic() < deadline:
            text = self.text()
            if predicate(text):
                return text
            if "NET rejected:" in text:
                raise Failure(f"{self.name}: server rejected connection; see {self.path}")
            if self.process.poll() is not None:
                raise Failure(f"{self.name} exited {self.process.returncode} before {description}; see {self.path}")
            time.sleep(0.1)
        raise Failure(f"{self.name}: timed out waiting for {description}; see {self.path}")

    def key(self, scan, down):
        os.write(self.events, f"{scan} {int(down)}\n".encode())

    def command_text(self, commands, timeout=10):
        self.command_serial += 1
        marker = f"NETWORK_TEST_COMMAND_{self.command_serial}"
        script = commands + f"\necho {marker}\n"
        if self.dedicated:
            self.process.stdin.write(script.encode())
            self.process.stdin.flush()
        else:
            (self.path / "command.cfg").write_text(script)
            self.key(12, True)  # SDL_SCANCODE_I
            try:
                # Keep the key held until a simulation/render iteration has
                # consumed it; a slow frame can otherwise drain down+up at once.
                self.wait(lambda text: marker in text, marker, timeout)
            finally:
                self.key(12, False)
            time.sleep(0.1)
            return
        self.wait(lambda text: marker in text, marker, timeout)

    def stop(self):
        if self.process.poll() is None:
            try:
                if self.dedicated:
                    self.process.stdin.write(b"quit\n")
                    self.process.stdin.flush()
                else:
                    self.command_text("net_stoprecord\nnet_disconnect\nquit", timeout=2)
            except (BrokenPipeError, Failure):
                pass
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.forced = True
                self.process.send_signal(signal.SIGTERM)
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=5)
        self.log.close()
        if self.events is not None:
            os.close(self.events)
            self.events = None

    def result(self):
        return {"command": self.command, "exit_code": self.process.poll(), "forced_stop": self.forced,
                "log": str(self.path / "nblood.log"), "status": records(self.text(), "NET status "),
                "prediction": records(self.text(), "NET prediction "),
                "players": records(self.text(), "NET player ")}


def inspect_demo(path):
    data = path.read_bytes()
    require(len(data) >= 20 and data[:8] == b"NBPKDEM\x1a", "packet demo missing header")
    version, size, checksum = struct.unpack_from("<III", data, 8)
    require(version == 1 and 29 <= size <= 1052, "packet demo header is invalid")
    metadata = data[20:20+size]
    require(zlib.crc32(metadata) == checksum, "packet demo metadata checksum mismatch")
    offset = 20+size
    packets = []
    while offset < len(data):
        require(offset+24 <= len(data), "packet demo record truncated")
        marker, length, elapsed, tick, crc = struct.unpack_from("<IIQII", data, offset)
        header = data[offset:offset+20]
        offset += 24
        require(length <= 16*1024*1024+4096 and offset+length <= len(data), "packet demo payload bounds")
        body = data[offset:offset+length]
        offset += length
        require(zlib.crc32(body, zlib.crc32(header)) == crc, "packet demo record checksum mismatch")
        if marker == 0x31444e45:
            require(length == 0 and offset == len(data), "packet demo trailing data")
            break
        require(marker in (0x31544b50, 0x31545352) and length >= 28, "unexpected packet demo record")
        magic, protocol, kind, session, generation, sequence, payload_length = struct.unpack_from("<7I", body)
        require(magic == 0x314e424e and protocol == 1 and payload_length == len(body)-28, "bad recorded envelope")
        record = {"type": kind, "session": session, "generation": generation,
                  "sequence": sequence, "elapsed_ticks": elapsed, "tick": tick,
                  "sha256": hashlib.sha256(body).hexdigest()}
        if kind == 5:
            state_tick, snapshot, baseline = struct.unpack_from("<III", body, 28)
            require(baseline == 0, "recorded snapshot depends on live network baseline")
            # Tick/id/base + 8 command ACKs + roster byte + packed length.
            compression = 28+12+32+1+4
            require(len(body) >= compression+28, "recorded snapshot header is truncated")
            require(body[compression:compression+4] == b"NSZ1", "recorded snapshot has wrong codec")
            flags = struct.unpack_from("<I", body, compression+8)[0]
            state_hash = struct.unpack_from("<I", body, compression+20)[0]
            require(not flags & 1, "recorded snapshot is a delta")
            record.update(snapshot=snapshot, state_tick=state_tick, hash=f"{state_hash:08x}")
        packets.append(record)
    else:
        raise Failure("packet demo missing end marker")
    require(packets and packets[0]["type"] == 2, "packet demo must start with Welcome")
    require(sum(row["type"] == 5 for row in packets) >= 10, "too few recorded snapshots")
    return {"path": str(path), "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data), "packets": packets}


def build_driver(output):
    library = output / "input-driver.so"
    source = Path(__file__).with_name("input-driver.c")
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "sdl2"], text=True))
    command = shlex.split(os.environ.get("CC", "cc")) + ["-Wall", "-Wextra", "-Werror", "-shared", "-fPIC"]
    command += shlex.split(os.environ.get("CFLAGS", "")) + flags + [str(source), "-o", str(library), "-ldl"]
    subprocess.run(command, check=True)
    return library


def play_demo(name, demo, args, root, library, games, existing=None):
    # Start a normal offline map so ordinary console key bindings are enabled.
    replay = existing
    if replay is None:
        replay = Game(name, args, root, library, playback=True)
        games.append(replay)
        replay.wait(lambda text: "> This map" in text, "offline playback setup", args.timeout)
        time.sleep(1)
    replay.command_text(('net_disconnect\n' if existing else '') + f'net_playback "{demo["path"]}"')
    last_snapshot = [row for row in demo["packets"] if row["type"] == 5][-1]
    duration = max(row["elapsed_ticks"] for row in demo["packets"])/30
    deadline = time.monotonic()+duration+args.timeout
    # Query through the normal console. A frozen/broken replay fails the final
    # id/hash comparison; no internal timing or snapshot state is overwritten.
    time.sleep(duration+1)
    replay.command_text("net_status\nscreenshot")
    require(time.monotonic() < deadline, "packet playback exceeded deadline")
    require("NET client slot=" in replay.text(), "packet demo did not apply authoritative snapshots")
    playback_status = records(replay.text(), "NET status ")[-1]
    require(int(playback_status["snapshot"]) == last_snapshot["snapshot"] and
            int(playback_status["tick"]) == last_snapshot["state_tick"] and
            playback_status.get("snapshot_hash") == last_snapshot["hash"],
            f"{name}: final authoritative snapshot differs from recorded state")
    replay.stop()
    return {"expected_last_snapshot": last_snapshot, "actual_status": playback_status}


def main():
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=repository / "nblood")
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--engine-data", type=Path, default=repository)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--renderer", type=int, choices=(0, 3, 4, 5), default=0,
                        help="graphical processes: software 0, Polymost 3, Polymer 4, Vulkan 5")
    parser.add_argument("--skip-map-change", action="store_true")
    args = parser.parse_args()
    for name in ("binary", "assets", "engine_data"):
        setattr(args, name, getattr(args, name).resolve())
    require(os.environ.get("DISPLAY"), "clients need DISPLAY (use an external Xvfb wrapper)")
    require(args.binary.is_file() and args.binary.stat().st_size > 0 and
            os.access(args.binary, os.X_OK) and args.assets.is_dir(), "build executable and supply game assets first")
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="netblood-network-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print(f"Network evidence: {root}", flush=True)
    library = build_driver(root)
    source_binary = args.binary
    args.binary = root / "nblood-tested"
    shutil.copy2(source_binary, args.binary)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    games, relays = [], []
    result = {"passed": False, "source_binary": str(source_binary),
              "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(), "failures": []}
    try:
        server = Game("server", args, root, library, port, dedicated=True)
        games.append(server)
        server.wait(lambda text: "Dedicated server running:" in text and
                    any(int(row["server_tick"]) >= 60 and int(row["players"]) == 0
                        for row in records(text, "NET server_tick=")), "simulation advances before any clients", args.timeout)
        relay1 = Relay(("127.0.0.1", port), seed=10).start()
        relays.append(relay1)
        first = Game("client-1", args, root, library, relay1.port)
        games.append(first)
        first.wait(lambda text: "NET client slot=" in text, "first authoritative snapshot", args.timeout)
        first.command_text("net_record session.nbpd\nnet_status")
        server.command_text("net_record server-session.nbpd")
        first_tick = int(records(first.text(), "NET client slot=")[-1]["tick"])
        server.wait(lambda text: any(int(row["server_tick"]) >= first_tick+90
                                    for row in records(text, "NET server_tick=")), "server runs before delayed second join")
        relay2 = Relay(("127.0.0.1", port), loss=0.08, delay_ms=55, jitter_ms=15, reorder=0.1, seed=29).start()
        relays.append(relay2)
        second = Game("client-2-impaired", args, root, library, relay2.port)
        games.append(second)
        second.wait(lambda text: "NET client slot=" in text, "delayed impaired client snapshot", args.timeout)
        server.wait(lambda text: any(row.get("players") == "2" for row in records(text, "NET joined ")), "two active players")
        second.command_text("net_status")
        second.key(26, True)  # W: movement through ordinary input path.
        time.sleep(2)
        second.key(26, False)
        second.key(13, True)  # J: immediate weapon feedback and server fire.
        time.sleep(0.7)
        second.key(13, False)
        time.sleep(2)
        second.command_text("net_status\nscreenshot")
        first.command_text("net_status\nscreenshot")
        server.command_text("net_status")
        prediction = records(second.text(), "NET prediction ")[-1]
        require(int(prediction["commands"]) > 30 and int(prediction["replayed"]) > 0 and
                int(prediction["reconciliations"]) > 5, "impaired client did not predict and replay authoritative updates")
        require(int(prediction.get("max_correction", 0)) > 0, "impaired movement never exercised prediction correction")
        slot = records(second.text(), "NET welcome ")[-1]["slot"]
        local_states = [row for row in records(second.text(), "NET player ") if row["slot"] == slot]
        require(len(local_states) >= 2 and any(local_states[0][axis] != local_states[-1][axis] for axis in ("x", "y", "z")),
                "scripted client movement did not change its authoritative player position")
        before_outage = int(records(second.text(), "NET client slot=")[-1]["ack"])
        # Suppress command-bearing datagrams without intentionally interrupting
        # connection liveness traffic for longer than ENet's timeout threshold.
        relay2.block_client_payload = struct.pack("<III", 0x314e424e, 1, 4)
        time.sleep(10)
        relay2.block_client_payload = None
        second.wait(lambda text: any(int(row["ack"]) > before_outage+256
                                    for row in records(text, "NET client slot=")), "input recovery after ten-second outage", args.timeout)
        time.sleep(1)
        second.command_text("net_status")
        recovered = records(second.text(), "NET prediction ")[-1]
        require(int(recovered.get("history_overflows", 0)) > 0 and int(recovered["pending"]) < 32,
                "prediction did not bound and recover its history after the outage")
        result["outage_recovery"] = {"command_loss_seconds": 10, "before_ack": before_outage, "prediction": recovered}
        if not args.skip_map_change:
            previous = len(records(first.text(), "NET welcome "))
            server.command_text("map E1M2")
            first.wait(lambda text: len(records(text, "NET welcome ")) > previous and
                       len(records(text, "NET client map loaded ")) > previous, "map transition Welcome", args.timeout)
            second.wait(lambda text: len(records(text, "NET client map loaded ")) >= 2, "impaired map transition", args.timeout)
            time.sleep(2)
        first.command_text("net_stoprecord\nnet_status")
        roundtrips = records(first.text(), "NET snapshot roundtrip ")
        require(len(roundtrips) >= 30 and all(row.get("equal") == "1" for row in roundtrips),
                "installed snapshots did not reproduce authoritative bytes before prediction")
        result["exact_installed_snapshot_roundtrips"] = len(roundtrips)
        server_hashes = snapshot_hashes(server.text(), server=True)
        result["snapshot_comparisons"] = {}
        for game in (first, second):
            client_hashes = snapshot_hashes(game.text())
            shared = sorted(set(server_hashes) & set(client_hashes))
            require(len(shared) >= 2, f"{game.name}: fewer than two server/client canonical snapshots compared")
            require(all(server_hashes[key] == client_hashes[key] for key in shared), f"{game.name}: authoritative snapshot hash mismatch")
            result["snapshot_comparisons"][game.name] = [dict(generation=key[0], snapshot=key[1], tick=key[2],
                                                            hash=client_hashes[key]) for key in shared]
        demo = inspect_demo(first.path / "session.nbpd")
        result["packet_demo"] = demo
        first.stop()
        server.wait(lambda text: records(text, "NET server_tick=")[-1].get("players") == "1", "recording viewpoint leaves with another player active")
        time.sleep(2)
        second.stop()
        empty_text = server.wait(lambda text: records(text, "NET server_tick=")[-1].get("players") == "0", "server empty after disconnects")
        empty_tick = int(records(empty_text, "NET server_tick=")[-1]["server_tick"])
        server.wait(lambda text: any(int(row["server_tick"]) >= empty_tick+60 and row["players"] == "0"
                                    for row in records(text, "NET server_tick=")), "empty server continues simulation")
        relay3 = Relay(("127.0.0.1", port), seed=30).start()
        relays.append(relay3)
        third = Game("client-rejoin", args, root, library, relay3.port)
        games.append(third)
        third.wait(lambda text: "NET client slot=" in text, "join after all clients left", args.timeout)
        third.command_text("net_status")
        time.sleep(2)
        server.command_text("net_stoprecord\nnet_status")
        server_demo = inspect_demo(server.path / "server-session.nbpd")
        require(sum(row["type"] == 2 for row in server_demo["packets"]) >= 3,
                "server packet demo did not bootstrap changed recording viewpoints")
        result["server_packet_demo"] = server_demo
        third.stop()
        server.stop()
        result["client_demo_playback"] = play_demo("packet-playback", demo, args, root, library, games)
        result["server_demo_playback"] = play_demo("server-packet-playback", server_demo, args, root, library, games)
        # The same authoritative tick path must also work with a local player
        # and renderer on the host, not just inside the dedicated loop.
        listen = Game("listen-server", args, root, library, port, listen=True)
        games.append(listen)
        listen.wait(lambda text: any(int(row["server_tick"]) >= 30 for row in records(text, "NET server_tick=")), "listen server tick", args.timeout)
        listen_relay = Relay(("127.0.0.1", port), seed=41).start()
        relays.append(listen_relay)
        listener_client = Game("listen-client", args, root, library, listen_relay.port)
        games.append(listener_client)
        listener_client.wait(lambda text: "NET client slot=" in text, "listen-server client snapshot", args.timeout)
        time.sleep(2)
        listen.command_text("net_status")
        listener_client.command_text("net_status")
        require(records(listen.text(), "NET status ")[-1]["players"] == "2" and
                records(listener_client.text(), "NET status ")[-1]["players"] == "2", "listen server roster mismatch")
        listener_client.command_text("net_record listen-session.nbpd")
        time.sleep(1)
        listen.command_text("map E1M2")
        listener_client.wait(lambda text: len(records(text, "NET client map loaded ")) >= 2,
                             "listen-server second map", args.timeout)
        time.sleep(2)
        listener_client.command_text("net_stoprecord\nnet_status")
        listen_demo = inspect_demo(listener_client.path / "listen-session.nbpd")
        require(len({row["generation"] for row in listen_demo["packets"]}) == 2,
                "listen-server demo missed one of its maps")
        result["listen_packet_demo"] = listen_demo
        # Reuse the client that already loaded both maps. Its session-scoped map
        # registry must be restored before replay starts from the earlier map.
        result["listen_demo_playback"] = play_demo("listen-client-replay", listen_demo, args, root, library,
                                                   games, existing=listener_client)
        reconnect_relay = Relay(("127.0.0.1", port), seed=42).start()
        relays.append(reconnect_relay)
        reconnect = Game("listen-rejoin", args, root, library, reconnect_relay.port)
        games.append(reconnect)
        reconnect.wait(lambda text: "NET client slot=" in text, "fresh join into listen-server second map", args.timeout)
        reconnect.command_text("net_status")
        require(records(reconnect.text(), "NET status ")[-1]["players"] == "2", "listen rejoin roster mismatch")
        reconnect.stop()
        listen.stop()
        require("NETWORK_TEST_DEVICES windows=0 audio=0" in server.console(), "dedicated server opened a device or missed orderly shutdown")
        impairment = relay2.report()["counters"]
        require(impairment.get("client_to_server_lost", 0) > 0 and impairment.get("server_to_client_lost", 0) > 0,
                "impairment run did not drop actual datagrams in both directions")
        require(impairment.get("server_to_client_reordered", 0) > 0, "impairment run did not reorder actual datagrams")
        require(impairment.get("client_to_server_filtered", 0) > 100, "command outage did not suppress actual input datagrams")
        for relay in relays:
            report = relay.report()
            require(not report["counters"].get("queue_overflow", 0) and
                    not report["counters"].get("socket_backpressure_drop", 0), "test relay overflowed its own bounded resources")
            for direction in ("client_to_server", "server_to_client"):
                require(report["identical_datagram_multiplicity"][direction].get(3, 0) > 0,
                        "default game transport did not emit three identical datagram copies")
        for game in games:
            require(game.process.returncode == 0 and not game.forced, f"{game.name} did not shut down cleanly")
            require(not re.search(r"NET (?:apply snapshot|capture failed|invalid bootstrap|local map CRC mismatch|snapshot verification failed)|AddressSanitizer|runtime error:|Segmentation fault",
                                  game.text()+game.console()), f"{game.name} reported a runtime/snapshot error")
            for status in records(game.text(), "NET status "):
                require(status["copies"] == "3", f"{game.name}: packet copies default changed")
        result["passed"] = True
    except (Failure, OSError, subprocess.SubprocessError) as error:
        result["failures"].append(str(error))
    finally:
        for game in reversed(games):
            game.stop()
        for relay in relays:
            relay.close()
        result["games"] = {game.name: game.result() for game in games}
        result["relays"] = [relay.report() for relay in relays]
        (root / "result.json").write_text(json.dumps(result, indent=2)+"\n")
    print(("PASS" if result["passed"] else "FAIL") + f": {root / 'result.json'}", flush=True)
    for failure in result["failures"]:
        print(failure, file=sys.stderr)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
