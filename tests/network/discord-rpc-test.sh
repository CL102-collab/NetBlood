#!/bin/sh
# Exercise the production Discord RPC client against a private local IPC server.
# No Discord account, registered application, game data, or network is needed.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/netblood-discord-test.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
sanitize=
if [ "${SANITIZE:-0}" = 1 ]; then
    sanitize='-fsanitize=address,undefined -fno-omit-frame-pointer'
fi
cat > "$build_dir/sjson.cpp" <<'EOF'
#include <cstdint>
#include <cstdlib>
#define UNREFERENCED_PARAMETER(value) ((void)(value))
#define ABORT_IF_F(condition, ...) do { if (condition) std::abort(); } while (0)
#define SJSON_IMPLEMENT
#include "sjson.h"
EOF
"${CXX:-c++}" -std=c++11 -O1 -g $sanitize ${CPPFLAGS:-} ${CXXFLAGS:-} \
    -I"$repo/source/build/include" -c "$build_dir/sjson.cpp" -o "$build_dir/sjson.o"
"${CXX:-c++}" -std=c++11 -O1 -g -Wall -Wextra -Werror $sanitize \
    ${CPPFLAGS:-} ${CXXFLAGS:-} -I"$repo/source/blood/src" -I"$repo/source/build/include" \
    "$repo/tests/network/discord-rpc.cpp" "$repo/source/blood/src/discord_rpc.cpp" \
    "$build_dir/sjson.o" ${LDFLAGS:-} -pthread -o "$build_dir/discord-rpc-test"
"$build_dir/discord-rpc-test"
