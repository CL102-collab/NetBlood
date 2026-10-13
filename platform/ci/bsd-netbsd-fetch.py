#!/usr/bin/env python3
"""Download fixed NetBSD release sets and verify the release SHA512 manifest."""
import argparse
import hashlib
import pathlib
import re
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", choices=("source", "i386"), required=True)
    parser.add_argument("--destination", type=pathlib.Path, required=True)
    parser.add_argument("sets", nargs="+")
    args = parser.parse_args()
    suffix = "source/sets" if args.kind == "source" else "i386/binary/sets"
    base = f"https://cdn.NetBSD.org/pub/NetBSD/NetBSD-10.1/{suffix}"
    manifest = urllib.request.urlopen(base + "/SHA512", timeout=120).read().decode("ascii")
    entries = dict(re.findall(r"SHA512 \(([^)]+)\) = ([0-9a-fA-F]{128})", manifest))
    args.destination.mkdir(parents=True, exist_ok=True)
    (args.destination / "SHA512").write_text(manifest)
    for name in args.sets:
        # NetBSD changed the set compression format between releases. Select
        # only an exact set name present in this fixed release's checksum list.
        matches = [filename for filename in (name + ".tar.xz", name + ".tgz") if filename in entries]
        if len(matches) != 1:
            raise SystemExit(f"Expected one {name} set in NetBSD 10.1 {suffix}")
        filename = matches[0]
        path = args.destination / filename
        if not path.exists():
            temporary = path.with_suffix(path.suffix + ".partial")
            with urllib.request.urlopen(base + "/" + filename, timeout=120) as source, temporary.open("wb") as output:
                while block := source.read(1024 * 1024):
                    output.write(block)
            temporary.rename(path)
        digest = hashlib.sha512()
        with path.open("rb") as source:
            while block := source.read(1024 * 1024):
                digest.update(block)
        if digest.hexdigest() != entries[filename].lower():
            raise SystemExit(f"SHA512 mismatch: {path}")
        print(path)


if __name__ == "__main__":
    main()
