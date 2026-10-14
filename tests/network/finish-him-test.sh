#!/bin/sh
# Real callback and presentation event codec; no game assets or audio device.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/netblood-finish-him-test.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
sanitize=
callback_sanitize=
if [ "${SANITIZE:-0}" = 1 ]; then
    sanitize='-fsanitize=address,undefined -fno-omit-frame-pointer'
    # ASan's global registry would retain the unused gCallback table and every
    # unrelated callback. Disable only that registration in callback.cpp;
    # function instrumentation and the test world's global checks stay active.
    case "$("${CXX:-c++}" --version)" in
        *clang*) callback_sanitize='-mllvm -asan-globals=0' ;;
        *) callback_sanitize='--param=asan-globals=0' ;;
    esac
fi
compile()
{
    "${CXX:-c++}" -std=c++14 -O1 -g -Wall -Wextra -Wno-unused-parameter $sanitize \
        ${CPPFLAGS:-} ${CFLAGS:-} ${CXXFLAGS:-} ${3:-} -DNOASM -DNOONE_EXTENSIONS -DDEBUG_MAIN_ARRAYS \
        -DRENDERTYPESDL=1 -DSDL_TARGET=2 -funsigned-char -ffunction-sections -fdata-sections \
        $(pkg-config --cflags sdl2) \
        -I"$repo/source/blood/src" -I"$repo/source/build/include" \
        -I"$repo/source/mact/include" -I"$repo/source/audiolib/include" -I"$repo/source/glad/include" \
        -c "$1" -o "$2"
}
compile "$repo/tests/network/finish-him.cpp" "$build_dir/finish-him.o"
compile "$repo/source/blood/src/callback.cpp" "$build_dir/callback.o" "$callback_sanitize"
compile "$repo/source/blood/src/netevents.cpp" "$build_dir/netevents.o"
"${CXX:-c++}" $sanitize ${CPPFLAGS:-} ${CFLAGS:-} ${CXXFLAGS:-} \
    "$build_dir/finish-him.o" "$build_dir/callback.o" "$build_dir/netevents.o" \
    ${LDFLAGS:-} -Wl,--gc-sections -o "$build_dir/finish-him-test"
"$build_dir/finish-him-test"
