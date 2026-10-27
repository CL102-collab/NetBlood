#!/bin/sh
# Production service + discovery + fragmented ENet across separate processes;
# only the companion's external ZeroTier network is replaced by a lossy UDP bus.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/netblood-zerotier-service.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
sanitize=
if [ "${SANITIZE:-0}" = 1 ]; then sanitize='-fsanitize=address,undefined -fno-omit-frame-pointer'; fi
cat > "$build_dir/enet.cpp" <<'CPP'
#define UNREFERENCED_PARAMETER(value) ((void)(value))
#define ENET_IMPLEMENTATION
#include "enet.h"
CPP
"${CXX:-c++}" -std=c++11 -O1 -g $sanitize ${CPPFLAGS:-} ${CXXFLAGS:-} \
    -I"$repo/source/build/include" -c "$build_dir/enet.cpp" -o "$build_dir/enet.o"
"${CXX:-c++}" -std=c++11 -O1 -g -Wall -Wextra -Werror $sanitize ${CPPFLAGS:-} ${CXXFLAGS:-} \
    -I"$repo/source/build/include" "$repo/tests/network/zerotier-service.cpp" \
    "$repo/source/build/src/zerotier.cpp" "$build_dir/enet.o" ${LDFLAGS:-} -pthread -o "$build_dir/test"
python3 - "$build_dir" "$repo" <<'PY'
from pathlib import Path
import subprocess
import sys
work, repo = map(Path, sys.argv[1:])
helper = repo / "tests/network/zerotier-service-helper.py"
with (work / "host.log").open("w+") as log:
    host = subprocess.Popen([str(work / "test"), "host", str(helper), str(work / "host")], stdout=log, stderr=subprocess.STDOUT)
    try:
        client = subprocess.run([str(work / "test"), "client", str(helper), str(work / "client")], timeout=25)
        if client.returncode:
            raise RuntimeError(f"client exit {client.returncode}")
        if host.wait(timeout=25):
            raise RuntimeError(f"host exit {host.returncode}")
    finally:
        if host.poll() is None:
            host.terminate()
            host.wait(timeout=5)
        log.seek(0)
        print(log.read(), end="")
print("Production ZeroTier service/IPC/browser: full protected host, live updates, 256 KiB reliable transfer through loss, socket reuse passed")
PY
# Ensure network-disabled builds don't acquire any ENet link dependency.
cat > "$build_dir/disabled.cpp" <<'CPP'
#include "zerotier.h"
int main(){std::string error;zerotier::Config config;return zerotier::start(config,error)?1:0;}
CPP
"${CXX:-c++}" -std=c++11 -O1 -DNETCODE_DISABLE -I"$repo/source/build/include" \
    "$build_dir/disabled.cpp" "$repo/source/build/src/zerotier.cpp" -o "$build_dir/disabled"
"$build_dir/disabled"
