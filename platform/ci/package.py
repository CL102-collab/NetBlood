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


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args], text=True).strip()


def stage(destination):
    revision = os.environ.get("NETBLOOD_REVISION") or git("rev-parse", "HEAD")
    epoch = os.environ.get("SOURCE_DATE_EPOCH") or git("show", "-s", "--format=%ct", "HEAD")
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise ValueError("NETBLOOD_REVISION must be the full source commit SHA")
    destination.mkdir(parents=True, exist_ok=True)
    # Explicit allowlist: never stage a developer's game data, config, or saves.
    resources = [(ROOT / "nblood.pk3", "nblood.pk3"),
                 (ROOT / "README.md", "readme.txt"),
                 (ROOT / "AUTHORS.md", "AUTHORS.md"),
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
    (destination / "build-info.json").write_text(json.dumps({
        "application": "NetBlood", "revision": revision, "source_date_epoch": int(epoch),
        "source": "https://github.com/dukenukem-king/NetBlood/tree/" + revision,
        "game_data_included": False,
    }, indent=2) + "\n")
    (destination / "BUILD-NOTICE.txt").write_text(
        '"Build Engine & Tools" Copyright (c) 1993-1997 Ken Silverman\n'
        "Ken Silverman's official web site: http://www.advsys.net/ken\n"
        "See the included license file buildlic.txt for license info.\n\n"
        "Blood game data is required and is not included. See readme.txt.\n"
        "The exact application source revision is recorded in build-info.json.\n")
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
    verify_parser = commands.add_parser("verify-release")
    verify_parser.add_argument("--directory", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "stage":
        stage(args.destination)
    else:
        verify_release(args.directory)


if __name__ == "__main__":
    main()
