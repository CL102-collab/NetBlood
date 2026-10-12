#!/bin/sh
# Generate redistributable synthetic media; no game assets or codec packages
# are needed to build the decoder. ffmpeg is needed only to create fixtures.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/netblood-cinematic-ogv.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
sanitize=
if [ "${SANITIZE:-0}" = 1 ]; then
    sanitize='-fsanitize=address,undefined -fno-omit-frame-pointer'
fi
for chroma in 420 422 444; do
    audio=
    if [ "$chroma" = 420 ]; then
        audio='-f lavfi -i sine=frequency=440:sample_rate=44100:duration=2'
    fi
    ffmpeg -nostdin -loglevel error -threads 1 -f lavfi -i testsrc=size=62x46:rate=10:duration=2 \
        $audio -filter_threads 1 -c:v libtheora -q:v 10 -pix_fmt "yuv${chroma}p" \
        -c:a libvorbis -ac 2 -shortest "$build_dir/$chroma.ogv"
    ffmpeg -nostdin -loglevel error -threads 1 -i "$build_dir/$chroma.ogv" -frames:v 1 \
        -filter_threads 1 -f rawvideo -pix_fmt rgba "$build_dir/$chroma.ogv.rgba"
done
ffmpeg -nostdin -loglevel error -threads 1 -f lavfi -i testsrc=size=320x240:rate=10:duration=30 \
    -f lavfi -i sine=frequency=440:sample_rate=44100:duration=30 \
    -filter_threads 1 -c:v libtheora -pix_fmt yuv420p -c:a libvorbis -ac 2 -shortest "$build_dir/long.ogv"
printf 'not an Ogg video' > "$build_dir/invalid.ogv"
dd if="$build_dir/420.ogv" of="$build_dir/truncated-header.ogv" bs=1 count=90 2>/dev/null
dd if="$build_dir/420.ogv" of="$build_dir/truncated-data.ogv" bs=1 count=12000 2>/dev/null
includes="-I$repo/source/build/include -I$repo/source/libtheora/include -I$repo/source/audiolib/include -I$repo/source/blood/src"
objects=
for source in "$repo"/source/libtheora/src/*.c; do
    object="$build_dir/$(basename "$source" .c).o"
    "${CC:-cc}" -std=c99 -O1 -g -funsigned-char $sanitize $includes \
        -include "$repo/source/libtheora/include/ogg/ogg.h" -c "$source" -o "$object"
    objects="$objects $object"
done
printf '#include <alloca.h>\n#define OGG_IMPL\n#define VORBIS_IMPL\n#define OV_EXCLUDE_STATIC_CALLBACKS\n#include "%s/source/audiolib/src/minivorbis.h"\n' "$repo" > "$build_dir/vorbis.c"
# The existing minivorbis decoder uses intentional signed bit shifts; exclude
# only that sanitizer for its object. New decoder/Theora keep full coverage.
"${CC:-cc}" -std=c99 -O1 -g -funsigned-char $sanitize -fno-sanitize=shift $includes -c "$build_dir/vorbis.c" -o "$build_dir/vorbis.o"
"${CXX:-c++}" -std=c++14 -O1 -g -funsigned-char $sanitize $includes \
    "$repo/tests/network/cinematic-ogv.cpp" "$repo/source/blood/src/cinematic_ogv.cpp" \
    $objects "$build_dir/vorbis.o" -lm -o "$build_dir/cinematic-ogv-test"
UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1}" "$build_dir/cinematic-ogv-test" "$build_dir"
