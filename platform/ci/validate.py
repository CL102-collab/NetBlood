#!/usr/bin/env python3
"""Run a mandatory CI test shard, retaining logs and machine-readable status."""

import argparse
from contextlib import contextmanager
import glob
import json
import os
from pathlib import Path
import select
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
NETWORK = (
    "server-settings", "map-download", "content-mismatch", "content-handshake",
    "client-version", "player-names", "password", "addon-join", "scoreboard", "cinematics",
    "team-balance", "team-shuffle", "team-chat", "coop-lives", "map-vote",
    "intermission", "uri-join", "discord-presence", "stun-live",
)
SUITES = ("demos", "unit", "shaders", "windows-runtime", "backend", "render-game", "render-fixtures",
          "render-ror", "ambient-occlusion", "save-preview", "network") + NETWORK


@contextmanager
def display(environment, output):
    """A separate display prevents test windows from stealing another shard's focus."""
    if not shutil.which("Xvfb"):
        raise RuntimeError("Xvfb is required; graphical tests cannot be skipped")
    if not environment.get("VK_DRIVER_FILES") and not environment.get("VK_ICD_FILENAMES"):
        drivers = sorted(glob.glob("/usr/share/vulkan/icd.d/lvp_icd*.json"))
        if not drivers:
            raise RuntimeError("Mesa lavapipe is required; install mesa-vulkan-drivers")
        environment["VK_DRIVER_FILES"] = drivers[0]
    read_fd, write_fd = os.pipe()
    process = None
    try:
        with (output / "xvfb.log").open("w") as log:
            process = subprocess.Popen(
                ["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "1280x720x24",
                 "-nolisten", "tcp", "-ac", "-noreset"],
                pass_fds=(write_fd,), stdout=log, stderr=subprocess.STDOUT,
            )
            os.close(write_fd)
            write_fd = None
            if not select.select([read_fd], [], [], 20)[0]:
                raise RuntimeError("Xvfb did not report a ready display within 20 seconds")
            number = os.read(read_fd, 64).decode().strip()
            if not number.isdecimal() or process.poll() is not None:
                raise RuntimeError("Xvfb failed to start; inspect xvfb.log")
            environment.update(DISPLAY=":" + number, SDL_VIDEODRIVER="x11",
                               SDL_AUDIODRIVER="dummy", LIBGL_ALWAYS_SOFTWARE="1")
            environment.setdefault("LP_NUM_THREADS", "2")
            runtime = output / "xdg-runtime"
            runtime.mkdir(mode=0o700)
            environment["XDG_RUNTIME_DIR"] = str(runtime)
            yield
    finally:
        os.close(read_fd)
        if write_fd is not None:
            os.close(write_fd)
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def commands(args):
    python = sys.executable
    output = args.output
    base = ["--binary", str(args.binary), "--assets", str(args.data_dir)]

    def game(script, name="evidence", extra=()):
        return [python, str(ROOT / script), *base, "--output", str(output / name), *extra]

    if args.suite == "demos":
        return [[python, str(ROOT / "tests/demo-validation/run.py"),
                 "--binary", str(args.binary), "--data-dir", str(args.data_dir),
                 "--require-all", "--output-dir", str(output / "evidence")]]
    if args.suite == "unit":
        # protocol-test.sh already runs these three scripts. Keep automatic
        # discovery so adding another standalone regression test adds it to CI.
        nested = {"compatibility-test.sh", "uri-test.sh", "chat-test.sh"}
        standalone = [[str(path)] for path in sorted((ROOT / "tests/network").glob("*-test.sh"))
                      if path.name not in nested]
        return [[python, str(ROOT / "platform/ci/test_pipeline.py")],
                [python, str(ROOT / "platform/ci/test_windows_verify.py")],
                ["sh", str(ROOT / "platform/ci/enet-ipv4-test.sh")]] + standalone + [
            [python, str(ROOT / "tests/network/uri-registration.py")]]
    if args.suite == "shaders":
        return [[python, str(ROOT / "platform/ci/embed-metal-shaders.py"), "--check"],
                [python, str(ROOT / "tests/vulkan/generate-shaders.py"), "--check"]]
    if args.suite == "windows-runtime":
        return [["bash", str(ROOT / "platform/ci/validate-windows.sh"), str(output / "windows-work")]]
    if args.suite == "backend":
        return [[str(ROOT / "tests/vulkan/backend-test.sh")]]
    if args.suite in ("render-game", "render-fixtures", "render-ror"):
        extra = ["--validation", "--wait-ready", "--initial-delay-ms", "1000",
                 "--engine-data", str(ROOT), "--width", "800", "--height", "600"]
        if args.suite == "render-game":
            return [game("tests/vulkan/run.py", extra=extra + ["--mode", "all"]),
                    game("tests/vulkan/run.py", "without-opengl",
                         extra + ["--mode", "vulkan", "--disable-opengl"])]
        generator = "make-ror-fixtures.py" if args.suite == "render-ror" else "make-fixtures.py"
        fixtures = output / "fixtures"
        return [[python, str(ROOT / "tests/vulkan" / generator), "--output", str(fixtures)],
                game("tests/vulkan/run.py", extra=extra + ["--mode", "compare", "--fixtures", str(fixtures)])]
    if args.suite == "ambient-occlusion":
        return [game("tests/ambient-occlusion/run.py",
                     extra=["--validation", "--wait-ready", "--initial-delay-ms", "1000"])]
    if args.suite == "save-preview":
        return [game("tests/save-preview/run.py")]
    if args.suite == "network":
        # Keep the input-history stress independent of software Vulkan's GPU
        # emulation speed. Other multiplayer shards exercise Vulkan rendering.
        return [game("tests/network/run.py", extra=["--timeout", "120"])]
    extra = []
    # The other suites use software or exercise both backends internally.
    if args.suite in ("password", "scoreboard", "cinematics", "team-chat"):
        extra = ["--renderer", "5"]
    if args.suite == "team-chat":
        extra += ["--listen-renderer", "3"]
    return [game(f"tests/network/{args.suite}.py", extra=extra)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", required=True, choices=SUITES)
    parser.add_argument("--binary", type=Path, default=ROOT / "nblood")
    parser.add_argument("--data-dir", type=Path)
    parser.add_argument("--output", type=Path, required=True, help="new evidence directory")
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    args.output = args.output.resolve()
    asset_free = args.suite in ("unit", "shaders", "windows-runtime", "backend")
    if not asset_free:
        if not args.data_dir or not args.data_dir.is_dir():
            parser.error("--data-dir must contain the Blood 1.21 assets; this test cannot be skipped")
        args.data_dir = args.data_dir.resolve()
        if not args.binary.is_file() or not os.access(args.binary, os.X_OK):
            parser.error(f"build the game first: {args.binary}")
    args.output.mkdir(parents=True, exist_ok=False)
    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    # A UBSan report must fail the gate instead of silently continuing.
    environment.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
    if args.suite == "backend":
        environment["VK_INSTANCE_LAYERS"] = "VK_LAYER_KHRONOS_validation"
    result = {"suite": args.suite, "passed": False, "commands": [], "error": None}

    def run():
        for index, command in enumerate(commands(args)):
            print("Running: " + " ".join(command), flush=True)
            started = time.monotonic()
            log_path = args.output / f"command-{index:02}.log"
            record = {"argv": command, "log": log_path.name, "exit_code": None}
            result["commands"].append(record)
            with log_path.open("w") as log:
                process = subprocess.Popen(command, cwd=ROOT, env=environment,
                                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                           text=True, errors="replace")
                for line in process.stdout:
                    print(line, end="", flush=True)
                    log.write(line)
                record["exit_code"] = process.wait()
            record["seconds"] = round(time.monotonic() - started, 3)
            if record["exit_code"]:
                raise RuntimeError(f"test exited {record['exit_code']}: {command[0]} (see {log_path})")

    try:
        if args.suite in ("demos", "unit", "shaders"):
            run()
        else:
            with display(environment, args.output):
                run()
        result["passed"] = True
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        result["error"] = str(error)
        print(f"FAIL: {error}", file=sys.stderr)
    finally:
        (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
