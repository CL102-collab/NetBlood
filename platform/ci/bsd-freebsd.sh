#!/usr/bin/env bash
# Run in the FreeBSD 14.5 amd64 VM, hosted by an Ubuntu Actions runner.
set -euo pipefail
cd "$(dirname "$0")/../.."
repo=$PWD
[[ $(uname -s) == FreeBSD && $(uname -m) == amd64 ]] || {
    echo 'Requires the FreeBSD amd64 VM.' >&2; exit 1;
}
work=${NETBLOOD_BUILD_ROOT:-$repo/.ci-build/freebsd}
stage=$work/netblood
mkdir -p "$work" "$repo/dist"
gmake -j"${BUILD_JOBS:-2}" "$work/nblood" \
    PLATFORM=BSD ARCH=x86_64 CC=clang CXX=clang++ NOASM=1 \
    obj="$work/obj" blood_game="$work/nblood" LTO=0 \
    HAVE_GTK2=0 USE_VULKAN=1 DEPENDENCY_PREFIX=/usr/local \
    SDLCONFIG=/usr/local/bin/sdl2-config 'OPTOPT=-march=x86-64 -mtune=generic' \
    'VULKAN_CFLAGS=-I/usr/local/include' \
    'CUSTOMOPT=-Wl,-rpath,/usr/local/lib'
file "$work/nblood" | tee "$work/binary-format.txt"
file "$work/nblood" | grep -q 'ELF 64-bit.*x86-64'
ldd "$work/nblood" | tee "$work/runtime-libraries.txt"
if grep -q 'not found' "$work/runtime-libraries.txt"; then exit 1; fi
"$work/nblood" -dedicated '-?' > "$work/smoke.log" 2>&1
grep -q 'Usage:' "$work/smoke.log"
python3 platform/ci/package.py stage --destination "$stage"
install -m 755 "$work/nblood" "$stage/nblood"
cp "$work/runtime-libraries.txt" "$stage/runtime-libraries.txt"
pkg info sdl2 flac libvpx vulkan-loader > "$stage/dependency-versions.txt"
cat > "$stage/INSTALL.txt" <<'EOF'
NetBlood for FreeBSD 14.5 or later, amd64.

Install the runtime libraries with:
  pkg install sdl2 flac libvpx vulkan-loader
Install the Vulkan/OpenGL driver appropriate to your GPU separately.

Extract this directory anywhere. Supply your own Blood 1.21 game data:
  ./nblood -j /path/to/blood
The engine, nblood.pk3 and licenses are included; retail assets are not.
EOF
tar -cJf "$repo/dist/netblood-freebsd-amd64.tar.xz" -C "$work" netblood
sha256 "$repo/dist/netblood-freebsd-amd64.tar.xz"
