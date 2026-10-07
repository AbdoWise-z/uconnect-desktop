# Screen sharing dependencies

uConnect dynamically links FFmpeg's libavcodec, libavutil and libswscale.
Windows builds use BtbN's FFmpeg n7.1.5-12-g1fdbca85aa LGPL shared archive,
release `autobuild-2026-07-31-14-10`, SHA-256
`0f376f96fb38554ccefb1b2ae9c7c6a7b351f0e60a372b38262c320e8392c5d0`.

The archive's LGPL v3 license is included as `FFmpeg-LICENSE.txt`. FFmpeg
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
