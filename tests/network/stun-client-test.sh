#!/bin/sh
# Same-socket STUN client tests use local UDP mocks; never contact public STUN.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/netblood-stun-client-test.XXXXXX")
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
"${CXX:-c++}" -std=c++11 -O1 -g $sanitize ${CPPFLAGS:-} ${CXXFLAGS:-} \
    -I"$repo/source/build/include" -c "$build_dir/enet.cpp" -o "$build_dir/enet.o"
"${CXX:-c++}" -std=c++11 -O1 -g -Wall -Wextra -Werror $sanitize \
    ${CPPFLAGS:-} ${CXXFLAGS:-} -I"$repo/source/blood/src" -I"$repo/source/build/include" \
    "$repo/tests/network/stun-client-test.cpp" "$repo/source/blood/src/netstun_client.cpp" \
    "$build_dir/enet.o" ${LDFLAGS:-} -pthread -o "$build_dir/stun-client-test"
"$build_dir/stun-client-test"
