#!/usr/bin/env python3
"""Run the DOS Blood 1.21 regression demos in an isolated, headless session."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def main():
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, required=True,
                        help="directory containing the original Blood 1.21 assets")
    parser.add_argument("--binary", type=Path, default=repository / "nblood")
    parser.add_argument("--archive", type=Path,
                        default=Path(__file__).resolve().with_name("validation.pk3"))
    parser.add_argument("--timeout", type=float, default=600,
                        help="maximum run time in seconds (default: 600)")
    parser.add_argument("--output-dir", type=Path,
                        help="parent directory for a fresh run directory (default: system temp)")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    binary, assets, archive = (p.resolve() for p in (args.binary, args.data_dir, args.archive))
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error(f"build the executable first: {binary}")
    if not archive.is_file():
        parser.error(f"validation archive is missing: {archive}")
    if not assets.is_dir():
        parser.error(f"game data directory is missing: {assets}")
    required = ["BLOOD.INI", "BLOOD.RFF", "GUI.RFF", "SOUNDS.RFF", "SURFACE.DAT", "VOXEL.DAT"]
    required += [f"TILES{i:03}.ART" for i in range(18)]
    present = {p.name.upper() for p in assets.iterdir() if p.is_file() and p.stat().st_size}
    missing = [name for name in required if name not in present]
    if missing:
        parser.error("missing Blood 1.21 assets: " + ", ".join(missing))

    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=True)
    runtime = Path(tempfile.mkdtemp(prefix="nblood-validation-", dir=args.output_dir)).resolve()
    (runtime / "validation.cfg").write_text(
        "[Setup]\nForceSetup = 0\nNoAutoLoad = 1\n\n"
        "[Screen Setup]\nScreenMode = 0\nScreenBPP = 8\nScreenWidth = 640\nScreenHeight = 480\n"
    )
    (runtime / "runtime").mkdir(mode=0o700)
    (runtime / "cache").mkdir()
    environment = os.environ.copy()
    environment.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
                       SDL_RENDER_DRIVER="software", XDG_RUNTIME_DIR=str(runtime / "runtime"),
                       XDG_CACHE_HOME=str(runtime / "cache"))
    command = [str(binary), "-validate", "-usecwd", "-cfg", "validation.cfg",
               f"-j={assets}", f"-j={repository}", f"-g={archive}"]
    print(f"Validation artifacts: {runtime}", flush=True)
    failures = []
    returncode = None
    with (runtime / "console.log").open("w") as log:
        try:
            process = subprocess.run(command, cwd=runtime, env=environment, stdout=log,
                                     stderr=subprocess.STDOUT, timeout=args.timeout)
            returncode = process.returncode
        except subprocess.TimeoutExpired:
            failures.append(f"timed out after {args.timeout:g} seconds")
        except OSError as error:
            failures.append(str(error))

    # OSD messages live in the game log; use one stream to avoid double counting.
    game_log = runtime / "nblood.log"
    log_path = game_log if game_log.is_file() else runtime / "console.log"
    log_text = log_path.read_text(errors="replace")
    synced = re.findall(r"Demo Synced: (/validatedemos/TEST\d{3}\.DEM)", log_text)
    summaries = re.findall(r"1\.21 Validation Successful! \((\d+) passed, (\d+) skipped\)", log_text)
    if returncode != 0:
        failures.append(f"game exit status: {returncode}")
    if len(summaries) != 1:
        failures.append("expected exactly one successful validation summary")
    else:
        passed, skipped = map(int, summaries[0])
        if passed + skipped != 144 or passed == 0:
            failures.append(f"incomplete suite: {passed} passed, {skipped} skipped (expected 144 total)")
        if len(synced) != passed or len(set(synced)) != passed:
            failures.append("per-demo pass records do not match the summary")
    result = {"passed": not failures, "command": command, "exit_code": returncode,
              "synced_demos": synced, "summary": summaries[0] if len(summaries) == 1 else None,
              "failures": failures, "log": str(log_path)}
    (runtime / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    if failures:
        print(log_text[-6000:], file=sys.stderr)
        print("FAIL: " + "; ".join(failures), file=sys.stderr)
        return 1
    print(f"PASS: {passed} demos synced, {skipped} skipped; results: {runtime / 'result.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
