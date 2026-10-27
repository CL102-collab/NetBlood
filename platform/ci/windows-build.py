#!/usr/bin/env python3
"""Build and package Windows targets entirely on a Linux host.

All third-party source archives and the ARM64 cross compiler are pinned by
SHA256. The XP build compiles its own dependencies without SSE2; it never uses
the precompiled libraries in platform/Windows/lib. No commercial game assets
are included. See platform/ci/README.md for the CI matrix and prerequisites.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[2]
SOURCES = json.loads(Path(__file__).with_name("windows-sources.json").read_text())
TARGETS = ("windows-x86_64", "windows-i686", "windows-xp-i686-nosse2", "windows-arm64")


def run(args, **kwargs):
    print("+", shlex.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True, **kwargs)


def source(name, work):
    spec = SOURCES[name]
    archive = work / "downloads" / (name + (".tar.xz" if spec["url"].endswith(".xz") else ".tar.gz"))
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        temporary = archive.with_suffix(".part")
        with urllib.request.urlopen(spec["url"], timeout=120) as response, temporary.open("wb") as output:
            shutil.copyfileobj(response, output)
        temporary.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != spec["sha256"]:
        raise RuntimeError(f"SHA256 mismatch for {archive}; remove the cached archive and retry")
    destination = work / "sources" / (name + "-" + spec["sha256"][:12])
    if not destination.exists():
        temporary = destination.with_name(destination.name + ".extracting")
        if temporary.exists():
            shutil.rmtree(temporary)
        temporary.mkdir(parents=True)
        with tarfile.open(archive) as package:
            package.extractall(temporary, filter="data")
        children = list(temporary.iterdir())
        if len(children) != 1 or not children[0].is_dir():
            raise RuntimeError(f"Unexpected archive structure: {archive}")
        children[0].rename(destination)
        temporary.rmdir()
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=TARGETS, required=True)
    parser.add_argument("--output", type=Path, default=Path("dist"))
    parser.add_argument("--work-dir", type=Path, default=Path(".ci-windows"))
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--dependencies-only", action="store_true")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    overlay_network = os.environ.get("ZEROTIER_NETWORK_ID", "")
    if overlay_network and not re.fullmatch(r"[0-9a-fA-F]{16}", overlay_network):
        parser.error("ZEROTIER_NETWORK_ID must be a 16-digit hexadecimal network ID")
    work, output = args.work_dir.resolve(), args.output.resolve()
    xp = args.target == "windows-xp-i686-nosse2"
    arm = args.target == "windows-arm64"
    architecture = "aarch64" if arm else "i686" if "i686" in args.target else "x86_64"
    triple = architecture + "-w64-mingw32"
    env = os.environ.copy()
    # A locally activated Linux SDK must not leak headers or libraries into
    # the cross build. Target flags and pkg-config roots are set below.
    for name in ("CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "CPATH", "C_INCLUDE_PATH",
                 "CPLUS_INCLUDE_PATH", "LIBRARY_PATH", "PKG_CONFIG_SYSROOT_DIR"):
        env.pop(name, None)
    if arm:
        compiler_root = source("llvm-mingw", work)
        env["PATH"] = str(compiler_root / "bin") + os.pathsep + env["PATH"]
        cc, cxx = triple + "-clang", triple + "-clang++"
    else:
        cc, cxx = triple + "-gcc-posix", triple + "-g++-posix"
    for tool in (cc, cxx, triple + "-ar", triple + "-windres", "cmake", "make", "pkg-config"):
        if not shutil.which(tool, path=env["PATH"]):
            raise RuntimeError(f"Required build tool is missing: {tool}")
    target_work = work / args.target
    prefix = target_work / "prefix"
    prefix.mkdir(parents=True, exist_ok=True)
    # x87 avoids an accidental SSE2 requirement in floating-point generated code.
    baseline = "-march=pentium3 -mno-sse2 -mfpmath=387" if xp else ""
    minimum = "0x0501" if xp else "0x0a00" if arm else "0x0601"
    cflags = f"-O2 {baseline} -D_WIN32_WINNT={minimum} -DWINVER={minimum}"
    if xp:
        cflags += " -DENET_IPV4_ONLY=1"
    toolchain = target_work / "toolchain.cmake"
    toolchain.write_text("\n".join([
        "set(CMAKE_SYSTEM_NAME Windows)",
        f"set(CMAKE_SYSTEM_PROCESSOR {'arm64' if arm else architecture})",
        f"set(CMAKE_C_COMPILER {cc})", f"set(CMAKE_CXX_COMPILER {cxx})",
        f"set(CMAKE_RC_COMPILER {triple}-windres)",
        f"set(CMAKE_AR {triple}-ar)", f"set(CMAKE_RANLIB {triple}-ranlib)",
        f'set(CMAKE_C_FLAGS_INIT "{cflags}")', f'set(CMAKE_CXX_FLAGS_INIT "{cflags}")',
        'set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")',
        'set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")',
        "set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)",
        "set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)",
        "set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)",
        "set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)",
        f'set(CMAKE_FIND_ROOT_PATH "{prefix}")',
    ]) + "\n")
    env["PKG_CONFIG_LIBDIR"] = str(prefix / "lib/pkgconfig")
    env["PKG_CONFIG_PATH"] = ""
    source_dirs = {}

    def cmake_library(name, options):
        src = source_dirs.setdefault(name, source(name, work))
        build = target_work / (name + "-build")
        # A stamp is valid only for the exact source, compiler and configuration.
        configuration = [SOURCES[name], cc, cflags, options, "recipe-3"]
        stamp = build / "netblood-installed.json"
        if stamp.exists() and json.loads(stamp.read_text()) == configuration:
            return
        run(["cmake", "-S", src, "-B", build,
             f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", "-DCMAKE_BUILD_TYPE=Release",
             f"-DCMAKE_INSTALL_PREFIX={prefix}", "-DCMAKE_INSTALL_LIBDIR=lib", *options], env=env)
        run(["cmake", "--build", build, "--parallel", args.jobs], env=env)
        run(["cmake", "--install", build], env=env)
        stamp.write_text(json.dumps(configuration))

    sdl_options = ["-DSDL_SHARED=OFF", "-DSDL_STATIC=ON", "-DSDL_TEST=OFF", "-DSDL_TESTS=OFF",
                   "-DSDL_LIBC=ON", "-DSDL_WERROR=OFF", "-DSDL_SSE3=OFF"]
    if xp:
        # SDL's configure probes otherwise add -msse2 globally. Its optimized
        # translation units are disabled too, not merely the application's flags.
        sdl_options += ["-DSDL_ASSEMBLY=OFF", "-DSDL_SSEMATH=OFF", "-DSDL_SSE=OFF",
                        "-DSDL_SSE2=OFF", "-DSDL_SSE3=OFF", "-DSDL_MMX=OFF",
                        "-DSDL_VULKAN=OFF", "-DSDL_WASAPI=OFF", "-DSDL_HIDAPI=OFF"]
    cmake_library("sdl2", sdl_options)
    cmake_library("flac", ["-DBUILD_SHARED_LIBS=OFF", "-DBUILD_CXXLIBS=OFF", "-DBUILD_PROGRAMS=OFF",
                           "-DBUILD_EXAMPLES=OFF", "-DBUILD_TESTING=OFF", "-DBUILD_DOCS=OFF",
                           "-DWITH_OGG=OFF", "-DWITH_STACK_PROTECTOR=OFF", "-DWITH_FORTIFY_SOURCE=OFF",
                           "-DWITH_ASM=OFF", "-DWITH_AVX=OFF"])

    src = source_dirs["vpx"] = source("vpx", work)
    vpx_build = target_work / "vpx-build"
    vpx_build.mkdir(exist_ok=True)
    vpx_stamp = vpx_build / "netblood-installed.json"
    vpx_configuration = [SOURCES["vpx"], cc, cflags, "portable-decoder-2"]
    if not vpx_stamp.exists() or json.loads(vpx_stamp.read_text()) != vpx_configuration:
        vpx_env = dict(env, CC=cc, CXX=cxx, AR=triple + "-ar", LD=cxx,
                       STRIP=triple + "-strip", RANLIB=triple + "-ranlib",
                       CFLAGS=cflags, CXXFLAGS=cflags, LDFLAGS="-static")
        run([src / "configure", "--target=generic-gnu", f"--prefix={prefix}",
             "--disable-shared", "--enable-static", "--disable-examples", "--disable-tools",
             "--disable-docs", "--disable-unit-tests", "--disable-vp8-encoder", "--disable-vp9-encoder",
             "--disable-multithread", "--disable-runtime-cpu-detect"], cwd=vpx_build, env=vpx_env)
        run(["make", f"-j{args.jobs}"], cwd=vpx_build, env=vpx_env)
        run(["make", "install"], cwd=vpx_build, env=vpx_env)
        vpx_stamp.write_text(json.dumps(vpx_configuration))

    # These GUID objects are the project's SDK additions, not precompiled x86 archives.
    shutil.copytree(ROOT / "platform/Windows/include/dx", prefix / "include/dx", dirs_exist_ok=True)
    for name in ("dxguid", "dxguid_sdl"):
        obj = target_work / (name + ".o")
        run([cc, *shlex.split(cflags), "-I" + str(ROOT / "platform/Windows/include"), "-c",
             ROOT / f"platform/Windows/src/dxlibs/{name}.c", "-o", obj], env=env)
        run([triple + "-ar", "rcs", prefix / f"lib/lib{name}.a", obj], env=env)
    if not xp:
        cmake_library("vulkan-headers", ["-DVULKAN_HEADERS_ENABLE_TESTS=OFF", "-DVULKAN_HEADERS_ENABLE_MODULE=OFF"])
    if args.dependencies_only:
        return

    executable = target_work / "nblood.exe"
    make_options = ["PLATFORM=WINDOWS", f"CROSS={triple}-", f"CC={cc}", f"CXX={cxx}",
                    f"obj={target_work / 'obj'}", f"blood_game={target_work / 'nblood'}",
                    f"DEPENDENCY_PREFIX={prefix}", f"SDLCONFIG={prefix / 'bin/sdl2-config'}",
                    # This SDL SDK is static-only: its --libs already names the
                    # archive. Avoid Common.mak's Linux -Bdynamic SDL wrapper.
                    "SDL_STATIC=0", "LTO=0", "NOASM=1", "HAVE_GTK2=0",
                    f"USE_VULKAN={0 if xp else 1}", f"USE_MIMALLOC={0 if xp else 1}",
                    f"CUSTOMOPT={cflags}", f"OPTOPT={baseline}",
                    f"ZEROTIER_NETWORK_ID={overlay_network}",
                    f"WINDOWS_MAJOR={5 if xp else 10 if arm else 6}", f"WINDOWS_MINOR={1 if xp else 0 if arm else 1}"]
    if xp:
        # Retain symbols for the CPU audit, which identifies libstdc++'s
        # CPUID-dispatched entropy routines. Strip only after verification.
        make_options += ["STRIP="]
    else:
        make_options += [f"VULKAN_CFLAGS=-I{prefix / 'include'}", "VULKAN_LIBS="]
    run(["make", f"-j{args.jobs}", executable, *make_options], cwd=ROOT, env=env)
    stage = target_work / "stage"
    if stage.exists():
        shutil.rmtree(stage)
    stage_options = []
    if xp:
        # Do not turn the legacy package into an SSE2/modern-Windows package
        # merely to include an SDK whose baseline has not been validated.
        stage_options = ["--zerotier-unavailable",
                         "This Windows XP/no-SSE2 package provides Internet multiplayer only. "
                         "The ZeroTier SDK has no validated XP/no-SSE2 baseline."]
    else:
        companion = target_work / "zerotier-output"
        run([sys.executable, ROOT / "tools/zerotier/build.py",
             "--work-dir", target_work / "zerotier-build", "--output-dir", companion,
             "--jobs", args.jobs, "--", f"-DCMAKE_TOOLCHAIN_FILE={toolchain}"], env=env)
        stage_options = ["--zerotier-build", companion]
    run([sys.executable, ROOT / "platform/ci/package.py", "stage", "--destination", stage,
         *stage_options], env=env)
    shutil.copy2(executable, stage / "nblood.exe")
    (stage / "INSTALL.txt").write_text(
        "Extract this directory together with its licenses.\n"
        "Supply your own Blood 1.21 game files using the startup window.\n"
        + ("This legacy Windows XP/no-SSE2 package supports Internet multiplayer only.\n" if xp else
           "The bundled ZeroTier companion needs no separate installation or administrator access.\n")
        + "See docs/zerotier.md and ZEROTIER-NOTICE.txt for platform and network setup.\n")
    licenses = stage / "licenses" / "windows-dependencies"
    licenses.mkdir(parents=True)
    for name, src in source_dirs.items():
        dest = licenses / name
        dest.mkdir()
        for pattern in ("LICENSE*", "COPYING*", "AUTHORS*", "PATENTS*"):
            for file in src.glob(pattern):
                if file.is_file():
                    shutil.copy2(file, dest / file.name)
        if name == "vulkan-headers":
            # Public API headers are dual MIT/Apache2; choose MIT for this GPLv2
            # application. No Apache2 Vulkan loader is linked or redistributed.
            shutil.copy2(src / "LICENSES/MIT.txt", dest / "MIT.txt")
    shutil.copy2(Path(__file__).with_name("windows-sources.json"), licenses / "sources.json")
    runtime_licenses = licenses / "compiler-runtime"
    runtime_licenses.mkdir()
    if arm:
        shutil.copy2(compiler_root / "LICENSE.TXT", runtime_licenses / "LLVM-LICENSE.TXT")
        for file in (compiler_root / triple / "share/mingw32").glob("COPYING*"):
            shutil.copy2(file, runtime_licenses / file.name)
    else:
        # Debian/Ubuntu cross-compiler packages install the runtime exception and
        # MinGW licensing here. Also support a locally extracted .deb sysroot.
        compiler_bin = Path(shutil.which(cc, path=env["PATH"]))
        doc_roots = [Path("/usr/share/doc"), compiler_bin.parent.parent / "share/doc"]
        for package_name in ("gcc-mingw-w64-base", "mingw-w64-common"):
            found = next((base / package_name / "copyright" for base in doc_roots
                          if (base / package_name / "copyright").is_file()), None)
            if found is None:
                raise RuntimeError(f"Missing compiler runtime license: {package_name}/copyright")
            shutil.copy2(found, runtime_licenses / (package_name + "-copyright.txt"))
        # Debian copyright files include the runtime exception but refer to this
        # external file for the full GNU license; Windows recipients need both.
        shutil.copy2("/usr/share/common-licenses/GPL-3", runtime_licenses / "GPL-3.txt")
    (stage / "windows-build.json").write_text(json.dumps({
        "target": args.target, "compiler": subprocess.check_output([cxx, "--version"], env=env, text=True).splitlines()[0],
        "compiler_flags": cflags, "make_options": make_options,
        "vulkan": not xp, "vulkan_runtime": "external GPU-driver runtime, loaded on demand" if not xp else "unsupported on XP",
        "network": "IPv4" if xp else "IPv4 and IPv6",
        "zerotier_companion": not xp,
        "zerotier_shared_network_configured": bool(overlay_network),
    }, indent=2) + "\n")
    run([sys.executable, Path(__file__).with_name("windows-verify.py"), "--directory", stage,
         "--target", args.target, "--objdump", triple + "-objdump"], env=env)
    if xp:
        run([triple + "-strip", stage / "nblood.exe"], env=env)
        report_path = stage / "windows-verification.json"
        report = json.loads(report_path.read_text())
        report["nblood.exe"]["cpu_audit"]["symbols_stripped_after_audit"] = True
        report["nblood.exe"]["cpu_audit"]["packaged_sha256"] = hashlib.sha256((stage / "nblood.exe").read_bytes()).hexdigest()
        report_path.write_text(json.dumps(report, indent=2) + "\n")
    output.mkdir(parents=True, exist_ok=True)
    archive = output / ("netblood-" + args.target + ".zip")
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as package:
        for file in sorted(stage.rglob("*")):
            if file.is_file():
                package.write(file, file.relative_to(stage))
    print(f"Package: {archive} ({archive.stat().st_size} bytes)", flush=True)


if __name__ == "__main__":
    main()
