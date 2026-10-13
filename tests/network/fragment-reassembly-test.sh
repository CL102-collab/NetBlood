#!/bin/sh
# Exercise production ENet receive queues with deterministic packet reordering.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/netblood-fragment-test.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
sanitize=
if [ "${SANITIZE:-0}" = 1 ]; then
    sanitize='-fsanitize=address,undefined -fno-omit-frame-pointer'
fi
"${CXX:-c++}" -std=c++11 -O1 -g $sanitize ${CPPFLAGS:-} ${CXXFLAGS:-} \
    -I"$repo/source/build/include" "$repo/tests/network/fragment-reassembly.cpp" \
    ${LDFLAGS:-} -o "$build_dir/fragment-test"
"$build_dir/fragment-test"
