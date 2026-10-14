#!/bin/sh
# Test world shadow geometry without game assets, a window, or a Vulkan device.
# Requires C++14 and SDL2 development headers (included by the engine headers).
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/netblood-shadow-mesh.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

pkg-config --exists sdl2
"${CXX:-c++}" -std=c++14 -O2 -funsigned-char -DNDEBUG -DNOASM \
    -DRENDERTYPESDL=1 -DSDL_TARGET=2 -DUSE_OPENGL \
    -Wall -Wextra -Werror -Wno-char-subscripts -Wno-unused-function \
    -I"$repo/source/build/include" -I"$repo/source/glad/include" \
    ${CPPFLAGS:-} ${CFLAGS:-} ${CXXFLAGS:-} $(pkg-config --cflags sdl2) \
    "$repo/tests/realtime-lighting/shadow-mesh.cpp" \
    "$repo/source/build/src/realtime_lighting_mesh.cpp" \
    ${LDFLAGS:-} -o "$build_dir/shadow-mesh-test"
"$build_dir/shadow-mesh-test"
