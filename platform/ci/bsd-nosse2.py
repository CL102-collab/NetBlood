#!/usr/bin/env python3
"""Audit NetBSD package code, including bundled SDL2, for the i386 CPU floor."""
import argparse
import importlib.util
from pathlib import Path
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--library-dir", type=Path, required=True)
    parser.add_argument("--objdump", required=True)
    args = parser.parse_args()
    # The instruction audit operates on GNU objdump output, independently of
    # its caller's container format. Share the same CPU rules with the XP build.
    spec = importlib.util.spec_from_file_location("cpu_audit", Path(__file__).with_name("windows-verify.py"))
    verifier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verifier)
    directory = args.library_dir.resolve()
    paths = {args.binary.resolve()}
    for library in directory.iterdir():
        resolved = library.resolve(strict=True)
        if not resolved.is_relative_to(directory):
            raise SystemExit(f"Bundled library points outside the package: {library}")
        paths.add(resolved)
    for path in sorted(paths):
        with path.open("rb") as stream:
            header = stream.read(20)
        if header[:6] != b"\x7fELF\x01\x01" or struct.unpack_from("<H", header, 18)[0] != 3:
            raise SystemExit(f"Expected an ELF32 little-endian i386 binary: {path}")
        verifier.verify_no_sse2(path, args.objdump)
        print(f"Verified ELF32 i386 and no instructions beyond Pentium III: {path.name}")


if __name__ == "__main__":
    main()
