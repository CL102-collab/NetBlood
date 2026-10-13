#!/usr/bin/env python3
"""Assemble and audit a self-contained, two-architecture macOS application."""
import argparse
import pathlib
import plistlib
import shutil
import subprocess
import sys


def run(*args):
    return subprocess.check_output(args, text=True)


def dependencies(path):
    return {line.strip().split(" (", 1)[0] for line in run("otool", "-L", str(path)).splitlines()
            if line.startswith("\t")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-root", required=True, type=pathlib.Path)
    parser.add_argument("--destination", required=True, type=pathlib.Path)
    args = parser.parse_args()
    repo = pathlib.Path(__file__).resolve().parents[2]
    work, app = args.build_root.resolve(), args.destination.resolve()
    if app.exists():
        shutil.rmtree(app)
    contents = app / "Contents"
    frameworks = contents / "Frameworks"
    resources = contents / "Resources"
    executables = contents / "MacOS"
    for path in (frameworks, resources, executables):
        path.mkdir(parents=True)
    run(sys.executable, str(repo / "platform/ci/package.py"), "stage",
        "--destination", str(resources))
    for name in ("nblood.icns", "game.png"):
        shutil.copy2(repo / "platform/Apple/bundles/NBlood.app/Contents/Resources" / name, resources)
    binary = executables / "NBlood"
    run("lipo", "-create", str(work / "x86_64/nblood"), str(work / "arm64/nblood"), "-output", str(binary))

    # FLAC and VPX are linked statically. SDL is the only bundled dylib; audit
    # its transitive dependencies instead of trusting the build environment.
    sdl_name = "libSDL2-2.0.0.dylib"
    sdl = frameworks / sdl_name
    run("lipo", "-create", str(work / "x86_64/prefix/lib" / sdl_name),
        str(work / "arm64/prefix/lib" / sdl_name), "-output", str(sdl))
    dylibs = (sdl,)
    bundled = {path.name for path in dylibs}
    for path in (binary, *dylibs):
        run("lipo", "-verify_arch", "x86_64", "arm64", str(path))
        for dependency in dependencies(path):
            if dependency.startswith(("/usr/lib/", "/System/Library/")):
                continue
            name = pathlib.PurePosixPath(dependency).name
            if name not in bundled:
                raise SystemExit(f"Unbundled dependency in {path.name}: {dependency}")
            run("install_name_tool", "-change", dependency, "@rpath/" + name, str(path))
        if path in dylibs:
            run("install_name_tool", "-id", "@rpath/" + path.name, str(path))
    run("install_name_tool", "-add_rpath", "@executable_path/../Frameworks", str(binary))
    for path in (binary, *dylibs):
        for dependency in dependencies(path):
            if not dependency.startswith(("/usr/lib/", "/System/Library/", "@rpath/")):
                raise SystemExit(f"Non-relocatable dependency: {dependency}")

    license_dir = resources / "licenses"
    license_dir.mkdir(exist_ok=True)
    for source, target in (("SDL2/LICENSE.txt", "SDL2.txt"), ("FLAC/COPYING.Xiph", "FLAC.txt"),
                           ("libvpx/LICENSE", "libvpx.txt"), ("libvpx/PATENTS", "libvpx-PATENTS.txt")):
        shutil.copy2(work / "src" / source, license_dir / target)
    info = {
        "CFBundleDevelopmentRegion": "English", "CFBundleDisplayName": "NetBlood",
        "CFBundleExecutable": "NBlood", "CFBundleIconFile": "nblood.icns",
        "CFBundleIdentifier": "org.netblood.NetBlood", "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": "NetBlood", "CFBundlePackageType": "APPL", "CFBundleVersion": "1",
        "CFBundleShortVersionString": "1.0", "LSMinimumSystemVersion": "11.0",
        "LSApplicationCategoryType": "public.app-category.action-games",
        "NSHighResolutionCapable": True, "NSSupportsAutomaticGraphicsSwitching": True,
        "NSPrincipalClass": "NSApplication",
    }
    (contents / "Info.plist").write_bytes(plistlib.dumps(info))
    (resources / "INSTALL.txt").write_text(
        "Requires macOS 11 or later. Both Intel and Apple Silicon are included.\n"
        "Supply your own Blood 1.21 game files in the startup window.\n"
        "This macOS package includes software and Polymost OpenGL rendering.\n"
        "This development package is ad-hoc signed, not Apple-notarized.\n")
    for path in (*dylibs, binary, app):
        run("codesign", "--force", "--sign", "-", str(path))
    run("codesign", "--verify", "--deep", "--strict", str(app))
    if "Usage:" not in run(str(binary), "-dedicated", "-?"):
        raise SystemExit("Packaged native executable did not print its command-line help")
    print(f"Verified both architectures, dependency closure and signature: {app}")


if __name__ == "__main__":
    main()
