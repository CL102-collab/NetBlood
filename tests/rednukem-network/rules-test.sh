#!/usr/bin/env sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM
sanitize=
if [ "${SANITIZE:-0}" = 1 ]; then
    sanitize='-fsanitize=address,undefined -fno-omit-frame-pointer'
fi
"${CXX:-c++}" -std=c++14 -Wall -Wextra -Werror $sanitize ${CPPFLAGS:-} ${CXXFLAGS:-} -I"$root/source/rr/src" \
    "$root/tests/rednukem-network/rules-test.cpp" ${LDFLAGS:-} -o "$test_dir/rules-test"
"$test_dir/rules-test"
