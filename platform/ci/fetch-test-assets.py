#!/usr/bin/env python3
"""Restore checksum-pinned Blood 1.21 test data; never include it in packages."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import time
import urllib.request

DISC_URL = "https://archive.org/download/blood121/BLOOD121%20%28Track%201%29.bin"
DISC_SHA256 = "c0c37ba75f9a13918dc83405d9b58a597144399477351d8842f18fddb8fe89f7"
DISC_SIZE = 415810080
TOOL_COMMIT = "4e5700fb97ba9f0338b49c1031834ce42177f265"
TOOL_SHA256 = "25270989563a7a56b8ad3a86f513ca940c63554d1c21bb03e6423ab713c6cfe1"
MANIFEST = Path(__file__).with_name("fetch-test-assets.sha256.json")


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def verify(directory):
    manifest = json.loads(MANIFEST.read_text())
    for name, expected in manifest.items():
        path = directory / name
        if not path.is_file() or path.is_symlink() or digest(path) != expected:
            raise RuntimeError(f"missing or changed Blood 1.21 test asset: {path}")
    print(f"Verified all {len(manifest)} Blood 1.21 test files in {directory}", flush=True)


def download(url, path, checksum):
    if path.exists():
        if digest(path) != checksum:
            raise RuntimeError(f"corrupt download cache: {path}; remove it and retry")
        return
    partial = path.with_name(path.name + ".partial")
    for attempt in range(3):
        try:
            with urllib.request.urlopen(url, timeout=90) as response, partial.open("wb") as target:
                shutil.copyfileobj(response, target)
            if digest(partial) != checksum:
                raise RuntimeError(f"download checksum mismatch: {url}")
            partial.replace(path)
            return
        except (OSError, RuntimeError) as error:
            partial.unlink(missing_ok=True)
            if attempt == 2:
                raise RuntimeError(f"required test-data download failed: {error}") from error
            print(f"Download attempt {attempt + 1} failed: {error}; retrying", flush=True)
            time.sleep(2 ** attempt)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--cache-dir", type=Path, required=True)
    parser.add_argument("--disc-url", default=os.environ.get("NETBLOOD_TEST_DISC_URL", DISC_URL),
                        help="optional mirror of the exact pinned disc; checksums remain mandatory")
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    output, cache = args.output_dir.resolve(), args.cache_dir.resolve()
    if output.exists() or args.verify_only:
        verify(output)
        return
    # Only the fetch stage needs the ISO library. Validation of cached data does not.
    import pycdlib

    cache.mkdir(parents=True, exist_ok=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    raw = cache / "BLOOD121-track1.bin"
    download(args.disc_url, raw, DISC_SHA256)
    if raw.stat().st_size != DISC_SIZE:
        raise RuntimeError("unexpected Blood 1.21 disc size")
    tool_archive = cache / "unshieldv3.tar.gz"
    download(f"https://codeload.github.com/wfr/unshieldv3/tar.gz/{TOOL_COMMIT}",
             tool_archive, TOOL_SHA256)
    with tempfile.TemporaryDirectory(prefix="blood121-extract-", dir=output.parent) as temporary:
        temporary = Path(temporary)
        iso_path = temporary / "disc.iso"
        with raw.open("rb") as source, iso_path.open("wb") as target:
            while sector := source.read(2352):
                if len(sector) != 2352 or sector[15] != 1:
                    raise RuntimeError("unexpected raw CD sector format")
                target.write(sector[16:2064])
        with tarfile.open(tool_archive) as archive:
            archive.extractall(temporary, filter="data")
        tool_source = temporary / f"unshieldv3-{TOOL_COMMIT}"
        config = (tool_source / "config.h.in").read_text()
        (tool_source / "config.h").write_text(config.replace("@PROJECT_NAME@", "unshieldv3")
                                             .replace("@PROJECT_VERSION@", "0.2.2"))
        subprocess.run(["cc", "-O2", "-c", "blast.c", "-o", "blast.o"], cwd=tool_source, check=True)
        subprocess.run(["c++", "-O2", "-std=c++17", "main.cpp", "ISArchiveV3.cpp", "blast.o",
                        "-o", "unshieldv3"], cwd=tool_source, check=True)
        disc = pycdlib.PyCdlib()
        disc.open(str(iso_path))
        try:
            if disc.pvd.volume_identifier.decode("ascii").strip() != "BLOOD121":
                raise RuntimeError("unexpected disc volume identifier")
            packed = temporary / "data.z"
            disc.get_file_from_iso(str(packed), iso_path="/data.z")
            unpacked = temporary / "unpacked"
            unpacked.mkdir()
            subprocess.run([str(tool_source / "unshieldv3"), "extract", str(packed), str(unpacked)], check=True)
            staging = temporary / "assets"
            staging.mkdir()
            for path in unpacked.iterdir():
                if path.suffix.lower() in {".rff", ".art", ".dat", ".dem"}:
                    shutil.copyfile(path, staging / path.name.upper())
            disc.get_file_from_iso(str(staging / "BLOOD.INI"), iso_path="/blood.ini")
            disc.get_file_from_iso(str(staging / "README-1.21.TXT"), iso_path="/readme.txt")
            for directory, _, files in disc.walk(iso_path="/cryptic"):
                for name in files:
                    if Path(name).suffix.lower() in {".ini", ".map", ".ar_", ".smk", ".wav"}:
                        disc.get_file_from_iso(str(staging / name.upper()), iso_path=directory + "/" + name)
            verify(staging)
            staging.rename(output)
        finally:
            disc.close()
    print(f"Restored required test data to {output}; do not upload it as a build artifact.")


if __name__ == "__main__":
    main()
