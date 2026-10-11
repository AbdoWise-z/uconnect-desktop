# Plan: extended display

Status: **driver fork, virtual monitors and sessions implemented; nothing
verified on a real monitor yet** (2026-10-10). Each phase is implemented,
tested and committed before the next one. Wire details are in
`docs/protocol.md` section 7.6.

Execution notes:

- **Fork:** `github.com/AbdoWise-z/uconnect-vdd`, CI green (x64, ARM64).
  - Builds from the NuGet WDK 10.0.26100.6584 (`packages.config`) with VS
    2022: CI on `windows-2022` with 64-bit MSBuild. WDK 26100 has no build
    tasks for VS 2026; that needs WDK 28000.
  - This machine has VS 2022 Build Tools without the WDK extension, and the
    agent shell is not elevated, so the WDK extension cannot be installed.
    Local builds go through a private copy of VS's `VC\v170` build rules
    with a `WindowsUserModeDriver10.0` toolset added
    (`CLionProjects\vdd-tools`, outside both repositories; `build.sh` there).
  - Phase 1 changes are in (see "Phase 1"): own identity, monitors owned by
    handle (`EvtFileCleanup`), per-handle watchdog, two locks, validation,
    interactive-users-only access, SDR unless the registry says HDR, the
    upstream fall-through and cursor-event leak fixed. The planned
    `IOCTL_SWEEP` was dropped: with every monitor owned by a live handle
    there are no orphans to sweep.
  - `tools/vddctl` installs the driver and runs the acceptance checks
    (`vddctl selftest`); `scripts/dev-install.ps1` trusts the build's test
    certificate and installs it. **Not run yet:** both need an elevated
    shell.
- **App, phases 2-3 in code:**
  - `src/core/display/`: `VirtualDisplay` (Windows driver client; macOS
    `CGVirtualDisplay`, **not compiled yet**: the Mac was unreachable),
    `DriverSetup_win.cpp` (install/remove, run elevated through
    `--display-driver`), `DisplayService` (handshake, mode, monitor
    lifetime). `uconnect-vdd.h` is copied from the fork.
  - `ScreenService` gained `Kind {Screen, Camera, Display}`: display sessions
    use `dsp.*` and datagram kinds 4/5, capture natively only (retried every
    500 ms; error after 15 s without a frame), start at 20 Mbit/s with a cap
    up to 100 Mbit/s, and end when the path is not direct.
  - `Session` routes the handshake to `DisplayService` before the media
    sessions, and opens display datagrams with no fallback.
  - Tests: `DisplayServiceTest` (19 cases, a fake monitor: every ending
    leaves none behind, including ending while the monitor is being made)
    and `ScreenSessionTest::twoRealSessionsExtendedDisplay` (two real
    Sessions, H.264 on kinds 4/5). All 14 suites pass on Windows.
- **App, phase 4 in code:**
  - `DisplayController` (GUI thread): driver state and install/remove
    through the elevated relaunch, local screens, consent, host and display
    states, video sinks, the host's pointer fed at 60 Hz while live, and the
    display device kept awake (Windows `SetThreadExecutionState`, macOS
    `IOPMAssertion`; nothing on Linux yet).
  - QML: `DisplaySetup` (Services > Extended Display > device: mode, start,
    stop, driver banner), `DisplaySafety` (consent with a screen choice, the
    host's "Extending to" bar, opening the views), `DisplayView` (full screen
    on the chosen screen, local pointer hidden, remote pointer drawn, bar on
    mouse move or Esc, hold Esc to stop), Settings > Services > Extended
    Display (bitrate cap, keep awake, driver).
  - The stop hotkey and quitting stop extended displays too.
  - `scripts/package-windows.ps1 -DriverPackage <folder>` ships the driver
    in `drivers/uconnect-vdd`, where the install button looks for it.
  - `SettingsTest` loads the three QML files and checks the settings
    survive a restart. Nothing has been looked at on a real screen yet.
  - Known limit: removing a monitor makes `ScreenController` stop screen
    sharing (its existing `screenRemoved` rule), so ending an extended
    display also ends a screen share running at the same time.
- **Not started:** LAN tuning (phase 5).
- **Verified on this Windows 11 laptop (2026-10-11)**, driver installed with
  `scripts/dev-install.ps1` (the build's test certificate trusted in
  LocalMachine Root and TrustedPublisher):
  - **No test-signing mode needed:** the user-mode driver loads with a locally
    trusted certificate. A release signature then only replaces the trust step.
  - **Windows does not turn the new monitor on** with one screen (the laptop
    panel): it knows the path but leaves it inactive. Extending the desktop
    (`SDC_TOPOLOGY_EXTEND`) turns it on; the app now does that after 1 s.
  - **Capture:** desktop duplication captures the virtual monitor on the RTX
    3050 Ti, and the app's DXGI + NVENC path encodes it at 1920×1080 and,
    after a mode change, 2560×1440 (`VirtualDisplayTest`, opt-in with
    `UCD_TEST_VIRTUAL_DISPLAY=1`).
  - **Removal:** on remove 50 ms, on closing the handle 62 ms, on killing the
    owning process 63 ms, by the watchdog 3052 ms after pings stop. All 20
    `vddctl selftest` checks and all 5 `VirtualDisplayTest` cases pass.
- **macOS (M5, macOS 26.6.2), 2026-10-11:** the macOS backend compiles
  without warnings and all display suites pass there. A real
  `CGVirtualDisplay` is created (also from an SSH session), seen by other
  processes, removed by `destroy()` while the process lives, and gone within
  0.2 s of the owning process being killed. Capturing it needs Screen
  Recording, which a test binary run over SSH does not have, so capture on a
  Mac host is not verified. CoreGraphics in a long-running process can keep a
  stale display list; the test asks `system_profiler` instead.
- **First real session, Windows laptop (host, real driver) → Mac (display),
  same Wi-Fi network, 2026-10-11:**
  - The Mac's screen was matched at 2940×1912, 60 Hz; Windows created
    `\\.\DISPLAY15` at that mode; the path was direct.
  - Host: DXGI / D3D11 NV12 / `h264_nvenc`, 45-59 encodes/s at 8-11 ms,
    18-35 Mbit/s as congestion control moved.
  - Mac: VideoToolbox decoding at 5.7-8 ms a frame, 60 fps shown at first,
    then dips (15-50 fps, one near-stall) while congestion control cut the
    rate and frame rate: loss or delay on the link, not decoding. Tuning
    this is phase 5 (and a wired link should be measured for comparison).
  - Killing the host app: the virtual monitor was gone at once on Windows,
    and the Mac's view ended ("device disconnected").
  - Not checked yet: the pointer on the Mac, consent through the UI (the run
    used a temporary hook granting `display.direct`), glass-to-glass latency.
- **Waiting on you:** looking at the result on the Mac's screen during a
  session (picture, pointer, text sharpness), and a wired run for the
  latency and loss numbers.

## The feature

A device (the **host**) asks a device it trusts (the **display device**) to
act as an extra monitor.

- **On the host**, the operating system sees an ordinary monitor. Windows can
  be dragged onto it, it has a resolution and a refresh rate, and it appears
  in the display settings. Only uConnect drives it.
- **On the display device**, it is an application window showing a video
  stream, like a screen-sharing viewer, normally full screen.
- **The host chooses the screen's properties**: resolution, refresh rate,
  bitrate. By default it matches the display device's own screen.
- **Both devices are close**, on the same network. The session needs a
  direct path and refuses relay and TCP fallback.

Most of the pipeline already exists for screen sharing: native capture of a
monitor by name, GPU encoding, datagram transport with recovery and
congestion control, hardware decoding, the cursor channel, and consent. The
new parts are the virtual monitor itself (a driver on Windows, a system API
on macOS), a handshake started by the sending side, and a receiver window
built for full-screen use.

## Decisions taken

| # | Question | Decision |
|---|---|---|
| 1 | Windows virtual monitor | **Fork SudoVDA** (SudoMaker, MIT/CC0, based on Microsoft's IddSampleDriver) into our own Indirect Display Driver, with its own hardware ID, and sign it ourselves. Bundling VirtualDrivers' driver was rejected: it cannot add or remove monitors at runtime, its monitors outlive the app, and it is a shared, globally configured install. |
| 2 | Network | **Same network, direct path only.** If the datagram path is relay or TCP fallback, the session ends with code `path`. |
| 3 | Who may ask | The host only asks devices **it trusts**. |

### Proposed, to confirm

| # | Question | Proposal |
|---|---|---|
| 4 | Accepting | The display device accepts requests only from **trusted** devices that hold `display.show`. It asks each time unless the device also holds `display.direct`. Like decision 2 of the screen plan, untrusted devices cannot ask at all. So trust is required both ways. |
| 5 | Scope of v1 | **One** extended display per host and one per display device at a time. Several displays per host would need one encoder per display. |
| 6 | Platforms | **Host:** Windows 10/11 x64 and macOS. Linux Wayland hosts come later (phase 6); Linux X11 is not planned; Windows ARM64 depends on signing (see risks). **Display device:** every desktop platform from v1 (it is only a viewer), and the Flutter app later. |
| 7 | Codec | H.264 4:2:0, as for screen sharing. 4:4:4 or HEVC for sharper text is a follow-up. |
| 8 | Input | v1 drives the extended display with the host's own mouse and keyboard. Input from the display device (keyboard, mouse, later touch) is an optional phase 6, enabled per session by the host. |

## Why SudoVDA

Upstream SudoVDA already has the control interface this feature needs
(`Common/Include/sudovda-ioctl.h`):

- `IOCTL_ADD_VIRTUAL_DISPLAY {Width, Height, RefreshRate, MonitorGuid,
  DeviceName[14], SerialNumber[14]}` returns `{AdapterLuid, TargetId}`.
- `IOCTL_REMOVE_VIRTUAL_DISPLAY {MonitorGuid}`.
- `IOCTL_SET_RENDER_ADAPTER {AdapterLuid}`, which picks the GPU that renders
  the monitor.
- `IOCTL_DRIVER_PING`, `IOCTL_GET_WATCHDOG`, `IOCTL_GET_PROTOCOL_VERSION`.
- **A watchdog:** if no IOCTL arrives for 3 s (configurable), it removes
  every virtual monitor.
- **A hardware cursor:** the cursor is not drawn into frames.
- **Modes:** the requested mode is preferred, plus scaled variants at single
  and double refresh.

What it lacks, and the fork adds (phase 1):

- **Its own identity**, so it does not collide with Apollo's SudoVDA. Today
  it uses `root\sudomaker\sudovda` and a fixed interface GUID.
- **Ownership by handle:** a crash should remove the monitor at once, not
  after the watchdog fires.
- **Device interface security.** Upstream has a
  `TODO set proper SECURITY_ATTRIBUTES`.
- **Signed releases.** Upstream publishes none, and it has had no commits
  since April 2025.

User-mode drivers need no Microsoft signature on Windows 10 1607 and later,
so a regular code-signing certificate is enough. VirtualDrivers' driver
loads on x64 with only a SignPath Foundation signature. There is no EV
certificate or Partner Center step.

Two upstream bugs found while forking, both fixed: the add IOCTL passed the
fixed 14-byte name and serial to `strlen` (an unterminated name read past
the buffer), and every swap-chain assignment created a new named event for
the cursor and never closed it.

## Architecture

```
HOST                                                               DISPLAY DEVICE
VirtualDisplay ── creates ──► OS monitor (driver / CGVirtualDisplay)
                                   │ desktop composed by the OS
DesktopEncoder (DXGI / SCK, by monitor id) ─► Packetizer ─► datagrams ─► Reassembler ─► Decoder ─► DisplayView (full screen)
     ▲                                        kind 4                          │
     └──── bitrate / keyframe ◄──────────── dsp.report / nack / keyframe ────┘
CursorSource ──► dsp.cursor (shape) + kind 5 (position) ─────────────────────► cursor overlay
(phase 6) InputInjector ◄──────────────── in.* (reliable) ◄─────────────────── InputCapture
```

### Reuse

- **Capture:** `DesktopEncoder::encode(monitor, ...)` already takes a
  monitor id: the DXGI `DeviceName` on Windows (`DesktopEncoder_win.cpp:66`)
  or the `CGDirectDisplayID` on macOS (`DesktopEncoder_mac.mm:72`). A
  virtual monitor is passed by its id like any other. Qt's `QScreenCapture`
  stays the fallback.
- **Transport:** `PacedSender`, `Reassembler`, `CongestionController`,
  NACKs, keyframe gating and reports are reused unchanged, apart from LAN
  tuning (phase 5).
- **Service:** `ScreenService` becomes a third instance in `Session`, as the
  camera already is, with its own message prefix and datagram kinds.
- **Receiving side:** decoding, the YUV video sink, the cursor overlay, the
  stats and the consent prompt pattern are reused.

### New code

| Where | What |
|---|---|
| fork `uconnect-vdd` (separate repo) | The Windows driver, built with MSVC + WDK in CI, then signed and released. |
| `src/core/display/VirtualDisplay.hpp` | Creates, changes and destroys one virtual monitor, and reports whether this machine can (`Ready`, `NotInstalled`, `Outdated`, `NotSupported`, with a detail string). |
| `src/core/display/VirtualDisplay_win.cpp` | Device interface lookup, the IOCTLs, `QueryDisplayConfig`, `ChangeDisplaySettingsExW`, the watchdog ping. |
| `src/core/display/VirtualDisplay_mac.mm` | `CGVirtualDisplay` (private CoreGraphics API, declared in a local header the way DeskPad does it). |
| `src/core/display/VirtualDisplay.cpp` | `NotSupported` everywhere else. |
| `src/core/display/DriverSetup_win.cpp` | Installs and removes the driver, elevated (phase 2). |
| `src/core/display/KeepAwake.*` | Keeps the display device's screen on during a session. |
| `src/app/DisplayController.*` | QML-facing control for both roles, like `ScreenController`. |
| `qml/DisplayView.qml` | The full-screen receiving window. |
| `qml/DisplaySetup.qml` | The host's request dialog: mode, refresh rate, bitrate. |

`VirtualDisplay` sketch:

```cpp
namespace ucd::display {
struct Mode { int width = 1920, height = 1080, refresh = 60; bool hiDpi = false; };
enum class Availability { Ready, NotInstalled, Outdated, NotSupported };

class VirtualDisplay {
public:
    virtual ~VirtualDisplay() = default;     // destroys the monitor if still there
    // identity: stable per display device (derived from its key), so the OS
    // remembers arrangement and scale for that device.
    virtual bool create(const QByteArray& identity, const QString& name, const Mode& mode, QString* error) = 0;
    virtual bool setMode(const Mode& mode, QString* error) = 0;
    virtual QString captureId() const = 0;   // what DesktopEncoder::encode takes as `monitor`
    virtual void destroy() = 0;
    virtual void keepAlive() = 0;            // Windows: feeds the driver watchdog; no-op elsewhere
};
Availability availability(QString* detail = nullptr);
std::unique_ptr<VirtualDisplay> virtualDisplay();
}
```

### Stable identity

- **Monitor identity:** the monitor's GUID is the first 16 bytes of
  SHA-256(`"uconnect-display v1"` ‖ the display device's public key). Its
  serial is 13 hex digits of the same hash, and its name is the display
  device's name cut to 13 characters (the EDID limit).
- **Result:** Windows and macOS remember position, arrangement and scale per
  display device. Each device comes back where the user last put it.

## The safety invariant

**A virtual monitor never outlives its session.** A leftover monitor hides
windows on a screen nobody can see. That makes this the most important
correctness rule of the feature. Every ending path destroys the monitor
before anything else:

| Path | Who removes it |
|---|---|
| Stop on either side, stop hotkey, permission withdrawn | the host app, at once |
| Peer disconnect; no valid `dsp.report` for 5 s; the path leaves direct | the host app, at once |
| App quits | `VirtualDisplay` destructor, plus an explicit remove on shutdown |
| App crashes or is killed | **Windows:** the driver, when the process's handle closes (fork change). **macOS:** the system, when the `CGVirtualDisplay` object dies with the process. |
| App hangs | **Windows:** the driver watchdog, 3 s without a ping. **macOS:** not covered; the monitor stays until the app ends. |
| Sleep, resume, lock screen | On resume the host re-checks the session; if it ended, it removes the monitor. |
| Next app start | On Windows, a sweep removes any monitor owned by no live handle. |

When a monitor is removed, Windows and macOS move its windows to the
remaining screens.

## Protocol (section 7.6, revision `display 1.0`)

### Hello and permissions

- **Hello:** a new service name in `svc`, `display`: "this device can be
  used as a display". It is offered by the display device.
- **Permissions**, both trusted-only and granted on the display device:

| id | allows |
|---|---|
| `display.show` | ask to use this device as a display |
| `display.direct` | do so without the consent prompt |

The host needs no permission from the display device beyond these. Its own
rule is decision 3: it only asks devices it trusts.

### Messages (reliable, kind 1 JSON)

| type | direction | fields / meaning |
|---|---|---|
| `dsp.request` | host → display | nonzero u32 `request`; `mode`: `"match"` or `{width, height, fps}`; `codecs:["h264"]` |
| `dsp.wait` | display → host | `request`; the display device is asking its user |
| `dsp.err` | either | `request`, `code`: `forbidden`, `busy`, `codec`, `denied`, `path`, `driver`, `ended` |
| `dsp.accept` | display → host | `request`; `screen {width, height, fps, dpi}`, the local screen chosen to show it; `maxWidth`, `maxFps` (decoder limits) |
| `dsp.started` | host → display | `request`, nonzero u16 `session`, `codec:"h264"`, `width`, `height`, `fps`, `cursorEmbedded` |
| `dsp.mode` | host → display | not in version 1 (a new mode is a new session); see follow-ups |
| `dsp.keyframe`, `dsp.nack`, `dsp.report`, `dsp.cursor`, `dsp.pipeline`, `dsp.status`, `dsp.error` | as the `scr.*` messages (7.1, 7.2, 7.5) | same fields, same rules |
| `dsp.stop` | either | `session`; cancelling a pending request carries `request` |

**Datagrams:** kind **4** is video, with the 7.1 header. Kind **5** is cursor
position, with the 7.5 layout.

### Flow

1. **The host asks:** `dsp.request`. This is only possible when the peer
   offers `display`, the host trusts it, and `VirtualDisplay` is `Ready`.
2. **The display device checks** trust, `display.show` and a free slot. If
   not allowed, it answers `dsp.err` `forbidden` or `busy`. Otherwise it
   sends `dsp.wait` and prompts its user:
   - "Alice's Laptop wants to use this screen as a display".
   - If it has several screens, which screen to use.
   - **Allow / Deny**, and "Don't ask again", which grants `display.direct`.
   - The prompt denies after 30 s. The host gives up after 35 s.
3. **Accept:** the display device sends `dsp.accept` with its chosen
   screen's size, refresh rate and DPI. Both sides open the datagram
   channel.
4. **The host resolves the mode:**
   - `"match"` takes the display device's pixels and refresh rate.
   - The result is capped by the host's limits: today the encoder's
     3840-pixel width (`DesktopEncoder_win.cpp:98`), `MaxFps`, and the H.264
     level (see risks).
   - On macOS, a screen above ~180 dpi gets a HiDPI mode.
5. **The host creates the virtual monitor** and waits up to 5 s for it to
   come up. Failure gives `dsp.err` `driver`.
6. **The host starts:**
   - It sends `dsp.started`, and its first frame is a keyframe.
   - If the datagram path is not `direct` within 3 s, or drops out of direct
     later, the host ends the session with `dsp.err` `path`.
7. **During the session:**
   - A different mode is a new session (decided while implementing: the
     encoder, the stream and the receiving window all size from the start;
     `dsp.mode` is a follow-up).
   - The display device letterboxes when its screen and the stream differ.
8. **Ending:** `dsp.stop` from either side, or any path in the safety table.

Older peers ignore `dsp.*` and the `display` service. A host whose request is
ignored times out after 35 s.

## Host details

### Windows

1. **Find the driver:** `SetupDiGetClassDevs` with the fork's interface GUID,
   then `CreateFile`. Check `IOCTL_GET_PROTOCOL_VERSION`; a different major
   version means `Outdated`.
2. **Choose the GPU:** `IOCTL_SET_RENDER_ADAPTER` with the adapter the
   encoder uses (the first NVENC/QSV/AMF adapter in DXGI order). The
   monitor's surfaces then live on that GPU, and the D3D11 → NV12 → NVENC
   path stays zero-copy, which matters on hybrid laptops.
3. **Add the monitor:** `IOCTL_ADD_VIRTUAL_DISPLAY` returns
   `{AdapterLuid, TargetId}`.
4. **Find its name:** poll `QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS)` until a
   path with that target is active, then
   `DisplayConfigGetDeviceInfo(GET_SOURCE_NAME)` gives the GDI name
   (`\\.\DISPLAYn`). That is `captureId()`.
5. **Apply the mode:** `ChangeDisplaySettingsExW` with the exact
   width/height/refresh, if Windows did not pick the preferred mode.
6. **Keep it alive:** `IOCTL_DRIVER_PING` every second from the session
   worker.
7. **Remove it:** `IOCTL_REMOVE_VIRTUAL_DISPLAY`.

Scale (DPI) is left to Windows. It remembers it per monitor serial, so the
user sets it once per display device. The public API cannot set it; an
undocumented `DisplayConfigSetDeviceInfo` type can, and is a follow-up.

### Driver installation (Windows)

- **Packaging:** `scripts/package-windows.ps1` downloads the pinned
  `uconnect-vdd` release (version and SHA-256 in the script, as for FFmpeg)
  into `drivers/uconnect-vdd/`.
- **Settings > Extended display** shows the driver state with an Install,
  Update or Remove button. Clicking it relaunches
  `uconnect_desktop.exe --display-driver install|remove` elevated
  (`ShellExecuteEx` `runas`). In `main.cpp`, that branch runs before
  `QGuiApplication` is created.
- **Install:**
  - Create the root-enumerated device: `SetupDiCreateDeviceInfo`,
    `SPDRP_HARDWAREID = root\uconnect\vdd`,
    `SetupDiCallClassInstaller(DIF_REGISTERDEVICE)`.
  - Then `UpdateDriverForPlugAndPlayDevicesW` with the INF.
  - Both are available in MinGW (`setupapi`, `newdev`).
- **Remove:** `DiUninstallDevice` + `DiUninstallDriver`.
- **Publisher prompt:** until the signing certificate is widely trusted,
  Windows shows "Would you like to install this device software?" once,
  naming the publisher. That is expected; the UI says so.

### macOS

1. **Create the descriptor:** a `CGVirtualDisplayDescriptor` with name,
   `maxPixelsWide/High`, `sizeInMillimeters` (from the display device's DPI),
   vendor/product/serial (from the stable identity), a queue, and a
   `terminationHandler` that ends the session.
2. **Apply settings:** `CGVirtualDisplaySettings` with `hiDPI` and one
   `CGVirtualDisplayMode` (width, height, refresh), via `applySettings:`.
3. **Capture:** `displayID` is `captureId()`. ScreenCaptureKit captures it
   through the existing path, at pixel size (`CGDisplayModeGetPixelWidth`).
4. **Check availability:** `NSClassFromString(@"CGVirtualDisplay")` decides
   `Ready` or `NotSupported`.

There is no driver and no new permission: Screen Recording is already
granted for screen sharing.

## Display device details

- **The window (`DisplayView.qml`):**
  - Frameless and full screen on the chosen screen (`QWindow::setScreen`,
    then `showFullScreen`), black background.
  - The video is shown 1:1 when sizes match, otherwise fitted.
  - The local pointer is hidden over the window, and the remote cursor
    overlay is shown.
- **Controls:**
  - Moving the local mouse, or pressing Esc, shows a thin top bar for 2 s:
    host name, mode, Stats, **Stop**.
  - Holding Esc for 1 s, or the stop hotkey, stops.
  - Closing the window stops.
- **States:** waiting for consent, starting ("waiting for the first
  picture", as screen viewing does), live, connection lost, ended.
- **Keep awake while live:**
  - Windows: `SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED)`.
  - macOS: `IOPMAssertionCreateWithName(kIOPMAssertPreventUserIdleDisplaySleep)`.
  - Linux: `org.freedesktop.ScreenSaver.Inhibit`.
- **Linux display devices:** they work from v1 on X11 and Wayland, since
  they only decode and show.

## Transport tuning for LAN (display sessions only)

- **Bitrate:**
  - Starting bitrate is **20 Mbit/s** on direct (screen sharing starts at 4).
  - The default cap is **40 Mbit/s**, configurable up to 100.
  - `CongestionController::reset` gains a starting bitrate.
  - The batching of hardware encoder bitrate changes stays: NVENC turns each
    change into a keyframe.
- **Pacing:** on a direct path, spread each frame over a quarter of a frame
  interval instead of half. At these bitrates the half-interval spread costs
  up to 8 ms at 60 Hz.
- **Capture timing:** for display sessions, capture when the frame arrives
  (DXGI `AcquireNextFrame` waits for a new frame) instead of on the timer.
  That saves up to a frame interval.
- **Throughput check (phase 0):** 40 Mbit/s is about 4,500 datagrams/s, and
  100 Mbit/s about 11,000. The uconnect datagram path's ceiling and CPU cost
  at those rates are unmeasured.
- **Targets (phase 5):**
  - 1440p60 and 1080p120 sustained, without frame drops, on a wired or
    5 GHz LAN.
  - **≤ 50 ms glass to glass** at 60 Hz, measured with a common-clock
    recording.

## UI

- **Host:**
  - The device menu gets **Use as display…** for trusted peers offering
    `display`. It is disabled with a reason when the driver is missing or
    outdated, or the platform is unsupported.
  - The dialog offers a mode (Match that screen / common resolutions /
    custom), a refresh rate and a bitrate cap.
  - During a session, a small status window shows state, mode (changeable),
    stats and **Stop**.
  - The sharing bar and the status bar show "Extending to iPad · Stop".
  - The existing stop hotkey ends display sessions too.
  - The activity log records start, stop, mode changes and refusals.
- **Display device:** the consent prompt, with the screen choice, and
  `DisplayView`.
- **Settings > Extended display**, both roles:
  - **Use this device as a display for others:** offers `display` in hello.
  - **Screen to use:** ask, the screen the app is on, or a named screen.
  - **Keep the screen awake:** on by default.
  - **Driver** (Windows only): state, version, Install / Update / Remove.
  - **Default mode** (Match / custom) and **bitrate cap**.
- **Device page:** `display.show` and `display.direct` appear automatically
  from the permission table.

## Phases

### Phase 0: verification spikes (2–3 days)

Nothing here is product code. Each result is recorded in this plan before
phase 1 starts. A failure revises the design.

- **Windows,** using upstream SudoVDA built with MSVC + WDK x64 and signed
  with a self-signed certificate trusted on the test machine:
  - **Load:** does it install and load without test-signing mode?
    (Expected for a user-mode driver. Otherwise use test mode on a test
    machine only, never on the dev machine.)
  - **Capture:** a small program adds a 2560×1440@60 monitor. Do
    `DesktopEncoder_win` and NVENC capture it zero-copy, with
    `UCD_SCREEN_STATS=1` numbers?
  - **Cursor:** with the driver's hardware cursor, does `NativeCursor`
    report positions on that monitor, and do frames come without the
    cursor?
  - **Crash:** kill the controlling process. When does the monitor go (the
    upstream watchdog should take 3 s)?
- **macOS (the M5 test Mac):** a spike that creates a 2560×1600 HiDPI
  `CGVirtualDisplay`, finds it in `SCShareableContent`, encodes it through
  `DesktopEncoder_mac`, and checks that it disappears when the process
  exits.
- **Throughput:** uconnect datagrams Windows ↔ Mac on the LAN at 20/40/80
  Mbit/s. Record delivery rate, loss and CPU per side.
- **Done when:** all of the above are recorded.

### Phase 1: the driver fork (about 1 week)

Repository `AbdoWise-z/uconnect-vdd`, forked from `SudoMaker/SudoVDA`, MIT
notices kept.

1. **Identity:**
   - Hardware ID `root\uconnect\vdd`, a new interface GUID, provider
     "uConnect" in the INF, and its own registry key.
   - It installs alongside Apollo's SudoVDA.
2. **Ownership:**
   - Each monitor belongs to the file handle that added it.
   - `EvtFileCleanup` removes that handle's monitors, so a crash removes them
     at once.
   - The ping watchdog becomes per handle and stays as the backstop for a
     hung app.
3. **Access:**
   - The device interface gets an SDDL allowing SYSTEM, Administrators and
     interactive users only.
   - `IOCTL_REMOVE` only removes the caller's own monitors.
   - A separate `IOCTL_SWEEP` removes monitors with no live owner, for the
     startup sweep.
4. **Validation:**
   - Width 640–7680, height 480–4320, refresh 24–240 Hz.
   - Duplicate GUIDs are refused, and monitors per handle are bounded.
5. **Cursor:** raise the hardware cursor limit from upstream's 64×64
   (marked TODO), or verify that large and scaled cursors still work.
6. **Versioning:** our own protocol version, starting at 1.0. The app refuses
   a different major version.
7. **Build and sign:**
   - GitHub Actions (windows-latest, VS 2022, WDK from NuGet), x64 and
     ARM64.
   - The catalog is signed through **SignPath Foundation** (free for
     open-source projects). Apply early; until accepted, use an OV
     code-signing certificate or a self-signed one for testing.
   - Releases ship `uconnect-vdd-<version>-<arch>.zip` (INF, DLL, CAT) plus
     SHA-256.
   - The driver is built outside this repository's MinGW toolchain.

**Done when:**
- A signed release installs on a clean Windows 11 x64 VM without test mode.
- Monitors can be added and removed through the IOCTLs.
- Killing the owner removes them within 1 s.
- Suspending the owner removes them within 3 s.

### Phase 2: virtual monitors in the app (about 1 week)

- `VirtualDisplay` with its Windows, macOS and stub backends, as described
  above.
- Driver install and removal through the elevated `--display-driver` mode,
  plus packaging.
- The startup sweep on Windows.
- CMake: `UCD_WITH_DISPLAY` (on by default, requires `UCD_WITH_SCREEN`).
- **Test `VirtualDisplayTest`**, run only with `UCD_TEST_VIRTUAL_DISPLAY=1`
  (like `UCD_TEST_DESKTOP_CAPTURE`):
  - create a monitor, find it, encode one real frame through
    `DesktopEncoder`, change its mode, destroy it, check it is gone;
  - start a child process that creates one and is killed, then check the
    monitor is gone within 1 s.

**Done when:** the test passes on Windows with the driver installed and on
the Mac.

### Phase 3: protocol and sessions (1–1.5 weeks)

- **ScreenService:**
  - Its `bool camera` becomes `enum class Kind {Screen, Camera, Display}`,
    which drives the message prefix (`scr.` / `cam.` / `dsp.`), the datagram
    kinds (1-2 / 3 / 4-5) and the handshake.
  - `Session` creates the third instance and routes kinds 4 and 5 to it.
- **Reversed handshake:**
  - The sending side gets `offerDisplay(peer, mode)`.
  - The receiving side gets the consent handling for `dsp.request`.
  - On `dsp.accept`, the sender calls a `prepareSource` callback, which
    creates the `VirtualDisplay`, and only then starts hosting.
- **Sources:**
  - The host's source becomes a source id per service, instead of an index
    into `settings.monitors`.
  - Screen sharing keeps using the monitor list; display sessions use
    `captureId()`.
- **Rules:**
  - One display session per side, the direct-path rule, the 5 s report
    timeout.
  - The safety invariant, with every ending path going through one
    `endDisplay()` that destroys the monitor first.
- **Permissions and hello:**
  - `perm::DisplayShow` and `perm::DisplayDirect` (trusted-only) in
    `Permissions`.
  - `ServiceDisplay` in `Hello`.
- **Protocol doc:** `docs/protocol.md` section 7.6, the revision line, and
  the change table.
- **Tests** (two real sessions, like `ScreenSessionTest`, with a fake
  `VirtualDisplay` and a synthetic source):
  - consent allow, deny and timeout; busy; untrusted and missing-permission
    refusals; the `display.direct` grant;
  - path refusal; stop from either side; disconnect; report timeout;
    shutdown;
  - **in every case, the fake monitor is created at most once and destroyed
    exactly once.**

**Done when:** all suites pass, and a real session runs end to end between
two instances on one Windows machine (`--profile`) with the real driver.

### Phase 4: UI (about 1 week)

- **Host:** the menu entry, `DisplaySetup.qml`, the status window, the
  sharing-bar entry, the stop hotkey, settings, and the driver
  install/update/remove flow.
- **Display device:** the consent prompt with screen choice, `DisplayView`,
  `KeepAwake`, settings.

**Done when:**
- QML loads in the offscreen tests.
- Manual: Windows → Mac and Mac → Windows sessions, with matched and custom
  modes, mode changes during a session, and every stop path.

### Phase 5: LAN tuning and latency (about 1 week)

- Starting bitrate and cap, the shorter pacing spread, capture on frame
  arrival.
- Glass-to-glass measurement: one camera filming both the host's main screen
  and the display device, with a millisecond clock spanning the two.
- Text legibility check at the default bitrate: an IDE, a terminal, and a
  web page with small text.

**Done when:** the targets above are met, or the measured numbers are
recorded here with the reason they are not.

### Phase 6 (optional): input from the display device, Linux hosts

- **Input:**
  - The display device sends `in.*` (with its `dsp` session) while the host
    allows it. That is a per-session toggle on the host, off by default.
  - Coordinates are normalized to the virtual monitor.
  - `Session` routes `in.*` by session token, since screen and display
    sessions can run at once.
  - Touch input waits for the Flutter app.
- **Linux Wayland host:** the ScreenCast portal's `VIRTUAL` source type
  through our own portal/PipeWire client, since Qt's `QScreenCapture` cannot
  ask for a virtual source.
  - KDE Plasma 6 first.
  - For GNOME, check whether its portal offers `VIRTUAL`; if not, use
    Mutter's `RecordVirtual` D-Bus API.
  - X11 is not planned (it would need the `evdi` kernel module).

**Total for Windows and macOS hosts (phases 0–5):** about 5–6 weeks, plus the
calendar time for the signing certificate.

## Testing

- **Unit:** mode resolution (match, caps, HiDPI), the stable identity
  derivation, and the `dsp.*` parsing and state machine.
- **Two instances:** the phase 3 suite, with a fake monitor and a synthetic
  source.
- **Real monitors** (opt-in by environment variable): `VirtualDisplayTest` on
  Windows and macOS.
- **Driver:** the phase 1 acceptance checks on a clean VM, repeated for each
  driver release.
- **Manual:**
  - Windows ↔ Mac on the LAN, a Linux display device, and a hybrid-GPU
    laptop as host.
  - Sleep and resume, the lock screen, and a UAC prompt while extended.

## Risks

- **Driver signing and trust:**
  - SignPath Foundation acceptance takes time, and an OV certificate costs
    money.
  - Until the certificate builds reputation, users see a publisher prompt
    once.
  - ARM64 Windows may require more than x64 does (VirtualDrivers needs test
    signing there), so ARM64 hosts are unconfirmed.
- **macOS private API:** `CGVirtualDisplay` can change or break in a macOS
  release, and it rules out the Mac App Store. Mitigation: the runtime
  availability check, and testing on each new macOS version.
- **Text sharpness:** H.264 4:2:0 blurs colored text and fine lines, which
  matters more for a monitor than for screen sharing. Mitigations: higher
  bitrates on LAN, then 4:4:4 or HEVC (follow-up).
- **H.264 levels:** 4K at 60 Hz fits level 5.2. 4K above 60 Hz does not, and
  the encoder's width cap is 3840. Higher modes need HEVC or a level-6
  check.
- **Datagram throughput:** the uconnect datagram path has not been run at
  tens of Mbit/s; phase 0 measures it.
- **NVENC session limits:** a display session plus screen sharing plus camera
  each hold an encoder. Consumer NVIDIA drivers cap concurrent sessions.
- **Hybrid GPUs:** if the monitor renders on a different GPU than the
  encoder, frames cross adapters. `IOCTL_SET_RENDER_ADAPTER` avoids that, but
  only if set before the monitor is added.
- **HDR:** if the user turns on HDR for the virtual monitor, capture gets FP16
  frames the pipeline does not handle. v1 shows SDR only; phase 0 checks what
  happens.
- **Toolchain:** the driver needs MSVC + WDK, which this machine's agent shell
  cannot configure (no `rc.exe` environment). It is built in GitHub Actions
  or a VS developer prompt, never in this repository's CMake.

## Licensing

- **The fork:** SudoVDA's own changes are MIT or CC0 at the user's choice,
  but its base, Microsoft's indirect display sample, is **MS-PL** (not MIT, as
  first written here). MS-PL allows shipping the driver binary next to the
  app under any compatible license; the driver's source must stay MS-PL. The
  fork's `LICENSE` lists the three layers; uConnect's changes are MIT.
- **Notices:** `docs/third-party-screen.md` gains an entry for the bundled
  driver.
- **macOS:** `CGVirtualDisplay` is a system API and adds no third-party code.

## Follow-ups, not planned yet

- Several extended displays per host, with one encoder per display.
- 4:4:4 H.264 or HEVC for text sharpness; HEVC also for 4K above 60 Hz.
- Setting Windows scale (DPI) from the host, through the undocumented
  display-config call.
- HDR virtual monitors.
- The Flutter app as a display device: tablets are the obvious screens, with
  touch as input.
- Audio routed to the display device.
- Linux X11 hosts, through `evdi`.
