#!/bin/sh
# ENet custom-backend conformance; no ZeroTier SDK, controller, or game data.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/netblood-zerotier-enet.XXXXXX")
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
for family in dual-stack ipv4-only; do
    family_flags=
    if [ "$family" = ipv4-only ]; then family_flags=-DENET_IPV4_ONLY; fi
    "${CXX:-c++}" -std=c++11 -O1 -g $sanitize $family_flags ${CPPFLAGS:-} ${CXXFLAGS:-} \
        -I"$repo/source/build/include" -c "$build_dir/enet.cpp" -o "$build_dir/enet.o"
    "${CXX:-c++}" -std=c++11 -O1 -g -Wall -Wextra -Werror $sanitize $family_flags \
        ${CPPFLAGS:-} ${CXXFLAGS:-} -I"$repo/source/build/include" \
        "$repo/tests/network/zerotier-enet.cpp" "$build_dir/enet.o" \
        ${LDFLAGS:-} -pthread -o "$build_dir/test"
    "$build_dir/test"
done
