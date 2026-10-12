#!/usr/bin/env python3
"""Exercise co-op cinematic barriers with real UDP, SDL input and movie decoders.

Requires an X11 display, ffmpeg with Theora/Vorbis encoders, and Blood 1.21
assets including CRYPTIC.SMK. All maps and OGV movies are generated locally;
the copyrighted Smacker fixture is read from the supplied installation.
"""

import argparse
import ctypes
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
import time
import zlib

from run import Game, build_driver, inspect_demo, play_demo, records, require


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def exit_map(revision):
    """Two empty rooms; walking east naturally triggers the normal level exit.

    Use the existing eight-start fixture's geometry, with a level floor. The
    second room has a regular XSECTOR Enter trigger sending to channel 4. No
    cheats, game-state injection, synthetic packets or accelerated clocks.
    """
    spec = importlib.util.spec_from_file_location("team_balance_fixture", Path(__file__).with_name("team-balance.py"))
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    data = bytearray(fixture.death_map(revision)[:-4])
    second_sector = 6 + 37 + 2 + 40
    struct.pack_into("<i", data, second_sector + 8, 0)
    struct.pack_into("<h", data, second_sector + 38, 1)
    # BLM6 XSECTOR disk fields, in the order read by dbLoadMap's BitReader.
    fields = [
        ("reference", 14), ("state", 1), ("busy", 17), ("data", 16),
        ("txID", 10), ("busyWaveA", 3), ("busyWaveB", 3), ("rxID", 10),
        ("command", 8), ("triggerOn", 1), ("triggerOff", 1),
        ("busyTimeA", 12), ("waitTimeA", 12), ("restState", 1),
        ("interruptable", 1), ("amplitude", 8), ("freq", 8),
        ("reTriggerA", 1), ("reTriggerB", 1), ("phase", 8), ("wave", 4),
        ("shadeAlways", 1), ("shadeFloor", 1), ("shadeCeiling", 1),
        ("shadeWalls", 1), ("shade", 8), ("panAlways", 1), ("panFloor", 1),
        ("panCeiling", 1), ("Drag", 1), ("Underwater", 1), ("Depth", 3),
        ("panVel", 8), ("panAngle", 11), ("unused1", 1), ("decoupled", 1),
        ("triggerOnce", 1), ("isTriggered", 1), ("Key", 3), ("Push", 1),
        ("Vector", 1), ("Reserved", 1), ("Enter", 1),
    ]
    values = {"reference": 1, "txID": 4, "command": 1,
              "triggerOn": 1, "triggerOnce": 1, "Enter": 1}
    bits = offset = 0
    for name, width in fields:
        bits |= values.get(name, 0) << offset
        offset += width
    data[second_sector + 40:second_sector + 40] = bits.to_bytes(60, "little")
    return bytes(data) + struct.pack("<I", zlib.crc32(data))


def episode_ini(intro_level=2):
    return (f"[Episode1]\nTitle=Cinematic regression fixture\n"
            f"CutSceneA=INTRO.SMK\nCutSceneALevel={intro_level}\n"
            "CutSceneB=OUTRO.SMK\n"
            "Map1=NBCSTART\nMap2=NBCINTRO\nMap3=NBCEND\n\n"
            "[NBCSTART]\nTitle=Before the cinematic\nEndingA=2\nEndingB=2\n\n"
            "[NBCINTRO]\nTitle=Between the cinematics\nEndingA=3\nEndingB=3\n"
            "CutScene=MAP.SMK\n\n"
            "[NBCEND]\nTitle=Episode ending\nEndingA=0\nEndingB=0\n").encode()


def generate_ogv(path, seconds):
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                    "-f", "lavfi", "-i", "testsrc2=size=320x200:rate=20",
                    "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=22050",
                    "-t", str(seconds), "-c:v", "libtheora", "-q:v", "5",
                    "-c:a", "libvorbis", "-q:a", "2", "-threads", "1", str(path)], check=True)


def local_events(game, cinematic_id=None):
    result = records(game.text(), "NET cinematic local ")
    return [row for row in result if cinematic_id is None or row.get("id") == str(cinematic_id)]


def server_events(game, cinematic_id=None):
    result = [row for row in records(game.text(), "NET cinematic ") if "kind" in row]
    return [row for row in result if cinematic_id is None or row.get("id") == str(cinematic_id)]


def wait_local(game, cinematic_id, state, reason=None, timeout=45):
    def found(_):
        return any(row.get("state") == state and (reason is None or row.get("reason") == reason)
                   for row in local_events(game, cinematic_id))
    game.wait(found, f"cinematic {cinematic_id}: {state}/{reason}", timeout)
    return local_events(game, cinematic_id)


def is_finished(game, cinematic_id):
    return any(row.get("state") == "waiting" for row in local_events(game, cinematic_id))


def skip(game, cinematic_id, timeout):
    focus_window(game)
    started = time.monotonic()
    game.key(44, True)  # SDL_SCANCODE_SPACE, independent of gameplay bindings.
    try:
        time.sleep(2.6)
        require(not is_finished(game, cinematic_id), f"{game.name}: skipped before a three-second hold")
        wait_local(game, cinematic_id, "waiting", "skipped", timeout)
    finally:
        game.key(44, False)
    duration = time.monotonic() - started
    require(duration >= 2.9, f"{game.name}: Space skipped too early ({duration:.3f}s)")
    return duration


def focus_window(game):
    """Use real X11 focus events: losing focus must reset a Space hold."""
    tree = subprocess.check_output(["xwininfo", "-root", "-tree"], text=True)
    window = None
    for candidate in re.findall(r"^\s+(0x[0-9a-f]+) ", tree, re.MULTILINE):
        prop = subprocess.run(["xprop", "-id", candidate, "_NET_WM_PID"],
                              text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        if re.search(rf"=\s*{game.process.pid}\s*$", prop.stdout):
            window = int(candidate, 16)
            break
    require(window is not None, f"no X11 window for {game.name}")
    x11 = ctypes.CDLL("libX11.so.6")
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XRaiseWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    x11.XSetInputFocus.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
    x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    display = x11.XOpenDisplay(None)
    require(display, "cannot open X11 display for capture")
    try:
        x11.XRaiseWindow(display, window)
        x11.XSetInputFocus(display, window, 2, 0)  # RevertToParent, CurrentTime.
        x11.XSync(display, 0)
        time.sleep(0.15)
    finally:
        x11.XCloseDisplay(display)
    return window


def capture_window(game, path):
    """Capture an ordinary X11 game window without injecting a render command."""
    from PIL import Image

    window = focus_window(game)
    subprocess.run(["import", "-window", hex(window), str(path)], check=True)
    image = Image.open(path).convert("RGB")
    pixels = list(image.get_flattened_data() if hasattr(image, "get_flattened_data") else image.getdata())
    return {"path": str(path), "width": image.width, "height": image.height,
            "black_fraction": sum(max(pixel) < 12 for pixel in pixels) / len(pixels),
            "white_pixels": sum(min(pixel) > 200 and max(pixel)-min(pixel) < 12 for pixel in pixels),
            "colored_pixels": sum(max(pixel)-min(pixel) > 40 for pixel in pixels)}


def main():
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=repository / "nblood")
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--engine-data", type=Path, default=repository)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--renderer", type=int, choices=(0, 3, 4, 5), default=0)
    parser.add_argument("--skip-screenshots", action="store_true",
                        help="omit ImageMagick/Pillow pixel checks")
    args = parser.parse_args()
    for name in ("binary", "assets", "engine_data"):
        setattr(args, name, getattr(args, name).resolve())
    require(os.environ.get("DISPLAY"), "clients require an X11 DISPLAY (Xvfb is suitable)")
    smacker = next((path for path in args.assets.iterdir() if path.name.lower() == "cryptic.smk"), None)
    require(smacker is not None, "supply original CRYPTIC.SMK in the asset directory")
    require(shutil.which("ffmpeg"), "ffmpeg with Theora/Vorbis encoders is required")
    for tool in ("xwininfo", "xprop") + (() if args.skip_screenshots else ("import",)):
        require(shutil.which(tool), f"{tool} is required for X11 input/capture checks")
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="netblood-cinematics-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print(f"Cinematic evidence: {root}", flush=True)
    source_binary = args.binary
    args.binary = root / "nblood-tested"
    shutil.copy2(source_binary, args.binary)
    library = build_driver(root)
    long_ogv, short_ogv = root / "long.ogv", root / "short.ogv"
    generate_ogv(long_ogv, 40)
    generate_ogv(short_ogv, 5)
    fixtures = {"NBCINE.INI": episode_ini(), "NBCSTART.MAP": exit_map(0),
                "NBCINTRO.MAP": exit_map(1), "NBCEND.MAP": exit_map(2)}
    games = []
    result = {"passed": False, "source_binary": str(source_binary),
              "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
              "phases": {}, "failures": []}

    def launch(name, port, role="client", movies=None, initial_intro=False):
        files = dict(fixtures)
        files.update(movies or {})
        if initial_intro:
            files["NBCINE.INI"] = episode_ini(1)
        options = ["-ini", "NBCINE.INI"]
        if role == "dedicated":
            options += ["-dedicated", "-port", str(port), "-gametype", "coop", "-maxplayers", "4", "-episode", "1"]
        elif role == "listen":
            options += ["-server", "4", "-port", str(port), "-gametype", "coop"]
        else:
            options += ["-client", "127.0.0.1", "-port", str(port)]
        game = Game(name, args, root, library, port, dedicated=role == "dedicated",
                    initial_files=files, launch_arguments=options)
        games.append(game)
        return game

    def ready(game):
        game.wait(lambda text: "NET client slot=" in text, "initial authoritative snapshot", args.timeout)

    def started(host, kind, after=0):
        host.wait(lambda _: any(row.get("kind") == kind and row.get("state") == "active"
                                and int(row["id"]) > after for row in server_events(host)),
                  f"{kind} cinematic after {after}", args.timeout)
        return next(int(row["id"]) for row in server_events(host)
                    if row.get("kind") == kind and row.get("state") == "active" and int(row["id"]) > after)

    def ended(host, cinematic_id):
        # Consecutive scenes advance directly to the next ID without exposing
        # a gameplay frame or an intermediate inactive barrier.
        host.wait(lambda _: any(row.get("state") == "end" and int(row["id"]) == cinematic_id
                                or int(row["id"]) > cinematic_id for row in server_events(host)),
                  f"all players finish cinematic {cinematic_id}", args.timeout)

    def walk_exit(game, host):
        before = len(records(host.text(), "NET map generation="))
        before_scenes = len(server_events(host))
        focus_window(game)
        game.key(26, True)
        try:
            host.wait(lambda _: len(records(host.text(), "NET map generation=")) > before or
                                len(server_events(host)) > before_scenes,
                      "ordinary level exit", args.timeout)
        finally:
            game.key(26, False)

    def assert_frozen(host):
        host.command_text("net_status")
        before = records(host.text(), "NET status ")[-1]
        time.sleep(0.6)
        host.command_text("net_status")
        after = records(host.text(), "NET status ")[-1]
        require(before["tick"] == after["tick"], "world simulation advanced during a cinematic")
        return before["tick"]

    try:
        print("Dedicated: natural exits, SMK/OGV/missing peers, personal skip and hot joins", flush=True)
        port = free_port()
        host = launch("server", port, "dedicated")
        host.wait(lambda text: "Dedicated server running:" in text, "dedicated startup", args.timeout)
        first = launch("client-1", port, movies={"INTRO.SMK": smacker})
        ready(first)
        second = launch("client-ogv", port, movies={"INTRO.ogv": long_ogv, "MAP.ogv": short_ogv,
                                                   "OUTRO.ogv": short_ogv})
        ready(second)
        missing = launch("client-missing", port)
        ready(missing)
        first.command_text("net_record cinematics.nbpd")
        walk_exit(first, host)
        intro = started(host, "intro")
        wait_local(first, intro, "playing", timeout=args.timeout)
        wait_local(second, intro, "playing", timeout=args.timeout)
        wait_local(missing, intro, "waiting", "missing", args.timeout)
        require("format=smk" in first.text().lower(), "original Smacker movie was not decoded")
        require("format=ogv" in second.text().lower(), "SMK reference did not find the local OGV replacement")
        phase = {"intro": intro, "frozen_tick": assert_frozen(host)}
        if not args.skip_screenshots:
            phase["missing_screenshot"] = capture_window(missing, root / "waiting.png")
            require(phase["missing_screenshot"]["black_fraction"] > 0.9 and
                    phase["missing_screenshot"]["white_pixels"] > 100,
                    "missing cinematic did not show white text on a black screen")
            phase["ogv_screenshot"] = capture_window(second, root / "ogv.png")
            require(phase["ogv_screenshot"]["colored_pixels"] > 10000, "OGV frames were not visible")
        # Neither another key nor separate short holds may skip the movie.
        focus_window(first)
        first.key(4, True)  # A: ordinary unrelated input must not skip a movie.
        first.key(4, False)
        for _ in range(2):
            first.key(44, True)
            time.sleep(1.65)
            first.key(44, False)
            time.sleep(0.15)
            require(not is_finished(first, intro), "a partial/released hold skipped the cinematic")
        phase["first_skip_seconds"] = skip(first, intro, args.timeout)
        require(not is_finished(second, intro), "one player's Space hold skipped another player's movie")
        require(not any(row.get("state") == "end" for row in server_events(host, intro)),
                "server released barrier while another player was watching")
        late = launch("client-during", port, movies={"INTRO.ogv": long_ogv})
        wait_local(late, intro, "playing", timeout=args.timeout)
        phase["second_skip_seconds"] = skip(second, intro, args.timeout)
        require(not is_finished(late, intro), "hot join did not extend the cinematic barrier")
        phase["frozen_after_skip"] = assert_frozen(host)
        # The ordinary console disconnect must work even without a first
        # gameplay snapshot: this player joined while the world was paused.
        focus_window(late)
        late.stop()
        require(not late.forced, "watching client could not disconnect normally")
        ended(host, intro)
        map_scene = started(host, "map", intro)
        wait_local(second, map_scene, "playing", timeout=args.timeout)
        wait_local(second, map_scene, "waiting", "completed", args.timeout)
        ended(host, map_scene)
        phase["map"] = map_scene
        after = launch("client-after", port, movies={"INTRO.SMK": smacker})
        ready(after)
        time.sleep(0.4)
        require(not local_events(after), "a join after completion replayed an old cinematic")
        after.stop()
        # Finish the next map and then the episode using its actual exit sectors.
        walk_exit(first, host)
        time.sleep(0.7)
        walk_exit(first, host)
        outro = started(host, "outro", map_scene)
        wait_local(second, outro, "playing", timeout=args.timeout)
        wait_local(first, outro, "waiting", "missing", args.timeout)
        wait_local(second, outro, "waiting", "completed", args.timeout)
        ended(host, outro)
        phase["outro"] = outro
        time.sleep(1)
        first.command_text("net_stoprecord\nnet_status")
        demo = inspect_demo(first.path / "cinematics.nbpd")
        require(all(row["type"] not in (19, 20) for row in demo["packets"]),
                "offline packet demo contains a remote cinematic barrier")
        phase["packet_demo"] = {key: value for key, value in demo.items() if key != "packets"}
        roundtrips = records(first.text(), "NET snapshot roundtrip ")
        require(len(roundtrips) >= 10 and all(row.get("equal") == "1" for row in roundtrips),
                "cinematic transitions changed an installed authoritative snapshot")
        phase["exact_snapshot_roundtrips"] = len(roundtrips)
        for game in (first, second, missing, host):
            game.stop()
        replay = Game("playback", args, root, library, playback=True,
                      initial_files=fixtures, launch_arguments=["-ini", "NBCINE.INI", "-map", "NBCSTART"])
        games.append(replay)
        replay.wait(lambda text: "> This map" in text, "offline playback setup", args.timeout)
        time.sleep(0.5)
        phase["playback"] = play_demo("playback", demo, args, root, library, games, existing=replay)
        require(not local_events(replay), "offline playback waited for remote movie watchers")
        result["phases"]["dedicated"] = phase

        print("Initial empty server: defer intro until a real player joins", flush=True)
        port = free_port()
        empty = launch("empty-server", port, "dedicated", initial_intro=True)
        empty.wait(lambda text: "Dedicated server running:" in text, "empty server startup", args.timeout)
        time.sleep(1)
        require(not any(row.get("state") == "end" for row in server_events(empty)),
                "initial cinematic was consumed before any player joined")
        watcher = launch("empty-first-client", port, movies={"INTRO.ogv": short_ogv}, initial_intro=True)
        initial_id = started(empty, "intro")
        wait_local(watcher, initial_id, "playing", timeout=args.timeout)
        damaged_smk = bytearray(104)
        damaged_smk[:4] = b"SMK2"
        struct.pack_into("<II", damaged_smk, 4, 320, 200)
        # An invalid zero-frame Smacker must behave like unavailable media,
        # without preventing other players from finishing the real movie.
        damaged = launch("empty-damaged-client", port,
                         movies={"INTRO.SMK": bytes(damaged_smk)}, initial_intro=True)
        wait_local(damaged, initial_id, "waiting", "missing", args.timeout)
        wait_local(watcher, initial_id, "waiting", "completed", args.timeout)
        ended(empty, initial_id)
        ready(watcher)
        ready(damaged)
        result["phases"]["empty_server"] = {"intro": initial_id, "events": server_events(empty)}
        damaged.stop()
        watcher.stop()
        empty.stop()

        print("Listen server: local host and remote client participate independently", flush=True)
        port = free_port()
        listen = launch("listen-server", port, "listen", {"INTRO.SMK": smacker}, initial_intro=True)
        listen_id = started(listen, "intro")
        wait_local(listen, listen_id, "playing", timeout=args.timeout)
        remote = launch("listen-remote", port, movies={"INTRO.ogv": long_ogv}, initial_intro=True)
        wait_local(remote, listen_id, "playing", timeout=args.timeout)
        host_skip = skip(listen, listen_id, args.timeout)
        require(not is_finished(remote, listen_id), "listen host skipped the remote player's cinematic")
        remote_skip = skip(remote, listen_id, args.timeout)
        ended(listen, listen_id)
        ready(remote)
        result["phases"]["listen"] = {"intro": listen_id, "host_skip_seconds": host_skip,
                                        "remote_skip_seconds": remote_skip}
        remote.stop()
        listen.stop()
        result["passed"] = True
    except Exception as error:
        result["failures"].append(str(error))
        # Keep transport counters and barrier state when an integration phase
        # fails; this distinguishes delivery failures from decoding/UI bugs.
        for game in games:
            if game.process.poll() is None:
                try:
                    game.command_text("net_status", timeout=2)
                except Exception:
                    pass
        raise
    finally:
        for game in reversed(games):
            game.stop()
        result["games"] = {game.name: game.result() for game in games}
        if result["passed"]:
            for game in games:
                if game.forced or game.process.returncode != 0:
                    result["passed"] = False
                    result["failures"].append(f"{game.name}: unclean shutdown")
                if game.dedicated and "NETWORK_TEST_DEVICES windows=0 audio=0" not in game.console():
                    result["passed"] = False
                    result["failures"].append(f"{game.name}: missing headless device audit")
                if re.search(r"NET (?:apply snapshot|capture failed|invalid bootstrap|local map CRC mismatch|"
                             r"snapshot verification failed)|AddressSanitizer|runtime error:|Segmentation fault",
                             game.text() + game.console()):
                    result["passed"] = False
                    result["failures"].append(f"{game.name}: runtime or snapshot validation error")
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    require(result["passed"], "; ".join(result["failures"]))
    print(f"PASS: synchronized co-op SMK/OGV cinematics; evidence {root / 'result.json'}", flush=True)


if __name__ == "__main__":
    main()
