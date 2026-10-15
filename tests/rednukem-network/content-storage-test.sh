#!/bin/sh
# Exercise the real addon manifest codec and streamed filesystem cache.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/rednukem-content-test.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
sanitize=
if [ "${SANITIZE:-0}" = 1 ]; then
    sanitize='-fsanitize=address,undefined -fno-omit-frame-pointer'
fi
"${CXX:-c++}" -std=c++14 -O1 -g -Wall -Wextra -Werror $sanitize \
    ${CPPFLAGS:-} ${CFLAGS:-} ${CXXFLAGS:-} -I"$repo/source/rr/src" \
    "$repo/tests/rednukem-network/content-storage.cpp" "$repo/source/rr/src/rnet_addon.cpp" \
    ${LDFLAGS:-} -o "$build_dir/addon-test"
mkdir "$build_dir/run"
cd "$build_dir/run"
"$build_dir/addon-test" "$@"
