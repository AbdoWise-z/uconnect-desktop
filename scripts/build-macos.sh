#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != Darwin ]]; then
    echo "Run this script on macOS with Xcode Command Line Tools installed." >&2
    exit 1
fi
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
qt_prefix="${1:-${CMAKE_PREFIX_PATH:-}}"
if [[ -z "$qt_prefix" ]] && command -v brew >/dev/null 2>&1; then
    qt_prefix="$(brew --prefix qt)"
fi
if [[ -z "$qt_prefix" || ! -x "$qt_prefix/bin/macdeployqt" ]]; then
    echo "Pass the Qt macOS installation prefix, for example: bash scripts/build-macos.sh ~/Qt/6.8.3/macos" >&2
    exit 1
fi
command -v cmake >/dev/null
command -v ninja >/dev/null
xcrun --find clang++ >/dev/null

build_dir="$project_dir/build/macos"
cmake -S "$project_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$qt_prefix" \
    -DUCD_DEBUG_UI=ON -DUCD_BUILD_TESTS=ON
cmake --build "$build_dir" --parallel
ctest --test-dir "$build_dir" --output-on-failure

arch="$(uname -m)"
revision="$(git -C "$project_dir" rev-parse --short HEAD 2>/dev/null || echo source)"
package_name="uConnect-0.1.0-$revision-macos-$arch"
mkdir -p "$project_dir/dist"
package_dir="$project_dir/dist/$package_name"
if [[ -e "$package_dir" || -e "$package_dir.zip" ]]; then
    echo "Package already exists: $package_dir" >&2
    exit 1
fi
mkdir "$package_dir"
ditto "$build_dir/uconnect_desktop.app" "$package_dir/uConnect.app"
cp "$project_dir/packaging/TESTING.txt" "$package_dir/README.txt"
printf 'Source revision: %s\nPlatform: macOS %s\nConfiguration: Release\n' "$revision" "$arch" > "$package_dir/BUILD.txt"
codesign --verify --deep --strict "$package_dir/uConnect.app"
ditto -c -k --sequesterRsrc --keepParent "$package_dir" "$package_dir.zip"
(cd "$project_dir/dist" && shasum -a 256 "$package_name.zip" > "$package_name.zip.sha256")
echo "Built: $package_dir.zip"
