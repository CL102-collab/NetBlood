#!/usr/bin/env python3
"""Regenerate the offline scoreboard country table and 16x12 RGB flag atlas.

Requires Python 3, CairoSVG 2.8.2 and Pillow 12.3.0 for reproducible artwork.
The game build/runtime does not need these packages or network access.
"""

import argparse
import hashlib
import io
import ipaddress
from pathlib import Path
import tarfile
import urllib.request

COUNTRY_REV = "a090ea1c7275cfa675000f53b71a630b466d7636"
COUNTRY_SHA = "48c467650fe4d456c1ae22515249225b4e09d27c9194c7c130e1c1e15506d4b7"
FLAGS_REV = "086f7e97d657358203916dbe84f61c2bccaa81eb"
FLAGS_SHA = "eb5b814c794cda735155e2874e288ad9302b6f7a5728e5e50d3ef0333b6c20f4"
ROOT = Path(__file__).resolve().parents[1]


def archive(cache, filename, project, revision, checksum):
    path = cache / filename
    if not path.is_file():
        url = f"https://github.com/{project}/archive/{revision}.tar.gz"
        with urllib.request.urlopen(url, timeout=120) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != checksum:
            raise ValueError(f"SHA256 mismatch for {url}")
        path.write_bytes(data)
    if hashlib.sha256(path.read_bytes()).hexdigest() != checksum:
        raise ValueError(f"SHA256 mismatch for cached {path}")
    return tarfile.open(path, "r:gz")


def read_member(archive_file, suffix):
    matches = [m for m in archive_file.getmembers() if m.name.endswith(suffix)]
    if len(matches) != 1:
        raise ValueError(f"Expected one archive member ending {suffix}, got {len(matches)}")
    return archive_file.extractfile(matches[0]).read()


def country_ranges(source, family="ipv4"):
    ranges = []
    for member in source.getmembers():
        if not member.name.endswith(f"/{family}-aggregated.txt"):
            continue
        code = member.name.split("/")[-2].upper()
        if len(code) != 2 or not code.isascii() or not code.isalpha():
            raise ValueError(f"Invalid country code {code}")
        for line in source.extractfile(member).read().decode("utf-8").splitlines():
            line = line.strip()
            if line and not line.startswith("#"):
                network = ipaddress.ip_network(line)
                ranges.append((int(network.network_address), int(network.broadcast_address), code))
    ranges.sort()
    merged = []
    for start, end, code in ranges:
        if merged and start <= merged[-1][1]:
            raise ValueError(f"Overlapping {family} country allocations at {start:x}")
        if merged and start == merged[-1][1] + 1 and code == merged[-1][2]:
            merged[-1] = (merged[-1][0], end, code)
        else:
            merged.append((start, end, code))
    if not merged:
        raise ValueError("Empty country database")
    return merged


def boundaries(ranges):
    # Starts of runs partition the entire address space, including unknown
    # gaps. No end-address array or runtime decompression is necessary.
    changes = {0: ""}
    for start, end, code in ranges:
        changes[start] = code
        if end < 0xffffffff:
            changes[end + 1] = ""
    result = []
    for address, code in sorted(changes.items()):
        if not result or result[-1][1] != code:
            result.append((address, code))
    return result


def flags(source, codes):
    import cairosvg
    from PIL import Image

    # Neutral fallback: a light grey question mark on a dark grey field.
    question = (".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#..")
    unknown = Image.new("RGB", (16, 12), (70, 70, 70))
    for y, row in enumerate(question, 2):
        for x, pixel in enumerate(row, 5):
            if pixel == "#":
                unknown.putpixel((x, y), (225, 225, 225))
    images = [unknown.tobytes()]
    for code in codes[1:]:
        svg = read_member(source, f"/flags/4x3/{code.lower()}.svg")
        # Supersample fine flag details, then filter to the small HUD bitmap.
        png = cairosvg.svg2png(bytestring=svg, output_width=64, output_height=48)
        rgba = Image.open(io.BytesIO(png)).convert("RGBA")
        image = Image.new("RGBA", rgba.size, (70, 70, 70, 255))
        image.alpha_composite(rgba)
        images.append(image.convert("RGB").resize((16, 12), Image.Resampling.LANCZOS).tobytes())
    return images


def array(output, declaration, values, width):
    output.write(declaration + " = {\n")
    for i in range(0, len(values), width):
        output.write("    " + ",".join(values[i:i + width]) + ",\n")
    output.write("};\n\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-dir", type=Path, default=Path.home() / ".cache/netblood-country")
    args = parser.parse_args()
    args.cache_dir.mkdir(parents=True, exist_ok=True)
    with archive(args.cache_dir, "country.tar.gz", "ipverse/country-ip-blocks", COUNTRY_REV, COUNTRY_SHA) as country, \
            archive(args.cache_dir, "flags.tar.gz", "lipis/flag-icons", FLAGS_REV, FLAGS_SHA) as artwork:
        ranges = country_ranges(country)
        ranges6 = country_ranges(country, "ipv6")
        prefixes6 = []
        for start, end, code in ranges6:
            for prefix in ipaddress.summarize_address_range(ipaddress.IPv6Address(start), ipaddress.IPv6Address(end)):
                # RIR delegation entries cover at least /64. Keeping only the
                # high word is lossless for these prefixes; never silently
                # truncate a narrower allocation if upstream changes format.
                if not 1 <= prefix.prefixlen <= 64:
                    raise ValueError(f"IPv6 allocation {prefix} requires a wider lookup table")
                prefixes6.append((int(prefix.network_address) >> 64, prefix.prefixlen, code))
        codes = [""] + sorted({code for _, _, code in ranges + ranges6})
        if len(codes) > 256:
            raise ValueError("Country index no longer fits in a byte")
        indices = {code: i for i, code in enumerate(codes)}
        changes = boundaries(ranges)
        images = flags(artwork, codes)
        path = ROOT / "source/blood/src/netcountry-data.inc"
        with path.open("w", encoding="ascii", newline="\n") as output:
            output.write("// Generated by tools/update-netcountry.py; do not edit.\n")
            output.write("// IP country registrations: ipverse/country-ip-blocks (CC0 1.0).\n")
            output.write(f"// Source revision: {COUNTRY_REV}\n")
            output.write("// Flag artwork: flag-icons, Copyright (c) 2013 Panayiotis Lipiridis (MIT).\n")
            output.write(f"// Source revision: {FLAGS_REV}\n")
            output.write("// Full notices and provenance: source/blood/data/netcountry/.\n\n")
            array(output, "static const uint16_t kCountryCodes[]",
                  ["0"] + [f"0x{ord(c[0]):02x}{ord(c[1]):02x}" for c in codes[1:]], 12)
            array(output, "static const uint32_t kCountryStarts[]",
                  [f"0x{address:08x}" for address, _ in changes], 8)
            array(output, "static const uint8_t kCountryIndices[]",
                  [str(indices[code]) for _, code in changes], 24)
            array(output, "static const uint64_t kCountryIPv6Starts[]",
                  [f"0x{address:016x}ULL" for address, _, _ in prefixes6], 4)
            array(output, "static const uint8_t kCountryIPv6PrefixLengths[]",
                  [str(length) for _, length, _ in prefixes6], 24)
            array(output, "static const uint8_t kCountryIPv6Indices[]",
                  [str(indices[code]) for _, _, code in prefixes6], 24)
            output.write("static const uint8_t kCountryFlags[][16 * 12 * 3] = {\n")
            for code, pixels in zip(codes, images):
                output.write(f"    {{ // {code or 'Unknown'}\n")
                for i in range(0, len(pixels), 24):
                    output.write("        " + ",".join(str(v) for v in pixels[i:i + 24]) + ",\n")
                output.write("    },\n")
            output.write("};\n")
        notices = ROOT / "source/blood/data/netcountry"
        notices.mkdir(parents=True, exist_ok=True)
        (notices / "IPVERSE-CC0.txt").write_bytes(read_member(country, "/LICENSE"))
        (notices / "FLAG-ICONS-MIT.txt").write_bytes(read_member(artwork, "/LICENSE"))
        print(f"{len(codes)-1} countries, {len(changes)} IPv4 boundaries, {len(prefixes6)} IPv6 prefixes, {len(images)} flags")
        print(f"Compiled data: {len(changes)*5 + len(prefixes6)*10 + len(codes)*2 + len(images)*576:,} bytes")
        print(f"Generated {path} ({path.stat().st_size:,} bytes)")


if __name__ == "__main__":
    main()
