# Scoreboard country data

The server looks up the connecting peer's IPv4 or IPv6 address using built-in tables.
Only the two-letter country code is sent to clients. There are no per-player
HTTP requests, API keys, runtime downloads, or external database dependencies.
Private, shared, loopback, link-local, documentation, benchmarking, multicast,
and reserved addresses show a neutral question-mark flag, as do unknown ranges.

These flags are **network registration-country estimates**, not reliable
physical locations or nationalities. ISPs may register ranges in another
country; a VPN or proxy can show its network's country. The dual-stack ENet
transport supplies IPv4 peers as IPv4-mapped IPv6 addresses; these use the IPv4
table. Native IPv6 uses the IPv6 delegation table. Local, documentation, and
special-purpose IPv6 ranges show the neutral flag too.

## Sources and licensing

* IP prefixes: [ipverse/country-ip-blocks](https://github.com/ipverse/country-ip-blocks),
  commit `a090ea1c7275cfa675000f53b71a630b466d7636`, retrieved 2026-10-12.
  The dataset aggregates delegation information from the five regional Internet
  registries. It is published under **CC0 1.0 Universal**; the unmodified notice
  is [IPVERSE-CC0.txt](IPVERSE-CC0.txt).
* Flag artwork: [lipis/flag-icons](https://github.com/lipis/flag-icons), commit
  `086f7e97d657358203916dbe84f61c2bccaa81eb`, retrieved 2026-10-12.
  Copyright (c) 2013 Panayiotis Lipiridis, **MIT license**; the unmodified notice
  is [FLAG-ICONS-MIT.txt](FLAG-ICONS-MIT.txt). The 4:3 SVGs are rasterized into
  16x12 RGB icons. The neutral question-mark icon is generated locally.

Both data licenses permit inclusion in GPLv2 software. Keep these notices when
redistributing the generated table or flag artwork. The generated file embeds
the source revisions and attribution as well.

The snapshot contains 178,857 IPv4 prefixes and 69,593 IPv6 prefixes in 238
countries. Adjacent IPv4 ranges with the same country are merged. The IPv4
table stores 150,922 boundaries (including unknown gaps) as separate 32-bit
starts and 8-bit country indices. IPv6 stores 69,593 prefix starts as 64-bit
words, with 8-bit prefix lengths and country indices. Every upstream IPv6
allocation is at least a /64, so keeping the high 64 bits loses no information;
the generator rejects narrower future prefixes instead of truncating them.
Country codes and 239 flag bitmaps bring the compiled data to 1,588,682 bytes.
Lookup performs a binary search and needs no allocation or decompression.

## Updating and reproducing

`tools/update-netcountry.py` pins both upstream revisions and their archive
SHA-256 checksums. It generates `source/blood/src/netcountry-data.inc` and copies
the licenses above; normal builds use the checked-in generated file.

```sh
python3 -m venv /tmp/netblood-country-tools
/tmp/netblood-country-tools/bin/pip install CairoSVG==2.8.2 Pillow==12.3.0
/tmp/netblood-country-tools/bin/python tools/update-netcountry.py
tests/network/country-test.sh
```

The generator's optional `--cache-dir` accepts cached `country.tar.gz` and
`flags.tar.gz` archives; their hashes are verified before use. To update data,
change the revision and checksum constants together, regenerate, update the
snapshot counts in this document, and review/run the country tests. CairoSVG
needs the system Cairo library only on the machine regenerating the artwork.
