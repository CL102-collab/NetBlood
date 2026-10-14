#!/bin/sh
# Build and run focused native Vulkan checks, with the current SDL display.
# Needs a C++11 compiler, pkg-config, SDL2, a Vulkan loader/driver, and optionally
# the Khronos validation layer. For a headless host, supply DISPLAY from Xvfb.
# Runs identical scenes on the caller alone and with up to four background
# recorders. Checks complete RGBA images, ordering/resource boundaries, caller
# participation, actual overlap, and background/caller error recovery.
# Also compares one/three slots and the default two slots, consecutive presents without
# readbacks, retirement fences, and injected submit/acquire/present failures.
# Pass --teardown-idle-only to run just the device-idle failure/recovery check.
# Pass --realtime-lighting-only for point-light and geometry shadow checks.
# Example: VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ./tests/vulkan/backend-test.sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/netblood-vulkan-test.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

pkg-config --exists sdl2 vulkan
"${CXX:-c++}" -std=c++11 -DUSE_VULKAN -DVULKAN_BACKEND_TEST_DISPATCH -Wall -Wextra -Werror \
    -I"$repo/source/build/include" -I"$repo/source/glad/include" \
    ${CPPFLAGS:-} ${CFLAGS:-} ${CXXFLAGS:-} $(pkg-config --cflags sdl2 vulkan) \
    "$repo/tests/vulkan/backend.cpp" "$repo/source/build/src/vulkan_renderer.cpp" \
    ${LDFLAGS:-} $(pkg-config --libs sdl2) -o "$build_dir/backend-test"

# A loader dependency here would break software/OpenGL on machines without
# Vulkan. The backend must resolve every Vulkan entry point through SDL.
if command -v readelf >/dev/null 2>&1; then
    if readelf -d "$build_dir/backend-test" | grep -E 'NEEDED.*(libvulkan|MoltenVK)' >/dev/null; then
        echo "FAIL: backend has a hard-linked Vulkan runtime dependency" >&2
        exit 1
    fi
fi
"$build_dir/backend-test" --missing-loader
result=0
# Include cross-command-buffer GPU synchronization checks whenever the caller
# enables Khronos validation; ordinary host threading checks remain enabled too.
case ":${VK_INSTANCE_LAYERS:-}:" in
    *VK_LAYER_KHRONOS_validation*)
        VK_LAYER_ENABLES="${VK_LAYER_ENABLES:+$VK_LAYER_ENABLES,}VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT"
        export VK_LAYER_ENABLES
        ;;
esac
"$build_dir/backend-test" "$@" >"$build_dir/output.log" 2>&1 || result=$?
cat "$build_dir/output.log"
if [ "$result" -ne 0 ]; then exit "$result"; fi
# Validation messages do not normally alter the process exit status.
if grep -E 'Validation Error|VUID-' "$build_dir/output.log" >/dev/null; then
    echo "FAIL: Vulkan validation reported an error" >&2
    exit 1
fi
