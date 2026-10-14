#!/usr/bin/env python3
"""Check actionable mod-content errors using a real server and joining clients.

The fixture adds unused resources without changing stock game assets. Missing
and changed loose DAT/RAW/DEF files activate automatically. An unselected ART
file outside the advertised package still produces missing/different errors.
Expected lengths and MD5s come from Python's hashlib, independently of the
game's manifest. An exact-match client must join the same running server.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

from run import Failure, Game, build_driver, records, require


def quit_before_map(game):
    """Use the menu because game-console binds are inactive before joining."""
    game.key(67, True)  # SDL_SCANCODE_F10
    time.sleep(0.25)
    game.key(67, False)
    game.key(28, True)  # SDL_SCANCODE_Y
    game.process.wait(timeout=10)
    game.stop()


def screenshot(root, name):
    """Preserve the actual visible error if ImageMagick is installed."""
    program = shutil.which("import")
    if not program:
        return None
    path = root / f"{name}.png"
    subprocess.run([program, "-window", "root", str(path)], check=True, timeout=15,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    return str(path)


def fixture_bytes():
    definitions = (b"rffdefineid NBMISS01 DAT 62001 SYSTEM\n"
                   b"rffdefineid NBCHANGE DAT 62002 SYSTEM\n"
                   b"rffdefineid NBMATCH1 DAT 62003 SYSTEM\n")
    return {
        "blood.def": definitions,
        "NBMISS01.DAT": bytes(range(256))*4,
        "NBCHANGE.DAT": bytes(range(256))*3,
        "NBMATCH1.DAT": b"unchanged netcontent fixture\n",
        # Loose override of a real sound-archive resource, not played at join.
        # It is not registered by rffdefineid or included in the legacy CRC.
        "TOM_FIRE.RAW": b"\x80"*257,
        # Valid ART version 1, one 1x1 tile outside the stock art range.
        "TILES018.ART": struct.pack("<4I2HI", 1, 1, 6000, 6000, 1, 1, 0)+b"\x40",
    }


def has_details(text, name, data, status):
    """A diagnostic must identify each file and its expected size and digest."""
    digest = hashlib.md5(data).hexdigest()
    size = re.compile(rf"\b{len(data)}\b")
    return any(name.lower() in line.lower() and digest in line.lower()
               and size.search(line) and status in line.lower()
               for line in text.splitlines())


def cached(game, name, data):
    matches = [path for path in (game.path / ".nblood-addon-cache").glob("*/" + name)
               if path.read_bytes() == data]
    require(len(matches) == 1, f"{game.name}: no byte-exact cached addon resource {name}")
    return matches[0]


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
    require(os.environ.get("DISPLAY"), "clients need DISPLAY (use an external Xvfb wrapper)")
    require(args.binary.is_file() and os.access(args.binary, os.X_OK) and args.assets.is_dir(),
            "build executable and supply game assets first")
    files = fixture_bytes()
    for name in files:
        if name == "blood.def":
            continue
        require(all(not (directory / name).exists() for directory in (args.assets, args.engine_data)),
                f"fixture {name} already exists in a shared search path")
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="netblood-content-mismatch-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print(f"Content-mismatch evidence: {root}", flush=True)
    library = build_driver(root)
    source_binary = args.binary
    args.binary = root / "nblood-tested"
    shutil.copy2(source_binary, args.binary)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    changed = dict(files)
    for name in ("NBMISS01.DAT", "TILES018.ART", "TOM_FIRE.RAW"):
        changed.pop(name)
    changed["NBCHANGE.DAT"] = bytes(range(255, -1, -1))*3
    # The normal base-ART prefix is not a distributable addon selection here.
    # Unlike loose DAT/RAW/DEF dependencies, this extra file stays outside the
    # automatically advertised profile and tests the final admission check.
    expected = [("TILES018.ART", "missing")]
    games = []
    result = {
        "passed": False, "source_binary": str(source_binary),
        "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(), "failures": [],
        "fixtures": {name: {"bytes": len(data), "md5": hashlib.md5(data).hexdigest()}
                     for name, data in files.items()},
        "changed_client_md5": hashlib.md5(changed["NBCHANGE.DAT"]).hexdigest(),
    }
    try:
        server = Game("server", args, root, library, port, dedicated=True, initial_files=files)
        games.append(server)
        server.wait(lambda text: "Dedicated server running:" in text and
                    any(int(row["server_tick"]) >= 30 and row["players"] == "0"
                        for row in records(text, "NET server_tick=")), "empty mod server", args.timeout)
        mismatch = Game("mismatched-client", args, root, library, port, initial_files=changed)
        games.append(mismatch)
        mismatch.wait(lambda text: all(has_details(text, name, files[name], status)
                                      for name, status in expected),
                      "complete missing/different file diagnostics", args.timeout)
        mismatch.wait(lambda text: "NET content mismatch report saved to" in text,
                      "saved missing-file diagnostic", args.timeout)
        text = mismatch.text()
        require("NET client slot=" not in text and "NET client map loaded " not in text and
                "NET map download started" not in text,
                "incompatible client downloaded a map or installed authoritative state")
        require(not any("NBMATCH1.DAT" in line and ("missing" in line.lower() or "different" in line.lower())
                        for line in text.splitlines()), "matching DAT was incorrectly reported")
        require(all((mismatch.path / name).read_bytes() == data for name, data in changed.items()),
                "content rejection modified the client's installed files")
        require(all(not (mismatch.path / name).exists() for name in ("NBMISS01.DAT", "TILES018.ART", "TOM_FIRE.RAW")),
                "activation wrote files outside its private addon cache")
        require("NET addon download complete" in mismatch.console() and
                mismatch.console().count("Started at ") == 2,
                "missing loose addon resources were not activated before the final ART check")
        for name in files:
            if name != "TILES018.ART":
                cached(mismatch, name, files[name])
        require(not any(name in line and ("missing" in line.lower() or "different" in line.lower())
                        for line in text.splitlines() for name in files if name != "TILES018.ART"),
                "activated resources were still reported missing or different")
        result["diagnostics"] = [line for line in text.splitlines()
                                 if any(name in line for name, _ in expected)]
        require(len(result["diagnostics"]) == len(expected),
                "content error lists the same required file more than once")
        report = mismatch.path / "net-content-mismatch.txt"
        require(report.is_file(), "content mismatch did not save its file inventory")
        report_text = report.read_text()
        for name, status in expected:
            require(has_details(report_text, name, files[name], status),
                    f"saved report omitted expected {status} file details for {name}")
        require(sum("NET asset " in line for line in report_text.splitlines()) == len(expected),
                "saved report duplicates required files")
        result["saved_report"] = str(report)
        time.sleep(0.3)
        result["visible_error"] = screenshot(root, "content-error")
        quit_before_map(mismatch)

        # Equal-size modified base ART must retain exact file/size/MD5 details.
        # Its addon profile already matches, so this also exercises admission
        # failure without a preceding activation/restart.
        different_art_files = dict(files)
        different_art_files["TILES018.ART"] = files["TILES018.ART"][:-1] + b"\x41"
        different_art = Game("different-art-client", args, root, library, port, initial_files=different_art_files)
        games.append(different_art)
        different_art.wait(lambda text: has_details(text, "TILES018.ART", files["TILES018.ART"], "different"),
                           "different ART file details", args.timeout)
        different_art.wait(lambda text: "NET content mismatch report saved to" in text,
                           "saved different-file diagnostic", args.timeout)
        require("NET client map loaded " not in different_art.text() and
                "NET addon download started" not in different_art.console(),
                "different base artwork was downloaded or admitted")
        different_report = different_art.path / "net-content-mismatch.txt"
        require(different_report.is_file() and
                has_details(different_report.read_text(), "TILES018.ART", files["TILES018.ART"], "different"),
                "different ART report omitted filename, size or MD5")
        require((different_art.path / "TILES018.ART").read_bytes() == different_art_files["TILES018.ART"],
                "different ART admission check modified installed bytes")
        result["different_art_report"] = str(different_report)
        quit_before_map(different_art)

        # The lazy RAW resource is outside the old aggregate fingerprint, but
        # is included in the addon profile and must be fetched before joining.
        missing_raw_files = dict(files)
        missing_raw_files.pop("TOM_FIRE.RAW")
        missing_raw = Game("missing-raw-client", args, root, library, port, initial_files=missing_raw_files)
        games.append(missing_raw)
        missing_raw.wait(lambda text: "NET content verified" in text and "NET client map loaded " in text
                         and "NET client slot=" in text, "missing loose sound downloaded and activated", args.timeout)
        raw_text = missing_raw.text()
        raw_checks = records(raw_text, "NET content check ")
        require(len(raw_checks) == 1 and raw_checks[0]["local"] == raw_checks[0]["remote"],
                "activated loose sound did not match the authoritative content fingerprint")
        result["missing_raw_content_check"] = raw_checks[0]
        require("NET addon download complete" in missing_raw.console() and
                missing_raw.console().count("Started at ") == 2,
                "missing loose sound did not cause automatic activation")
        result["downloaded_raw"] = str(cached(missing_raw, "TOM_FIRE.RAW", files["TOM_FIRE.RAW"]))
        require(not (missing_raw.path / "TOM_FIRE.RAW").exists() and
                not (missing_raw.path / "net-content-mismatch.txt").exists(),
                "activated loose sound overwrote local files or left a false mismatch report")
        missing_raw.command_text("net_status")
        missing_raw.stop()

        # The resources are already installed, but the local default definition
        # does not register them. The advertised definition is downloadable:
        # activate it after one restart instead of asking for manual setup.
        # Its original local replacement must remain untouched.
        unloaded_files = dict(files)
        unloaded_files["blood.def"] = b"// Mod resource definitions intentionally absent.\n"
        unloaded = Game("unloaded-mod-client", args, root, library, port, initial_files=unloaded_files)
        games.append(unloaded)
        unloaded.wait(lambda text: "NET content verified" in text and "NET client map loaded " in text
                      and "NET client slot=" in text, "automatic registration of installed resources", args.timeout)
        history = unloaded.console()  # Unlike nblood.log, stdout survives exec.
        require("NET addon download complete" in history and "NET addon restart required" in history,
                "unloaded definition was not downloaded and activated automatically")
        require(history.count("Started at ") == 2, "unloaded definition activation did not restart exactly once")
        require(all((unloaded.path / name).read_bytes() == data for name, data in unloaded_files.items()),
                "activation overwrote an installed resource or the original definition")
        require(not (unloaded.path / "net-content-mismatch.txt").exists(),
                "successfully activated definition still produced an asset mismatch")
        unloaded.command_text("net_status")
        result["unloaded_definition_activated"] = dict(restarts=1, original_files_preserved=True)
        unloaded.stop()

        matched = Game("matching-client", args, root, library, port, initial_files=files)
        games.append(matched)
        matched.wait(lambda text: "NET client map loaded " in text and "NET client slot=" in text,
                     "matching client authoritative state", args.timeout)
        matched.command_text("net_status")
        require("NET rejected:" not in matched.text(), "identical mod content was rejected")
        matched.stop()
        server.stop()
        require("NETWORK_TEST_DEVICES windows=0 audio=0" in server.console(), "dedicated server opened a device")
        for game in games:
            require(game.process.returncode == 0 and not game.forced, f"{game.name} did not shut down cleanly")
            require(not re.search(r"AddressSanitizer|runtime error:|Segmentation fault", game.text()+game.console()),
                    f"{game.name} reported a runtime error")
        result["passed"] = True
    except (Failure, OSError, subprocess.SubprocessError) as error:
        result["failures"].append(str(error))
    finally:
        for game in reversed(games):
            game.stop()
        result["games"] = {game.name: game.result() for game in games}
        (root / "result.json").write_text(json.dumps(result, indent=2)+"\n")
    print(("PASS" if result["passed"] else "FAIL")+f": {root / 'result.json'}", flush=True)
    for failure in result["failures"]:
        print(failure, file=sys.stderr)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
