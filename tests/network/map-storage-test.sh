#!/bin/sh
# Production BLM map validator and cache, without engine resource globals.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/netblood-map-storage-test.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
sanitize=
if [ "${SANITIZE:-0}" = 1 ]; then
    sanitize='-fsanitize=address,undefined -fno-omit-frame-pointer'
fi
"${CXX:-c++}" -std=c++14 -O1 -g -Wall -Wextra $sanitize \
    ${CPPFLAGS:-} ${CFLAGS:-} ${CXXFLAGS:-} -DNETMAP_STANDALONE \
    -I"$repo/source/blood/src" \
    "$repo/tests/network/map-storage.cpp" "$repo/source/blood/src/netmap.cpp" \
    ${LDFLAGS:-} -o "$build_dir/map-storage-test"
mkdir "$build_dir/run"
cd "$build_dir/run"
"$build_dir/map-storage-test" "$@"
