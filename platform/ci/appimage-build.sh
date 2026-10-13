#!/usr/bin/env bash
# Bundle the Debian 12 amd64 executable; graphics drivers stay on the host.
set -euo pipefail
binary=${1:?Usage: appimage-build.sh BINARY OUTPUT}
output=${2:?Output directory required}
repository=$(cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
appdir="$work/NetBlood.AppDir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/share/applications" "$output"
output=$(cd "$output" && pwd)
install -m755 "$binary" "$appdir/usr/bin/nblood"
python3 "$repository/platform/ci/package.py" stage --destination "$appdir/usr/share/games/nblood"
sed 's|Exec=/usr/games/nblood|Exec=nblood|' "$repository/platform/ci/linux.desktop" \
    > "$appdir/usr/share/applications/netblood.desktop"
python3 "$repository/platform/ci/linux-icon.py" "$appdir/usr/share/icons/hicolor/48x48/apps/netblood.png"

fetch_tool() {
    local url=$1 checksum=$2 destination=$3
    curl --fail --location --retry 3 --output "$destination" "$url"
    printf '%s  %s\n' "$checksum" "$destination" | sha256sum --check --strict
    chmod +x "$destination"
}
fetch_tool 'https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage' \
    c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d "$work/linuxdeploy"
fetch_tool 'https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage' \
    ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0 "$work/appimagetool"
fetch_tool 'https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64' \
    2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d "$work/runtime"

cat > "$work/AppRun" <<'EOF'
#!/usr/bin/env bash
set -e
appdir=${APPDIR:-$(cd -- "$(dirname -- "$0")" && pwd)}
export APPDIR="$appdir"
export LD_LIBRARY_PATH="$appdir/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export XDG_DATA_DIRS="$appdir/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
exec "$appdir/usr/bin/nblood" "$@"
EOF
chmod +x "$work/AppRun"
# GTK is dlopened by the startup window, so it is not in nblood's DT_NEEDED.
gtk=$(ldconfig -p | awk '/libgtk-x11-2.0.so.0 .*x86-64/ && !found++ { print $NF }')
[[ -n "$gtk" ]] || { echo 'GTK 2 runtime was not found.' >&2; exit 1; }
"$work/linuxdeploy" --appimage-extract-and-run --appdir "$appdir" \
    --executable "$appdir/usr/bin/nblood" --library "$gtk" \
    --exclude-library 'libvulkan.so*' \
    --desktop-file "$appdir/usr/share/applications/netblood.desktop" \
    --icon-file "$appdir/usr/share/icons/hicolor/48x48/apps/netblood.png" \
    --custom-apprun "$work/AppRun"
if find "$appdir" -name 'libvulkan.so*' -print -quit | grep -q .; then
    echo 'The Vulkan loader must be supplied by the host, not bundled.' >&2
    exit 1
fi

# Keep the distribution's copyright notices alongside bundled dependencies.
# Each library is still furnished under its original license, not the game's.
notices="$appdir/usr/share/doc/netblood/dependencies"
mkdir -p "$notices"
install -m644 "$repository/platform/ci/appimage-runtime-LICENSE" "$notices/appimage-runtime.txt"
while IFS= read -r -d '' library; do
    soname=$(basename "$library")
    original=$(ldconfig -p | awk -v name="$soname" '$1 == name && /x86-64/ && !found++ { print $NF }')
    owner=
    if [[ -n "$original" ]]; then
        owner=$(dpkg-query -S "$original" 2>/dev/null | head -n1 || true)
    fi
    if [[ -z "$owner" && -n "$original" ]]; then
        owner=$(dpkg-query -S "$(readlink -f "$original")" 2>/dev/null | head -n1 || true)
    fi
    if [[ -z "$owner" ]]; then
        # Private dependencies such as libpulsecommon do not appear in ldconfig.
        owner=$(dpkg-query -S "*/$soname" 2>/dev/null | head -n1 || true)
    fi
    owner=${owner%%: *}
    owner=${owner%%:*}
    if [[ -z "$owner" || ! -f "/usr/share/doc/$owner/copyright" ]]; then
        echo "Cannot find the license notice for bundled library: $soname" >&2
        exit 1
    fi
    install -m644 "/usr/share/doc/$owner/copyright" "$notices/$owner.txt"
done < <(find "$appdir/usr/lib" -type f -print0)

artifact="$output/netblood-linux-x86_64.AppImage"
ARCH=x86_64 "$work/appimagetool" --appimage-extract-and-run --no-appstream \
    --runtime-file "$work/runtime" "$appdir" "$artifact"
chmod +x "$artifact"
if ! SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    timeout 90 "$artifact" --appimage-extract-and-run -dedicated '-?' \
    > "$work/help.txt" 2>&1 || ! grep -q 'Usage:' "$work/help.txt"; then
    cat "$work/help.txt" >&2
    echo 'AppImage failed the startup smoke check.' >&2
    exit 1
fi
(cd "$output" && sha256sum "$(basename "$artifact")" > "$(basename "$artifact").sha256")
