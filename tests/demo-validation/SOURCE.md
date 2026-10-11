# Validation fixture provenance

`validation.pk3` contains only the `validatedemos/` subtree of
[NotBlood's `notblood.pk3`](https://github.com/clipmove/NotBlood/blob/4aa93cb12194a8864ff467a139cc2ca38595a1ff/notblood.pk3),
at commit `4aa93cb12194a8864ff467a139cc2ca38595a1ff`.

The upstream archive's SHA-256 is
`221f582492f46047a06445d430ef43059f39533d40048add0de6d63411c23d1d`.

The fixture archive has 168 ZIP entries: the `validatedemos/` directory,
`TEST.INI`, 144 demos (`TEST000.DEM` through `TEST143.DEM`), and 22 maps.
It is 4,390,863 bytes compressed and contains 39,303,254 bytes of file data.
Every file's original bytes, path, and filename case are preserved, including
`TESTE2M22.map`. No other NotBlood resources or files from a local Blood
installation are included. The original Blood 1.21 game data is still required
separately to run the validation suite.

`SHA256SUMS` records the repackaged archive checksum. Verify it from this
directory with `sha256sum -c SHA256SUMS`.

## Packaging

The fixture was repackaged with Python's `zipfile` module: entries sorted by
path, DEFLATE compression level 9, timestamps fixed at `1980-01-01 00:00:00`,
Unix ZIP attributes `040755` for the directory and `0100644` for files, and no
ZIP comments or extra fields. All 167 file payloads were compared byte for
byte with the pinned upstream archive, and the ZIP integrity check passed.
The archive can be regenerated from that source with the same ZIP/zlib tools;
different compression-library versions may produce different compressed bytes
while preserving identical file contents.

## Source notices and attribution

The upstream `validatedemos/` subtree does not include an asset-specific
license or README. `TEST.INI` retains this notice:

> Copyright (c)1997 Monolith Productions Inc.
> All Rights Reserved Worldwide

Its map sections retain credits including Dustin Twilley, Joel Blackwell,
Kurt Blackwell, Skobbejak Games, KNUKE-DMNUKEM, Martin Brentnall, Sunstorm
Interactive, and Seizhak, alongside the original Blood level credits.

The upstream validation implementation in
[`source/blood/src/demo.cpp`](https://github.com/clipmove/NotBlood/blob/4aa93cb12194a8864ff467a139cc2ca38595a1ff/source/blood/src/demo.cpp)
has a GPL version 2 source-code notice crediting the EDuke32 developers and
contributors and Nuke.YKT. That source-code notice does not establish a
license for these demo, map, and INI data files. This provenance record does
not grant additional rights or relicense the fixtures; the original notices
and attribution remain intact inside the archive.
