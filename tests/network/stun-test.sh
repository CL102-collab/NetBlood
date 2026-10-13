#!/bin/sh
# Standalone RFC 5389/8489 codec checks, no STUN service or game data needed.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/netblood-stun-test.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
sanitize=
if [ "${SANITIZE:-0}" = 1 ]; then
    sanitize='-fsanitize=address,undefined -fno-omit-frame-pointer'
fi
"${CXX:-c++}" -std=c++11 -O1 -g -Wall -Wextra -Werror $sanitize \
    ${CPPFLAGS:-} ${CXXFLAGS:-} -I"$repo/source/blood/src" \
    "$repo/tests/network/stun-test.cpp" ${LDFLAGS:-} -o "$build_dir/stun-test"
"$build_dir/stun-test"
