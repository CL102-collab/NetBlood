#!/bin/sh
# Actual Apple compiler/backend checks; exit 77 explicitly means no GPU test.
# SDL_CONFIG may select the native-architecture SDK's sdl2-config executable.
# --realtime-lighting-only isolates light/shadow rendering and its lifecycle.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec python3 "$repo/tests/metal/run.py" "$@"
