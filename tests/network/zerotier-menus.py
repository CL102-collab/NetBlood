#!/usr/bin/env python3
"""Drive the real Internet/ZeroTier menus through SDL and inspect screenshots.

This offline UI check deliberately clears the shared network ID. It verifies
that browsing and host settings remain usable before deployment, and that the
Internet port controls remain separate from the fixed-port ZeroTier host UI.
It does not claim to exercise live ZeroTier discovery or transport.

Requires X11/Xvfb, ImageMagick, Tesseract, Pillow, SDL2 development headers, and
the supplied game's ordinary assets. Either game or both may be selected.
"""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import difflib
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace

from cinematics import capture_window, focus_window
from password import replace_field, tap as blood_tap, type_text
from run import Failure, Game, build_driver, require

ROOT = Path(__file__).resolve().parents[2]


def tap(game, scan):
    if getattr(game, "menu_test_kind", "nblood") == "nblood":
        blood_tap(game, scan)
    else:
        # Rednukem samples menu controls on clocked input ticks, so a drained
        # SDL queue alone does not establish that a held key was consumed.
        game.key(scan, True)
        time.sleep(0.12)
        game.key(scan, False)
        game.input_barrier()
        time.sleep(0.12)


def normalized(text):
    # Narrow pixel-font glyphs produce these recurring OCR ambiguities. The
    # complete screenshots remain available for reviewing the decorative text.
    return re.sub(r"[^A-Z0-9]", "", text.upper()).translate(str.maketrans("FUH", "POM"))


def recognized(text, phrase):
    """Allow one OCR glyph error in these games' decorative menu fonts."""
    source, target = normalized(text), normalized(phrase)
    if target in source:
        return True
    if len(target) < 6:
        return False
    for length in range(max(1, len(target) - 1), len(target) + 2):
        for offset in range(max(0, len(source) - length + 1)):
            candidate = source[offset:offset + length]
            if difflib.SequenceMatcher(None, candidate, target).ratio() >= 0.85:
                return True
    return False


def screenshot(game, path):
    from PIL import Image, ImageOps
    capture = capture_window(game, path)
    require(capture["width"] >= 320 and capture["height"] >= 200,
            f"{game.name}: screenshot is too small to inspect")
    require(capture["black_fraction"] < 0.98,
            f"{game.name}: menu screenshot is almost entirely black")
    image = Image.open(path).convert("RGB")
    gray = ImageOps.grayscale(image).resize((image.width * 2, image.height * 2))
    # Rednukem's red font includes a dark gradient that grayscale thresholding
    # breaks into disconnected glyphs. Separate it from the gray menu texture.
    pixels = list(image.get_flattened_data() if hasattr(image, "get_flattened_data") else image.getdata())
    red = Image.new("L", image.size, 255)
    red.putdata([0 if r > 5 and r > g * 1.4 and r > b * 1.4 else 255 for r, g, b in pixels])
    red_bright = Image.new("L", image.size, 255)
    red_bright.putdata([0 if r > 30 and r > g * 1.4 and r > b * 1.4 else 255 for r, g, b in pixels])
    white = Image.new("L", image.size, 255)
    white.putdata([0 if min(r, g, b) > 60 and max(r, g, b) - min(r, g, b) < 20 else 255
                   for r, g, b in pixels])
    variants = (ImageOps.autocontrast(gray),
                ImageOps.invert(gray.point(lambda pixel: 255 if pixel > 125 else 0)),
                red.resize(gray.size), red_bright.resize(gray.size), white.resize(gray.size))
    texts = []
    for index, variant in enumerate(variants):
        prepared = path.with_name(path.stem + f"-ocr-{index}.png")
        variant.save(prepared)
        text = subprocess.check_output(
            ["tesseract", str(prepared), "stdout", "--psm", "11"],
            text=True, stderr=subprocess.DEVNULL, timeout=30,
            env=dict(os.environ, OMP_THREAD_LIMIT="1"))
        texts.append(text)
        prepared.unlink()
    capture["ocr"] = "\n".join(texts)
    path.with_suffix(".txt").write_text(capture["ocr"])
    return capture


def menu(game, result, name, expected=(), forbidden=()):
    time.sleep(0.25)
    require(game.process.poll() is None, f"{game.name}: exited while opening {name}")
    capture = screenshot(game, game.path.parent / f"{game.name}-{name}.png")
    result["screenshots"][name] = capture
    for phrase in expected:
        require(recognized(capture["ocr"], phrase),
                f"{game.name}/{name}: rendered {phrase!r} was not recognized; see {capture['path']}")
    for phrase in forbidden:
        require(not recognized(capture["ocr"], phrase),
                f"{game.name}/{name}: unexpected rendered {phrase!r}; see {capture['path']}")
    return capture


def exercise(game, kind, result, timeout):
    game.menu_test_kind = kind
    if kind == "nblood":
        game.wait(lambda text: "Waiting for network players!" in text,
                  "title screen startup", timeout)
    else:
        # The generated CON does not start a level from this command line.
        # Wait for the ordinary SDL event loop rather than assuming a log marker.
        game.input_barrier(timeout)
    time.sleep(0.7)
    focus_window(game)
    # The Blood main menu uses a decorative font that OCR cannot read reliably;
    # inspect the transport menus' ordinary labels after actual navigation.
    menu(game, result, "main")
    tap(game, 81)  # Main menu: New Game -> Multiplayer.
    tap(game, 40)
    menu(game, result, "multiplayer")

    tap(game, 40)  # Internet keeps the existing Host and Join controls.
    menu(game, result, "internet")
    if kind == "rednukem":
        tap(game, 81)  # Internet: Join Game -> Host Game.
    tap(game, 40)
    internet_host = menu(game, result, "internet-host", ("PORT",) if kind == "nblood" else ("GAME TYPE",))
    require("ZEROTIER" not in normalized(internet_host["ocr"]),
            "Internet host unexpectedly displays ZeroTier settings")
    tap(game, 41)  # Internet.
    tap(game, 41)  # Multiplayer.
    tap(game, 81)
    tap(game, 40)  # ZeroTier.
    menu(game, result, "zerotier")
    tap(game, 40)
    menu(game, result, "browser", ("MAP", "REFRESH"))
    tap(game, 62)  # F5 refresh must remain nonblocking with no network configured.
    tap(game, 81)  # Empty-list navigation must be harmless.
    tap(game, 82)
    tap(game, 40)  # Joining an empty list must not leave the browser.
    menu(game, result, "browser-after-refresh", ("REFRESH",))
    require("NET server listening" not in game.text(),
            f"{game.name}: browsing incorrectly started a game server")
    tap(game, 41)
    tap(game, 81)
    tap(game, 40)
    menu(game, result, "zerotier-host", ("MAX PLAYERS", "PASSWORD"), ("PORT",))
    tap(game, 81)  # Password field.
    if kind == "nblood":
        replace_field(game, "crypt key", 0)
    else:
        tap(game, 40)  # Rednukem uses its existing masked password form.
        game.enter_text("crypt key")
        menu(game, result, "zerotier-password-form", forbidden=("crypt key",))
        tap(game, 40)
    menu(game, result, "zerotier-host-password", ("PASSWORD",), ("PORT", "crypt key"))
    tap(game, 41)  # ZeroTier.
    tap(game, 41)  # Multiplayer.
    tap(game, 82)  # Return to Internet.
    tap(game, 40)
    # Return to Join without depending on the retained submenu selection.
    if kind == "nblood":
        tap(game, 81)
    else:
        tap(game, 82)
    tap(game, 40)
    menu(game, result, "internet-join", ("PORT",))
    result["phases"] = {
        "internet-and-zerotier-parent": True,
        "internet-keeps-port-controls": True,
        "unconfigured-browser-refresh-and-empty-selection": True,
        "zerotier-host-password-with-no-port-control": True,
        "host-password-is-masked": True,
        "internet-join-still-available": True,
    }


def stop(game, kind):
    if game.process.poll() is None and kind == "nblood":
        try:
            focus_window(game)
            tap(game, 53)  # Open the ordinary console while a menu is active.
            type_text(game, "quit")
            tap(game, 40)
            game.process.wait(timeout=10)
        except (Failure, subprocess.TimeoutExpired):
            pass
    game.stop()


def rednukem_runner():
    directory = ROOT / "tests/rednukem-network"
    sys.path.insert(0, str(directory))
    spec = importlib.util.spec_from_file_location("zerotier_rednukem_fixture", directory / "run.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nblood", type=Path, default=ROOT / "nblood")
    parser.add_argument("--nblood-assets", type=Path)
    parser.add_argument("--rednukem", type=Path, default=ROOT / "rednukem")
    parser.add_argument("--rednukem-assets", type=Path)
    parser.add_argument("--rednukem-grp", type=Path)
    parser.add_argument("--renderer", type=int, choices=(0, 3, 5), default=0)
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--output", type=Path)
    options = parser.parse_args()
    require(options.nblood_assets or options.rednukem_assets, "supply assets for at least one game")
    require(os.environ.get("DISPLAY"), "graphical menu checks need DISPLAY (use Xvfb)")
    for tool in ("xwininfo", "xprop", "import", "tesseract"):
        require(shutil.which(tool), f"missing screenshot/OCR tool {tool}")
    root = options.output.resolve() if options.output else Path(tempfile.mkdtemp(prefix="zerotier-menus-"))
    if options.output:
        root.mkdir(parents=True, exist_ok=False)
    result = {"passed": False, "scope": "offline real UI; no live ZeroTier network",
              "games": {}, "failures": []}
    print(f"ZeroTier menu evidence: {root}", flush=True)
    for kind in ("nblood", "rednukem"):
        assets = getattr(options, kind + "_assets")
        if assets is None:
            continue
        game = None
        evidence = result["games"][kind] = {"screenshots": {}}
        try:
            binary = getattr(options, kind).resolve()
            require(binary.is_file(), f"missing {kind} executable: {binary}")
            evidence["binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
            args = SimpleNamespace(binary=binary, assets=assets.resolve(), engine_data=ROOT,
                                   renderer=options.renderer)
            if kind == "nblood":
                library = build_driver(root)
                game = Game(kind, args, root, library, launch_arguments=[],
                            initial_files={"network_cvars.cfg": b'net_zerotier_network ""\n'})
            else:
                runner = rednukem_runner()
                args.grp = (options.rednukem_grp or next(
                    (path for path in args.assets.iterdir() if path.name.lower() == "duke3d.grp"),
                    args.assets / "DUKE3D.GRP")).resolve()
                require(args.grp.is_file(), f"supply Duke art/palette assets: {args.grp}")
                runner.generate(root / "fixture")
                library = runner.build_driver(root)
                game = runner.Game(kind, args, root, library, launch_arguments=[],
                                   autoexec_extra='net_zerotier_network ""\n')
            exercise(game, kind, evidence, options.timeout)
            print(f"PASS: {kind} rendered Internet/ZeroTier menus and offline browser", flush=True)
        except Exception as error:
            result["failures"].append(f"{kind}: {type(error).__name__}: {error}")
            print("FAIL: " + result["failures"][-1], flush=True)
        finally:
            if game:
                stop(game, kind)
                evidence["process"] = game.result()
                if game.process.returncode != 0 or game.forced:
                    result["failures"].append(f"{kind}: unclean exit {game.process.returncode}")
    result["passed"] = bool(result["games"]) and not result["failures"]
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"passed": result["passed"], "failures": result["failures"], "evidence": str(root)}), flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
