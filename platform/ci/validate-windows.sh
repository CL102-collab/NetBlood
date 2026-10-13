#!/usr/bin/env bash
# Exercise Windows-only registry/shell and named-pipe code on a Linux runner.
# Wine establishes these API regressions; it is not native Windows hardware testing.
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/../.." && pwd)
scratch_parent=${1:?Usage: validate-windows.sh SCRATCH_DIRECTORY}
mkdir -p "$scratch_parent"
scratch_parent=$(cd -- "$scratch_parent" && pwd)
work=$(mktemp -d "$scratch_parent/runtime.XXXXXX")
compiler=${WINDOWS_TEST_CXX:-x86_64-w64-mingw32-g++-posix}
wine_command=${WINE:-wine}
wine_server=${WINESERVER:-wineserver}
export WINEPREFIX="$work/prefix"
export WINEARCH=win64
export WINEDEBUG=-all
# Disable optional Wine browser/Mono installation and desktop integration.
export WINEDLLOVERRIDES='winemenubuilder.exe=d;mscoree,mshtml='
cleanup() {
    "$wine_server" -k >/dev/null 2>&1 || true
    rm -rf "$work"
}
trap cleanup EXIT
uri_directory="$work/NetBlood test % path"
mkdir -p "$uri_directory"
"$compiler" -std=c++14 -O1 -g -Wall -Wextra -static -static-libgcc -static-libstdc++ \
    -I"$repo/source/blood/src" \
    "$repo/tests/network/uri-registration-windows.cpp" "$repo/source/blood/src/neturireg.cpp" \
    -ladvapi32 -lshell32 -o "$uri_directory/uri-registration.exe"
"$compiler" -std=c++14 -O1 -g -Wall -Wextra -static -static-libgcc -static-libstdc++ \
    -I"$repo/source/blood/src" -I"$repo/source/build/include" \
    "$repo/tests/network/discord-rpc-windows.cpp" "$repo/source/blood/src/discord_rpc.cpp" \
    -pthread -o "$work/discord-rpc.exe"
echo "Initializing the isolated Wine prefix"
timeout 120 "$wine_command" wineboot.exe -u
# This fixture verifies HKCU values first. The explicit test-only mirror works
# around Wine versions that do not merge HKCU classes into their HKCR view.
echo "Checking Windows URI registration and shell launch"
NETBLOOD_TEST_WINE_CLASSES=1 timeout 120 "$wine_command" "$uri_directory/uri-registration.exe"
echo "Checking Windows Discord named-pipe transport"
timeout 120 "$wine_command" "$work/discord-rpc.exe"
