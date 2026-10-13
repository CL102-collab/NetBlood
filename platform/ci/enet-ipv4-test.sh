#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/netblood-ipv4-test.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
sanitize=
if [ "${SANITIZE:-0}" = 1 ]; then
    sanitize='-fsanitize=address,undefined -fno-omit-frame-pointer'
fi
cat > "$build_dir/enet.cpp" <<'EOF'
#define UNREFERENCED_PARAMETER(value) ((void)(value))
#define ENET_IMPLEMENTATION
#include "enet.h"
EOF
"${CXX:-c++}" -std=c++11 -O1 -g $sanitize -DENET_IPV4_ONLY=1 ${CPPFLAGS:-} ${CXXFLAGS:-} \
    -I"$repo/source/build/include" -c "$build_dir/enet.cpp" -o "$build_dir/enet.o"
"${CXX:-c++}" -std=c++11 -O1 -g $sanitize -Wall -Wextra -Werror -DENET_IPV4_ONLY=1 \
    ${CPPFLAGS:-} ${CXXFLAGS:-} -I"$repo/source/build/include" \
    "$repo/platform/ci/enet-ipv4-test.cpp" "$build_dir/enet.o" \
    ${LDFLAGS:-} -o "$build_dir/ipv4-test"
"$build_dir/ipv4-test"
# Reuse the full reliable/fragmented UDP, packet triplication and deduplication
# integration test against the alternate socket implementation too.
"${CXX:-c++}" -std=c++11 -O1 -g $sanitize -Wall -Wextra -Werror -DENET_IPV4_ONLY=1 \
    ${CPPFLAGS:-} ${CXXFLAGS:-} -I"$repo/source/build/include" \
    "$repo/tests/network/transport.cpp" "$build_dir/enet.o" \
    ${LDFLAGS:-} -pthread -o "$build_dir/transport-test"
"$build_dir/transport-test"
"${CXX:-c++}" -std=c++11 -O1 -g $sanitize ${CPPFLAGS:-} ${CXXFLAGS:-} \
    -I"$repo/source/build/include" -c "$build_dir/enet.cpp" -o "$build_dir/enet-default.o"
"${CXX:-c++}" -std=c++11 -O1 -g $sanitize -Wall -Wextra -Werror \
    ${CPPFLAGS:-} ${CXXFLAGS:-} -I"$repo/source/build/include" \
    "$repo/platform/ci/enet-ipv6-test.cpp" "$build_dir/enet-default.o" \
    ${LDFLAGS:-} -o "$build_dir/ipv6-test"
"$build_dir/ipv6-test"
