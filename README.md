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

- **Devices you trust.** Devices meet in a *topic*, shared by an invite link.
  Each verifies the others' fingerprints, then decides exactly what they may
  do: every service is behind a permission.
- **Storage.** Browse, download and upload files and whole folders in the
  folders a device shares, with read-only shares and a log of what others did.
- **Folder sync.** Keep a folder the same on several devices, with conflicts
  kept rather than overwritten.
- **Screen sharing and remote control.** Each behind a consent prompt on the
  sharing device, with a sharing bar and a hotkey to stop. Hardware
  encoding (NVENC, VideoToolbox) where available.
- **Extended display.** A Windows or macOS computer gains a real extra monitor,
  shown full screen on another device: pick the resolution and refresh rate,
  or match the other screen. Windows uses the uConnect display driver
  ([uconnect-vdd](https://github.com/AbdoWise-z/uconnect-vdd)), installed by
  the app; macOS needs nothing extra.
- **Lives in the tray.** Closing the window keeps uConnect running in the
  system tray (the menu bar on macOS); it can start when you sign in.

Camera sharing is in progress and not finished in this version.

### Screenshots

<table>
  <tr>
    <td width="50%"><img src="docs/images/devices.png" alt="A trusted device, its connection and the permissions it has"></td>
    <td width="50%"><img src="docs/images/storage.png" alt="Browsing another device's shared folder"></td>
  </tr>
  <tr>
    <td align="center"><sub>A trusted device: its connection, fingerprint and what it may do</sub></td>
    <td align="center"><sub>Browsing a MacBook's shared folder from a Windows PC</sub></td>
  </tr>
  <tr>
    <td width="50%"><img src="docs/images/extended-display-setup.png" alt="Starting an extended display, and the other device's consent prompt"></td>
    <td width="50%"><img src="docs/images/settings.png" alt="Settings for the extended display service"></td>
  </tr>
  <tr>
    <td align="center"><sub>An extended display, running, and the prompt the other device sees first</sub></td>
    <td align="center"><sub>Every service is opt-in, under Settings</sub></td>
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
   the others; they paste it and connect.
5. **Trust** each other's devices after checking the fingerprints, then grant
   the permissions you want under each device.

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
| Tray, start at sign-in   | ✓ | ✓ | ✓ (where the desktop has a tray) |

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
| `tests/` | Qt Test suites, run by `ctest` |
| `packaging/`, `scripts/` | Icons, package READMEs and the packaging scripts |
| `docs/protocol.md` | The app protocol on top of uconnect: messages, permissions, every service |
| `docs/plans/` | Design notes for screen sharing and the extended display |

## License

uConnect Desktop is released under the [MIT License](LICENSE). The
third-party software it uses keeps its own licenses, below.

## Third-party software

uConnect uses [Qt](https://www.qt.io) (LGPL v3), [FFmpeg](https://ffmpeg.org)
(LGPL, dynamically linked and replaceable) and, optionally, Cisco's OpenH264
binary, downloaded separately by the user. The Windows extended display
driver is a fork of [SudoVDA](https://github.com/SudoMaker/SudoVDA), based on
Microsoft's indirect display driver sample (MS-PL). See
[docs/third-party-screen.md](docs/third-party-screen.md) for the details and
notices.
