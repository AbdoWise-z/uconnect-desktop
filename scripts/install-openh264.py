#!/usr/bin/env python3
"""Download the Cisco binary separately to a chosen folder; never bundle it.

Usage: python scripts/install-openh264.py <destination-folder>
Enable its use in Settings > Screen Sharing and enter the resulting path.
(Settings > Screen Sharing > Download from Cisco does the same in the app;
keep the pins in step with src/core/OpenH264Installer.cpp.)
"""
import bz2
import hashlib
import pathlib
import platform
import sys
import urllib.request

machine = platform.machine().lower()
architecture = "arm64" if machine in ("arm64", "aarch64") else "x64" if machine in ("amd64", "x86_64") else machine
# SHA-256 pins of the separate HTTPS Cisco 2.6.0 compressed binaries.
releases = {
    ("win32", "x64"): ("openh264-2.6.0-win64.dll", "dab5f2a872777f9a58b69bfa9fbcf20d9f82f2d6ec91383fd70bff49bd34ac9f"),
    ("win32", "arm64"): ("openh264-2.6.0-win-arm64.dll", "6b57d3ecd06bd80b5f3ee1abbb31d48798ca6d89ef1bc98cf2e6d1ebddf5cf45"),
    ("darwin", "arm64"): ("libopenh264-2.6.0-mac-arm64.dylib", "6db362ee5abdab572311aeadb96d3f44b0617d9a4a4b9f4db4cb5ac4d968da71"),
    ("darwin", "x64"): ("libopenh264-2.6.0-mac-x64.dylib", "38b2ed6d1d45b6a3e408c734173f2d67ab44a10d0e154ff3489b89877cd60e7e"),
    ("linux", "x64"): ("libopenh264-2.6.0-linux64.8.so", "27ab53323c110b76214c1c72222f459d17febbcd1e252136cadc292b0308d75b"),
    ("linux", "arm64"): ("libopenh264-2.6.0-linux-arm64.8.so", "a78aea7970150f46bcd3bb7994c9e6dd90bd7a9ea785920f5a73f6964e3fcda7"),
}
if (sys.platform, architecture) not in releases:
    raise SystemExit("Unsupported platform/architecture: " + sys.platform + "/" + architecture)
name, digest = releases[sys.platform, architecture]
if len(sys.argv) != 2:
    raise SystemExit(__doc__)
folder = pathlib.Path(sys.argv[1]).expanduser().resolve()
data = urllib.request.urlopen("https://ciscobinary.openh264.org/" + name + ".bz2", timeout=30).read(4 * 1024 * 1024)
if hashlib.sha256(data).hexdigest() != digest:
    raise SystemExit("Cisco archive checksum mismatch")
library = bz2.decompress(data)
folder.mkdir(parents=True, exist_ok=True)
target = folder / name
target.write_bytes(library)
(folder / "OpenH264-BINARY-LICENSE.txt").write_bytes(
    (pathlib.Path(__file__).resolve().parent.parent / "docs/OpenH264-BINARY-LICENSE.txt").read_bytes())
print(target)
