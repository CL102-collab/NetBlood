#!/usr/bin/env bash
# Called inside linux.Dockerfile, once for each target architecture.
set -euo pipefail

kind=${1:?Usage: linux-build.sh deb|appimage standard|nosse2 OUTPUT [JOBS]}
baseline=${2:?CPU baseline required}
output=${3:?Output directory required}
jobs=${4:-2}
repository=$(cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repository"
architecture=$(dpkg --print-architecture)
[[ -z ${ZEROTIER_NETWORK_ID:-} || ${ZEROTIER_NETWORK_ID} =~ ^[0-9a-fA-F]{16}$ ]] || {
    echo 'ZEROTIER_NETWORK_ID must be a 16-digit hexadecimal network ID.' >&2; exit 2;
}

case "$kind" in deb|appimage) ;; *) echo "Unknown package kind: $kind" >&2; exit 2 ;; esac
case "$baseline" in standard|nosse2) ;; *) echo "Unknown CPU baseline: $baseline" >&2; exit 2 ;; esac
case "$architecture" in
    amd64) build_arch=x86_64; cpu_flags='-march=x86-64 -mtune=generic' ;;
    i386) build_arch=i386; cpu_flags='-march=pentium4 -mtune=generic -msse2 -mfpmath=sse' ;;
    armhf) build_arch=armv7l; cpu_flags='-march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard' ;;
    arm64) build_arch=arm64; cpu_flags='-march=armv8-a' ;;
    *) echo "Unsupported Debian architecture: $architecture" >&2; exit 2 ;;
esac
if [[ "$kind" == appimage && ( "$architecture" != amd64 || "$baseline" != standard ) ]]; then
    echo 'The AppImage target requires standard amd64.' >&2
    exit 2
fi
vulkan=1
allocator=1
suffix=
if [[ "$baseline" == nosse2 ]]; then
    [[ "$architecture" == i386 ]] || { echo 'nosse2 requires Debian i386.' >&2; exit 2; }
    # Debian 12's i386 runtime baseline is i686; unlike Debian 13, it does not
    # require SSE2. Its optimized libraries select implementations at runtime.
    # Do not use a newer Debian image for this target without revisiting this.
    cpu_flags='-march=i686 -mtune=generic -mno-sse -mno-sse2 -mfpmath=387'
    vulkan=0
    allocator=0
    suffix=-nosse2
fi

: "${NETBLOOD_REVISION:?Set NETBLOOD_REVISION to the source commit}"
: "${SOURCE_DATE_EPOCH:?Set SOURCE_DATE_EPOCH to the source commit timestamp}"
[[ "$NETBLOOD_REVISION" =~ ^[0-9a-f]{40}$ ]] || { echo 'Invalid source revision.' >&2; exit 2; }
[[ "$SOURCE_DATE_EPOCH" =~ ^[0-9]+$ && "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'Invalid timestamp or job count.' >&2; exit 2; }
mkdir -p "$output"
output=$(cd "$output" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

lto_workers=$jobs
# Common.mak treats literal LTO=1 as "enabled with 12 workers". GCC accepts 01
# as an explicit single worker, preserving BUILD_JOBS=1 on small CI runners.
[[ "$jobs" != 1 ]] || lto_workers=01
make -j"$jobs" blood PLATFORM=LINUX ARCH="$build_arch" NOASM=1 \
    USE_SSE2="$([[ "$baseline" == nosse2 ]] && echo 0 || echo 1)" \
    OPTOPT="$cpu_flags" USE_VULKAN="$vulkan" USE_MIMALLOC="$allocator" \
    HAVE_GTK2=1 LTO="$lto_workers" VC_REV=ci VC_HASH="${NETBLOOD_REVISION:0:9}" \
    "ZEROTIER_NETWORK_ID=${ZEROTIER_NETWORK_ID:-}"
file nblood
readelf -h nblood
# Help runs before assets are needed, and catches missing runtime libraries.
if ! SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 90 ./nblood -dedicated '-?' \
    > "$work/help.txt" 2>&1 || ! grep -q 'Usage:' "$work/help.txt"; then
    cat "$work/help.txt" >&2
    echo 'Packaged executable failed the startup smoke check.' >&2
    exit 1
fi
if [[ "$baseline" == nosse2 ]]; then
    # Audit all engine/static-code sections. Shared Debian libraries may contain
    # CPUID-dispatched SIMD implementations; the CPU smoke below exercises their
    # baseline selection instead of rejecting those unreachable instructions.
    python3 - <<'PY'
from pathlib import Path
import runpy
verify = runpy.run_path("platform/ci/windows-verify.py")["verify_no_sse2"]
verify(Path("nblood"), "objdump")
print("Verified engine instruction baseline: no SSE2 or newer instructions")
PY
    # Enforce the advertised CPU baseline against the executable and its actual
    # dynamically loaded Debian libraries, rather than checking flags alone.
    if ! SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        timeout 90 qemu-i386 -cpu pentium3 ./nblood -dedicated '-?' \
        > "$work/help-pentium3.txt" 2>&1 || ! grep -q 'Usage:' "$work/help-pentium3.txt"; then
        cat "$work/help-pentium3.txt" >&2
        echo 'Executable failed the Pentium III (no SSE2) startup smoke check.' >&2
        exit 1
    fi
fi

companion_output="$work/zerotier-output"
overlay_summary='ZeroTier companion bundled; see docs/zerotier.md for network setup.'
stage_overlay=(--zerotier-build "$companion_output")
if [[ "$baseline" == nosse2 ]]; then
    overlay_summary='Internet multiplayer only: ZeroTier SDK no-SSE2 baseline is unvalidated.'
    stage_overlay=(--zerotier-unavailable "$overlay_summary")
else
    # Keep SDK compiler output at the package's advertised architecture floor.
    python3 tools/zerotier/build.py --work-dir "$work/zerotier-build" \
        --output-dir "$companion_output" --jobs "$jobs" -- \
        "-DCMAKE_C_FLAGS=$cpu_flags" "-DCMAKE_CXX_FLAGS=$cpu_flags"
    file "$companion_output/netblood-zerotier"
    readelf -h "$companion_output/netblood-zerotier"
    timeout 90 "$companion_output/netblood-zerotier" --help > "$work/zerotier-help.txt"
    grep -q 'IPC version' "$work/zerotier-help.txt"
fi

if [[ "$kind" == appimage ]]; then
    platform/ci/appimage-build.sh "$repository/nblood" "$output" "$companion_output"
    exit
fi

package_root="$work/package"
install -Dm755 nblood "$package_root/usr/games/nblood"
if [[ "$baseline" != nosse2 ]]; then
    stage_overlay+=(--executable-directory "$package_root/usr/games")
fi
python3 platform/ci/package.py stage --destination "$package_root/usr/share/games/nblood" \
    "${stage_overlay[@]}"
install -Dm644 package/common/gpl-2.0.txt "$package_root/usr/share/doc/netblood/copyright"
install -Dm644 platform/ci/linux.desktop "$package_root/usr/share/applications/netblood.desktop"
python3 platform/ci/linux-icon.py "$package_root/usr/share/icons/hicolor/48x48/apps/netblood.png"

# Generate versioned runtime dependencies from the linked executable. GTK and
# OpenGL are loaded dynamically, so their dependencies are declared explicitly.
mkdir -p "$work/debian" "$package_root/DEBIAN"
cat > "$work/debian/control" <<'EOF'
Source: netblood
Section: games
Priority: optional
Maintainer: NetBlood contributors <339625596+dukenukem-king@users.noreply.github.com>

Package: netblood
Architecture: any
Description: NetBlood source port of Blood
EOF
shlib_executables=(-e"$package_root/usr/games/nblood")
if [[ "$baseline" != nosse2 ]]; then
    shlib_executables+=(-e"$package_root/usr/games/netblood-zerotier")
fi
dependencies=$(cd "$work" && dpkg-shlibdeps -O "${shlib_executables[@]}")
[[ "$dependencies" == shlibs:Depends=* ]] || { echo 'Cannot determine runtime dependencies.' >&2; exit 1; }
dependencies=${dependencies#shlibs:Depends=}
if [[ "$vulkan" == 1 && "$dependencies" != *libvulkan1* ]]; then
    # Vulkan is loaded through SDL on demand; it need not appear in DT_NEEDED.
    dependencies+=', libvulkan1'
fi
version="0+git$(date -u -d "@$SOURCE_DATE_EPOCH" +%Y%m%d).${NETBLOOD_REVISION:0:9}"
[[ "$baseline" != nosse2 ]] || version+='+nosse2'
installed_size=$(du -sk "$package_root/usr" | cut -f1)
cat > "$package_root/DEBIAN/control" <<EOF
Package: netblood
Version: $version
Architecture: $architecture
Section: games
Priority: optional
Maintainer: NetBlood contributors <339625596+dukenukem-king@users.noreply.github.com>
Homepage: https://github.com/dukenukem-king/NetBlood
Depends: $dependencies, libgtk2.0-0, libgl1
Installed-Size: $installed_size
Description: NetBlood source port of Blood
 Includes the engine, multiplayer support and redistributable engine assets.
 Original Blood game data must be supplied separately.
 CPU baseline: $baseline. Vulkan support: $vulkan.
 $overlay_summary
EOF
artifact="$output/netblood-debian12-${architecture}${suffix}.deb"
dpkg-deb --root-owner-group --build "$package_root" "$artifact"
dpkg-deb --info "$artifact"
dpkg-deb --contents "$artifact" > "$work/package-files.txt"
grep -q '/usr/games/nblood' "$work/package-files.txt"
grep -q '/usr/share/games/nblood/nblood.pk3' "$work/package-files.txt"
(cd "$output" && sha256sum "$(basename "$artifact")" > "$(basename "$artifact").sha256")
