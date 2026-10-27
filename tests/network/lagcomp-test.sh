#!/bin/sh
# Production rewind and Build hitscan against a synthetic map; no game assets.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/netblood-lagcomp-test.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
sanitize=
if [ "${SANITIZE:-0}" = 1 ]; then
    sanitize='-fsanitize=address,undefined -fno-omit-frame-pointer'
fi
compile()
{
    lag_source=$1
    lag_object=$2
    shift 2
    "${CXX:-c++}" -std=c++14 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-attributes \
        -Wno-unused-variable -Wno-unused-but-set-variable -Wno-char-subscripts $sanitize \
        ${CPPFLAGS:-} ${CFLAGS:-} ${CXXFLAGS:-} -DNOASM -DNOONE_EXTENSIONS -DDEBUG_MAIN_ARRAYS \
        -DRENDERTYPESDL=1 -DSDL_TARGET=2 -funsigned-char -ffunction-sections -fdata-sections \
        $(pkg-config --cflags sdl2) \
        -I"$repo/source/blood/src" -I"$repo/source/build/include" \
        -I"$repo/source/mact/include" -I"$repo/source/audiolib/include" -I"$repo/source/glad/include" \
        "$@" -c "$lag_source" -o "$lag_object"
}
compile "$repo/tests/network/lagcomp.cpp" "$build_dir/test.o"
compile "$repo/source/blood/src/netlagcomp.cpp" "$build_dir/lagcomp.o"
compile "$repo/source/build/src/clip.cpp" "$build_dir/clip.o"
# Existing Build rintersect shifts negative fixed-point values. Keep this
# legacy operation outside shift-base instrumentation; new rewind code still
# receives every sanitizer, and engine shift counts and other UB remain checked.
compile "$repo/source/build/src/engine.cpp" "$build_dir/engine.o" -fno-sanitize=shift-base
compile "$repo/source/build/src/pragmas.cpp" "$build_dir/pragmas.o"
"${CXX:-c++}" $sanitize ${LDFLAGS:-} "$build_dir/test.o" "$build_dir/lagcomp.o" \
    "$build_dir/clip.o" "$build_dir/engine.o" "$build_dir/pragmas.o" \
    -Wl,--gc-sections -o "$build_dir/lagcomp-test"
"$build_dir/lagcomp-test"
# The production OpenGL/Polymer sprite fields carry write-tracking hooks.
# Reuse the identical plain sprite storage ABI in the collision primitives,
# but compile both the fixture and rewind module with those hooks enabled.
compile "$repo/tests/network/lagcomp.cpp" "$build_dir/test-tracked.o" -DUSE_OPENGL
compile "$repo/source/blood/src/netlagcomp.cpp" "$build_dir/lagcomp-tracked.o" -DUSE_OPENGL
"${CXX:-c++}" $sanitize ${LDFLAGS:-} "$build_dir/test-tracked.o" "$build_dir/lagcomp-tracked.o" \
    "$build_dir/clip.o" "$build_dir/engine.o" "$build_dir/pragmas.o" \
    -Wl,--gc-sections -o "$build_dir/lagcomp-tracked-test"
"$build_dir/lagcomp-tracked-test"
