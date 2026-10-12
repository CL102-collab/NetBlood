#!/bin/sh
# Pure production manifest parser and MD5; no game data, display or dependencies.
# SANITIZE=1 enables address/undefined sanitizers. --large streams over 4 GiB.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/netblood-assets-test.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
sanitize=
if [ "${SANITIZE:-0}" = 1 ]; then
    sanitize='-fsanitize=address,undefined -fno-omit-frame-pointer'
fi
"${CXX:-c++}" -std=c++11 -O2 -g -Wall -Wextra -Werror $sanitize \
    ${CPPFLAGS:-} ${CXXFLAGS:-} -I"$repo/source/blood/src" \
    "$repo/tests/network/assets.cpp" ${LDFLAGS:-} -o "$build_dir/assets-test"
"$build_dir/assets-test" "$@"
