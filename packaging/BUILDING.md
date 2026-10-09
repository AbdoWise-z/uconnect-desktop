Build Windows and macOS test packages from the same source revision.
The uconnect dependency is pinned to the revision used for the upload tests.

Windows requires CMake 4.3+, Ninja, MinGW and Qt 6.5+ for MinGW.
Configure a Release build with the compiler and Qt paths appropriate for your machine:

```powershell
cmake -S . -B cmake-build-pkg -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/mingw_64
cmake --build cmake-build-pkg --target uconnect_desktop IdentityTest StorageServiceTest StorageViewTest SettingsTest --parallel
ctest --test-dir cmake-build-pkg --output-on-failure
powershell -ExecutionPolicy Bypass -File scripts/package-windows.ps1 -Label test1
```

macOS requires Xcode Command Line Tools, CMake 4.3+, Ninja and Qt 6.5+ for macOS.
With Homebrew installed, dependencies can be installed with `brew install cmake ninja qt`.
Then run:

```bash
bash scripts/build-macos.sh "$(brew --prefix qt)"
```

Alternatively pass the Qt installer prefix, such as `~/Qt/6.8.3/macos`.
The script builds and tests the app, bundles Qt/QML with macdeployqt, verifies
the ad-hoc signature, and writes a ZIP and SHA-256 checksum under `dist/`.
It builds for the Mac running the script: Apple Silicon (`arm64`) or Intel (`x86_64`).
This source package is not a macOS executable; the script must run on a Mac.
The build fetches pinned dependencies from GitHub, so the first build needs internet access.

Linux (x86_64) packages are built on the oldest distribution they should run
on, since glibc is the one library not bundled; build on Ubuntu 24.04 for
24.04 and newer. With Qt 6.5+ (e.g. aqtinstall's `linux_gcc_64`) and a shared
LGPL FFmpeg SDK (BtbN's `linux64-lgpl-shared` or `scripts/build-ffmpeg-lgpl.sh`):

```bash
bash scripts/package-linux.sh ~/Qt/6.8.3/gcc_64 /path/to/ffmpeg-lgpl test1
```

It bundles the app, Qt (X11 and Wayland platforms), its QML modules, FFmpeg
and their libraries with linuxdeploy, and writes a `.tar.gz` (run `AppRun` in
the extracted folder), an AppImage and SHA-256 checksums under `dist/`.
