#!/usr/bin/env bash
#
# Builds VoiceTyper-<version>-x86_64.AppImage from an install prefix.
#
# Usage: build-appimage.sh <version> <install-prefix> <output-dir>
#
# The AppDir is staged from `cmake --install` (bin/, lib/, share/), then
# linuxdeploy with its Qt plugin collects the Qt plugins and the runtime
# libraries the binary links (Qt, libpulse, LayerShellQt). The engine shared
# objects already sit next to the executable (see the Linux install rules in
# CMakeLists.txt), so the dlopen loaders find them inside the AppImage.
#
# APPIMAGE_EXTRACT_AND_RUN=1 makes the tool AppImages work without FUSE, which
# is what a container CI needs.
#
# Not bundled on purpose: the models (the application downloads them into
# $XDG_DATA_HOME/VoiceTyper/models) and ydotool (a system daemon; without it the
# application degrades to clipboard-only, which the log states).

set -euo pipefail

version="${1:?usage: build-appimage.sh <version> <install-prefix> <output-dir>}"
prefix="${2:?install prefix}"
out_dir="${3:?output dir}"

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
tools_dir="${VOICETYPER_APPIMAGE_TOOLS:-$repo_root/build/appimage-tools}"
appdir="$out_dir/VoiceTyper.AppDir"
appimage="$out_dir/VoiceTyper-$version-x86_64.AppImage"

mkdir -p "$out_dir" "$tools_dir"

fetch_tool() {
    local url="$1" name="$2"
    if [[ ! -x "$tools_dir/$name" ]]; then
        echo "fetching $name"
        curl -fsSL --retry 3 -o "$tools_dir/$name" "$url"
        chmod +x "$tools_dir/$name"
    fi
}

fetch_tool \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage" \
    linuxdeploy
fetch_tool \
    "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage" \
    linuxdeploy-plugin-qt

export APPIMAGE_EXTRACT_AND_RUN=1
export PATH="$tools_dir:$PATH"
export OUTPUT="$appimage"
# linuxdeploy ships an old binutils whose strip chokes on the .relr.dyn section
# that current distributions emit ("unknown type [0x13] section .relr.dyn"), and
# linuxdeploy's bundled strip cannot read the .relr.dyn section that current
# distributions emit and it uses that bundled strip regardless of STRIP, so its
# own stripping is disabled here and the system binutils does it after the deploy.
export NO_STRIP=1
# linuxdeploy's Qt plugin decides which platform plugins to take from the linked
# modules and left the Wayland one out, which would make the AppImage unusable in
# a Wayland session; ask for it explicitly.
export EXTRA_QT_PLUGINS="wayland"
# The Qt plugin needs qmake/qtpaths; Arch names them qmake6/qtpaths6. An explicit
# QMAKE from the environment wins, which is also how a build can point the plugin
# at a filtered plugin directory.
if [[ -z "${QMAKE:-}" ]]; then
    if command -v qmake6 >/dev/null 2>&1; then
        export QMAKE="$(command -v qmake6)"
    elif command -v qmake >/dev/null 2>&1; then
        export QMAKE="$(command -v qmake)"
    fi
fi

rm -rf "$appdir"
mkdir -p "$appdir/usr"
cp -a "$prefix/." "$appdir/usr/"

desktop_file="$appdir/usr/share/applications/voicetyper.desktop"
icon_file="$appdir/usr/share/icons/hicolor/256x256/apps/voicetyper.png"
if [[ ! -f "$desktop_file" ]]; then
    echo "no desktop file at $desktop_file - run cmake --install first" >&2
    exit 1
fi
if [[ ! -f "$icon_file" ]]; then
    echo "no icon at $icon_file - run cmake --install first" >&2
    exit 1
fi

"$tools_dir/linuxdeploy" \
    --appdir "$appdir" \
    --desktop-file "$desktop_file" \
    --icon-file "$icon_file" \
    --plugin qt

# linuxdeploy leaves AppRun as a symlink to the executable because its Qt plugin
# skips the AppRun hook on Qt 6 ("skipping AppRun hook creation on Qt 6"), so
# nothing sets the bundled library and plugin paths. Such an image starts only when
# the caller happens to have those variables in the environment already - a plain
# double-click fails with "Could not find the Qt platform plugin", and so did the
# image restarted by the self-update. The hook is therefore written here.
rm -f "$appdir/AppRun"
cat > "$appdir/AppRun" <<'APPRUN'
#!/bin/sh
# The AppDir this file lives in. It is resolved from the file itself, so an
# inherited APPDIR from a previous mount cannot survive into the new process.
HERE="$(dirname "$(readlink -f "${0}")")"
export APPDIR="${HERE}"
export PATH="${APPDIR}/usr/bin:${PATH}"
export LD_LIBRARY_PATH="${APPDIR}/usr/lib:${APPDIR}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export QT_PLUGIN_PATH="${APPDIR}/usr/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="${APPDIR}/usr/plugins/platforms"
exec "${APPDIR}/usr/bin/voicetyper-qt-shell" "$@"
APPRUN
chmod 0755 "$appdir/AppRun"

# linuxdeploy's Qt plugin deploys the platform plugins it derives from the linked
# modules, and it left the Wayland one out - without it the AppImage cannot start
# in a Wayland session. EXTRA_QT_PLUGINS did not change that, so copy it here.
if [[ -n "${QMAKE:-}" ]]; then
    qt_plugins_dir="$("$QMAKE" -query QT_INSTALL_PLUGINS)"
    if [[ -f "$qt_plugins_dir/platforms/libqwayland.so" ]]; then
        mkdir -p "$appdir/usr/plugins/platforms"
        cp -a "$qt_plugins_dir/platforms/libqwayland.so" "$appdir/usr/plugins/platforms/"
    fi
fi

# Drop the plugins the application does not use. The Virtual Keyboard plugin drags
# in Qt Qml and Quick (13 MB), the KDE image formats drag in Pdf, PrintSupport and
# a pile of codec libraries, and the network-information plugins need NetworkManager
# and ConnMan at runtime for nothing. The image formats kept are the ones a desktop
# icon or a screenshot can be: png is built into Qt, plus gif/ico/jpeg/svg/webp.
rm -rf "$appdir/usr/plugins/networkinformation"
rm -f "$appdir/usr/plugins/platforminputcontexts/libqtvirtualkeyboardplugin.so"
rm -f "$appdir"/usr/plugins/imageformats/kimg_*
rm -f "$appdir"/usr/plugins/imageformats/libqjp2.so \
      "$appdir"/usr/plugins/imageformats/libqmng.so \
      "$appdir"/usr/plugins/imageformats/libqpdf.so \
      "$appdir"/usr/plugins/imageformats/libqtga.so \
      "$appdir"/usr/plugins/imageformats/libqtiff.so \
      "$appdir"/usr/plugins/imageformats/libqwbmp.so \
      "$appdir"/usr/plugins/imageformats/libqicns.so

# Then let the dependency graph decide: anything left that no kept binary or plugin
# links against goes away too (Qt Qml/Quick/Pdf/PrintSupport and their codecs).
python3 "$script_dir/prune-appdir.py" "$appdir"

# Strip with the system binutils, which understands the modern sections.
find "$appdir/usr/lib" "$appdir/usr/plugins" "$appdir/usr/bin" -type f \
    \( -name '*.so' -o -name '*.so.*' \) -exec strip --strip-unneeded {} + 2>/dev/null || true

fetch_tool \
    "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage" \
    appimagetool

# The AppDir is packaged here rather than by linuxdeploy, so the pruning and the
# strip above are part of the image.
rm -f "$appimage"
"$tools_dir/appimagetool" "$appdir" "$appimage"

if [[ ! -f "$appimage" ]]; then
    echo "appimagetool did not produce an AppImage" >&2
    exit 1
fi

sha256sum "$appimage" > "$appimage.sha256"
echo "built: $appimage"

