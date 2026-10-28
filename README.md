<p align="center">
  <img src="packaging/icons/uconnect-256.png" width="128" alt="uConnect">
</p>

<h1 align="center">uConnect Desktop</h1>

<p align="center">
  Your computers, connected: share files, keep folders in sync, share screens,
  and use another device's screen as an extra monitor, end-to-end encrypted.
</p>

<p align="center">
  <a href="https://github.com/AbdoWise-z/uconnect-desktop/releases">Download</a> ·
  <a href="docs/protocol.md">Protocol</a> ·
  <a href="packaging/BUILDING.md">Packaging</a>
</p>

> **Beta.** uConnect is in public beta: expect rough edges, and keep backups of
> anything you sync.

<p align="center">
  <img src="docs/images/extended-display.gif" width="760"
       alt="A uConnect window on a Windows PC moving onto a MacBook's screen, which the PC is using as a second monitor">
  <br><sub>A MacBook as a second monitor for a Windows PC: a window moves across onto it.</sub>
</p>

## Features

- **Topics and invite links.** Devices meet in a *topic*, shared by an
  invite link, or a QR code a phone scans to open uConnect Mobile on it.
- **Your rules, per device.** Every service is behind a permission, and each
  device gets what its **special rules** (permissions of its own, added after
  checking its fingerprint), this topic's **network rules** (ordered address
  patterns such as `m.m.m.x` for your own subnet, matched against the address
  its direct connection comes from) and your **defaults** grant, together.
  The device's tab marks which of them grants each permission. Mark a
  device's special rules as an **exception** to apply only those, for
  example to give it less than the defaults. A device can copy another's
  network rules for quick setup on a trusted network; the picker shows each
  device's fingerprint, since names can be copied.
- **Storage.** Browse, download and upload files and whole folders in the
  folders a device shares, with read-only shares and a log of what others did.
- **Folder sync.** Keep a folder the same on several devices, phones
  included, with conflicts kept rather than overwritten. A device takes part
  once you grant it **Offer sync** or **Receive sync**, like any other
  permission (special rules, a network rule or the defaults). The Synced
  Folders window shows every file with
  its state (synced, waiting to send or receive, transferring with progress,
  in conflict, ignored, skipped, or held back), with search, a filter and
  a history per file; edits `.syncignore` (every device) and `.syncskip`
  (this device only) in place, with a live preview of which line decides
  each path and a warning about what would stop syncing; shows, per device,
  whether it is online or when it was last seen, what is left to send and
  receive with a time estimate, the file in flight, when it was last fully
  in sync and how much it holds back, and lets you pause one device; lists
  the files syncing set aside in `.synctrash` and restores them; and holds
  back a round of deletes that would remove much of the folder at once
  until you choose. Transfers across folders are listed with their speed,
  and sync problems reach the system tray.
- **Screen, camera and remote control.** Share a screen or a camera, or be
  controlled, each behind a consent prompt on the sharing device unless that
  device's rules allow it without asking, with a sharing bar and a hotkey to
  stop. Watch a phone's screen or camera, and control an Android phone.
  Hardware encoding (NVENC, Quick Sync, AMF, VideoToolbox) where available;
  without it, Settings › Sharing downloads Cisco's OpenH264 for a software
  fallback in one click.
- **Smooth streams, your choice of quality.** The watching side picks the
  resolution, frame rate (up to 120 fps, 60 by default) and bitrate it
  receives, from the view menu or Settings › Watching; the sharing side's
  limits cap them. Video travels the way real-time services send it: paced
  packets with repair packets sized to what the path loses, lost pieces
  asked for again within a round trip, a bandwidth estimate from the
  viewer's feedback every 50 ms that the encoder follows, and a quality
  ladder that steps the picture down on a thin path and back up when it
  recovers, so a phone on mobile data stays within a fraction of a second.
  The viewer plays frames back at the pace they were captured after a
  buffer that follows the network (or one you choose), so the stream
  neither speeds up nor stalls. Every limit, the queue and the threads are
  settings, and the view's statistics show how both sides keep up.
- **Another device's camera as a webcam.** While you watch a camera, the
  webcam button in its window shows it to every app on the computer as
  "uConnect Camera", for video calls (Discord, Teams...) or recording.
  Windows 11 uses a small media source, installed once with administrator
  rights from that button (or Settings › Watching, which also removes it).
  Linux uses a [v4l2loopback](https://github.com/umlaeute/v4l2loopback)
  device (not tested yet).
- **Extended display.** A Windows or macOS computer gains a real extra monitor,
  shown full screen on another device: pick the resolution and refresh rate,
  or match the other screen. Windows uses the uConnect display driver
  ([uconnect-vdd](https://github.com/AbdoWise-z/uconnect-vdd)), installed by
  the app; macOS needs nothing extra.
- **Lives in the tray.** Closing the window keeps uConnect running in the
  system tray (the menu bar on macOS); it can start when you sign in.
- **Searchable settings.** Search finds single settings: matches are listed
  under their page, outlined on it, and a click scrolls to one. Settings that
  several services share have their own pages: **Sharing** (the stop-sharing
  shortcut, video encoding, the sending pipeline) and **Watching** (using
  other devices: the quality to ask for, the playback buffer, the keyboard
  release key, uConnect Camera).
- **Color schemes.** Follow system, Dark, Darker, Light or Colorful.

### Screenshots

<p align="center">
  <img src="docs/images/tour.gif" width="760"
       alt="Opening a phone's device tab, its services and context menu, then the settings pages">
  <br><sub>A phone's device tab, what it lets you do, its menu, and the settings.</sub>
</p>

<table>
  <tr>
    <td width="50%"><img src="docs/images/device.png" alt="A phone with special rules: where it connects from, its fingerprint and what it may do"></td>
    <td width="50%"><img src="docs/images/services.png" alt="What the phone lets this computer do, per service, with buttons to open each"></td>
  </tr>
  <tr>
    <td align="center"><sub>A device: where it connects from, its fingerprint, the rules that apply</sub></td>
    <td align="center"><sub>What it lets you do, service by service</sub></td>
  </tr>
  <tr>
    <td width="50%"><img src="docs/images/settings.png" alt="Settings: default permissions, with the page tree on the left"></td>
    <td width="50%"><img src="docs/images/menu.png" alt="A device's context menu in the Devices list"></td>
  </tr>
  <tr>
    <td align="center"><sub>Default permissions, under Settings › Permissions</sub></td>
    <td align="center"><sub>The Devices list: a shield marks special rules</sub></td>
  </tr>
  <tr>
    <td width="50%"><img src="docs/images/storage.png" alt="Browsing another device's shared folder"></td>
    <td width="50%"><img src="docs/images/extended-display-setup.png" alt="Starting an extended display, and the other device's consent prompt"></td>
  </tr>
  <tr>
    <td align="center"><sub>Browsing a MacBook's shared folder from a Windows PC</sub></td>
    <td align="center"><sub>An extended display, and the prompt the other device sees first</sub></td>
  </tr>
</table>

## How it works

Devices find each other through a **rendezvous server**, which introduces
them and then gets out of the way: they connect directly, punching through
NAT where they can. When their networks will not allow that, the server relays
their traffic. Either way it holds no keys and cannot read or impersonate what
the devices send each other: every connection is authenticated and encrypted
end to end, with keys derived from the topic's invite link, which never reaches
the server.

Networking comes from the [uconnect](https://github.com/AbdoWise-z/uconnect)
library; the server is its `uconnect-rendezvous`.

## Getting started

1. **Download** the package for each computer from the
   [releases](https://github.com/AbdoWise-z/uconnect-desktop/releases) and
   extract it. Each includes everything it needs (Qt, FFmpeg).
2. **Run a rendezvous server** on a machine every device can reach. Each
   package includes it as `uconnect-rendezvous`; `SERVER.txt` beside it says
   how to run it (open its port, 4433 by default, for both TCP and UDP).
3. **Start uConnect** on each device and give it the server's address
   (`host:port`).
4. On one device create a **New Topic** and send its invite link privately to
   the others; they paste it and connect. A phone can scan it instead (Topic ›
   Show Invite QR Code).
5. On each device's tab, compare fingerprints and **add special rules** to
   the devices you own, then choose what they may do. For a home network, a
   network rule (Settings › Permissions › Network Rules) can cover them all
   at once.

The beta builds are not signed yet: on Windows, SmartScreen may ask for
confirmation; on macOS, choose **Open Anyway** under System Settings → Privacy
& Security the first time.

## Platforms

|                          | Windows 10/11 x64 | macOS (Apple silicon) | Linux x86_64 |
|--------------------------|:-----------------:|:---------------------:|:------------:|
| Storage, folder sync     | ✓ | ✓ | ✓ |
| Share a screen           | ✓ | ✓ | ✓ (X11; on Wayland view-only, through the desktop portal) |
| Watch a shared screen    | ✓ | ✓ | ✓ |
| Gain an extended display | ✓ | ✓ | — |
| Be an extended display   | ✓ | ✓ | ✓ |
| A camera as a webcam     | ✓ (Windows 11) | — | v4l2loopback (untested) |
| Tray, start at sign-in   | ✓ | ✓ | ✓ (where the desktop has a tray) |

Phones join the same topics with
[uConnect Mobile](https://github.com/AbdoWise-z/uconnect-mobile) (Android).

The Linux packages need glibc 2.39 or newer (Ubuntu 24.04 or as new). macOS
asks for Screen Recording (and Accessibility, for being controlled) the first
time they are needed.

## Building

Requirements: CMake 4.3+, Ninja, a C++20 compiler, and Qt 6.5+ (Core, Gui,
Widgets, Network, Qml, Quick, QuickControls2, Multimedia). On Windows the
compiler is MinGW (as bundled with CLion or Qt). Screen features need FFmpeg 7
(an LGPL shared build): it is downloaded automatically on Windows; elsewhere
the system's is found through pkg-config, or pass `-DUCD_FFMPEG_ROOT` (or
build one with `scripts/build-ffmpeg-lgpl.sh`). Without it, the app builds
without them. The uconnect library is fetched at a pinned revision.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/Qt/6.8.3/<platform>
cmake --build build
ctest --test-dir build --output-on-failure
```

| Option | Default | |
|---|---|---|
| `UCD_WITH_SCREEN` | `ON` | Screen sharing and extended display (needs FFmpeg) |
| `UCD_BUILD_SERVER` | `ON` | Build `uconnect-rendezvous` alongside the app |
| `UCD_BUILD_TESTS` | `ON` | The test suites |
| `UCD_DEBUG_UI` | `ON` | The Dear ImGui debug window (View → Debug Window) |

Release packages (Windows ZIP, macOS app bundle, Linux AppImage and tar.gz)
are made by the scripts in `scripts/`; [packaging/BUILDING.md](packaging/BUILDING.md)
describes each. The app icons are drawn by `scripts/make-icons.py`.

Useful command-line options: `--profile <name>` runs a separate settings
profile (two instances on one machine), `--minimized` starts in the tray,
`--server`, `--topic` and `--connect` set up a connection, and `--help` lists
the rest.

## Repository

| Path | |
|---|---|
| `src/core/` | Sessions, permissions, storage, sync, screen sharing and extended display (no UI) |
| `src/app/` | The Qt side: controllers exposed to QML, the tray, sign-in start |
| `qml/` | The interface |
| `vcam/` | The uConnect Camera: the Windows media source (MSVC) and the frame channel it shares with the app |
| `tests/` | Qt Test suites, run by `ctest` |
| `packaging/`, `scripts/` | Icons, package READMEs and the packaging scripts |
| `docs/protocol.md` | The app protocol on top of uconnect: messages, permissions, every service |
| `docs/plans/` | Design notes for screen sharing and the extended display |

## Known issues and next up

Where the project stands (updated 2026-10-28):

- **Streaming over mobile data** now holds a lag budget of a second on both
  ends and recovers lost packets without keyframes; a phone on cellular data
  streams at 30 fps/480p on a ~1 Mbit/s uplink and watches a desktop at 60+
  fps. Media travels over UDP only (direct or through the relay); there is no
  TCP fallback for video any more, by design.
- **macOS:** beta 3 ships an Apple-silicon package, ad-hoc signed
  (right-click › Open the first time). VideoToolbox stalls on some sessions
  ("buffered too many frames"); the encoder then falls back to OpenH264,
  which still wants a longer soak. The virtual camera has no macOS
  extension yet.
- **Linux:** beta 3 packages (AppImage and tar.gz) carry the streaming
  rewrite and folder sync; the extended-display and camera features remain
  Windows-first.
- **Next up:** bandwidth probing when the encoder has little to send (so a
  still screen that starts moving finds the path's capacity faster), temporal
  layering so the frame rate can drop without a resize, a Linux end-to-end
  pass, and the mobile host's single-viewer limit.
- Test coverage of the media layer is simulated (`MediaTransportTest`) plus
  the offscreen `StreamBenchTest`; see [docs/screen-testing.md](docs/screen-testing.md).
- **Folder sync UI (2026-10-28):** the file list, rules editor, per-device
  status, kept files, large-delete guard and transfer list are covered by
  `SyncServiceTest` and the offscreen `SyncViewTest`; the conflict view
  compares size and date per device but does not yet show the contents side
  by side (the other device's version is only fetched when you take it).
- **Sync engine cost (2026-10-28):** planning is paced by how long a round
  takes (it used to run again after every finished file, quadratic in the
  folder), a fetch reuses one index of local blocks per change of the index,
  and the state file is written every 30 s while fetches run. The same
  change on the phone (uconnect-mobile #3) also moved the engine off the UI
  isolate; here it already ran on the Session thread.

## License

uConnect Desktop is released under the [MIT License](LICENSE). The
third-party software it uses keeps its own licenses, below.

## Third-party software

uConnect uses [Qt](https://www.qt.io) (LGPL v3), [FFmpeg](https://ffmpeg.org)
(LGPL, dynamically linked and replaceable) and, optionally, Cisco's OpenH264
binary, downloaded from Cisco by the user (Settings › Sharing, or
`scripts/install-openh264.py`). The Windows extended display
driver is a fork of [SudoVDA](https://github.com/SudoMaker/SudoVDA), based on
Microsoft's indirect display driver sample (MS-PL). See
[docs/third-party-screen.md](docs/third-party-screen.md) for the details and
notices.
