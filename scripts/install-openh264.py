#!/usr/bin/env python3
"""Download the Cisco binary separately to a chosen folder; never bundle it.

Usage: python scripts/install-openh264.py <destination-folder>
Enable its use in Settings > Screen Sharing and enter the resulting path.
"""
import bz2
import hashlib
import pathlib
import platform
import sys
import urllib.request

machine = platform.machine().lower()
if sys.platform == "win32" and machine in ("amd64", "x86_64"):
    name = "openh264-2.6.0-win64.dll"
    digest = "dab5f2a872777f9a58b69bfa9fbcf20d9f82f2d6ec91383fd70bff49bd34ac9f"
else:
    raise SystemExit("This installer currently supports Windows x64. See Cisco's v2.6.0 releases for other platforms.")
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
