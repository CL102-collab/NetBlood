#!/bin/sh
# Compile the real OpenGL lighting backend and synthetic scene/state checks.
# Requires a compatibility OpenGL driver, SDL2 headers, Python 3 and DISPLAY.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/netblood-opengl-lighting.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
pkg-config --exists sdl2
python3 - "$repo" "$build_dir/shaders.cpp" <<'PY'
from pathlib import Path
import sys
root, destination = map(Path, sys.argv[1:])
source = ''
for name in ('ambient_occlusion', 'realtime_lighting'):
    shader = (root / 'source/build/src' / (name + '.glsl')).read_text()
    source += 'char const *' + name + ' = R"NBLOOD_GLSL(' + shader + ')NBLOOD_GLSL";\n'
destination.write_text(source)
PY
"${CC:-cc}" -I"$repo/source/glad/include" ${CPPFLAGS:-} ${CFLAGS:-} \
    -c "$repo/source/glad/src/glad.c" -o "$build_dir/glad.o"
"${CXX:-c++}" -std=c++14 -O2 -funsigned-char -DNDEBUG -DNOASM \
    -DRENDERTYPESDL=1 -DSDL_TARGET=2 -DUSE_OPENGL \
    -Wall -Wextra -Werror -Wno-char-subscripts -Wno-unused-function \
    -I"$repo/source/build/include" -I"$repo/source/glad/include" \
    ${CPPFLAGS:-} ${CFLAGS:-} ${CXXFLAGS:-} $(pkg-config --cflags sdl2) \
    "$repo/tests/realtime-lighting/opengl-backend.cpp" \
    "$repo/source/build/src/polymost_lighting.cpp" "$build_dir/shaders.cpp" "$build_dir/glad.o" \
    ${LDFLAGS:-} $(pkg-config --libs sdl2) -ldl -o "$build_dir/opengl-lighting-test"
MESA_DEBUG=context "$build_dir/opengl-lighting-test"
