#!/bin/sh
# Exercise the production resource implementation with an isolated VFS.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/netblood-resource-test.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
sanitize=
if [ "${SANITIZE:-0}" = 1 ]; then
    sanitize='-fsanitize=address,undefined -fno-sanitize=vptr -fno-sanitize-recover=all -fno-omit-frame-pointer'
fi
"${CXX:-c++}" -std=c++14 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-attributes $sanitize \
    ${CPPFLAGS:-} ${CFLAGS:-} ${CXXFLAGS:-} -DNOASM -DNOONE_EXTENSIONS -DDEBUG_MAIN_ARRAYS \
    -DRENDERTYPESDL=1 -DSDL_TARGET=2 -funsigned-char -ffunction-sections -fdata-sections \
    $(pkg-config --cflags sdl2) \
    -I"$repo/source/blood/src" -I"$repo/source/build/include" \
    -I"$repo/source/mact/include" -I"$repo/source/audiolib/include" -I"$repo/source/glad/include" \
    "$repo/tests/security/resource-test.cpp" "${RESOURCE_SOURCE:-$repo/source/blood/src/resource.cpp}" \
    "$repo/source/build/src/compat.cpp" "$repo/source/build/src/crc32.cpp" \
    ${LDFLAGS:-} -Wl,--gc-sections -o "$build_dir/resource-test"
"$build_dir/resource-test"
