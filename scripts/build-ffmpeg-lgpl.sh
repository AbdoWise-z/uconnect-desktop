#!/usr/bin/env bash
# A reproducible shared LGPL SDK for macOS/Linux. No GPL/nonfree switches.
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
prefix="${1:-$project_dir/build/deps/ffmpeg-lgpl}"
mkdir -p "$prefix"
prefix="$(cd "$prefix" && pwd)"
source_dir="$project_dir/build/deps/ffmpeg-7.1.5"
archive="$project_dir/build/deps/ffmpeg-7.1.5.tar.xz"
mkdir -p "$(dirname "$archive")"
if [[ ! -e "$archive" ]]; then
    curl --fail --location --proto '=https' --tlsv1.2 https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz -o "$archive"
fi
expected=de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f
actual="$(shasum -a 256 "$archive" | cut -d ' ' -f1)"
[[ "$actual" == "$expected" ]] || { echo "FFmpeg source checksum mismatch" >&2; exit 1; }
if [[ ! -d "$source_dir" ]]; then tar -xJf "$archive" -C "$(dirname "$source_dir")"; fi
args=(--prefix="$prefix" --enable-shared --disable-static --disable-autodetect --disable-programs --disable-doc --enable-pic)
case "$(uname -s)" in
    Darwin) args+=(--enable-videotoolbox) ;;
    Linux) if command -v pkg-config >/dev/null && pkg-config --exists libva; then args+=(--enable-vaapi); fi ;;
    *) echo "Run on macOS or Linux." >&2; exit 1 ;;
esac
cd "$source_dir"
./configure "${args[@]}"
make -j "${JOBS:-4}"
make install
cp COPYING.LGPLv2.1 "$prefix/FFmpeg-LICENSE.txt"
echo "Shared LGPL SDK: $prefix"
echo "Configure the app with -DUCD_FFMPEG_ROOT=$prefix"
