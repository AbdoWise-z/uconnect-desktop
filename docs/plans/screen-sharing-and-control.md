# Plan: screen sharing and remote control

Status: **phases 0-4 implemented, plus post-phase fixes; verified on Windows, macOS and Linux X11; Wayland, QSV/AMF and latency pending** (2026-10-09). Each phase is implemented,
tested and committed before the next phase. Wire details land in `docs/protocol.md`.

Execution notes:

- Phase 0: Qt Multimedia installed; pinned FFmpeg shared SDK, optional build,
  authenticated datagram routing and trusted-only screen/control grants added.
  Foundation tests cover real direct UDP, UDP relay and TCP fallback, codec
  round trip, and default-grant filtering. **Validated:** Windows Debug build
  and all 7 CTest suites pass, including the three real network paths.
- Phase 1: paced fragmentation, bounded ordered reassembly, one-shot keyframe
  NACKs, keyframe gating, feedback and AIMD implemented. Synthetic tests cover
  5% seeded loss, reordering, duplicates, delay spikes, TCP backpressure,
  timestamp/frame wrap and malformed/oversized inputs. Transport tests pass.
- Phase 2: Qt screen capture, hardware H.264 selection and a separately loaded
  Cisco fallback, off-thread codec work, consent/timeout/direct grants,
  cancellation/revocation/disconnect handling, sharing bar and native stop
  shortcut, monitor selection and viewer stats implemented. Automated Windows
  tests verify two real authenticated sessions carrying synthetic video, codec
  PSNR above 28 dB (NVENC, Media Foundation and Cisco), settings and QML loading.
  A real 1920×1080 Windows desktop capture/codec smoke test passed. QSV and AMF
  were unavailable on this machine. LAN 30 fps / <150 ms glass-to-glass and
  macOS/Linux acceptance remain manual tests, not established by this run.
  All 11 CTest suites pass. The app also builds with `UCD_WITH_SCREEN=OFF`;
  its foundation and settings tests pass in that separate build.
- Phase 3: native input backends, physical HID mapping, Unicode/IME capture,
  separate control consent, focus/Right Ctrl release and cursor shape/position
  channels implemented. Recording-backend tests cover invalid input and held
  key/button release on all principal stop paths. Windows app build and all
  12 suites pass. Cross-layout/native desktop acceptance remains manual.
- Phase 4: 1-8 viewers share an encoder, with independent pacing and one
  control owner. Camera sharing runs alongside screen sessions on its own
  datagram kind and always asks consent. Native DXGI/D3D11 NV12 encoding,
  ScreenCaptureKit/VideoToolbox surfaces, hardware decoding, Wayland portal
  selection, configurable keyboard release, platform installers and protocol
  7 are implemented. Windows tests verify real GPU capture via NVENC, shared
  access units and simultaneous screen/camera over two real Sessions. QSV/AMF
  remain unverified on unavailable hardware; macOS/Linux sources require native
  builds and manual tests. XOR parity is conditional on field-loss evidence,
  which has not been provided, so it remains deferred.
  All 13 suites pass in Windows Debug and Release, including real desktop
  capture and NVENC. The screen-disabled build and its foundation/settings
  tests also pass. The datagram-opening race regression passed 15 consecutive
  runs of both foundation and session tests.
- Post-phase fixes (2026-10-08/09), after the first real two-machine use
  showed the frame rate collapsing, silent starts, and a needless trust rule:
  - **Transport:** the pacer queues up to 3 frames and retries refused
    datagrams instead of dropping every frame that arrives while the previous
    one is sending. Incomplete delta frames are nacked too, from a 500 ms send
    history. Congestion control cuts less, at most once per few reports, and
    a slow decoder lowers the frame rate instead of the bitrate.
  - **Encoders:** FFmpeg's NVENC restarts with a keyframe on every bitrate
    change, so hardware encoders get bitrate changes in batches (cuts at most
    once a second, raises every few seconds); only a change beyond 2x reopens
    them. DXGI keeps its desktop duplication across encoder changes and
    reopens it on access loss (display change, UAC, lock screen).
  - **Capture:** native capture (DXGI) goes first and starts as soon as a
    viewer is live; Qt's capture runs only when native capture is
    unavailable, never alongside it (they compete for the desktop, and the
    second one fails). A native path that worked is retried after a loss.
  - **Viewer:** frames that complete mid-decode queue for the decoder instead
    of costing a keyframe. Decoded frames go to the video sink in their YUV
    layout (labelled BT.709/BT.601) for GPU conversion, instead of a CPU RGBA
    conversion and copy: 5-7 ms instead of 13-16 ms per 1080p frame.
  - **Start feedback:** the host reports (`scr.status`) when nothing was sent
    5 s in; the viewer shows "waiting for the first picture", then a hint and
    Try Again.
  - **Trust:** watching needs only the host's proven identity; only the host
    has to trust the viewer (decision 2).
  - **Frame rate:** up to 165 fps (one `MaxFps` constant), with capture timed
    on fractional intervals. The viewer now requests its configured rate
    (it always asked for 30).
  - **App:** quitting while sharing exits instead of leaving an invisible
    process; the sharing bar never takes focus; `UCD_SCREEN_STATS=1` logs
    per-second capture, encode and decode counters.
  - **Measured** on one Windows machine (DXGI + NVENC, loopback): 60 fps
    target, 61 fps encoded at ~4 ms and 61 shown at 5-7 ms per frame; 165 fps
    target, 161-165 encoded at ~2.5 ms and 143-165 shown at ~5 ms per frame.
    All 13 suites pass, plus the real-desktop NVENC test.
- Hardware, macOS/Linux capture, and glass-to-glass latency require real
  desktops; automated synthetic tests cannot establish those acceptance values.
- The pinned BtbN build embeds source-built OpenH264. Its software encoder
  does not carry Cisco's binary patent license. The Cisco binary fallback
  must load separately; see `docs/third-party-screen.md`.

## Decisions taken

| # | Question | Decision |
|---|---|---|
| 1 | Codec library | **FFmpeg** (libavcodec, libswscale), LGPL shared builds only; no GPL or non-free parts. |
| 2 | Consent | Only **trusted** devices may send a request. The host is asked each time, unless the device holds a "without asking" permission (`screen.direct` / `control.direct`). Those are only grantable to trusted devices. Trust is one-way: the viewer needs only the host's proven identity, not to trust it back. |
| 3 | Unattended access | **Out of scope.** No system service, so no lock screen, UAC prompts or Ctrl+Alt+Del. |
| 4 | Viewers | Phase 2 starts with one viewer; phase 4 implements a configurable **1-8 viewers**, sharing one encoder and one control owner. |
| 5 | Transport | **Datagrams from the start** (uconnect's datagram channel), no TCP-first version. |
| 6 | Frame rate | Up to **165 fps**, for high refresh displays (was 60). |

Also settled while drafting:

- **Codec: H.264.** It has hardware encoders on all three desktop systems, and
  hardware decoders on every phone (which the Flutter app needs).
- **Out of scope for now:** audio, clipboard sync and file drag-in. They are
  listed as follow-ups at the end.
- **Platforms:** Windows, macOS and Linux X11 hosts for both view and
  control. Linux Wayland: view through the desktop portal (phase 4); no
  control in v1.

## Building blocks already there

- **Permissions** `screen.view` and `control.input`, shown as "not available
  yet".
- **Windows:** Screen and Remote Control service windows, with toolbars and an
  empty video area.
- **Trust:** proven identities, trust, and per-device grants.
- **uconnect** (`include/uconnect/uconnect.hpp`):
  - Reliable messages of up to 1 MiB.
  - A **datagram channel**: `open_datagrams()`, `send_datagram()`,
    `on_datagram()`. It is unreliable, unordered and encrypted, and carries
    at most `max_datagram()` = **1100 bytes** per datagram.
  - The channel reports its path (direct UDP, relay, or TCP fallback).

## Architecture

```
HOST                                                    VIEWER
ScreenSource ─► Encoder ─► Packetizer ─► datagrams ─► Reassembler ─► Decoder ─► VideoSink (QML)
     ▲             ▲   (FFmpeg)           (paced)         │   (FFmpeg)
     │             └──── bitrate / keyframe ◄── feedback ─┘ (reliable msgs)
InputInjector ◄──────────────── input events (reliable msgs) ◄──────────── InputCapture
CursorSource ──► cursor shape (reliable) + position (datagram) ──────────► cursor overlay
```

New code lives in `src/core/screen/`, plus platform folders.

### Host side

- **`ScreenSource`**: capture of one monitor. Native capture comes first:
  DXGI Desktop Duplication on Windows, ScreenCaptureKit on macOS, encoding
  GPU surfaces directly. Qt Multimedia's `QScreenCapture` (Qt 6.5+; wraps
  Windows.Graphics.Capture/DXGI, ScreenCaptureKit and X11/PipeWire) is the
  fallback, and only runs while native capture is unavailable: the two
  compete for the desktop.
  - Needs the `qtmultimedia` module (`aqt install-qt ... -m qtmultimedia`).
  - Monitor list and switching.
  - Capture without the cursor; the cursor travels separately.
- **`Encoder`**: FFmpeg H.264, low-latency setup:
  - **Encoders, in order:** a hardware encoder first (`h264_nvenc`,
    `h264_qsv`, `h264_amf`, `h264_mf`, `h264_videotoolbox`, `h264_vaapi`),
    then the software `libopenh264` fallback.
  - **Settings:** no B-frames, a constrained bitrate with a small buffer, a
    keyframe every ~4 s plus on request, and periodic intra refresh where
    supported.
  - **Pipelines:** native capture stays on the GPU (D3D11 video processor
    to NV12, then NVENC/QSV/AMF). The Qt fallback maps the frame to the CPU,
    swscales to NV12 and encodes.
  - **Scaling:** downscale to what the viewer asked for (`maxWidth`),
    within the host's limits.
  - **Bitrate:** set from viewer feedback. Hardware encoders get it in
    batches, since FFmpeg's NVENC restarts with a keyframe on each change.
- **`Packetizer`**: splits each encoded frame into fragments that fit one
  datagram.
  - Paces each frame over about half a frame interval, so a burst cannot
    overflow queues. Up to 3 frames wait their turn.
  - Keeps the fragments of frames sent in the last 500 ms, to answer
    retransmit requests (see transport).
- **`InputInjector`** (per OS):
  | OS | API | Note |
  |---|---|---|
  | Windows | `SendInput` | Cannot reach elevated windows; that is expected, see decision 3. |
  | macOS | `CGEventPost` | Needs the user to grant Accessibility once; detect and guide. |
  | Linux X11 | XTest | |
  | Wayland | — | Reports "not supported" in v1. |
- **`CursorSource`**: the cursor's shape when it changes (PNG + hotspot, a
  reliable message) and its position (a small datagram, newest wins).
- **`ScreenHost`**: one per offered screen.
  - Accepts or denies requests (trust, permissions, consent).
  - Owns the session and its single viewer slot.
  - Starts and stops capture and encoding.
  - Logs to the activity log.

### Viewer side

- **`Reassembler`**: rebuilds frames from fragments.
  - Drops frames that are not complete by their deadline.
  - Tracks loss and jitter.
  - Asks for a keyframe when the decoder cannot continue.
- **`Decoder`**: FFmpeg with hardware acceleration where available (`d3d11va`
  / `dxva2`, `videotoolbox`, `vaapi`), else software.
  - Complete frames queue for the decoder in order; each is a reference
    for the next.
  - Output goes as a YUV `QVideoFrame` → `QVideoSink` → QML `VideoOutput`,
    which converts it on the GPU. Hardware-decoded frames are still copied
    to the CPU on the way.
- **`InputCapture`**: the viewer window turns mouse and keyboard into input
  events while control is on (see the input section).
- **`ScreenViewer`**: requests, the session state, feedback reports, and stats
  (fps, bitrate, latency, loss).

### Session plumbing

- **Datagram channel:**
  - `Session` opens the datagram channel to a peer when a screen session
    starts, and closes it at the end.
  - It routes `on_datagram` to the screen service by a 1-byte datagram kind.
  - It surfaces `on_datagram_path` changes (direct, relay or TCP fallback),
    which the UI shows.
- **Threads:**
  - Encoding and decoding run on their own threads, off the session worker.
    The worker only moves bytes.
  - Capture callbacks stay off the GUI thread.

## Transport (datagrams)

### Video fragments (datagram kind 1)

```
[u8 kind=1][u8 flags][u16 session][u32 frame][u16 index][u16 count][u32 capture-ms][payload ≤ 1084]
flags: keyframe, retransmit
```

- **Reassembly:** frames are reassembled by `frame` and `index`. The deadline
  is about 2 frame intervals, plus jitter, after the first fragment; a
  frame not complete by then is dropped.
- **Missing references:** a frame whose reference was lost is not decoded.
  The viewer asks for a keyframe (`scr.keyframe`) and shows the last good
  frame until one arrives.
- **Fragment loss:** the viewer asks for just the missing fragments of an
  incomplete frame (`scr.nack {frame, indexes}`), once. The host answers from
  its 500 ms send history. If that fails, the next keyframe heals the
  picture.
- **Later, if loss is common:** simple XOR parity per group of fragments.

### Feedback and congestion control (reliable messages)

- **Reports:** every 250–500 ms the viewer sends
  `scr.report {received, lost, late, jitterMs, owdTrendMs, decodeMs}`.
- **Host response (AIMD on bitrate):**
  - It lowers the bitrate by 20% on loss above 5% or rising delay, at most
    once per few reports.
  - It raises it gradually while the link is clean.
  - Bitrate stays between configured bounds (never below 300 kbit/s).
  - **Last resort:** fewer frames per second, then a smaller resolution.
  - A viewer that decodes too slowly gets fewer frames, not a lower bitrate.
- **Path changes:**
  - **Datagrams fall back to TCP** (reliable and ordered): the same reports
    show rising delay and the host lowers the bitrate. Datagrams the full
    fallback queue refuses are retried; frames too old to show are dropped
    and a keyframe follows. Video stays usable, only slower to react.
  - **Relay path:** a lower starting bitrate, shown to the user.

### Cursor position (datagram kind 2)

```
[u8 kind=2][u16 session][u32 seq][i32 x][i32 y][u8 visible]
```

Newest wins; older `seq` values are ignored.

### Input (reliable messages, never datagrams)

A lost key-up would leave a key stuck, so input always goes over reliable
messages.

- `in.mouse {x, y, buttons}`: `x` and `y` are normalized 0–1 in the shared
  monitor. Moves are coalesced to ≤ 120 per second; button changes are sent
  at once.
- `in.wheel {dx, dy}` in 1/120-notch units.
- `in.key {usage, down}`: USB HID usage codes, so it works whatever the
  keyboard layout. The host maps them to native codes.
- `in.text {text}`: typed text from IMEs and phone keyboards.
- `in.release`: sent when control ends or focus leaves. The host releases
  every key and button the viewer pressed. The host also does this on
  disconnect and on session end.

## Requests, consent and permissions

### Permissions

All of these are per device, and only for trusted devices.

| id | allows |
|---|---|
| `screen.view` (exists) | ask to view the screen |
| `control.input` (exists) | ask for control, during a view session |
| `screen.direct` (new) | view without the host being asked |
| `control.direct` (new) | control without the host being asked |

**Untrusted devices** cannot send screen requests at all, whatever the
default permissions say. They are answered with `scr.err {code:"forbidden"}`,
and the viewer explains that the host has to trust it. The viewer itself
does not need to trust the host, only to have its identity proven.

### The request flow (reliable messages)

1. **The viewer asks:** `scr.request {wantControl, maxWidth, maxFps, codecs:["h264"]}`.
2. **The host checks** trust, permissions and the free viewer slot (1 for
   now). An unavailable slot gets `scr.err {code:"busy"}`.
3. **Consent:** without the `.direct` permission, the host shows a prompt:
   device name + fingerprint, "wants to view / control your screen",
   **Allow / Deny**, and "Don't ask again for this device" (which grants the
   matching `.direct` permission). The prompt times out and denies after
   30 s.
4. **Start:**
   - The host answers `scr.started {session, codec, width, height, monitors,
     control}`.
   - Both sides open the datagram channel.
   - The first frame is a keyframe.
   - If the host has sent nothing 5 s in, it says why (`scr.status`); after
     8 s without a picture the viewer shows a hint and Try Again.
5. **During the session:** `scr.monitor {index}` switches monitor;
   `scr.control {on}` asks to add or drop control, through the same check.
6. **Ending:**
   - Either side sends `scr.stop`.
   - The host stops on the stop hotkey, on disconnect, or when the
     permission is withdrawn mid-session; it re-checks the permission on
     each report.

### On the host while shared

- **A floating, always-on-top bar:** "Sharing your screen with Alice · Control
  on" with **Stop** and **Stop control**, plus a tray/status-bar indicator.
  It never takes focus, so it cannot interrupt what the host is doing.
- **A global stop hotkey**, configurable, default Ctrl+Alt+Shift+S:
  `RegisterHotKey` on Windows, a CGEvent tap on macOS, `XGrabKey` on X11.
- **Activity log entries:** start, stop, control on and off, and denied
  requests.

## UI

- **Viewer window** (existing Screen and Remote Control windows):
  - the video, with Fit / 100%;
  - full screen;
  - a monitor picker;
  - a stats overlay (fps, bitrate, latency, loss, path);
  - a "Request control" toggle;
  - a "Keyboard captured" indicator, released with Right Ctrl
    (configurable);
  - session states: requesting, waiting for consent, denied, busy, live,
    ended; while live, "waiting for the first picture" until one arrives;
  - Try Again after it ends, and a Stats toggle.
- **Host:** the consent prompt, the sharing bar, and the stop hotkey.
- **Settings > Screen sharing:**
  - offer the service;
  - default monitor;
  - max fps (1-165) and quality cap;
  - the stop hotkey;
  - **max viewers** (1-8).
- **Device page and context menu:** the two new `.direct` permissions appear
  with the others, like the sync grants.

## Dependencies and build

- **FFmpeg 7.x, LGPL shared builds:**
  - **Windows:** prebuilt "lgpl-shared" MinGW builds (e.g. BtbN), pinned by
    version and hash in CMake.
  - **macOS:** Homebrew or our own LGPL build.
  - **Linux:** system packages.
- **Linking:** dynamic only, with LGPL notices in the About dialog and the
  package.
- **OpenH264:** Cisco's binary, for the software fallback. Using Cisco's
  binary keeps the H.264 patent licence on Cisco's side.
- **CMake:**
  - `find_package` / pkg-config for libavcodec, libavutil and libswscale.
  - Qt Multimedia (`Qt6::Multimedia`) added.
  - Screen features behind `UCD_WITH_SCREEN` (on by default), so a build
    without FFmpeg still works.
- **Packaging:** the Windows zip script copies the FFmpeg DLLs, the Qt
  Multimedia DLLs and plugins, and the OpenH264 DLL.

## Phases

### Phase 0: foundations
- Install Qt Multimedia and wire FFmpeg into CMake; check that a hello-encode
  and hello-decode work on Windows.
- Session: datagram channel open/close, kind routing, path reporting.
- The `screen.direct` / `control.direct` permissions, and the
  trusted-only rule.
- **Done when:** two instances exchange datagrams on direct, relay and TCP
  fallback paths in a test.

### Phase 1: media transport
- Packetizer, pacing, reassembler, NACK for keyframes, keyframe requests,
  reports, and AIMD bitrate control. All codec-agnostic and fed with
  synthetic "frames".
- **Unit tests with a simulated lossy link:** loss, reorder, duplication,
  delay spikes, and a TCP-like fallback.
- **Done when:** under 5% random loss, frames are delivered or cleanly
  dropped, keyframes recover, and the bitrate settles.

### Phase 2: screen view
- `ScreenSource` (QScreenCapture), `Encoder`, `Decoder`, `VideoSink` in the
  viewer window.
- The request / consent / started / stop flow, the host sharing bar, and the
  stop hotkey. One viewer.
- Monitor switching, viewer-driven `maxWidth`, and the stats overlay.
- **Done when:** a 1080p desktop is viewable at 30 fps on a LAN, with
  under 150 ms glass-to-glass latency measured with a common-clock recording
  (`capture-ms` alone cannot measure cross-device latency), and it
  recovers from induced loss within about a second.

### Phase 3: remote control
- `InputCapture` on the viewer: keyboard capture and release, HID usage
  mapping, and mouse mapping to normalized coordinates.
- `InputInjector` for Windows, macOS and X11, with `in.release` on every way
  a session can end.
- The cursor channel (shape + position) and the local cursor overlay.
- **Done when:** typing, shortcuts, drag and drop, and the wheel work across
  keyboard layouts, and no key or button stays stuck after a disconnect.

### Phase 4: polish and spread
- Several viewers (one encoder shared), and the max-viewers setting.
- Zero-copy GPU paths: D3D11 to NVENC/QSV/AMF, and VideoToolbox.
- Wayland: view through the ScreenCast portal.
- XOR parity if field loss warrants it.
- Camera sharing on the same pipeline (`QCamera` as the source).
- `docs/protocol.md` section 7 finalized (screen, control, datagram kinds).

## Testing

- **Unit:** packetizer/reassembler under simulated loss, HID ↔ native key
  maps, the permission and consent rules, and the session state machine.
- **Codec round trip:** encode synthetic frames, decode, and compare (PSNR
  bound), for each available encoder.
- **Two instances:** using a synthetic `ScreenSource`, since offscreen runs
  cannot capture a real desktop. They exercise the whole flow, including
  consent and the stop paths.
- **Manual, on real desktops:** you run these. Windows ↔ Windows first, then
  macOS and Linux hosts.

## Risks

- **TCP fallback:** if direct UDP cannot be punched, datagrams travel over TCP
  or the relay. Expect higher latency there; we show the path and adapt the
  bitrate.
- **Hardware encoder quirks** differ by vendor and driver. The software
  fallback must always work.
- **macOS permission prompts:** Screen Recording and Accessibility each need
  to be granted once, and the app must guide the user through them.
- **Licensing:** stay strictly LGPL with FFmpeg (no `--enable-gpl` /
  `--enable-nonfree`). Check H.264 decoding patent obligations for
  distribution.
- **CPU path cost at 4K and high frame rates:** the host is zero-copy on
  the native path, but the Qt fallback still goes through the CPU, and the
  viewer copies each decoded frame from the GPU and back. At 165 fps the
  viewer's ~5 ms per 1080p frame is close to the 6 ms budget.

## Open items

Verified on real systems (2026-10-09), each against Windows in both directions:

- **macOS** (Apple M5, macOS 26.6): ScreenCaptureKit / IOSurface /
  VideoToolbox at 60 fps (~5 ms per frame), VideoToolbox decoding (~4 ms),
  direct UDP over a LAN, and remote control through CGEvent after the
  Accessibility prompt. Screen Recording and Accessibility must be granted
  once per build while builds are ad-hoc signed.
- **Linux X11** (Ubuntu 26.04 under WSL, Qt 6.10): Qt capture with NVENC
  (60 fps, ~8 ms per frame) or Cisco OpenH264, decoding, and XTest control
  (on Xvfb). Linked through the relay, as WSL's virtual network stalls
  direct links to Windows.
- **Cisco OpenH264 fallback**: codec round trips and the two-session tests
  pass on Windows, Linux and macOS.

Not yet verified:

- The Wayland portal: WSLg has no screen-cast portal; needs a real Linux
  desktop.
- QSV and AMF: the code exists, but no test machine has Intel or AMD
  encoders.
- Glass-to-glass latency (phase 2's acceptance value): never measured.

Known limits:

- Wayland: view only; no control and no global stop shortcut. Asking for
  control there gives a view-only session with the reason.
- XOR parity: deferred until field loss shows a need.
- Viewer zero-copy: decoded frames go through the CPU on their way to the
  screen (see risks).

## Follow-ups, not planned yet

- Audio (system sound).
- Clipboard sync.
- Dropping files into the remote screen.
- Unattended access, through a system service.
- Mobile viewer: the Flutter app, decoding H.264 through MediaCodec or
  VideoToolbox in a plugin.
- Android as a host: MediaProjection; control only through accessibility
  gestures.
