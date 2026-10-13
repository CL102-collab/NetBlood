#!/usr/bin/env python3
"""Exercise dedicated passwords, rejected admission and in-game credentials.

Requires Blood 1.21 assets, a C++ compiler and X11/Xvfb. The actual executable
handles password checks; independent ENet peers supply malformed handshakes.
"""

import argparse
import hashlib
import importlib.util
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
from scoreboard import Game as BaseGame, expect


class Game(BaseGame):
    def wait(self, predicate, description, timeout=45):
        # Unlike ordinary join tests, this session deliberately retries after a
        # previous rejection; old diagnostic lines must not abort new commands.
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate(self.text()):
                return self.text()
            require(self.process.poll() is None,
                    f"{self.name}: exited before {description}; see {self.path}")
            time.sleep(0.05)
        raise Failure(f"{self.name}: timed out waiting for {description}; see {self.path}")


def quote(value):
    return json.dumps(value, ensure_ascii=True)


def tap(game, scan):
    game.key(scan, True)
    game.input_barrier()
    game.key(scan, False)
    game.input_barrier()


def type_text(game, text):
    for char in text:
        if "a" <= char <= "z":
            scan = 4 + ord(char) - ord("a")
        elif "1" <= char <= "9":
            scan = 30 + ord(char) - ord("1")
        elif char == "0":
            scan = 39
        elif char == " ":
            scan = 44
        elif char == ".":
            scan = 55
        else:
            raise Failure(f"unsupported menu fixture character {char!r}")
        tap(game, scan)


def replace_field(game, text, old_length):
    tap(game, 40)  # Enter edits the focused field.
    for _ in range(old_length):
        tap(game, 42)  # Backspace edits through the real menu implementation.
    type_text(game, text)
    tap(game, 40)


def wait_rejection(game, reason, timeout, offset=0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = game.text()[offset:]
        if "NET rejected: " + reason in text:
            return text
        require(game.process.poll() is None, f"{game.name}: exited before password rejection")
        time.sleep(0.05)
    raise Failure(f"{game.name}: missing rejection {reason!r}: {game.path}")


def cancel_password(game):
    focus_window(game)
    tap(game, 41)  # Escape stops editing, preserving the previous credential.
    tap(game, 81)  # Connect
    tap(game, 81)  # Cancel
    tap(game, 40)


def quit_before_map(game, prompt=True):
    if prompt:
        cancel_password(game)
    focus_window(game)
    tap(game, 41)  # Leave the main menu before the global F10 action.
    tap(game, 67)  # F10
    tap(game, 28)  # Y
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
    parser.add_argument("--renderer", type=int, choices=(0, 3, 5), default=0)
    parser.add_argument("--skip-screenshots", action="store_true")
    args = parser.parse_args()
    for name in ("binary", "assets", "engine_data"):
        setattr(args, name, getattr(args, name).resolve())
    require(os.environ.get("DISPLAY"), "graphical clients need DISPLAY (use Xvfb)")
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="netblood-password-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print(f"Password evidence: {root}", flush=True)
    source_binary = args.binary
    args.binary = root / "nblood-tested"
    shutil.copy2(source_binary, args.binary)
    library = build_driver(root)
    peer = root / "password-peer"
    command = shlex.split(os.environ.get("CXX", "c++")) + ["-std=c++11", "-O1", "-g", "-Wall", "-Wextra", "-Werror"]
    command += shlex.split(os.environ.get("CPPFLAGS", "")) + shlex.split(os.environ.get("CXXFLAGS", ""))
    command += [f"-I{repository / 'source/build/include'}", f"-I{repository / 'source/blood/src'}",
                str(Path(__file__).with_name("content-handshake.cpp")), "-pthread"]
    command += shlex.split(os.environ.get("LDFLAGS", "")) + ["-o", str(peer)]
    subprocess.run(command, check=True)
    result = dict(passed=False, source_binary=str(source_binary),
                  binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                  renderer=args.renderer, phases={}, screenshots={}, failures=[])
    games = []

    def saved(password):
        return {"network_cvars.cfg": ("password " + quote(password) + "\n").encode()}

    def server(name, password=None, initial=None):
        port = free_port()
        options = ["-dedicated", "-port", str(port), "-gametype", "bloodbath", "-map", "E1M1"]
        if password is not None:
            options += ["-password", password]
        game = Game(name, args, root, library, port, dedicated=True,
                    launch_arguments=options, initial_files=initial)
        games.append(game)
        game.wait(lambda text: "Dedicated server running:" in text and "NET server_tick=" in text,
                  "dedicated server", args.timeout)
        return game, port

    def client(name, port, password=None, initial=None):
        options = ["-client", "127.0.0.1", "-port", str(port)]
        options += ["-pname", name[:15]]
        if password is not None:
            options += ["-password", password]
        game = Game(name, args, root, library, port, launch_arguments=options, initial_files=initial)
        games.append(game)
        return game

    def joined(game):
        game.wait(lambda text: "NET client map loaded " in text and "NET client slot=" in text,
                  "authoritative gameplay", args.timeout)
        require("NET content verified" in game.text(), f"{game.name}: admitted without asset verification")

    def probe(name, port, mode, credential=""):
        completed = subprocess.run([str(peer), "password-probe", str(port), mode, credential],
                                   text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
        (root / f"{name}-peer.log").write_text(completed.stdout)
        require(completed.returncode == 0, f"{name}: {completed.stdout}")
        result["phases"][name] = completed.stdout.strip()

    def rejected(game, reason, offset=0, name=None):
        name = name or game.name
        wait_rejection(game, reason, args.timeout, offset)
        def text_mask(image):
            pixels = image.get_flattened_data() if hasattr(image, "get_flattened_data") else image.getdata()
            return bytes(min(pixel) > 160 and max(pixel) - min(pixel) < 12 for pixel in pixels)
        if not args.skip_screenshots:
            from PIL import Image
            deadline = time.monotonic() + args.timeout
            # Network rejection is logged before the first menu frame. Slow
            # software Vulkan startup can still have a completely black window
            # here; establish the visible diagnostic before testing persistence.
            while True:
                result["screenshots"][name] = capture_window(game, root / f"{name}.png")
                first = Image.open(root / f"{name}.png").convert("RGB")
                bounds = (0, int(first.height * 0.30), first.width, int(first.height * 0.40))
                before_mask = text_mask(first.crop(bounds))
                if sum(before_mask) > 100:
                    break
                require(game.process.poll() is None, f"{game.name}: exited before drawing the rejection")
                require(time.monotonic() < deadline, f"{game.name}: rejection screen has no visible diagnostic text")
                time.sleep(0.1)
        time.sleep(2)
        require(game.process.poll() is None, f"{game.name}: error closed the game")
        text = game.text()
        require(not any(marker in text for marker in
                        ("NET content verified", "NET client map loaded", "NET map download started", "NET client slot=")),
                f"{game.name}: password rejection reached content verification or gameplay")
        require(not (game.path / "net-content-mismatch.txt").exists(),
                f"{game.name}: password error incorrectly reported missing assets")
        if not args.skip_screenshots:
            later = capture_window(game, root / f"{name}-persistent.png")
            result["screenshots"][name + "-persistent"] = later
            second = Image.open(later["path"]).convert("RGB")
            # The rejection reason is an unselected row at nominal y=65.
            # Exclude the blinking password cursor and animated menu footer.
            bounds = (0, int(second.height * 0.30), second.width, int(second.height * 0.40))
            region = second.crop(bounds)
            pixels = region.get_flattened_data() if hasattr(region, "get_flattened_data") else region.getdata()
            require(sum(min(pixel) > 160 and max(pixel) - min(pixel) < 12 for pixel in pixels) > 100,
                    f"{game.name}: rejection screen has no visible diagnostic text")
            # Blood droplets can also cross in front of a few text pixels.
            # Require the same glyph positions with at most 5% occlusion.
            after_mask = text_mask(region)
            retained = sum(before and after for before, after in zip(before_mask, after_mask))
            require(retained >= 0.95 * max(sum(before_mask), sum(after_mask)),
                    f"{game.name}: diagnostic disappeared after disconnect")
        result["phases"][name] = dict(rejection=reason, persistence_seconds=2,
                                      content_verified=False, gameplay=False)

    def enter_prompt(game, password, previous=""):
        focus_window(game)
        # The automatic prompt focuses and edits Password immediately.
        for _ in previous:
            tap(game, 42)
        type_text(game, password)
        tap(game, 40)  # Submit through the actual menu callback.

    try:
        open_server, open_port = server("default-open", initial=saved("remembered client password"))
        for mode in ("open-missing", "open-empty"):
            probe(mode, open_port, mode)
        guest = client("open-client", open_port)
        joined(guest)
        result["phases"]["saved-client-password-does-not-lock-server"] = True
        guest.stop()
        open_server.stop()
        print("PASS: dedicated default remains open even with a remembered client password", flush=True)

        protected, port = server("protected", "open sesame", saved("incorrect remembered value"))
        for mode, credential in (("missing", ""), ("empty", ""), ("wrong", "wrong"),
                                 ("case-sensitive", "Open Sesame"), ("truncated", ""),
                                 ("oversized", ""), ("nul", ""), ("control", ""),
                                 ("trailing", "open sesame"), ("rejected-bypass", "wrong"),
                                 ("bypass", "open sesame"), ("accepted", "open sesame")):
            probe("protected-" + mode, port, mode, credential)
        expect(protected, lambda rows: not rows, "no probe admitted before content readiness", args.timeout)
        require("NET joined slot=" not in protected.text(), "malformed/unauthenticated probe joined the world")
        print("PASS: independent UDP peers verify malformed passwords and pre-admission bypasses", flush=True)

        missing = client("missing-client", port)
        rejected(missing, "Server password required")
        offset = len(missing.text())
        enter_prompt(missing, "wrong")
        rejected(missing, "Incorrect server password", offset, "prompt-wrong-retry")
        enter_prompt(missing, "open sesame", "wrong")
        joined(missing)
        missing.stop()
        result["phases"]["automatic-prompt-wrong-then-correct"] = True
        wrong = client("wrong-client", port, "Open Sesame")
        rejected(wrong, "Incorrect server password")
        cancel_password(wrong)
        time.sleep(0.5)
        require("NET content verified" not in wrong.text(), "Cancel admitted a rejected client")
        expect(protected, lambda rows: not rows, "Cancel leaves no admitted player", args.timeout)
        quit_before_map(wrong, prompt=False)
        result["phases"]["automatic-prompt-cancel"] = True

        # The desktop/Discord launch contract uses a URI-only command line.
        # Reuse the portable installation fixture, launched from another cwd.
        spec = importlib.util.spec_from_file_location("uri_password_fixture", Path(__file__).with_name("uri-join.py"))
        uri_fixture = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(uri_fixture)

        class UriPasswordGame(uri_fixture.UriGame):
            wait = Game.wait

        uri = UriPasswordGame("uri-client", args, root, library, os.environ.copy(),
                              f"netblood://127.0.0.1:{port}")
        games.append(uri)
        rejected(uri, "Server password required")
        enter_prompt(uri, "open sesame")
        joined(uri)
        uri.stop()
        result["phases"]["uri-launch-prompts-before-content"] = True

        correct = client("correct-client", port, "open sesame", saved("wrong saved password"))
        joined(correct)
        query_offset = len(correct.text())
        correct.command_text("password")
        require('open sesame' in correct.text()[query_offset:], "console cannot show the stored password")
        correct.stop()
        persisted = {"network_cvars.cfg": correct.path / "network_cvars.cfg"}
        require('password "open sesame"' in persisted["network_cvars.cfg"].read_text(),
                "client password was not saved in the cvar configuration")
        empty_override = client("empty-override", port, "", persisted)
        rejected(empty_override, "Server password required")
        quit_before_map(empty_override)
        remembered = client("remembered", port, initial=persisted)
        joined(remembered)
        remembered.command_text('password ""')
        remembered.stop()
        cleared = {"network_cvars.cfg": remembered.path / "network_cvars.cfg"}
        password_lines = [line for line in cleared["network_cvars.cfg"].read_text().splitlines()
                          if line.startswith("password ")]
        require(password_lines in ([], ['password ""']), "clearing password retained a nonempty saved value")
        cleared_client = client("cleared", port, initial=cleared)
        rejected(cleared_client, "Server password required")
        quit_before_map(cleared_client)
        result["phases"]["client-cli-override-remember-clear"] = True
        print("PASS: actual clients reject missing/wrong credentials and remember/clear correct credentials", flush=True)

        # No CLI credential or injected cvar: enter all fields through the
        # normal title-screen Multiplayer > Join A Game menu.
        menu = Game("menu-client", args, root, library, launch_arguments=[])
        games.append(menu)
        menu.wait(lambda text: "Waiting for network players!" in text, "title startup", args.timeout)
        time.sleep(1)
        focus_window(menu)
        # With -nodemo and no initial map, startup opens the main menu itself.
        tap(menu, 81)  # Multiplayer
        tap(menu, 40)
        tap(menu, 81)  # Join A Game
        tap(menu, 40)
        replace_field(menu, "127.0.0.1", len("localhost"))
        tap(menu, 81)
        replace_field(menu, str(port), len("23513"))
        tap(menu, 81)
        replace_field(menu, "open sesame", 0)
        if not args.skip_screenshots:
            result["screenshots"]["join-menu"] = capture_window(menu, root / "join-menu.png")
        tap(menu, 81)
        tap(menu, 40)
        joined(menu)
        menu.stop()
        require('password "open sesame"' in (menu.path / "network_cvars.cfg").read_text(),
                "menu password did not persist")
        result["phases"]["typed-menu-password-joins"] = True
        protected.stop()
        print("PASS: a password typed into the in-game Join menu joins the protected server", flush=True)

        # Quotes, backslashes and semicolons must survive the console serializer
        # exactly, rather than truncating or becoming an executable command.
        complex_password = 'crypt" key\\path;stay'
        special_server, special_port = server("special-server", complex_password)
        special = client("special-client", special_port, complex_password)
        joined(special)
        special.command_text("password " + quote(complex_password))
        special.stop()
        special_restart = client("special-restart", special_port,
                                 initial={"network_cvars.cfg": special.path / "network_cvars.cfg"})
        joined(special_restart)
        special_restart.stop()
        special_server.stop()
        result["phases"]["escaped-password-restart"] = True
        # This resembles a launch URI but is an ordinary password option value.
        maximum = "netblood:" + "x" * (128 - len("netblood:"))
        maximum_server, maximum_port = server("maximum-server", maximum)
        probe("maximum-128-bytes", maximum_port, "accepted", maximum)
        maximum_server.stop()
        result["passed"] = True
    except Exception as error:
        result["failures"].append(f"{type(error).__name__}: {error}")
        print(f"FAIL: {type(error).__name__}: {error}", flush=True)
    finally:
        for game in reversed(games):
            if (game.process.poll() is None and "NET rejected:" in game.text()
                    and "NET client map loaded " not in game.text()):
                try:
                    quit_before_map(game)
                except (Failure, subprocess.TimeoutExpired):
                    pass
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
