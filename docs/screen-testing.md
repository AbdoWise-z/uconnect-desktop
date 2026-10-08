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

`ScreenCodecTest` round-trips H.264 through each encoder that can initialize,
requires PSNR above 28 dB, and exercises bitrate and size changes. Enable the
Cisco row and real-session test with CMake's `UCD_TEST_OPENH264=<library-path>`.
Missing hardware is reported as skipped, not passed.

`ScreenSessionTest` checks trust, denial, busy slots, consent timeout, stale
approval, cancellation, revocation and feedback timeout. It also starts two
actual Sessions with proven identities, requests consent, sends synthetic
frames over their datagram channel, decodes them, and stops from the viewer.

`ScreenCaptureTest` is opt-in on a real desktop:
`UCD_TEST_DESKTOP_CAPTURE=1 ctest --test-dir <build> -R ScreenCaptureTest`.
It captures the primary desktop and round-trips its image through H.264.
On Windows PowerShell set `$env:UCD_TEST_DESKTOP_CAPTURE='1'` first.

Manual phase-2 acceptance:

1. Offer Screen Sharing on the host, trust both devices and grant the viewer
   `screen.view`. Open its Screen service; deny once, then allow once.
2. Confirm the always-on-top sharing bar remains visible, and that its Stop
   action and Ctrl+Alt+Shift+S both stop capture immediately.
3. Switch monitors, resize/Fit/100%, and enter/leave full screen. Check the
   shown path and stats; revoke the permission while sharing.
4. Animate a 1080p desktop on a LAN and measure actual visible frame latency
   with a common-clock high-speed recording. The acceptance target is 30 fps
   and <150 ms. Reported decode time is not this measurement.
5. Introduce 5% packet loss and verify picture recovery within about a second.
6. On macOS grant Screen Recording; on Linux test X11 and the desktop portal
   separately. An OS/backend capture error must stop hosting and explain why.
