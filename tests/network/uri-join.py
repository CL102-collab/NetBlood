#!/usr/bin/env python3
"""Join a real server through the Linux netblood:// desktop handler.

Requires Blood 1.21 assets, X11/Xvfb, gio and xdg-mime. Registration and every
configuration file are isolated under a temporary HOME; no installed user's
handler is changed. The actual GIO-launched game receives normal SDL input.
"""

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import tempfile
import time

from cinematics import free_port
from run import build_driver, records, require
from scoreboard import Game


class AdoptedProcess:
    """Track the desktop-launched child adopted by our Linux subreaper."""

    def __init__(self, pid):
        self.pid = pid
        self.returncode = None

    def poll(self):
        if self.returncode is None:
            child, status = os.waitpid(self.pid, os.WNOHANG)
            if child:
                self.returncode = os.waitstatus_to_exitcode(status)
        return self.returncode

    def wait(self, timeout):
        deadline = time.monotonic() + timeout
        while self.poll() is None:
            if time.monotonic() >= deadline:
                raise subprocess.TimeoutExpired(str(self.pid), timeout)
            time.sleep(0.05)
        return self.returncode

    def send_signal(self, number):
        os.kill(self.pid, number)

    def kill(self):
        self.send_signal(signal.SIGKILL)


class UriGame(Game):
    def __init__(self, name, args, root, library, environment, uri, desktop=False, positional=False):
        self.name = name
        self.path = root / name
        self.path.mkdir()
        executable = self.path / "nblood"
        shutil.copy2(args.binary, executable)
        # URI launches deliberately have no -j, -usecwd, -cfg or -nosetup flags.
        # This models a portable installation opened from an unrelated browser
        # working directory, including a desktop Exec path containing spaces.
        for source in args.assets.iterdir():
            if source.is_file():
                (self.path / source.name).symlink_to(source)
        (self.path / "nblood.pk3").symlink_to(args.engine_data / "nblood.pk3")
        (self.path / "user_profiles_disabled").touch()
        (self.path / "nblood.cfg").write_text(
            "[Setup]\nForceSetup = 0\nNoAutoLoad = 1\n\n"
            "[Screen Setup]\nScreenMode = 0\nScreenBPP = 8\nRenderer = 0\n"
            "ScreenWidth = 640\nScreenHeight = 480\n")
        (self.path / "autoexec.cfg").write_text(
            'r_maxfps 60\nnet_lagometer 1\nbind I norepeat "exec command.cfg"\n')
        self.fifo = self.path / "input.fifo"
        os.mkfifo(self.fifo)
        self.events = os.open(self.fifo, os.O_RDWR | os.O_NONBLOCK)
        self.command_serial = 0
        self.dedicated = False
        self.forced = False
        self.log = (self.path / "console.log").open("w")
        environment = dict(environment, NETWORK_TEST_EVENTS=str(self.fifo), SDL_AUDIODRIVER="dummy")
        environment["LD_PRELOAD"] = str(library) + (
            ":" + environment["LD_PRELOAD"] if environment.get("LD_PRELOAD") else "")
        self.command = [str(executable), uri] if positional else [str(executable), "-connect-uri", uri]
        if desktop:
            registration = subprocess.run([str(executable), "-register-uri"], cwd=root,
                                          env=environment, text=True, stdout=subprocess.PIPE,
                                          stderr=subprocess.STDOUT, timeout=15)
            (self.path / "registration.log").write_text(registration.stdout)
            require(registration.returncode == 0, f"URI registration failed: {registration.stdout}")
            handler = subprocess.check_output(["xdg-mime", "query", "default",
                                               "x-scheme-handler/netblood"], env=environment, text=True).strip()
            require(handler == "netblood.desktop", f"unexpected URI handler: {handler!r}")
            self.command = ["gio", "open", uri]
            dispatcher = subprocess.run(self.command, cwd=root, env=environment,
                                        stdout=self.log, stderr=subprocess.STDOUT, timeout=15)
            require(dispatcher.returncode == 0, "GIO rejected the registered netblood handler")
            deadline = time.monotonic() + 10
            pid = None
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
            require(pid is not None, "GIO did not launch the registered executable")
            self.process = AdoptedProcess(pid)
        else:
            self.process = subprocess.Popen(self.command, cwd=root, env=environment,
                                            stdin=subprocess.PIPE, stdout=self.log,
                                            stderr=subprocess.STDOUT)


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
    require(os.environ.get("DISPLAY"), "clients require DISPLAY (use an Xvfb wrapper)")
    require(shutil.which("gio") and shutil.which("xdg-mime"), "install gio and xdg-utils")
    # GIO exits after spawning its child. Adopting that child lets us verify its
    # actual exit code, rather than treating desktop dispatch as a successful join.
    require(ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0) == 0,
            "could not enable Linux child subreaper")
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="netblood-uri-join-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print(f"URI join evidence: {root}", flush=True)
    source_binary = args.binary
    args.binary = root / "nblood-tested"
    shutil.copy2(source_binary, args.binary)
    library = build_driver(root)
    environment = os.environ.copy()
    for variable, relative in (("HOME", "home"), ("XDG_DATA_HOME", "xdg-data"),
                               ("XDG_CONFIG_HOME", "xdg-config"), ("XDG_CACHE_HOME", "xdg-cache")):
        directory = root / relative
        directory.mkdir()
        environment[variable] = str(directory)
    # Keep GIO dispatch local to this isolated process environment.
    environment.pop("DBUS_SESSION_BUS_ADDRESS", None)
    environment["GIO_USE_VFS"] = "local"
    result = dict(passed=False, source_binary=str(source_binary),
                  binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                  phases={}, failures=[])
    games = []

    def server(name, port):
        game = Game(name, args, root, library, port, dedicated=True,
                    launch_arguments=["-dedicated", "-port", str(port), "-gametype", "bloodbath",
                                      "-maxplayers", "8", "-map", "E1M1"])
        games.append(game)
        game.wait(lambda text: "Dedicated server running:" in text and "NET server_tick=" in text,
                  "dedicated server", args.timeout)
        return game

    def join(name, uri, host, port, **kwargs):
        game = UriGame(name, args, root, library, environment, uri, **kwargs)
        games.append(game)
        game.wait(lambda text: "NET client map loaded " in text and "NET client slot=" in text,
                  "first authoritative snapshot", args.timeout)
        require(f"NET connecting UDP {host}:{port}" in game.text(),
                f"URI selected the wrong destination: {uri}")
        game.command_text("net_status")
        result["phases"][name] = dict(uri=uri, destination=f"{host}:{port}",
                                     welcome=records(game.text(), "NET welcome "),
                                     status=records(game.text(), "NET status "))
        game.stop()
        print(f"PASS: {name} joined {host}:{port}", flush=True)

    try:
        registration = subprocess.run([str(args.binary), "-register-uri"], cwd=root, env=environment,
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=15)
        require(registration.returncode == 0, f"asset-free URI registration failed: {registration.stdout}")
        require(not (root / "nblood.log").exists(), "registration reached ordinary game initialization")
        result["phases"]["asset-free-registration"] = dict(exit_code=registration.returncode,
                                                           output=registration.stdout)
        # Exact early argument parsing must reject these before -record and
        # other ordinary game switches can change application behavior.
        reject_dir = root / "invalid-links"
        reject_dir.mkdir()
        invalid = [
            ["-connect-uri", "netblood://127.0.0.1:0"],
            ["-connect-uri", "netblood://127.0.0.1:65536"],
            ["netblood://user@127.0.0.1:23513"],
            ["netblood://127.0.0.1:23513/path"],
            ["-connect-uri", 'netblood://127.0.0.1:23513/" -record INJECTED.dem'],
            ["-connect-uri", "netblood://127.0.0.1:23513", "-record", "INJECTED.dem"],
            ["netblood://127.0.0.1:23513", "-record", "INJECTED.dem"],
            ["-register-uri", "-record", "INJECTED.dem"],
        ]
        rejected = []
        invalid_environment = dict(environment, SDL_VIDEODRIVER="dummy")
        for variable in ("DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY"):
            invalid_environment.pop(variable, None)
        for index, arguments in enumerate(invalid):
            process = subprocess.run([str(args.binary)] + arguments, cwd=reject_dir, env=invalid_environment,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=15)
            (reject_dir / f"{index}.log").write_text(process.stdout)
            require(process.returncode != 0, f"accepted malformed URI/arguments: {arguments!r}")
            require("NET connecting UDP" not in process.stdout, "malformed URI attempted a connection")
            rejected.append(dict(arguments=arguments, exit_code=process.returncode))
        require(not list(root.rglob("INJECTED.dem")), "URI argument injection created a demo")
        require(not list(reject_dir.glob("nblood*")), "invalid URI reached ordinary game initialization")
        result["phases"]["invalid-links"] = rejected
        print("PASS: malformed links and injected command-line arguments rejected", flush=True)

        port = free_port()
        host = server("server", port)
        join("desktop app $data %u", f"netblood://127.0.0.1:{port}/", "127.0.0.1", port, desktop=True)
        join("positional-dns", f"NeTbLoOd://localhost:{port}", "localhost", port, positional=True)
        join("ipv6", f"netblood://[::1]:{port}", "::1", port)
        # An unabbreviated IPv6 address exceeds the former 32-byte network
        # address buffer. Exercise desktop dispatch and the actual resolver,
        # then require the full address in the log and a server snapshot.
        full_ipv6 = "0000:0000:0000:0000:0000:0000:0000:0001"
        join("desktop-full-ipv6", f"netblood://[{full_ipv6}]:{port}", full_ipv6, port, desktop=True)
        host.stop()
        with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as probe:
            probe.bind(("::", 23513))
        default = server("default-port-server", 23513)
        join("default-port", "netblood://localhost/", "localhost", 23513, positional=True)
        default.stop()
        result["passed"] = True
    except Exception as error:
        result["failures"].append(f"{type(error).__name__}: {error}")
        print(f"FAIL: {type(error).__name__}: {error}", flush=True)
    finally:
        for game in reversed(games):
            game.stop()
        result["games"] = {game.name: game.result() for game in games}
        for game in games:
            if game.process.returncode != 0 or game.forced:
                result["failures"].append(f"{game.name}: unclean exit {game.process.returncode}")
            if re.search(r"runtime error:|AddressSanitizer|NET snapshot invalid", game.text() + game.console()):
                result["failures"].append(f"{game.name}: runtime/snapshot error")
            if game.dedicated and "NETWORK_TEST_DEVICES windows=0 audio=0" not in game.console():
                result["failures"].append(f"{game.name}: missing headless device audit")
        result["passed"] = result["passed"] and not result["failures"]
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(dict(passed=result["passed"], failures=result["failures"], evidence=str(root))), flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
