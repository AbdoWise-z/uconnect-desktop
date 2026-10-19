#!/usr/bin/env bash
# A standalone Linux x86_64 package: a folder with the app, Qt (X11 and
# Wayland), its QML modules, FFmpeg and the other libraries it needs, as a
# .tar.gz, and the same as an AppImage. Build on the oldest distribution to
# support, as glibc is the one library not bundled: Ubuntu 24.04 gives 24.04
# and newer.
#
# Usage: scripts/package-linux.sh <Qt prefix, e.g. ~/Qt/6.8.3/gcc_64> <FFmpeg LGPL shared root> [label]
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
qt="$(cd "$1" && pwd)"; ffmpeg="$(cd "$2" && pwd)"; label="${3:-test}"
build="$project_dir/cmake-build-linux"
tools="$build/deploy-tools"
version="$(sed -n 's/^project(uconnect_desktop VERSION \([0-9.]*\).*/\1/p' "$project_dir/CMakeLists.txt")"
revision="$(git -C "$project_dir" rev-parse --short HEAD 2>/dev/null || echo unknown)"
name="uConnect-$version-$revision-linux-x86_64-$label"
dist="$project_dir/dist"

echo "== build"
cmake -S "$project_dir" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$qt" -DUCD_FFMPEG_ROOT="$ffmpeg" -DUCD_BUILD_TESTS=OFF > "$build.configure.log"
cmake --build "$build" --target uconnect_desktop

echo "== deployment tools"
mkdir -p "$tools"
fetch() {  # name url: an AppImage, extracted so FUSE is not needed, run as tools/bin/<name>
    if [ ! -x "$tools/$1.d/AppRun" ]; then
        curl -fsSL -o "$tools/$1.AppImage" "$2" && chmod +x "$tools/$1.AppImage"
        (cd "$tools" && rm -rf squashfs-root "$1.d" && "./$1.AppImage" --appimage-extract > /dev/null && mv squashfs-root "$1.d")
    fi
    mkdir -p "$tools/bin" && ln -sfn "$tools/$1.d/AppRun" "$tools/bin/$1"
}
fetch linuxdeploy https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
fetch linuxdeploy-plugin-qt https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
fetch appimagetool https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage

echo "== desktop entry and icon"
meta="$build/deploy-meta"; mkdir -p "$meta"
cat > "$meta/uconnect-desktop.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=uConnect Desktop
Comment=Share files, folders and screens between your devices
Exec=uconnect_desktop
Icon=uconnect-desktop
Categories=Network;FileTransfer;
Terminal=false
EOF
cp "$project_dir/packaging/icons/uconnect-256.png" "$meta/uconnect-desktop.png"  # see scripts/make-icons.py

echo "== bundle"
appdir="$build/AppDir"; rm -rf "$appdir"
deploy_qml="$build/deploy-qml"; [ -d "$deploy_qml" ] || deploy_qml="$project_dir/qml"
export QMAKE="$qt/bin/qmake" QML_SOURCES_PATHS="$deploy_qml"
export LD_LIBRARY_PATH="$ffmpeg/lib:$qt/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export EXTRA_PLATFORM_PLUGINS="libqwayland-egl.so;libqwayland-generic.so"
export EXTRA_QT_PLUGINS="wayland-decoration-client;wayland-graphics-integration-client;wayland-shell-integration;xcbglintegrations"
export PATH="$tools/bin:$PATH"
"$tools/bin/linuxdeploy" --appdir "$appdir" -e "$build/uconnect_desktop" \
    -d "$meta/uconnect-desktop.desktop" -i "$meta/uconnect-desktop.png" --plugin qt > "$build.deploy.log" 2>&1 \
    || { tail -40 "$build.deploy.log"; exit 1; }
cp "$ffmpeg/LICENSE.txt" "$appdir/usr/FFmpeg-LICENSE.txt" 2>/dev/null || true
cp "$project_dir/docs/OpenH264-BINARY-LICENSE.txt" "$project_dir/docs/third-party-screen.md" "$appdir/usr/" 2>/dev/null || true

echo "== package"
mkdir -p "$dist"
rm -rf "$build/$name" && cp -a "$appdir" "$build/$name"
tar -C "$build" -czf "$dist/$name.tar.gz" "$name"
ARCH=x86_64 "$tools/bin/appimagetool" --no-appstream "$appdir" "$dist/$name.AppImage" > "$build.appimage.log" 2>&1 \
    || { tail -20 "$build.appimage.log"; exit 1; }
(cd "$dist" && sha256sum "$name.tar.gz" "$name.AppImage" > "$name.sha256")
ls -la "$dist" | grep "$name"
