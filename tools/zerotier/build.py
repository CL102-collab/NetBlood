#!/usr/bin/env python3
"""Build the independent ZeroTier UDP companion from checksum-pinned sources.

Requires Python 3.9+, CMake 3.12+, and a C/C++ toolchain. The game executables
never link this SDK. Downloads happen only during this explicit build step.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile
import urllib.request

HERE = Path(__file__).resolve().parent
SOURCES = json.loads((HERE / "sources.json").read_text())
RECIPE = "companion-sdk-5"


def run(args):
    print("+", " ".join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True)


def fetch(name, spec, work, offline):
    archive = work / "downloads" / f"{name}-{spec['revision']}.tar.gz"
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        if offline:
            raise RuntimeError(f"Missing cached source: {archive}")
        temporary = archive.with_suffix(".part")
        with urllib.request.urlopen(spec["url"], timeout=120) as response, temporary.open("wb") as output:
            shutil.copyfileobj(response, output)
        temporary.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != spec["sha256"]:
        raise RuntimeError(f"SHA256 mismatch: {archive}")
    return archive


def extract(archive, destination):
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as package:
        # All four checksum-pinned archives contain only regular files and
        # directories. Extract those explicitly rather than relying on a
        # Python-version-dependent tarfile security filter or following links.
        root_name = None
        total = 0
        for member in package.getmembers():
            parts = PurePosixPath(member.name).parts
            if not parts or member.name.startswith("/") or "\\" in member.name or ":" in member.name \
                    or any(part in (".", "..") for part in parts):
                raise RuntimeError("Unsafe source archive path")
            if root_name is None:
                root_name = parts[0]
            if parts[0] != root_name or not (member.isfile() or member.isdir()):
                raise RuntimeError("Unsupported source archive structure or link")
            if len(parts) == 1:
                if not member.isdir():
                    raise RuntimeError("Missing source archive root directory")
                continue
            target = destination.joinpath(*parts[1:])
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                total += member.size
                if member.size < 0 or member.size > 128 * 1024 * 1024 or total > 1024 * 1024 * 1024:
                    raise RuntimeError("Source archive exceeds size limit")
                target.parent.mkdir(parents=True, exist_ok=True)
                with package.extractfile(member) as data, target.open("xb") as output:
                    shutil.copyfileobj(data, output)
                target.chmod(member.mode & 0o777)


def patch_sdk(source):
    """Small, fail-closed portability patch to the exact pinned upstream tree."""
    path = source / "CMakeLists.txt"
    text = path.read_text()
    original = 'cmake_minimum_required(VERSION 3.0)'
    if text.count(original) != 1:
        raise RuntimeError("Unexpected libzt minimum CMake version")
    text = text.replace(original, 'cmake_minimum_required(VERSION 3.12)')
    original = 'if(BUILD_WIN)\n    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")'
    if text.count(original) != 1:
        raise RuntimeError("Unexpected libzt Windows compiler flags")
    text = text.replace(original, 'if(MSVC)\n    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")')
    start = text.index('# WINDOWS-specific MSVC flags and libraries')
    end = text.index('# |                              OPTIONAL FEATURES', start)
    text = text[:start] + '''# Portable Windows system-library names work with MSVC and MinGW.
if(BUILD_WIN)
    set(ws2_32_LIBRARY_PATH ws2_32)
    set(shlwapi_LIBRARY_PATH shlwapi)
    set(iphlpapi_LIBRARY_PATH iphlpapi)
    add_definitions(-DADD_EXPORTS=1)
endif()

# ------------------------------------------------------------------------------
''' + text[end:]
    text = "# Modified for the NetBlood UDP companion (2026): portable CMake and Windows linkage.\n" + text
    path.write_text(text)
    header = source / "include/ZeroTierSockets.h"
    text = header.read_text()
    original = "#if defined(_WIN32)\n#ifdef ADD_EXPORTS\n#define ZTS_API __declspec(dllexport)"
    if text.count(original) != 1:
        raise RuntimeError("Unexpected libzt Windows API linkage")
    text = text.replace(original, "#if defined(_WIN32)\n#if defined(ZTS_STATIC)\n#define ZTS_API\n#elif defined(ADD_EXPORTS)\n#define ZTS_API __declspec(dllexport)")
    header.write_text("/* Modified for the NetBlood UDP companion (2026): static Windows API linkage. */\n" + text)
    # The bundled miniupnpc revision predates correct Windows timeout handling.
    # Winsock expects milliseconds in a DWORD, while POSIX expects timeval.
    miniupnpc = source / "ext/ZeroTierOne/ext/miniupnpc/connecthostport.c"
    text = miniupnpc.read_text()
    if text.count("struct timeval timeout;") != 1:
        raise RuntimeError("Unexpected miniupnpc timeout declaration")
    text = text.replace("struct timeval timeout;", "#ifdef _WIN32\nDWORD timeout;\n#else\nstruct timeval timeout;\n#endif")
    text, count = re.subn(r"timeout\.tv_sec = 3;\s*timeout\.tv_usec = 0;", "#ifdef _WIN32\ntimeout = 3000;\n#else\ntimeout.tv_sec = 3;\ntimeout.tv_usec = 0;\n#endif", text)
    if count != 4 or text.count("&timeout, sizeof(struct timeval)") != 4:
        raise RuntimeError("Unexpected miniupnpc timeout calls")
    text = text.replace("&timeout, sizeof(struct timeval)", "(const char *)&timeout, sizeof(timeout)")
    miniupnpc.write_text("/* Modified for the NetBlood UDP companion (2026): correct Winsock timeout ABI. */\n" + text)


def stage_licenses(source, destination):
    destination.mkdir(parents=True, exist_ok=True)
    destination.chmod(0o755)
    notices = [
        (HERE / "LICENSE", "companion-MIT.txt"),
        (HERE / "NOTICE", "NOTICE.txt"),
        (source / "LICENSE.txt", "libzt-original-license.txt"),
        (source / "ext/ZeroTierOne/LICENSE.txt", "ZeroTierOne-original-license.txt"),
        (source / "ext/ZeroTierOne/AUTHORS.md", "ZeroTierOne-AUTHORS.md"),
        (source / "ext/THIRDPARTY.txt", "libzt-third-party.txt"),
        (source / "ext/lwip/COPYING", "lwip-COPYING.txt"),
        (HERE / "sources.json", "sources.json"),
    ]
    for origin, name in notices:
        if not origin.is_file():
            raise RuntimeError(f"Missing dependency license/notice: {origin}")
        shutil.copyfile(origin, destination / name)
        (destination / name).chmod(0o644)
    # Both pinned BSL works have passed their explicit Apache-2.0 change dates.
    # Full Apache text is supplied independently of third-party licenses.
    shutil.copyfile(HERE / "Apache-2.0.txt", destination / "Apache-2.0.txt")
    (destination / "Apache-2.0.txt").chmod(0o644)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path, default=Path(".ci-build/zerotier"))
    parser.add_argument("--output-dir", type=Path, default=Path("dist/zerotier"))
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 4))
    parser.add_argument("--offline", action="store_true", help="Require all pinned archives in the cache")
    parser.add_argument("cmake_options", nargs=argparse.REMAINDER, help="CMake options after --, e.g. toolchain/architecture")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    work, output = args.work_dir.resolve(), args.output_dir.resolve()
    identity = hashlib.sha256((json.dumps(SOURCES, sort_keys=True) + RECIPE).encode()).hexdigest()[:16]
    source = work / ("sdk-" + identity)
    stamp = source / ".complete"
    if not stamp.is_file():
        staging = source.with_name(source.name + ".extracting")
        if staging.exists():
            shutil.rmtree(staging)
        for name, spec in SOURCES.items():
            extract(fetch(name, spec, work, args.offline), staging / spec["destination"])
        patch_sdk(staging)
        (staging / ".complete").write_text(identity + "\n")
        if source.exists():
            shutil.rmtree(source)
        staging.rename(source)
    options = args.cmake_options
    if options[:1] == ["--"]:
        options = options[1:]
    build = work / "build"
    run([args.cmake, "-S", HERE, "-B", build, "-DCMAKE_BUILD_TYPE=Release",
         "-DZEROTIER_SDK_SOURCE=" + str(source), *options])
    run([args.cmake, "--build", build, "--config", "Release", "--target", "netblood-zerotier", "--parallel", args.jobs])
    run([args.cmake, "--install", build, "--config", "Release", "--prefix", output])
    stage_licenses(source, output / "zerotier-licenses")
    info = output / "zerotier-build-info.json"
    info.write_text(json.dumps({"recipe": RECIPE, "sources": SOURCES}, indent=2) + "\n")
    info.chmod(0o644)
    print("Companion and dependency notices staged in", output)


if __name__ == "__main__":
    main()
