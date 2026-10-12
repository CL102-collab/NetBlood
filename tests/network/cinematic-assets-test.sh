#!/bin/sh
# Production optional movie inventory; fake resource/VFS I/O, no assets needed.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/netblood-cinematic-assets-test.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
sanitize=
if [ "${SANITIZE:-0}" = 1 ]; then
    sanitize='-fsanitize=address,undefined -fno-sanitize=vptr -fno-omit-frame-pointer'
fi
"${CXX:-c++}" -std=c++14 -O1 -g -flto -Wall -Wextra -Wno-unused-parameter -Wno-attributes $sanitize \
    ${CPPFLAGS:-} ${CFLAGS:-} ${CXXFLAGS:-} -DNOASM -DNOONE_EXTENSIONS -DDEBUG_MAIN_ARRAYS \
    -DRENDERTYPESDL=1 -DSDL_TARGET=2 -funsigned-char -ffunction-sections -fdata-sections \
    $(pkg-config --cflags sdl2) \
    -I"$repo/source/blood/src" -I"$repo/source/build/include" \
    -I"$repo/source/mact/include" -I"$repo/source/audiolib/include" -I"$repo/source/glad/include" \
    "$repo/tests/network/cinematic-assets.cpp" "$repo/source/blood/src/levels.cpp" \
    "$repo/source/build/src/crc32.cpp" \
    ${LDFLAGS:-} -Wl,--gc-sections -o "$build_dir/cinematic-assets-test"
"$build_dir/cinematic-assets-test"
