#!/usr/bin/env python3
"""Stage redistributable NetBlood resources; verify a complete release matrix."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
PACKAGES = (
    "netblood-windows-x86_64.zip",
    "netblood-windows-i686.zip",
    "netblood-windows-xp-i686-nosse2.zip",
    "netblood-windows-arm64.zip",
    "netblood-debian12-amd64.deb",
    "netblood-debian12-i386.deb",
    "netblood-debian12-i386-nosse2.deb",
    "netblood-debian12-armhf.deb",
    "netblood-debian12-arm64.deb",
    "netblood-linux-x86_64.AppImage",
    "netblood-macos-universal.zip",
    "netblood-freebsd-amd64.tar.xz",
    "netblood-netbsd-i386-nosse2.tar.xz",
)
ZEROTIER_NOTICES = ("companion-MIT.txt", "NOTICE.txt", "libzt-original-license.txt",
                    "ZeroTierOne-original-license.txt", "ZeroTierOne-AUTHORS.md",
                    "libzt-third-party.txt", "lwip-COPYING.txt", "sources.json", "Apache-2.0.txt")


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args], text=True).strip()


def stage(destination, zerotier_build=None, executable_directory=None, zerotier_unavailable=None):
    revision = os.environ.get("NETBLOOD_REVISION") or git("rev-parse", "HEAD")
    epoch = os.environ.get("SOURCE_DATE_EPOCH") or git("show", "-s", "--format=%ct", "HEAD")
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise ValueError("NETBLOOD_REVISION must be the full source commit SHA")
    destination.mkdir(parents=True, exist_ok=True)
    # Explicit allowlist: never stage a developer's game data, config, or saves.
    resources = [(ROOT / "nblood.pk3", "nblood.pk3"),
                 (ROOT / "README.md", "readme.txt"),
                 (ROOT / "AUTHORS.md", "AUTHORS.md"),
                 (ROOT / "docs/zerotier.md", "docs/zerotier.md"),
                 (ROOT / "tools/zerotier/NOTICE", "licenses/zerotier-companion-NOTICE.txt"),
                 (ROOT / "tools/zerotier/LICENSE", "licenses/zerotier-companion-MIT.txt"),
                 (ROOT / "platform/ci/thirdparty-notices.txt", "licenses/thirdparty-notices.txt")]
    resources += [(ROOT / "package/common" / name, name) for name in
                  ("buildlic.txt", "gpl-2.0.txt", "gamecontrollerdb.txt")]
    resources += [(ROOT / path, "licenses/" + path.removeprefix("source/").replace("/", "-"))
                  for path in ("source/libtheora/COPYING", "source/libsmackerdec/COPYING",
                               "source/mimalloc/LICENSE", "source/voidwrap/license.txt",
                               "source/libxmp-lite/README", "source/blood/data/netcountry/IPVERSE-CC0.txt",
                               "source/blood/data/netcountry/FLAG-ICONS-MIT.txt",
                               "source/blood/data/netcountry/README.md")]
    for source, name in resources:
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        target.chmod(0o644)
    network = os.environ.get("ZEROTIER_NETWORK_ID", "")
    if network and not re.fullmatch(r"[0-9a-fA-F]{16}", network):
        raise ValueError("ZEROTIER_NETWORK_ID must be a 16-digit hexadecimal network ID")
    overlay = {"bundled": bool(zerotier_build), "shared_network_configured": bool(network)}
    if zerotier_build:
        helpers = [zerotier_build / name for name in ("netblood-zerotier", "netblood-zerotier.exe")
                   if (zerotier_build / name).is_file()]
        if len(helpers) != 1:
            raise ValueError(f"Expected exactly one ZeroTier companion executable in {zerotier_build}")
        notices = zerotier_build / "zerotier-licenses"
        manifest = zerotier_build / "zerotier-build-info.json"
        if not manifest.is_file() or any(not (notices / name).is_file() for name in ZEROTIER_NOTICES):
            raise ValueError("ZeroTier companion build is missing its licenses or source manifest")
        binaries = executable_directory or destination
        binaries.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(helpers[0], binaries / helpers[0].name)
        (binaries / helpers[0].name).chmod(0o755)
        shutil.copytree(notices, destination / "licenses/zerotier", dirs_exist_ok=True)
        shutil.copyfile(manifest, destination / manifest.name)
        overlay_notice = ("The ZeroTier companion and its dependency licenses are included.\n"
                          "See docs/zerotier.md for multiplayer setup and network availability.\n")
        if not network:
            overlay_notice += "No shared ZeroTier network is configured in this build.\n"
    else:
        overlay["reason"] = zerotier_unavailable or (
            "The ZeroTier companion is not included for this platform. "
            "Internet multiplayer remains available.")
        overlay_notice = overlay["reason"] + "\nSee docs/zerotier.md.\n"
    (destination / "ZEROTIER-NOTICE.txt").write_text(overlay_notice)
    (destination / "build-info.json").write_text(json.dumps({
        "application": "NetBlood", "revision": revision, "source_date_epoch": int(epoch),
        "source": "https://github.com/dukenukem-king/NetBlood/tree/" + revision,
        "game_data_included": False,
        "zerotier": overlay,
    }, indent=2) + "\n")
    (destination / "BUILD-NOTICE.txt").write_text(
        '"Build Engine & Tools" Copyright (c) 1993-1997 Ken Silverman\n'
        "Ken Silverman's official web site: http://www.advsys.net/ken\n"
        "See the included license file buildlic.txt for license info.\n\n"
        "Blood game data is required and is not included. See readme.txt.\n"
        "The exact application source revision is recorded in build-info.json.\n"
        "ZeroTier availability is recorded in ZEROTIER-NOTICE.txt.\n")
    print(f"Staged {len(resources)} resources in {destination}")


def verify_release(directory):
    actual = {p.name for p in directory.iterdir() if p.is_file()
              and (p.suffix in (".zip", ".deb", ".AppImage") or p.name.endswith(".tar.xz"))}
    if actual != set(PACKAGES):
        raise ValueError(f"Incomplete package matrix: missing={sorted(set(PACKAGES) - actual)}, "
                         f"unexpected={sorted(actual - set(PACKAGES))}")
    checksums = []
    for name in PACKAGES:
        path = directory / name
        if path.stat().st_size < 1024:
            raise ValueError(f"Empty or implausibly small package: {name}")
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        checksums.append(f"{digest}  {name}\n")
    (directory / "SHA256SUMS").write_text("".join(checksums))
    print(f"All {len(PACKAGES)} packages present; wrote SHA256SUMS")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    stage_parser = commands.add_parser("stage")
    stage_parser.add_argument("--destination", type=Path, required=True)
    overlay = stage_parser.add_mutually_exclusive_group()
    overlay.add_argument("--zerotier-build", type=Path,
                         help="Bundle the executable, licenses and source manifest from build.py")
    overlay.add_argument("--zerotier-unavailable", help="Explain an Internet-only platform/CPU package")
    stage_parser.add_argument("--executable-directory", type=Path,
                              help="Companion location when binaries and resources are installed separately")
    verify_parser = commands.add_parser("verify-release")
    verify_parser.add_argument("--directory", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "stage":
        if args.executable_directory and not args.zerotier_build:
            parser.error("--executable-directory requires --zerotier-build")
        stage(args.destination, args.zerotier_build, args.executable_directory, args.zerotier_unavailable)
    else:
        verify_release(args.directory)


if __name__ == "__main__":
    main()
