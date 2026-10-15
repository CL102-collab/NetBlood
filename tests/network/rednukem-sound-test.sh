#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
flags=''
if [ "${SANITIZE:-0}" = 1 ]; then flags='-fsanitize=address,undefined -fno-omit-frame-pointer'; fi
${CXX:-c++} -std=c++14 -O1 -g -Wall -Wextra -Werror $flags tests/network/rednukem-sound.cpp -o "$tmp/sound"
"$tmp/sound"
