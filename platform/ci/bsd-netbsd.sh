#!/usr/bin/env bash
# vmactions has no i386 NetBSD image. Use its amd64 VM as a cross-builder,
# with official i386 release libraries, never its amd64 pkgsrc dependencies.
set -euo pipefail
cd "$(dirname "$0")/../.."
repo=$PWD
[[ $(uname -s) == NetBSD && $(uname -m) == amd64 ]] || {
    echo 'Requires the NetBSD 10.1 amd64 builder VM.' >&2; exit 1;
}
export PATH="/usr/pkg/bin:/usr/pkg/sbin:$PATH"
python=${PYTHON:-python3.11}
work=${NETBLOOD_BUILD_ROOT:-$repo/.ci-build/netbsd}
sysroot=/emul/netbsd32
prefix=$work/prefix
tools=$work/tools
jobs=${BUILD_JOBS:-2}
mkdir -p "$work/downloads/source" "$work/downloads/i386" "$work/src" "$prefix" "$sysroot" "$repo/dist"

"$python" platform/ci/bsd-netbsd-fetch.py --kind source --destination "$work/downloads/source" \
    src syssrc gnusrc sharesrc > "$work/source-sets.txt"
while IFS= read -r archive; do tar -xpf "$archive" -C "$work/src"; done < "$work/source-sets.txt"
"$python" platform/ci/bsd-netbsd-fetch.py --kind i386 --destination "$work/downloads/i386" \
    base comp xbase xcomp > "$work/binary-sets.txt"
while IFS= read -r archive; do tar -xpf "$archive" -C "$sysroot"; done < "$work/binary-sets.txt"
(
    cd "$work/src/usr/src"
    # NetBSD's own supported cross-toolchain records i386 ABI defaults. Reusing
    # an amd64 compiler with only ARCH=i386 would produce the wrong package.
    ./build.sh -U -u -m i386 -j "$jobs" -T "$tools" -O "$work/tools-obj" -D "$sysroot" tools
)
cross=$tools/bin/i486--netbsdelf-
test -x "${cross}gcc"
test -x "${cross}g++"
flags='-O2 -march=pentium3 -mtune=generic -mno-sse2 -mno-sse3 -mno-ssse3 -mno-sse4 -mno-avx -mfpmath=387'

fetch_source() {
    local url=$1 digest=$2 name=$3
    if [[ ! -f $work/downloads/$name.tar.gz ]]; then
        curl --fail --location --retry 3 "$url" -o "$work/downloads/$name.tar.gz"
    fi
    "$python" - "$work/downloads/$name.tar.gz" "$digest" <<'PY'
import hashlib, pathlib, sys
path = pathlib.Path(sys.argv[1])
if hashlib.sha256(path.read_bytes()).hexdigest() != sys.argv[2]:
    raise SystemExit(f'SHA256 mismatch: {path}')
PY
    mkdir -p "$work/deps-src/$name"
    tar -xzf "$work/downloads/$name.tar.gz" --strip-components=1 -C "$work/deps-src/$name"
}
fetch_source https://github.com/libsdl-org/SDL/archive/refs/tags/release-2.30.11.tar.gz \
    cc6136dd964854e8846c679703322f3e2a341d27a06a53f8b3f642c26f1b0cfd SDL2
fetch_source https://github.com/xiph/flac/archive/refs/tags/1.4.3.tar.gz \
    0a4bb82a30609b606650d538a804a7b40205366ce8fc98871b0ecf3fbb0611ee FLAC
fetch_source https://github.com/webmproject/libvpx/archive/refs/tags/v1.14.1.tar.gz \
    901747254d80a7937c933d03bd7c5d41e8e6c883e0665fadcb172542167c7977 libvpx

cat > "$work/netbsd-i386.cmake" <<EOF
set(CMAKE_SYSTEM_NAME NetBSD)
set(CMAKE_SYSTEM_VERSION 10.1)
set(CMAKE_SYSTEM_PROCESSOR i386)
set(CMAKE_C_COMPILER "${cross}gcc")
set(CMAKE_CXX_COMPILER "${cross}g++")
set(CMAKE_AR "${cross}ar")
set(CMAKE_RANLIB "${cross}ranlib")
set(CMAKE_SYSROOT "$sysroot")
set(CMAKE_FIND_ROOT_PATH "$sysroot" "$prefix")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_C_FLAGS_INIT "$flags")
set(CMAKE_CXX_FLAGS_INIT "$flags")
EOF
cmake_args=(-DCMAKE_TOOLCHAIN_FILE="$work/netbsd-i386.cmake" -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib)
# Build external multimedia code at the same CPU floor. Disabling SIMD here
# also prevents optional routines from silently inheriting the builder's SSE2.
cmake -S "$work/deps-src/SDL2" -B "$work/sdl" "${cmake_args[@]}" \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_TESTS=OFF \
    -DSDL_ASSEMBLY=OFF -DSDL_SSEMATH=OFF -DSDL_SSE=OFF -DSDL_SSE2=OFF -DSDL_SSE3=OFF \
    -DSDL_VULKAN=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_DBUS=OFF -DSDL_IBUS=OFF \
    -DSDL_ALSA=OFF -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF \
    -DSDL_SNDIO=OFF -DSDL_LIBSAMPLERATE=OFF -DSDL_HIDAPI=OFF \
    -DSDL_X11=ON -DSDL_X11_SHARED=ON -DSDL_OPENGL=ON \
    -DCMAKE_PREFIX_PATH="$sysroot/usr/X11R7"
cmake --build "$work/sdl" --parallel "$jobs"
cmake --install "$work/sdl"
grep -q '^#define SDL_VIDEO_DRIVER_X11 1' "$prefix/include/SDL2/SDL_config.h"
grep -q '^#define SDL_VIDEO_OPENGL_GLX 1' "$prefix/include/SDL2/SDL_config.h"
cmake -S "$work/deps-src/FLAC" -B "$work/flac" "${cmake_args[@]}" \
    -DBUILD_SHARED_LIBS=OFF -DBUILD_CXXLIBS=OFF -DBUILD_PROGRAMS=OFF \
    -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF \
    -DWITH_OGG=OFF -DWITH_ASM=OFF -DWITH_AVX=OFF
cmake --build "$work/flac" --parallel "$jobs"
cmake --install "$work/flac"
mkdir -p "$work/vpx"
(
    cd "$work/vpx"
    export CC="${cross}gcc" CXX="${cross}g++" AR="${cross}ar" LD="${cross}gcc"
    export CFLAGS="$flags --sysroot=$sysroot" CXXFLAGS="$flags --sysroot=$sysroot" LDFLAGS="--sysroot=$sysroot"
    "$work/deps-src/libvpx/configure" --prefix="$prefix" --target=generic-gnu \
        --enable-pic --disable-shared --disable-examples --disable-tools --disable-docs \
        --disable-unit-tests --disable-vp8-encoder --disable-vp9-encoder
    gmake -j"$jobs"
    gmake install
)
gmake -j"$jobs" "$work/nblood.bin" PLATFORM=BSD ARCH=i386 CROSS="$cross" \
    obj="$work/obj" blood_game="$work/nblood.bin" LTO=0 NOASM=1 USE_MIMALLOC=0 \
    USE_SSE2=0 HAVE_GTK2=0 USE_VULKAN=0 "DEPENDENCY_PREFIX=$prefix" \
    "SDLCONFIG=$prefix/bin/sdl2-config" "OPTOPT=$flags" \
    "CUSTOMOPT=--sysroot=$sysroot -I$sysroot/usr/X11R7/include" \
    "LIBDIRS=-L$prefix/lib -L$sysroot/usr/lib -L$sysroot/usr/X11R7/lib"
"${cross}readelf" -h "$work/nblood.bin" | tee "$work/binary-format.txt"
grep -q 'Class:.*ELF32' "$work/binary-format.txt"
grep -q 'Machine:.*Intel 80386' "$work/binary-format.txt"

stage=$work/netblood
"$python" platform/ci/package.py stage --destination "$stage"
mkdir -p "$stage/lib" "$stage/licenses"
install -m 755 "$work/nblood.bin" "$stage/nblood.bin"
cp -P "$prefix"/lib/libSDL2*.so* "$stage/lib/"
cp "$work/deps-src/SDL2/LICENSE.txt" "$stage/licenses/SDL2.txt"
cp "$work/deps-src/FLAC/COPYING.Xiph" "$stage/licenses/FLAC.txt"
cp "$work/deps-src/libvpx/LICENSE" "$stage/licenses/libvpx.txt"
cp "$work/deps-src/libvpx/PATENTS" "$stage/licenses/libvpx-PATENTS.txt"
printf '%s\n' "$flags" > "$stage/cpu-build-flags.txt"
cat > "$stage/nblood" <<'EOF'
#!/bin/sh
appdir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LD_LIBRARY_PATH="$appdir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LD_LIBRARY_PATH
exec "$appdir/nblood.bin" -j "$appdir" "$@"
EOF
chmod 755 "$stage/nblood"
"$python" platform/ci/bsd-nosse2.py --binary "$stage/nblood.bin" \
    --library-dir "$stage/lib" --objdump "${cross}objdump"
cat > "$stage/INSTALL.txt" <<'EOF'
NetBlood for NetBSD 10.1 i386, Pentium III or newer; SSE2 is not required.
Install the standard NetBSD base and X11 sets (base, xbase, xetc, xserver).
The included SDL2 and statically linked FLAC/VPX were rebuilt without SSE2.
This package offers software rendering and Polymost OpenGL. Vulkan is disabled
for this legacy CPU target. OpenGL driver support depends on the installed GPU.

Extract the directory anywhere, then supply your own Blood 1.21 game data:
  ./nblood -j /path/to/blood
Use the nblood launcher so the included SDL2 library is found.
EOF
# The amd64 GENERIC kernel implements COMPAT_NETBSD32. The release i386 loader
# installed above resolves the 32-bit runtime; no amd64 pkgsrc libraries are used.
"$stage/nblood" -dedicated '-?' > "$work/smoke.log" 2>&1
grep -q 'Usage:' "$work/smoke.log"
tar -cJf "$repo/dist/netblood-netbsd-i386-nosse2.tar.xz" -C "$work" netblood
sha256 "$repo/dist/netblood-netbsd-i386-nosse2.tar.xz"
# vmactions syncs the workspace back to Ubuntu. Keep the package and diagnostics,
# not gigabytes of unpacked operating-system sources and compiler intermediates.
rm -rf "$work/src" "$work/tools-obj" "$work/tools" "$work/obj" \
    "$work/deps-src" "$work/downloads" "$work/sdl" "$work/flac" "$work/vpx"
