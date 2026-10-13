#!/usr/bin/env python3
"""Verify Tab team chat using real UDP clients, SDL input and packet demos.

Requires X11, Blood 1.21 assets and the same build environment as run.py.
The fixture types chat normally; it does not inject packets or player state.
"""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import tempfile
import time

from cinematics import capture_window, focus_window
from run import Failure, Game, build_driver, inspect_demo, play_demo, records, require


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def tap(game, scan):
    game.key(scan, True)
    # Chat consumes one scan per rendered frame. Let it consume each key
    # before queuing another, including text preceding a modifier shortcut.
    game.input_barrier()
    game.key(scan, False)
    game.input_barrier()


def type_text(game, text):
    for char in text:
        require(char == " " or "a" <= char <= "z", "chat fixture only types lowercase ASCII and spaces")
        tap(game, 44 if char == " " else 4 + ord(char) - ord("a"))


def command(game, text):
    if not game.dedicated:
        focus_window(game)
    game.command_text(text)


def begin_chat(game, draft="", team=False):
    focus_window(game)
    tap(game, 23)  # SDL_SCANCODE_T, bound to the ordinary Send_Message action.
    type_text(game, draft)
    if team:
        tap(game, 43)  # SDL_SCANCODE_TAB, while the draft is open.


def send_chat(game, text, team=False):
    begin_chat(game, text, team)
    tap(game, 40)  # SDL_SCANCODE_RETURN


def chat_lines(game, text):
    # Prefixes vary with console logging; match the actual rendered message.
    return [line for line in game.text().splitlines() if line.rstrip().endswith(": " + text)]


def delivered(text, recipients, excluded=(), team=False):
    for game in recipients:
        game.wait(lambda _: bool(chat_lines(game, text)), f"chat {text!r}")
    # Wait past multiple duplicated UDP sends and server event/snapshot flushes.
    time.sleep(0.5)
    observed = {}
    for game in recipients:
        lines = chat_lines(game, text)
        require(len(lines) == 1, f"{game.name}: expected exactly one echo/delivery of {text!r}, got {lines}")
        require(("[TEAM] " in lines[0]) == team, f"{game.name}: incorrect public/team chat label: {lines}")
        observed[game.name] = lines
    for game in excluded:
        require(text not in game.text() and text not in game.console(),
                f"{game.name}: private/cancelled draft leaked to an unauthorized recipient")
    return observed


def status(game):
    offset = len(game.text())
    command(game, "net_status")
    players = records(game.text()[offset:], "NET player ")
    require(players, f"{game.name}: missing player roster")
    return {int(row["slot"]): row for row in players}


def main():
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=repository / "nblood")
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--engine-data", type=Path, default=repository)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--renderer", type=int, choices=(0, 3, 5), default=0)
    parser.add_argument("--listen-renderer", type=int, choices=(0, 3, 5),
                        help="optionally use another renderer for the graphical listen host")
    parser.add_argument("--skip-screenshots", action="store_true")
    args = parser.parse_args()
    for name in ("binary", "assets", "engine_data"):
        setattr(args, name, getattr(args, name).resolve())
    require(os.environ.get("DISPLAY"), "clients require DISPLAY (Xvfb is suitable)")
    require(args.binary.is_file() and args.assets.is_dir(), "supply a built executable and game assets")
    for tool in ("xwininfo", "xprop") + (() if args.skip_screenshots else ("import",)):
        require(shutil.which(tool), f"{tool} is needed for real X11 focus/capture checks")
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="netblood-team-chat-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print(f"Team-chat evidence: {root}", flush=True)
    source_binary = args.binary
    args.binary = root / "nblood-tested"
    shutil.copy2(source_binary, args.binary)
    library = build_driver(root)
    fixture_spec = importlib.util.spec_from_file_location("team_balance_fixture", Path(__file__).with_name("team-balance.py"))
    fixture = importlib.util.module_from_spec(fixture_spec)
    fixture_spec.loader.exec_module(fixture)
    files = {"NBCHAT.MAP": fixture.death_map()}
    games = []
    result = {"passed": False, "source_binary": str(source_binary),
              "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
              "phases": {}, "screenshots": {}, "failures": []}

    def start_server(name, mode, dedicated=True):
        port = free_port()
        options = ["-dedicated", "-maxplayers", "4"] if dedicated else ["-server", "4"]
        options += ["-port", str(port), "-gametype", mode, "-map", "NBCHAT.MAP", "-pname=Host"]
        game = Game(name, args, root, library, port, dedicated=dedicated, listen=not dedicated,
                    launch_arguments=options, initial_files=files,
                    renderer=None if dedicated else args.listen_renderer)
        games.append(game)
        game.wait(lambda text: "NET server_tick=" in text and
                  (not dedicated or "Dedicated server running:" in text), "running server", args.timeout)
        if not dedicated:
            command(game, 'bind T "gamefunc_Send_Message"')
        return game, port

    def client(name, port, slot):
        game = Game(name, args, root, library, port, initial_files=files,
                    launch_arguments=["-client", "127.0.0.1", "-port", str(port), "-pname=" + name])
        games.append(game)
        game.wait(lambda text: "NET client map loaded " in text and "NET client slot=" in text,
                  "authoritative client snapshot", args.timeout)
        require(records(game.text(), "NET welcome ")[-1]["slot"] == str(slot), f"{name}: unexpected slot")
        command(game, 'bind T "gamefunc_Send_Message"')
        return game

    def screenshot(name, game):
        if not args.skip_screenshots:
            time.sleep(0.3)
            result["screenshots"][name] = capture_window(game, root / (name + ".png"))

    def record(game):
        command(game, "net_record chat.nbpd")

    def stop_record(game):
        command(game, "net_stoprecord")
        return inspect_demo(game.path / "chat.nbpd")

    def check_demo(demo, messages, forbidden=()):
        chats = [packet for packet in demo["packets"] if packet["type"] in (7, 24)]
        for text, team, sender in messages:
            matches = [packet for packet in chats if packet["chat_text"] == text]
            require(len(matches) == 1 and matches[0]["team_chat"] == team and matches[0]["chat_sender"] == sender,
                    f"{demo['path']}: missing, duplicate or mislabeled recorded chat {text!r}: {matches}")
        data = Path(demo["path"]).read_bytes()
        for text in forbidden:
            require(text.encode() not in data, f"{demo['path']}: private text leaked through a chat/event/demo record")
        return chats

    def stop_all(current):
        for game in current:
            if not game.dedicated and game.process.poll() is None:
                focus_window(game)
            game.stop()

    try:
        server, port = start_server("dedicated-teams", "teams")
        first, enemy, teammate = [client(name, port, slot) for slot, name in
                                  enumerate(("Sender", "Opponent", "Teammate"))]
        roster = status(server)
        require({slot: int(row["team"]) for slot, row in roster.items()} == {0: 0, 1: 1, 2: 0},
                f"unexpected team fixture: {roster}")
        for game in (first, enemy, teammate):
            record(game)

        public = "public hello"
        begin_chat(first, public)
        screenshot("say-prompt", first)
        tap(first, 40)
        phase = {"roster": roster, "public": delivered(public, (first, enemy, teammate))}

        private = "private draft kept"
        begin_chat(first, "private draft", team=True)
        type_text(first, " kept")
        screenshot("team-say-prompt", first)
        tap(first, 40)
        phase["team"] = delivered(private, (first, teammate), (enemy,), team=True)

        # Each fresh input starts in public mode, even after a private send.
        reset = "new chat public"
        send_chat(first, reset)
        phase["reset"] = delivered(reset, (first, enemy, teammate))

        twice = "tab twice public"
        begin_chat(first, "tab twice", team=True)
        tap(first, 43)
        type_text(first, " public")
        tap(first, 40)
        phase["toggle-back"] = delivered(twice, (first, enemy, teammate))

        cleared, discard = "cleared private", "discard draft"
        begin_chat(first, discard, team=True)
        first.key(224, True)  # SDL_SCANCODE_LCTRL
        time.sleep(0.1)
        tap(first, 42)  # SDL_SCANCODE_BACKSPACE
        first.key(224, False)
        time.sleep(0.1)
        type_text(first, cleared)
        tap(first, 40)
        phase["clear-keeps-team"] = delivered(cleared, (first, teammate), (enemy,), team=True)

        cancelled = "cancelled private"
        begin_chat(first, cancelled, team=True)
        tap(first, 41)  # SDL_SCANCODE_ESCAPE
        after_cancel = "cancel reset public"
        send_chat(first, after_cancel)
        phase["cancel-reset"] = delivered(after_cancel, (first, enemy, teammate))
        delivered(cancelled, (), (first, enemy, teammate))

        routed = records(server.text(), "NET chat routed ")
        require(len(routed) == 6 and all(row["sender"] == "0" and
                row["recipients"] == ("05" if row["channel"] == "team" else "07") for row in routed),
                f"dedicated server used incorrect authoritative chat recipients: {routed}")
        phase["routing"] = routed

        expected = [(public, False, 0), (private, True, 0), (reset, False, 0),
                    (twice, False, 0), (cleared, True, 0), (after_cancel, False, 0)]
        demos = {game.name: stop_record(game) for game in (first, enemy, teammate)}
        phase["recorded-chat"] = {
            first.name: check_demo(demos[first.name], expected, (cancelled, discard)),
            teammate.name: check_demo(demos[teammate.name], expected, (cancelled, discard)),
            enemy.name: check_demo(demos[enemy.name], [item for item in expected if not item[1]],
                                   (private, cleared, cancelled, discard))}
        result["phases"]["dedicated"] = phase
        result["packet_demo"] = demos
        stop_all((first, enemy, teammate, server))
        print("PASS: public/team routing, Tab draft preservation, reset, clearing and cancellation", flush=True)

        # Replays must show private messages only for the recording recipient.
        for source, forbidden in ((teammate.name, (cancelled, discard)), (enemy.name, (private, cleared, cancelled, discard))):
            replay = Game("replay-" + source, args, root, library, playback=True,
                          launch_map="NBCHAT.MAP", initial_files=files)
            games.append(replay)
            replay.wait(lambda text: "> This map" in text, "offline replay map", args.timeout)
            time.sleep(1)
            focus_window(replay)
            played = play_demo(replay.name, demos[source], args, root, library, games, existing=replay)
            wanted = expected if source == teammate.name else [item for item in expected if not item[1]]
            for text, team, _ in wanted:
                lines = chat_lines(replay, text)
                require(len(lines) == 1 and ("[TEAM] " in lines[0]) == team,
                        f"{replay.name}: replay did not reproduce recorded channel/message {text!r}")
            for text in forbidden:
                require(text not in replay.text() and text not in replay.console(),
                        f"{replay.name}: private/cancelled message leaked during playback")
            result["phases"][replay.name] = played
        print("PASS: recipient and opponent demos replay exactly without private-chat leakage", flush=True)

        # A graphical authoritative host must not rebroadcast a private display
        # as a global presentation Event; it may also be an excluded opponent.
        host, port = start_server("listen-teams", "teams", dedicated=False)
        remote, ally = [client(name, port, slot) for slot, name in ((1, "Remote"), (2, "HostAlly"))]
        roster = status(host)
        require({slot: int(row["team"]) for slot, row in roster.items()} == {0: 0, 1: 1, 2: 0},
                f"unexpected listen team fixture: {roster}")
        for game in (host, remote, ally):
            record(game)
        isolated, hosted, listen_public = "remote secret", "host secret", "listen public"
        send_chat(remote, isolated, team=True)
        listen_phase = {"remote-private": delivered(isolated, (remote,), (host, ally), team=True)}
        begin_chat(host, hosted, team=True)
        screenshot("listen-team-say-prompt", host)
        tap(host, 40)
        listen_phase["host-private"] = delivered(hosted, (host, ally), (remote,), team=True)
        begin_chat(host, listen_public)
        screenshot("listen-say-prompt", host)
        tap(host, 40)
        listen_phase["public"] = delivered(listen_public, (host, remote, ally))
        listen_demos = {game.name: stop_record(game) for game in (host, remote, ally)}
        listen_phase["recorded-chat"] = {
            host.name: check_demo(listen_demos[host.name], [(hosted, True, 0), (listen_public, False, 0)], (isolated,)),
            ally.name: check_demo(listen_demos[ally.name], [(hosted, True, 0), (listen_public, False, 0)], (isolated,)),
            remote.name: check_demo(listen_demos[remote.name], [(isolated, True, 1), (listen_public, False, 0)], (hosted,))}
        result["phases"]["listen"] = listen_phase
        result["listen_packet_demo"] = listen_demos
        stop_all((remote, ally, host))
        print("PASS: listen-host team filtering and packet recording do not leak through global Events", flush=True)

        for mode, text, team in (("coop", "cooperative secret", True), ("bloodbath", "bloodbath public", False)):
            server, port = start_server(mode + "-server", mode)
            one, two = client(mode + "-sender", port, 0), client(mode + "-other", port, 1)
            for game in (one, two):
                record(game)
            # Co-op allows team mode for all cooperative players. Bloodbath
            # ignores Tab and sends to everyone as public chat.
            send_chat(one, text, team=True)
            mode_phase = {"delivery": delivered(text, (one, two), team=team)}
            mode_demos = {game.name: stop_record(game) for game in (one, two)}
            mode_phase["recorded-chat"] = {name: check_demo(demo, [(text, team, 0)]) for name, demo in mode_demos.items()}
            result["phases"][mode] = mode_phase
            stop_all((one, two, server))
        print("PASS: co-op shares team chat with all teammates; Bloodbath retains public chat", flush=True)
        result["passed"] = True
    except (Failure, OSError, subprocess.SubprocessError, KeyError, ValueError) as error:
        result["failures"].append(str(error))
        print(f"FAIL: {error}", flush=True)
    finally:
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
