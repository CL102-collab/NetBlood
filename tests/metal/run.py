#!/usr/bin/env python3
"""Compile and exercise the real macOS Metal backend; retain explicit evidence."""
# SPDX-License-Identifier: GPL-2.0-only
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="new directory for logs, binaries and result.json")
    parser.add_argument("--require-device", action="store_true", help="fail instead of exit 77 without native Metal")
    parser.add_argument("--realtime-lighting-only", action="store_true", help="isolate point lighting, shadows and their lifecycle")
    parser.add_argument("--timeout", type=float, default=180, help="runtime timeout in seconds")
    args = parser.parse_args()
    output = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="netblood-metal-test-"))
    if args.output:
        output.mkdir(parents=True, exist_ok=False)
    result: dict = {
        "platform": platform.platform(), "status": "failed", "shader_compile": "not-run",
        "backend_compile": "not-run", "runtime": "not-run", "commands": [],
        "require_device": args.require_device,
        "suite": "realtime-lighting" if args.realtime_lighting_only else "all",
    }
    code = 1

    def command(argv: list[str], name: str, *, env: dict | None = None, timeout: float = 600) -> int:
        print("+ " + shlex.join(argv), flush=True)
        started = time.monotonic()
        with (output / name).open("w", encoding="utf-8") as log:
            process = subprocess.run(argv, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                                     timeout=timeout, check=False)
        result["commands"].append({"argv": argv, "log": name, "exit_code": process.returncode,
                                   "seconds": round(time.monotonic() - started, 3)})
        print((output / name).read_text(encoding="utf-8", errors="replace"), end="", flush=True)
        return process.returncode

    def checked(argv: list[str], name: str) -> None:
        if command(argv, name):
            raise RuntimeError(f"{name}: command failed; see retained log")

    try:
        if sys.platform != "darwin":
            result["reason"] = "Native Metal, Cocoa and the Apple shader compiler require macOS; no rendering was tested."
            result["status"] = "failed" if args.require_device else "skipped"
            result["runtime"] = "unavailable"
            code = 1 if args.require_device else 77
            return code
        xcrun = shutil.which("xcrun")
        if not xcrun:
            raise RuntimeError("xcrun is missing; install/select Xcode with its Metal toolchain")
        sdl_config = os.environ.get("SDL_CONFIG", "sdl2-config")
        cppflags = shlex.split(os.environ.get("CPPFLAGS", ""))
        cxxflags = shlex.split(os.environ.get("CXXFLAGS", ""))
        ldflags = shlex.split(os.environ.get("LDFLAGS", ""))
        sdl_cflags = shlex.split(subprocess.check_output([sdl_config, "--cflags"], text=True))
        sdl_libs = shlex.split(subprocess.check_output([sdl_config, "--libs"], text=True))
        cxx = shlex.split(os.environ.get("CXX", "clang++"))
        checked([sys.executable, str(ROOT / "platform/ci/embed-metal-shaders.py"), "--check"],
                "shader-consistency.log")
        shader = ROOT / "source/build/src/metal_polymost.metal"
        result["shader_source_sha256"] = hashlib.sha256(shader.read_bytes()).hexdigest()
        checked([xcrun, "-sdk", "macosx", "metal", "-std=macos-metal2.3", "-mmacosx-version-min=11.0",
                 "-Wall", "-Wextra", "-Werror", "-c", str(shader), "-o", str(output / "polymost.air")],
                "shader-compile.log")
        checked([xcrun, "-sdk", "macosx", "metallib", str(output / "polymost.air"),
                 "-o", str(output / "polymost.metallib")], "shader-library.log")
        result["shader_compile"] = "passed"
        common = ["-std=c++11", "-DUSE_METAL", "-Wall", "-Wextra", "-Werror", "-mmacosx-version-min=11.0",
                  f"-I{ROOT / 'source/build/include'}", f"-I{ROOT / 'source/glad/include'}"]
        common += cppflags + cxxflags + sdl_cflags
        checked(cxx + common + ["-c", str(ROOT / "tests/metal/backend.cpp"),
                                 "-o", str(output / "test.o")], "test-compile.log")
        checked(cxx + common + ["-fobjc-arc", "-c", str(ROOT / "source/build/src/metal_renderer.mm"),
                                 "-o", str(output / "renderer.o")], "backend-compile.log")
        executable = output / "backend-test"
        checked(cxx + [str(output / "test.o"), str(output / "renderer.o")] + ldflags + sdl_libs +
                ["-framework", "Metal", "-framework", "QuartzCore", "-framework", "Cocoa", "-o", str(executable)],
                "link.log")
        checked(["otool", "-L", str(executable)], "dependencies.log")
        dependencies = (output / "dependencies.log").read_text()
        if re.search(r"OpenGL\.framework|libvulkan|MoltenVK", dependencies, re.I):
            raise RuntimeError("Metal backend unexpectedly links OpenGL or Vulkan")
        result["backend_compile"] = "passed"
        result["executable_sha256"] = hashlib.sha256(executable.read_bytes()).hexdigest()
        # These are Apple's actual API/shader validation controls. They do not
        # emulate Metal or permit missing-device results to count as rendering.
        env = os.environ.copy()
        env.update({"MTL_DEBUG_LAYER": "1", "MTL_SHADER_VALIDATION": "1", "NBLOOD_AO_TEST_OUTPUT": str(output),
                    "NBLOOD_LIGHTING_TEST_OUTPUT": str(output)})
        argv = [str(executable)] + (["--require-device"] if args.require_device else [])
        if args.realtime_lighting_only:
            argv.append("--realtime-lighting-only")
        code = command(argv, "runtime.log", env=env, timeout=args.timeout)
        runtime_log = (output / "runtime.log").read_text(errors="replace")
        if code == 77:
            result.update(status="skipped", runtime="unavailable",
                          reason="No native Metal device; shaders and backend compiled, GPU behavior was not tested.")
            if args.require_device:
                code = 1
                result["status"] = "failed"
        elif code == 0:
            marker = ("PASS: targeted native Metal point lighting and shadow checks" if args.realtime_lighting_only
                      else "PASS: native Metal backend contract suite")
            if marker not in runtime_log:
                raise RuntimeError("backend exited without completing its contract checks")
            if "PASS: native Metal point lighting, geometry shadows and lighting lifecycle" not in runtime_log:
                raise RuntimeError("backend exited without completing lighting and shadow checks")
            if re.search(r"(Metal (?:API|Shader) Validation.*(?:error|failed)|validate\w*:.*failed|GPU (?:fault|error)|MTLCommandBufferError)",
                         runtime_log, re.I):
                raise RuntimeError("Apple Metal validation reported an error")
            result.update(status="passed", runtime="passed")
        else:
            result["runtime"] = "failed"
            result["reason"] = f"native backend exited with status {code}"
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        result["reason"] = str(error)
        print(f"FAIL: {error}", file=sys.stderr)
        code = 1
    finally:
        result["exit_code"] = code
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(f"Metal validation: {result['status']}; evidence: {output}", flush=True)
        if result.get("reason"):
            print(result["reason"], flush=True)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
