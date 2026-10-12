# Theora cinematic decoder

Portable decoder-only files from Xiph.Org **libtheora 1.2.0**, tag `v1.2.0`,
commit `8e4808736e9c181b971306cc3f05df9e61354004`:
https://github.com/xiph/theora/tree/v1.2.0

The upstream source and headers are unmodified. The BSD license is in `COPYING`.
Only the modern decoding API is built; encoding, legacy API wrappers and
platform-specific assembly are omitted. No external Theora package is required.

`include/ogg/ogg.h` is a NetBlood adapter to the libogg/Vorbis declarations in
`source/audiolib/src/minivorbis.h`. Their existing implementation in
`source/audiolib/src/vorbis.cpp` supplies the Ogg symbols, so libogg is not duplicated.
