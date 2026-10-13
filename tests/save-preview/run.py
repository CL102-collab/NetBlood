#!/usr/bin/env python3
"""Exercise clean save thumbnails through real save/load menus and renderers.

Requires Blood 1.21 assets, X11/Xvfb, Pillow and ImageMagick. Saving, loading
and camera movement use ordinary SDL input. The generated gray test room has
no actors or animation, so unchanged-camera previews must be byte-identical
despite visible HUD, crosshair, message and first-person weapon changes.
"""

import argparse
import copy
from functools import lru_cache
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "network"))
from cinematics import capture_window, focus_window
from run import Failure, build_driver, records, require
from scoreboard import Game


def room():
    data = bytearray(45)
    data[:4] = b"BLM\x1a"
    struct.pack_into("<H", data, 4, 0x600)
    struct.pack_into("<HHH", data, 6 + 0x1f, 1, 4, 0)
    sector = bytearray(40)
    struct.pack_into("<HHii", sector, 0, 0, 4, -32768, 0)
    struct.pack_into("<h", sector, 16, 6005)
    struct.pack_into("<h", sector, 24, 6004)
    struct.pack_into("<h", sector, 38, -1)
    data += sector
    for index, (x, y) in enumerate(((-8192, -8192), (8192, -8192), (8192, 8192), (-8192, 8192))):
        wall = bytearray(32)
        struct.pack_into("<iiHhhHhh", wall, 0, x, y, (index + 1) % 4, -1, -1, 0, 6000 + index, -1)
        wall[22:24] = bytes((8, 8))
        struct.pack_into("<h", wall, 30, -1)
        data += wall
    return bytes(data) + struct.pack("<I", zlib.crc32(data))


def art():
    # Every texel uses the base palette's gray ramp. World-only rendering must
    # not introduce HUD, pitchfork, menu backdrop or blood QAV colors, though
    # shaded gray RGB values can quantize to slightly colored palette entries.
    data = bytearray(struct.pack("<4I", 1, 6, 6000, 6005))
    data += struct.pack("<12H", *([16] * 12))
    data += bytes(6 * 4)
    for tile in range(6):
        for x in range(16):
            for y in range(16):
                data.append((8 + tile * 3) if ((x // 4 + y // 4) % 2) else (4 + tile * 2))
    return bytes(data)


def base_palette(assets):
    """Read Blood's supplied palette solely to export the indexed preview."""
    path = next(path for path in assets.iterdir() if path.name.upper() == "BLOOD.RFF")
    data = path.read_bytes()
    require(data[:4] == b"RFF\x1a", "game data has an invalid RFF header")
    version, offset, count = struct.unpack_from("<H2xII", data, 4)
    directory = bytearray(data[offset:offset + count * 48])
    require(len(directory) == count * 48, "truncated resource directory")
    if version & 0xff00 == 0x300:
        key = (offset + (version & 255) * offset) & 65535
        for index in range(len(directory)):
            directory[index] ^= (((key + index) & 65535) >> 1) & 255
    for index in range(count):
        entry = directory[index * 48:(index + 1) * 48]
        if entry[33:36] != b"PAL" or entry[36:44].rstrip(b"\0") != b"BLOOD":
            continue
        begin, length = struct.unpack_from("<II", entry, 16)
        palette = bytearray(data[begin:begin + length])
        if entry[32] & 16:
            for position in range(min(256, len(palette))):
                palette[position] ^= position >> 1
        require(len(palette) == 768, "base palette has the wrong size")
        return bytes(palette)
    raise Failure("BLOOD.PAL not found in supplied game data")


def save_metadata(data):
    """Decode only the independently checksummed, optional save stats block."""
    start = max(0, len(data) - 64072)
    matches = []
    while True:
        offset = data.find(b"NBSTAT01", start)
        if offset < 0:
            break
        start = offset + 1
        block = data[offset:offset + 48]
        if len(block) != 48:
            continue
        version, size, utc, elapsed, kills, total_kills, secrets, total_secrets, crc = struct.unpack_from("<IIQI4iI", block, 8)
        if (version, size) != (1, 48) or zlib.crc32(block[:44]) != crc:
            continue
        matches.append(dict(offset=offset, utc_seconds=utc, elapsed_seconds=elapsed,
                            kills=kills, total_kills=total_kills, secrets=secrets,
                            total_secrets=total_secrets,
                            utc_text=time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(utc)),
                            local_text=time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(utc))))
    require(len(matches) <= 1, "ambiguous save metadata blocks")
    return matches[0] if matches else None


@lru_cache(maxsize=4)
def gray_palette_indices(palette):
    """Palette results reachable from gray RGB, including nearest-color ties."""
    colors = [tuple(palette[index:index + 3]) for index in range(0, 255 * 3, 3)]
    reachable = set()
    for gray in range(256):
        # screen.cpp initializes colmatch.cpp with these squared-distance
        # weights; save previews exclude transparent palette index 255.
        distances = [30 * (red - gray) ** 2 + 59 * (green - gray) ** 2
                     + 11 * (blue - gray) ** 2 for red, green, blue in colors]
        nearest = min(distances)
        reachable.update(index for index, distance in enumerate(distances) if distance == nearest)
    return frozenset(reachable)


def preview(path, palette, png, gray=True):
    from PIL import Image

    data = path.read_bytes()
    require(len(data) >= 64024 and data[-24:-16] == b"NBPREV01", f"{path}: save omitted the preview trailer")
    version, width, height, size, checksum = struct.unpack_from("<IHHII", data, len(data) - 16)
    require((version, width, height, size) == (1, 320, 200, 64000), "incorrect preview format/dimensions")
    pixels = data[-64024:-24]
    require(zlib.crc32(pixels) == checksum, "preview CRC mismatch")
    row_major = bytes(pixels[x * height + y] for y in range(height) for x in range(width))
    image = Image.frombytes("P", (width, height), row_major)
    image.putpalette(palette)
    image = image.convert("RGB")
    image.save(png)
    if gray:
        reachable = gray_palette_indices(palette)
        unexpected = sum(pixel not in reachable for pixel in pixels)
        require(unexpected == 0,
                f"{path}: world-only gray fixture contains {unexpected} palette pixels unreachable from grayscale")
    require(len(set(pixels)) >= 5, "preview is blank or does not contain the patterned world")
    return {"path": str(path), "image": str(png), "crc32": f"{checksum:08x}",
            "sha256": hashlib.sha256(pixels).hexdigest(), "bytes": len(data),
            "world_body_bytes": len(data) - 64024 - (48 if save_metadata(data) else 0),
            "metadata": save_metadata(data), "unique_colors": len(set(pixels))}, pixels


def menu_preview_similarity(screenshot, preview_image):
    """Compare the displayed thumbnail's geometry independently of gamma."""
    from PIL import Image

    actual = Image.open(screenshot["path"]).convert("L")
    sx, sy = actual.width / 320, actual.height / 200
    actual = actual.crop((round(20 * sx), round(45 * sy), round(180 * sx), round(145 * sy)))
    # Menu tiles are scaled from 320x200 into a 320x240 physical rectangle.
    # Compare low-frequency geometry so renderer-specific texture filtering of
    # the fixture's checker pattern does not look like a stale thumbnail.
    actual = actual.resize((64, 40), Image.Resampling.BOX)
    wanted = Image.open(preview_image).convert("L").resize((64, 40), Image.Resampling.BOX)
    a = list(actual.get_flattened_data() if hasattr(actual, "get_flattened_data") else actual.getdata())
    b = list(wanted.get_flattened_data() if hasattr(wanted, "get_flattened_data") else wanted.getdata())
    am, bm = sum(a) / len(a), sum(b) / len(b)
    aa = sum((value - am) ** 2 for value in a)
    bb = sum((value - bm) ** 2 for value in b)
    return sum((x - am) * (y - bm) for x, y in zip(a, b)) / max((aa * bb) ** 0.5, 1)


def menu_font(assets):
    """Read the native small menu font for exact digit checks (no general OCR)."""
    from PIL import Image

    glyphs = {}
    for path in assets.iterdir():
        if path.suffix.upper() != ".ART":
            continue
        data = path.read_bytes()
        _, _, first, last = struct.unpack_from("<4I", data)
        if last < 4384 or first >= 4480:
            continue
        count = last - first + 1
        widths = struct.unpack_from(f"<{count}H", data, 16)
        heights = struct.unpack_from(f"<{count}H", data, 16 + count * 2)
        offset = 16 + count * 8
        for index, (width, height) in enumerate(zip(widths, heights)):
            pixels = data[offset:offset + width * height]
            offset += width * height
            if 4384 <= first + index < 4480:
                # Blood's small font uses palette 24 for its light face, 0 for
                # its black outline, and 255 for transparent background.
                mask = bytes(255 if pixels[x * height + y] == 24 else 0
                             for y in range(height) for x in range(width))
                glyphs[chr(first + index - 4384 + 32)] = Image.frombytes("L", (width, height), mask)
    require(all(char in glyphs for char in "0123456789UNKNOWN"), "native menu font is unavailable")
    return glyphs


def menu_text(screenshot, text, x, y, font):
    """Recognize each expected digit independently against all ten glyphs."""
    from PIL import Image

    image = Image.open(screenshot["path"]).convert("L")
    sx, sy = image.width / 320, image.height / 200
    evidence = {"expected": text, "characters": []}
    for char in text:
        wanted = font[char]
        if char.isdigit() or ("UNKNOWN" in text and char.isalpha()):
            scores = []
            for candidate in ("0123456789" if char.isdigit() else char):
                glyph = font[candidate]
                glyph = glyph.resize((round(glyph.width * sx), round(glyph.height * sy)), Image.Resampling.NEAREST)
                pixels = glyph.get_flattened_data() if hasattr(glyph, "get_flattened_data") else glyph.getdata()
                expected = [value > 0 for value in pixels]
                best = 0
                for dx in range(-2, 4):
                    for dy in range(-4, 3):
                        left, top = round(x * sx) + dx, round(y * sy) + dy
                        crop = image.crop((left, top, left + glyph.width, top + glyph.height))
                        pixels = crop.get_flattened_data() if hasattr(crop, "get_flattened_data") else crop.getdata()
                        actual = [value >= 150 for value in pixels]
                        union = sum(a or b for a, b in zip(actual, expected))
                        score = sum(a and b for a, b in zip(actual, expected)) / max(union, 1)
                        best = max(best, score)
                scores.append((best, candidate))
            scores.sort(reverse=True)
            evidence["characters"].append(dict(expected=char, recognized=scores[0][1], score=scores[0][0]))
        x += wanted.width + 1
    return evidence


def restart(previous, name, library, root):
    """Preserve the real config, saves and working directory across processes."""
    require(previous.process.poll() == 0 and previous.events is None, "restart requires a clean first exit")
    directory = previous.path
    archive = root / (previous.name + "-previous-process")
    archive.mkdir()
    for filename in ("nblood.log", "console.log", "network.cfg", "network_cvars.cfg"):
        path = directory / filename
        if path.is_file():
            shutil.copy2(path, archive / filename)
    game = copy.copy(previous)
    previous.path = archive
    game.name, game.path = name, directory
    game.command_serial, game.forced = 0, False
    game.events = os.open(game.fifo, os.O_RDWR | os.O_NONBLOCK)
    (directory / "nblood.log").unlink(missing_ok=True)
    environment = os.environ.copy()
    environment.update(SDL_AUDIODRIVER="dummy", NETWORK_TEST_EVENTS=str(game.fifo),
                       LD_PRELOAD=str(library) + (":" + environment["LD_PRELOAD"] if environment.get("LD_PRELOAD") else ""))
    game.log = (directory / "console.log").open("w")
    game.process = subprocess.Popen(game.command, cwd=directory, env=environment,
                                    stdin=subprocess.PIPE, stdout=game.log, stderr=subprocess.STDOUT)
    return game


def main():
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=repository / "nblood")
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--engine-data", type=Path, default=repository)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--renderers", type=int, choices=(0, 3, 5), nargs="+", default=[0, 3, 5])
    parser.add_argument("--legacy-save", type=Path, help="optional genuine game0000.sav made by the prior executable")
    parser.add_argument("--stats-only", action="store_true", help="run only real E1M1 counters and timezone restart checks")
    parser.add_argument("--timezone", default="EST5", help="non-UTC POSIX timezone used for menu date checks")
    args = parser.parse_args()
    for name in ("binary", "assets", "engine_data"):
        setattr(args, name, getattr(args, name).resolve())
    require(os.environ.get("DISPLAY"), "run graphical tests under X11/Xvfb")
    require(args.binary.is_file() and args.assets.is_dir(), "supply a built executable and Blood data")
    for executable in ("xwininfo", "xprop", "import"):
        require(shutil.which(executable), f"{executable} is required for focus/screenshots")
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="netblood-save-preview-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print(f"Save-preview evidence: {root}", flush=True)
    source_binary = args.binary
    args.binary = root / "nblood-tested"
    shutil.copy2(source_binary, args.binary)
    library, palette = build_driver(root), base_palette(args.assets)
    font = menu_font(args.assets)
    original_timezone = os.environ.get("TZ")
    os.environ["TZ"] = args.timezone
    time.tzset()
    require(time.strftime("%z") not in ("+0000", "-0000"), "choose a non-UTC test timezone")
    files = {"NBPREV.MAP": room(), "TILES018.ART": art()}
    games = []
    result = {"passed": False, "source_binary": str(source_binary),
              "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
              "stats_only": args.stats_only,
              "timezone": args.timezone, "timezone_offset": time.strftime("%z"),
              "phases": {}, "failures": []}

    def tap(game, scan, seconds=0.12):
        focus_window(game)
        game.key(scan, True)
        game.input_barrier(args.timeout)
        time.sleep(seconds)
        game.key(scan, False)
        game.input_barrier(args.timeout)

    def started(game):
        game.wait(lambda text: "Level ready: map=" in text, "loaded offline map", args.timeout)
        # Ensure the first ordinary game iteration has consumed input before
        # opening a menu; texture precaching also pumps SDL events.
        game.command_text("echo SAVE_PREVIEW_WORLD_READY", args.timeout)
        time.sleep(0.6)

    def position(game):
        offset = len(game.text())
        game.command_text("net_status")
        rows = records(game.text()[offset:], "NET player ")
        require(len(rows) == 1, "offline player state missing")
        return {key: int(rows[0][key]) for key in ("x", "y", "z", "health", "weapon")}

    def level_seconds(game):
        offset = len(game.text())
        game.command_text("net_status")
        rows = records(game.text()[offset:], "NET intermission ")
        require(len(rows) == 1, "offline level time missing")
        return int(rows[0]["level_ticks"]) // 30

    def check_metadata(path, before, counters=(0, 0, 0, 0)):
        metadata = save_metadata(path.read_bytes())
        require(metadata is not None, f"{path}: save omitted valid statistics")
        require(before["utc"] <= metadata["utc_seconds"] <= int(time.time()),
                f"{path}: timestamp is not current UTC Unix seconds")
        require(before["elapsed"] <= metadata["elapsed_seconds"] <= level_seconds(before["game"]),
                f"{path}: elapsed time differs from actual simulation time")
        actual = tuple(metadata[key] for key in ("kills", "total_kills", "secrets", "total_secrets"))
        require(actual == counters, f"{path}: expected actual world counters {counters}, got {actual}")
        return metadata

    def check_menu_metadata(phase, label, shot, expected):
        if expected is None:
            fields = [("SAVED: UNKNOWN", 20, 181), ("TIME: UNKNOWN", 196, 155),
                      ("KILLS: UNKNOWN", 20, 168), ("SECRETS: UNKNOWN", 196, 168)]
        else:
            seconds = expected["elapsed_seconds"]
            fields = [("SAVED: " + expected["local_text"] + " (LOCAL)", 20, 181),
                      (f"TIME: {seconds // 3600:02d}:{seconds // 60 % 60:02d}:{seconds % 60:02d}", 196, 155),
                      (f"KILLS: {expected['kills']} / {expected['total_kills']}", 20, 168),
                      (f"SECRETS: {expected['secrets']} / {expected['total_secrets']}", 196, 168)]
            require(expected["local_text"] != expected["utc_text"], "timezone check did not differ from UTC")
        # The menu's animated blood QAV is drawn after the text and may cover
        # a digit for one frame. Retry the unchanged menu briefly; never relax
        # the required value or glyph match to accommodate an occlusion.
        for attempt in range(5):
            evidence = [menu_text(shot, text, x, y, font) for text, x, y in fields]
            phase.setdefault("menu_metadata", {})[label] = evidence
            failures = [(field["expected"], char) for field in evidence for char in field["characters"]
                        if char["score"] <= 0.65 or char["recognized"] != char["expected"]]
            if not failures:
                return
            phase.setdefault("menu_metadata_retries", {}).setdefault(label, []).append(
                dict(screenshot=shot, failures=failures))
            if attempt < 4:
                time.sleep(0.35)
                shot = capture_window(games[-1], root / f"{label}-text-retry-{attempt + 1}.png")
        raise Failure(f"{label}: menu field contains wrong/missing glyphs: {failures}")

    def select(game, menu_scan, slot):
        tap(game, menu_scan)
        tap(game, 75)  # PageUp selects the first slot, independent of prior focus.
        for _ in range(slot):
            tap(game, 81)

    def save(game, slot, name=None, counters=(0, 0, 0, 0)):
        before = dict(utc=int(time.time()), elapsed=level_seconds(game), game=game)
        offset = len(game.text())
        select(game, 59, slot)  # F2
        tap(game, 40)
        if name is not None:
            for _ in range(16):
                tap(game, 42)
            for char in name:
                tap(game, 4 + ord(char) - ord("a"))
        tap(game, 40)
        game.wait(lambda text: "Game saved" in text[offset:], "completed menu save", args.timeout)
        time.sleep(0.4)
        path = game.path / f"game00{slot:02d}.sav"
        check_metadata(path, before, counters)
        return path

    def capture(game, name):
        time.sleep(0.2)
        return capture_window(game, root / (name + ".png"))

    def after_load(game, wanted):
        tap(game, 40)
        game.wait(lambda text: game.process.poll() is None, "loaded game", args.timeout)
        time.sleep(0.5)
        actual = position(game)
        require(all(actual[key] == wanted[key] for key in ("x", "y", "z")),
                f"save/load changed the saved location: expected {wanted}, got {actual}")
        return actual

    def show_load(game, slot, name):
        select(game, 60, slot)  # F3
        return capture(game, name)

    def check_display(phase, label, shot, expected, positive=True):
        similarity = menu_preview_similarity(shot, expected)
        phase.setdefault("display_similarities", {})[label] = similarity
        require(similarity > 0.95 if positive else similarity < 0.9,
                f"{label}: unexpected displayed-preview similarity {similarity:.6f}")

    try:
        if not args.stats_only:
            for renderer in args.renderers:
                name = {0: "software", 3: "polymost", 5: "vulkan"}[renderer]
                args.renderer = renderer
                game = Game(name, args, root, library, playback=True, launch_map="NBPREV.MAP", initial_files=files)
                games.append(game)
                started(game)
                game.command_text('r_size 0\ncrosshair 0\nhud_messages 0\nhud_stats 0\nhud_showmaptitle 0\n'
                                  'bind H "gamefunc_Holster_Weapon"\nbind T "gamefunc_Send_Message"\n'
                                  'bind L "gamefunc_Turn_Right"')
                # Startup may still be raising the pitchfork when the first input
                # arrives on a busy software-rendering worker. Observe completion,
                # then retry the normal holster action once if it was ignored.
                for _ in range(2):
                    tap(game, 11)
                    deadline = time.monotonic() + 4
                    while True:
                        initial = position(game)
                        if initial["weapon"] == 0 or time.monotonic() >= deadline:
                            break
                        time.sleep(0.25)
                    if initial["weapon"] == 0:
                        break
                require(initial["weapon"] == 0, "reference still has a first-person weapon")
                phase = {"initial_position": initial, "clean_world": capture(game, name + "-clean-world")}
                result["phases"][name] = phase
                first = save(game, 0, "clean")
                clean_meta, clean_pixels = preview(first, palette, root / (name + "-preview-clean.png"))
                phase["clean_preview"] = clean_meta

                game.command_text("r_size 7\ncrosshair 1\nhud_messages 1\nhud_stats 2")
                tap(game, 11)
                time.sleep(1.1)
                tap(game, 23)
                for char in "overlay text":
                    tap(game, 44 if char == " " else 4 + ord(char) - ord("a"))
                tap(game, 40)
                phase["visible_hud"] = capture(game, name + "-visible-hud")
                busy = save(game, 5, "busy")
                busy_meta, busy_pixels = preview(busy, palette, root / (name + "-preview-busy.png"))
                phase["busy_preview"] = busy_meta
                require(busy_meta["metadata"]["elapsed_seconds"] > clean_meta["metadata"]["elapsed_seconds"],
                        "distinct slots did not capture progressing level time")
                require(busy_pixels == clean_pixels,
                        f"{name}: HUD/crosshair/text/weapon/view-size changes altered the unchanged-camera preview")
                phase["save_load_menu"] = show_load(game, 5, name + "-load-menu")
                check_display(phase, name + "-load-selected", phase["save_load_menu"], busy_meta["image"])
                check_menu_metadata(phase, name + "-load-selected", phase["save_load_menu"], busy_meta["metadata"])
                tap(game, 41)
                print(f"PASS: {name} previews exclude the HUD, text, crosshair, menu and weapon", flush=True)

                game.command_text("r_size 2")
                tap(game, 15, 0.6)  # L turns the real camera to another wall pattern.
                tap(game, 26, 0.3)  # W moves the player through normal movement.
                time.sleep(0.6)
                changed_position = position(game)
                save(game, 0)
                overwrite_meta, overwrite_pixels = preview(first, palette, root / (name + "-preview-overwrite.png"))
                require(overwrite_pixels != clean_pixels, f"{name}: overwrite retained the previous preview")
                tap(game, 15, 0.5)
                tap(game, 26, 0.3)
                time.sleep(0.6)
                saved_position = position(game)
                before_quicksave = dict(utc=int(time.time()), elapsed=level_seconds(game), game=game)
                offset = len(game.text())
                tap(game, 63)  # F6 quicksave overwrites the selected normal save.
                game.wait(lambda text: "Game saved" in text[offset:], "completed quicksave", args.timeout)
                check_metadata(first, before_quicksave)
                quick_meta, quick_pixels = preview(first, palette, root / (name + "-preview-quicksave.png"))
                require(quick_pixels != overwrite_pixels, f"{name}: quicksave retained an old preview")
                require(quick_meta["metadata"]["utc_seconds"] > overwrite_meta["metadata"]["utc_seconds"]
                        > clean_meta["metadata"]["utc_seconds"], "overwrite/quicksave did not refresh UTC timestamp")
                phase.update(overwrite=overwrite_meta, quicksave=quick_meta,
                             overwrite_position=changed_position, saved_position=saved_position)
                phase["quicksave_menu"] = show_load(game, 0, name + "-quicksave-menu")
                check_display(phase, name + "-quicksave-current", phase["quicksave_menu"], quick_meta["image"])
                check_menu_metadata(phase, name + "-quicksave-current", phase["quicksave_menu"], quick_meta["metadata"])
                check_display(phase, name + "-quicksave-not-old-slot", phase["quicksave_menu"], clean_meta["image"], False)
                tap(game, 81)
                phase["empty_slot"] = capture(game, name + "-empty-slot")
                check_display(phase, name + "-empty-placeholder", phase["empty_slot"], quick_meta["image"], False)
                tap(game, 41)
                original_save = first.read_bytes()
                if args.legacy_save:
                    # A backup-looking filename must not overwrite slot zero's
                    # title/cache when startup scans game*.sav files. This file's
                    # embedded slot is zero, but its title/image are an old save.
                    backup = game.path / "gamezzbackup.sav"
                    backup.write_bytes(args.legacy_save.read_bytes())
                    phase["ignored_backup"] = str(backup)
                game.stop()
                game = restart(game, name + "-restart", library, root)
                games.append(game)
                started(game)
                require(first.read_bytes() == original_save, "restart changed the saved data")
                phase["restart_nonzero_slot"] = show_load(game, 5, name + "-restart-slot-five")
                check_display(phase, name + "-restart-slot-five", phase["restart_nonzero_slot"], busy_meta["image"])
                check_menu_metadata(phase, name + "-restart-slot-five", phase["restart_nonzero_slot"], busy_meta["metadata"])
                tap(game, 75)
                phase["restart_latest_slot"] = capture(game, name + "-restart-slot-zero")
                check_display(phase, name + "-restart-slot-zero", phase["restart_latest_slot"], quick_meta["image"])
                check_menu_metadata(phase, name + "-restart-slot-zero", phase["restart_latest_slot"], quick_meta["metadata"])
                phase["loaded_position"] = after_load(game, saved_position)
                phase["loaded_world"] = capture(game, name + "-loaded-world")
                game.stop()
                result["phases"][name] = phase
                print(f"PASS: {name} overwrite, quicksave, restart and actual state loading", flush=True)

            # Optional thumbnail and statistics are independent. Damaging one must
            # neither fabricate the other nor prevent the unchanged state loading.
            args.renderer = args.renderers[-1]
            stats_offset = quick_meta["metadata"]["offset"]
            corrupt_stats = bytearray(original_save)
            corrupt_stats[stats_offset + 44] ^= 1
            preview_only = original_save[:stats_offset] + original_save[stats_offset + 48:]
            for label, save_bytes, wanted, expected_metadata, has_preview in (
                    ("corrupt-preview", original_save[:-1] + bytes([original_save[-1] ^ 1]),
                     saved_position, quick_meta["metadata"], False),
                    ("corrupt-metadata", bytes(corrupt_stats), saved_position, None, True),
                    ("preview-without-metadata", preview_only, saved_position, None, True),
                    ("stats-without-preview", original_save[:stats_offset + 48],
                     saved_position, quick_meta["metadata"], False),
                    ("old-save", args.legacy_save.read_bytes() if args.legacy_save else original_save[:stats_offset],
                     None if args.legacy_save else saved_position, None, False)):
                initial = dict(files)
                initial["game0000.sav"] = save_bytes
                game = Game(label, args, root, library, playback=True, launch_map="NBPREV.MAP", initial_files=initial)
                games.append(game)
                started(game)
                before_load = position(game)
                phase = {"bytes": len(save_bytes), "save_sha256": hashlib.sha256(save_bytes).hexdigest(),
                         "placeholder": show_load(game, 0, label + "-placeholder")}
                result["phases"][label] = phase
                check_display(phase, label + "-fallback", phase["placeholder"], quick_meta["image"], has_preview)
                check_menu_metadata(phase, label + "-metadata", phase["placeholder"], expected_metadata)
                tap(game, 40)
                time.sleep(0.7)
                actual = position(game)
                if wanted is not None:
                    require(all(actual[key] == wanted[key] for key in ("x", "y", "z")),
                            f"{label}: optional preview failure broke state loading")
                else:
                    require(actual["health"] > 0, "legacy save did not load a live player")
                    require(any(actual[key] != before_load[key] for key in ("x", "y", "z")),
                            "legacy save did not replace the initial fixture-world position")
                phase["loaded_position"] = actual
                phase["loaded_world"] = capture(game, label + "-loaded-world")
                result["phases"][label] = phase
                game.stop()
            print("PASS: absent/corrupt previews and metadata fall back independently and still load", flush=True)

        # Blood 1.21 E1M1 at the fixture's default difficulty has 18 enemies and
        # 11 normal secrets. First validate these genuine nonzero totals. Then
        # change only its uniquely identified legacy EndGame counter block to
        # exercise nonzero progress through real Load -> Save and restart.
        args.renderer = args.renderers[-1]
        game = Game("world-stats", args, root, library, playback=True, launch_map="E1M1")
        games.append(game)
        started(game)
        initial_counts = (0, 18, 0, 11)
        stat_save = save(game, 0, "fresh", initial_counts)
        fresh_meta, _ = preview(stat_save, palette, root / "world-stats-fresh.png", gray=False)
        phase = dict(fresh=fresh_meta)
        result["phases"]["world-stats"] = phase
        data = bytearray(stat_save.read_bytes())
        pattern = struct.pack("<5i", 11, 0, 0, 18, 0)
        offset = data.find(pattern, 0, fresh_meta["world_body_bytes"])
        require(offset >= 0 and data.find(pattern, offset + 1, fresh_meta["world_body_bytes"]) < 0,
                "E1M1 legacy EndGame counters are not uniquely identified")
        struct.pack_into("<5i", data, offset, 11, 3, 0, 18, 7)
        phase["synthetic_legacy_stats"] = dict(offset=offset, before=[11, 0, 0, 18, 0], after=[11, 3, 0, 18, 7])
        game.stop()
        stat_save.write_bytes(data)
        game = restart(game, "world-stats-reload", library, root)
        games.append(game)
        started(game)
        select(game, 60, 0)
        tap(game, 40)
        time.sleep(0.7)
        populated = save(game, 5, "progress", (7, 18, 3, 11))
        populated_meta, _ = preview(populated, palette, root / "world-stats-progress.png", gray=False)
        phase["progress"] = populated_meta
        phase["progress_menu"] = show_load(game, 5, "world-stats-progress-menu")
        check_menu_metadata(phase, "world-stats-progress", phase["progress_menu"], populated_meta["metadata"])
        tap(game, 75)
        phase["fresh_menu"] = capture(game, "world-stats-fresh-menu")
        check_menu_metadata(phase, "world-stats-fresh-slot", phase["fresh_menu"], fresh_meta["metadata"])
        tap(game, 41)
        game.stop()
        # Move an unchanged save between two local timezones. Only presentation
        # may change; a persisted local-time string would fail this restart.
        saved_bytes = populated.read_bytes()
        os.environ["TZ"] = "JST-9" if time.strftime("%z") != "+0900" else "EST5"
        time.tzset()
        game = restart(game, "world-stats-final", library, root)
        games.append(game)
        started(game)
        require(populated.read_bytes() == saved_bytes, "timezone restart rewrote the save")
        restarted_metadata = save_metadata(saved_bytes)
        require(restarted_metadata["local_text"] != populated_meta["metadata"]["local_text"],
                "restart timezone did not change the displayed date")
        phase["restart_timezone"] = os.environ["TZ"]
        phase["restart_menu"] = show_load(game, 5, "world-stats-restart-menu")
        check_menu_metadata(phase, "world-stats-restart", phase["restart_menu"], restarted_metadata)
        tap(game, 41)
        game.stop()
        print("PASS: actual E1M1 counters, distinct slot statistics and local save timestamps persist", flush=True)
        result["passed"] = True
    except (Failure, OSError, subprocess.SubprocessError, ValueError, KeyError) as error:
        result["failures"].append(str(error))
        print(f"FAIL: {error}", flush=True)
    finally:
        for game in reversed(games):
            game.stop()
        result["games"] = {game.name: game.result() for game in games}
        for game in games:
            if game.process.returncode != 0 or game.forced:
                result["failures"].append(f"{game.name}: unclean exit {game.process.returncode}")
            if re.search(r"AddressSanitizer|runtime error:|Segmentation fault|Assertion failed", game.text() + game.console()):
                result["failures"].append(f"{game.name}: runtime diagnostic")
        result["passed"] = result["passed"] and not result["failures"]
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        if original_timezone is None:
            os.environ.pop("TZ", None)
        else:
            os.environ["TZ"] = original_timezone
        time.tzset()
    print("PASS" if result["passed"] else "FAIL", flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
