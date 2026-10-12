#!/bin/sh
# Production dedicated limit/exit handlers; no assets, sockets or display.
# LTO discards unrelated gameplay retained by the production player's sequence
# callback registration. Vptr checks on discarded LoadSave methods would retain
# unrelated RTTI; the tested limits and score-reset code use no virtual calls.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/netblood-server-settings-test.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
sanitize=
if [ "${SANITIZE:-0}" = 1 ]; then
    sanitize='-fsanitize=address,undefined -fno-sanitize=vptr -fno-omit-frame-pointer'
fi
"${CXX:-c++}" -std=c++14 -O1 -g -flto -Wall -Wextra -Wno-unused-parameter $sanitize \
    ${CPPFLAGS:-} ${CFLAGS:-} ${CXXFLAGS:-} -DNOASM -DNOONE_EXTENSIONS -DDEBUG_MAIN_ARRAYS \
    -DRENDERTYPESDL=1 -DSDL_TARGET=2 -funsigned-char -ffunction-sections -fdata-sections \
    $(pkg-config --cflags sdl2) \
    -I"$repo/source/blood/src" -I"$repo/source/build/include" \
    -I"$repo/source/mact/include" -I"$repo/source/audiolib/include" -I"$repo/source/glad/include" \
    "$repo/tests/network/server-settings.cpp" "$repo/source/blood/src/player.cpp" \
    ${LDFLAGS:-} -Wl,--gc-sections -o "$build_dir/server-settings-test"
"$build_dir/server-settings-test"
