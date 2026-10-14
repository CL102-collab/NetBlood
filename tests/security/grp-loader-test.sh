#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/netblood-grp-loader-test.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
sanitize=
if [ "${SANITIZE:-0}" = 1 ]; then
    sanitize='-fsanitize=address,undefined -fno-sanitize=vptr -fno-sanitize-recover=all -fno-omit-frame-pointer'
fi
# ZIP decoding has pre-existing unaligned loads and signed hash shifts. Keep
# ASan and all other UBSan checks there; GRP/SSI mounting remains fully checked.
"${CXX:-c++}" -std=c++14 -O1 -g -Wno-attributes $sanitize \
    -fno-sanitize=alignment,shift-base \
    ${CPPFLAGS:-} ${CFLAGS:-} ${CXXFLAGS:-} -DNOASM \
    -DRENDERTYPESDL=1 -DSDL_TARGET=2 -funsigned-char -ffunction-sections -fdata-sections \
    $(pkg-config --cflags sdl2) \
    -I"$repo/source/build/include" -I"$repo/source/mact/include" \
    -I"$repo/source/audiolib/include" -I"$repo/source/glad/include" \
    -c "$repo/source/build/src/kplib.cpp" -o "$build_dir/kplib.o"
"${CXX:-c++}" -std=c++14 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-attributes $sanitize \
    ${CPPFLAGS:-} ${CFLAGS:-} ${CXXFLAGS:-} -DNOASM \
    -DRENDERTYPESDL=1 -DSDL_TARGET=2 -funsigned-char -ffunction-sections -fdata-sections \
    $(pkg-config --cflags sdl2) \
    -I"$repo/source/build/include" -I"$repo/source/mact/include" \
    -I"$repo/source/audiolib/include" -I"$repo/source/glad/include" \
    "$repo/tests/security/grp-loader.cpp" "$build_dir/kplib.o" \
    "$repo/source/build/src/compat.cpp" \
    "$repo/source/build/src/smmalloc.cpp" "$repo/source/build/src/smmalloc_tls.cpp" \
    "$repo/source/build/src/smmalloc_generic.cpp" \
    ${LDFLAGS:-} -Wl,--gc-sections -o "$build_dir/grp-loader-test"
python3 - "$build_dir/valid.zip" <<'PYZIP'
import sys
import zipfile
with zipfile.ZipFile(sys.argv[1], "w", compression=zipfile.ZIP_STORED) as archive:
    archive.writestr("ZIP.TEST", b"zip")
PYZIP
"$build_dir/grp-loader-test" "$build_dir"
