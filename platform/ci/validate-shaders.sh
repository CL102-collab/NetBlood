#!/usr/bin/env bash
# The byte-for-byte generated shader gate needs the same compiler release.
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/../.." && pwd)
cache=${1:?Usage: validate-shaders.sh CACHE_DIRECTORY [validate.py arguments...]}
shift
mkdir -p "$cache"
cache=$(cd -- "$cache" && pwd)
archive="$cache/glslang-15.2.0.tar.gz"
if [[ ! -f "$archive" ]]; then
    curl --fail --location --retry 3 \
        https://codeload.github.com/KhronosGroup/glslang/tar.gz/refs/tags/15.2.0 \
        --output "$archive.partial"
    mv "$archive.partial" "$archive"
fi
echo "45e3920d264d5c2cc3bfaec0e5dbb66cffd549255e0aaaf38cd283918e35c8ba  $archive" | sha256sum --check -
source_dir="$cache/glslang-15.2.0"
if [[ ! -d "$source_dir" ]]; then
    tar -xzf "$archive" -C "$cache"
fi
cmake -S "$source_dir" -B "$cache/glslang-build" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_EXTERNAL=OFF -DGLSLANG_TESTS=OFF -DENABLE_OPT=OFF \
    -DENABLE_HLSL=OFF -DENABLE_SPVREMAPPER=OFF
cmake --build "$cache/glslang-build" --target glslang-standalone --parallel "${CI_BUILD_JOBS:-2}"
mkdir -p "$cache/bin"
ln -sf "$cache/glslang-build/StandAlone/glslang" "$cache/bin/glslangValidator"
export PATH="$cache/bin:$PATH"
glslangValidator --version
python3 "$repo/platform/ci/validate.py" --suite shaders "$@"
