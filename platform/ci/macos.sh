#!/usr/bin/env bash
# Build both slices from source. A universal executable with thin Homebrew
# dependencies is not a universal application, so do not link Homebrew libraries.
set -euo pipefail
cd "$(dirname "$0")/../.."
repo=$PWD
[[ $(uname -s) == Darwin ]] || { echo 'This script requires macOS and Xcode.' >&2; exit 1; }
work=${NETBLOOD_BUILD_ROOT:-$repo/.ci-build/macos}
mkdir -p "$work/downloads" "$work/src" "$repo/dist"
export MACOSX_DEPLOYMENT_TARGET=11.0
jobs=${BUILD_JOBS:-$(sysctl -n hw.ncpu)}
makecmd=${MAKE:-gmake}

fetch_source() {
    local url=$1 digest=$2 name=$3
    if [[ ! -f $work/downloads/$name.tar.gz ]]; then
        curl --fail --location --retry 3 "$url" -o "$work/downloads/$name.tar.gz"
    fi
    printf '%s  %s\n' "$digest" "$work/downloads/$name.tar.gz" | shasum -a 256 -c -
    mkdir -p "$work/src/$name"
    tar -xzf "$work/downloads/$name.tar.gz" --strip-components=1 -C "$work/src/$name"
}
fetch_source https://github.com/libsdl-org/SDL/archive/refs/tags/release-2.30.11.tar.gz \
    cc6136dd964854e8846c679703322f3e2a341d27a06a53f8b3f642c26f1b0cfd SDL2
fetch_source https://github.com/xiph/flac/archive/refs/tags/1.4.3.tar.gz \
    0a4bb82a30609b606650d538a804a7b40205366ce8fc98871b0ecf3fbb0611ee FLAC
fetch_source https://github.com/webmproject/libvpx/archive/refs/tags/v1.14.1.tar.gz \
    901747254d80a7937c933d03bd7c5d41e8e6c883e0665fadcb172542167c7977 libvpx
for arch in x86_64 arm64; do
    prefix=$work/$arch/prefix
    mkdir -p "$prefix" "$work/$arch/vpx"
    cmake_args=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix"
        -DCMAKE_OSX_ARCHITECTURES="$arch" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
        -DCMAKE_INSTALL_LIBDIR=lib)
    cmake -S "$work/src/SDL2" -B "$work/$arch/sdl" "${cmake_args[@]}" \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_TESTS=OFF
    cmake --build "$work/$arch/sdl" --parallel "$jobs"
    cmake --install "$work/$arch/sdl"
    cmake -S "$work/src/FLAC" -B "$work/$arch/flac" "${cmake_args[@]}" \
        -DBUILD_SHARED_LIBS=OFF -DBUILD_CXXLIBS=OFF -DBUILD_PROGRAMS=OFF \
        -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF \
        -DWITH_OGG=OFF -DWITH_ASM=OFF
    cmake --build "$work/$arch/flac" --parallel "$jobs"
    cmake --install "$work/$arch/flac"
    (
        cd "$work/$arch/vpx"
        export CC="clang -arch $arch" CXX="clang++ -arch $arch"
        export CFLAGS='-O2 -mmacosx-version-min=11.0'
        export CXXFLAGS="$CFLAGS" LDFLAGS="$CFLAGS"
        "$work/src/libvpx/configure" --prefix="$prefix" --target=generic-gnu \
            --enable-pic --disable-shared --disable-examples --disable-tools \
            --disable-docs --disable-unit-tests --disable-vp8-encoder --disable-vp9-encoder
        "$makecmd" -j"$jobs"
        "$makecmd" install
    )
    # Command-line LIBDIRS overrides auto-detected Homebrew/Ports paths. The
    # dependency include directory must precede any host package manager headers.
    "$makecmd" -j"$jobs" "$work/$arch/nblood" PLATFORM=DARWIN ARCH="$arch" \
        CC=clang CXX=clang++ COBJC=clang COBJCXX=clang++ \
        obj="$work/$arch/obj" blood_game="$work/$arch/nblood" \
        LTO=0 NOASM=1 HAVE_GTK2=0 USE_VULKAN=0 POLYMER=0 \
        "DEPENDENCY_PREFIX=$prefix" \
        "SDLCONFIG=$prefix/bin/sdl2-config" \
        "LIBDIRS=-L$prefix/lib" \
        "CUSTOMOPT=-I$prefix/include -mmacosx-version-min=11.0" \
        LDFLAGS=-Wl,-headerpad_max_install_names
done

python3 platform/ci/macos-bundle.py --build-root "$work" --destination "$work/NetBlood.app"
ditto -c -k --sequesterRsrc --keepParent "$work/NetBlood.app" "$repo/dist/netblood-macos-universal.zip"
shasum -a 256 "$repo/dist/netblood-macos-universal.zip"
