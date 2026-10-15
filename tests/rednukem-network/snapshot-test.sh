#!/bin/sh
# Production snapshot parser with no engine/assets; section GC removes unused
# integration code. SANITIZE=1 exercises malformed inputs under ASan and UBSan.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/rednukem-snapshot.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
sanitize=
if [ "${SANITIZE:-0}" = 1 ]; then
    sanitize='-fsanitize=address,undefined -fno-omit-frame-pointer'
fi
"${CXX:-c++}" -std=c++14 -O1 -g -Wall -Wextra -Wno-unused-parameter $sanitize \
    ${CPPFLAGS:-} ${CFLAGS:-} ${CXXFLAGS:-} -DNOASM -DDEBUG_MAIN_ARRAYS \
    -DRENDERTYPESDL=1 -DSDL_TARGET=2 -funsigned-char -ffunction-sections -fdata-sections \
    $(pkg-config --cflags sdl2) \
    -I"$repo/source/rr/src" -I"$repo/source/build/include" \
    -I"$repo/source/mact/include" -I"$repo/source/audiolib/include" -I"$repo/source/glad/include" \
    "$repo/tests/rednukem-network/snapshot.cpp" ${LDFLAGS:-} -Wl,--gc-sections -o "$build_dir/snapshot-test"
"$build_dir/snapshot-test"
