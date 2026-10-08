# Screen sharing verification

Build with Qt 6 Multimedia and a shared LGPL FFmpeg 7 SDK. On Windows CMake
downloads the pinned SDK; pass `UCD_FFMPEG_ROOT` to use an existing copy.
On Unix use shared pkg-config libraries. `UCD_WITH_SCREEN=OFF` keeps the
storage and sync application usable without media dependencies.

Run `ctest --test-dir <build> --output-on-failure`.

`ScreenFoundationTest` starts a real loopback rendezvous and two encrypted
uConnect nodes. It verifies bidirectional datagrams over direct UDP, UDP
relay and TCP fallback, kind filtering, the size bound, and channel closure.
It also round-trips a synthetic image through FFmpeg and rejects GPL/nonfree
configurations. `SettingsTest` verifies trusted-only grants cannot be saved
as defaults, while a trusted device can hold a direct grant.

The foundation round trip uses MPEG-4 solely to verify the SDK API. H.264
selection, capture and consent are added in later phases.

`ScreenTransportTest` runs a reproducible 20-second synthetic link at 5%
random loss with reordering, duplicates and delay spikes. Every delivered
payload must equal the source; delta frames must follow an intact reference.
It also checks NACK repair/expiration, pacing and blocked TCP sends, bounded
memory, token isolation, malformed fragments, sequence wrap, and congestion
degradation/recovery. No real network timing is needed for these tests.

For real-desktop acceptance, use two trusted devices with screen permissions.
The later phases require measuring 1080p30 capture-to-display latency on a
LAN, recovery under loss, monitor switching, stop paths and native input.
Windows automation here cannot establish macOS or Linux runtime results.
