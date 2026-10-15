#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
binary=$(mktemp)
trap 'rm -f "$binary"' EXIT
"${CXX:-c++}" -std=c++11 -Wall -Wextra -Werror tests/network/osd-password-redaction.cpp -o "$binary"
"$binary"
