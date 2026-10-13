#!/bin/sh
# Standalone production protocol/snapshot checks, no game data or display needed.
# SANITIZE=1 enables address and undefined behavior sanitizers.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
"$repo/tests/network/compatibility-test.sh"
"$repo/tests/network/uri-test.sh"
"$repo/tests/network/chat-test.sh"
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/netblood-network-test.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
sanitize=
if [ "${SANITIZE:-0}" = 1 ]; then
    sanitize='-fsanitize=address,undefined -fno-omit-frame-pointer'
fi
"${CC:-cc}" -std=c99 -O1 -g $sanitize ${CPPFLAGS:-} ${CFLAGS:-} \
    -I"$repo/source/build/include" -c "$repo/source/build/src/lz4.c" -o "$build_dir/lz4.o"
"${CXX:-c++}" -std=c++11 -O1 -g -Wall -Wextra -Werror $sanitize \
    ${CPPFLAGS:-} ${CXXFLAGS:-} -I"$repo/source/blood/src" -I"$repo/source/build/include" \
    "$repo/tests/network/protocol.cpp" "$repo/source/blood/src/netsnapshot_codec.cpp" \
    "$build_dir/lz4.o" ${LDFLAGS:-} -o "$build_dir/protocol-test"
"$build_dir/protocol-test"
"${CXX:-c++}" -std=c++11 -O1 -g -Wall -Wextra -Werror $sanitize \
    ${CPPFLAGS:-} ${CXXFLAGS:-} -I"$repo/source/blood/src" \
    "$repo/tests/network/scoreboard.cpp" ${LDFLAGS:-} -o "$build_dir/scoreboard-test"
"$build_dir/scoreboard-test"
"${CXX:-c++}" -std=c++11 -O1 -g -Wall -Wextra -Werror $sanitize \
    ${CPPFLAGS:-} ${CXXFLAGS:-} -I"$repo/source/blood/src" \
    "$repo/tests/network/packet-demo.cpp" "$repo/source/blood/src/netdemo.cpp" \
    ${LDFLAGS:-} -o "$build_dir/packet-demo-test"
"$build_dir/packet-demo-test" "$build_dir"
"${CXX:-c++}" -std=c++11 -O1 -g -Wall -Wextra -Werror $sanitize \
    ${CPPFLAGS:-} ${CXXFLAGS:-} -DNETLAG_NO_DRAW -I"$repo/source/blood/src" \
    "$repo/tests/network/lagometer.cpp" "$repo/source/blood/src/netlag.cpp" \
    ${LDFLAGS:-} -o "$build_dir/lagometer-test"
"$build_dir/lagometer-test"
cat > "$build_dir/enet.cpp" <<'EOF'
#define UNREFERENCED_PARAMETER(value) ((void)(value))
#define ENET_IMPLEMENTATION
#include "enet.h"
EOF
"${CXX:-c++}" -std=c++11 -O1 -g $sanitize ${CPPFLAGS:-} ${CXXFLAGS:-} \
    -I"$repo/source/build/include" -c "$build_dir/enet.cpp" -o "$build_dir/enet.o"
"${CXX:-c++}" -std=c++11 -O1 -g -Wall -Wextra -Werror $sanitize \
    ${CPPFLAGS:-} ${CXXFLAGS:-} -I"$repo/source/build/include" \
    "$repo/tests/network/transport.cpp" "$build_dir/enet.o" \
    ${LDFLAGS:-} -pthread -o "$build_dir/transport-test"
"$build_dir/transport-test"
