#!/bin/sh
# Exercise the real addon manifest codec and streamed filesystem cache.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/netblood-addon-test.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
sanitize=
if [ "${SANITIZE:-0}" = 1 ]; then
    sanitize='-fsanitize=address,undefined -fno-omit-frame-pointer'
fi
"${CXX:-c++}" -std=c++14 -O1 -g -Wall -Wextra -Werror $sanitize \
    ${CPPFLAGS:-} ${CFLAGS:-} ${CXXFLAGS:-} -I"$repo/source/blood/src" \
    "$repo/tests/network/addon.cpp" "$repo/source/blood/src/netaddon.cpp" \
    ${LDFLAGS:-} -o "$build_dir/addon-test"
mkdir "$build_dir/run"
cd "$build_dir/run"
"$build_dir/addon-test" "$@"
