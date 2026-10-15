#!/bin/sh
# Exercise Rednukem's production packet-demo container with the shared corpus.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/rednukem-packet-demo.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
sanitize=
if [ "${SANITIZE:-0}" = 1 ]; then
    sanitize='-fsanitize=address,undefined -fno-omit-frame-pointer'
fi
# The container treats packet payloads as opaque bytes. Preserve the exhaustive
# corruption/timeline corpus, adapting only Rednukem's header and player bound.
python3 - "$repo" "$build_dir" <<'PY'
from pathlib import Path
import sys
repo, output = map(Path, sys.argv[1:])
source = (repo / 'tests/network/packet-demo.cpp').read_text()
for old, new in (
    ('#include "netdemo.h"', '#include "rnet_demo.h"'),
    ('source.localSlot = 7;', 'source.localSlot = 15;'),
    ("original[0] == 'N'", "original[0] == 'R' && original[1] == 'N'"),
    ('for (int slot : {-2, 8})', 'for (int slot : {-2, 16})'),
):
    assert source.count(old) == 1, f'Shared packet-demo corpus changed: {old}'
    source = source.replace(old, new)
(output / 'packet-demo.cpp').write_text(source)
PY
"${CXX:-c++}" -std=c++11 -O1 -g -Wall -Wextra -Werror $sanitize \
    ${CPPFLAGS:-} ${CXXFLAGS:-} -I"$repo/source/rr/src" -I"$repo/source/blood/src" \
    "$build_dir/packet-demo.cpp" "$repo/source/rr/src/rnet_demo.cpp" \
    ${LDFLAGS:-} -o "$build_dir/packet-demo-test"
"$build_dir/packet-demo-test" "$build_dir"
