# Screen sharing dependencies

uConnect dynamically links FFmpeg's libavcodec, libavutil and libswscale.
Windows builds use BtbN's FFmpeg n7.1.5-12-g1fdbca85aa LGPL shared archive,
release `autobuild-2026-07-31-14-10`, SHA-256
`0f376f96fb38554ccefb1b2ae9c7c6a7b351f0e60a372b38262c320e8392c5d0`.

The archive's LGPL v3 license is included as `FFmpeg-LICENSE.txt`, with the
incorporated GPL v3 text as `GPL-3.0.txt`. FFmpeg
source: https://github.com/FFmpeg/FFmpeg/tree/1fdbca85aa . Build recipes and
dependency sources: https://github.com/BtbN/FFmpeg-Builds . These shared
libraries may be replaced by compatible versions. FFmpeg is not part of
uConnect and its authors do not endorse this application.

Custom SDKs must be shared LGPL builds, without `--enable-gpl` or
`--enable-nonfree`; the codec wrapper checks this at runtime. Homebrew and
distribution packages may enable GPL components: check their configuration.

The BtbN build contains a source-built OpenH264 implementation. This is not
Cisco's binary distribution and must not be represented as carrying Cisco's
binary patent license. A Cisco binary fallback is a separate dependency.

The fallback uses OpenH264 API headers from Cisco commit
`652bdb7719f30b52b08e506645a7322ff1b2cc6f` (v2.6.0), under the BSD license
retained in each header. The binary is downloaded separately from Cisco,
never embedded in our packages. Installer for Windows, macOS and Linux (x64/ARM64):
`python scripts/install-openh264.py <codec-folder>`.
The Windows x64 compressed archive SHA-256 is
`dab5f2a872777f9a58b69bfa9fbcf20d9f82f2d6ec91383fd70bff49bd34ac9f`.

Enable or disable it in Settings > Screen Sharing, where the Cisco attribution
is displayed. Its complete license is in `OpenH264-BINARY-LICENSE.txt`.
The application invokes that library directly for the software fallback;
it does not select FFmpeg's source-built `libopenh264` encoder. Hardware
encoding and decoding use FFmpeg. This differs from the draft's assumption
that FFmpeg's fallback would load Cisco's binary itself.

For a shared LGPL SDK on macOS/Linux, run `bash scripts/build-ffmpeg-lgpl.sh`.
It pins FFmpeg 7.1.5's source archive and disables dependency autodetection;
it never enables GPL or nonfree components. Building this script requires
native compiler tools, make, pkg-config and nasm on x86. The other pinned
Cisco archive hashes are listed in `scripts/install-openh264.py`.
