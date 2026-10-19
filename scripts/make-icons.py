#!/usr/bin/env python3
"""Draws the uConnect icon and writes every platform's file from one design.

The mark is the name: a bold "u" whose two arm tips are round nodes, two
devices joined by the u, on the app's accent blue (Theme.accent #3574F0).

    python scripts/make-icons.py            # writes packaging/icons/

Outputs: uconnect.svg (the master), uconnect-<n>.png (16-1024),
uconnect.ico (Windows, 16-256) and uconnect.icns (macOS, with the inset
macOS icons use). Needs Pillow.
"""
import os
import sys

from PIL import Image, ImageChops, ImageDraw

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "packaging", "icons")

# Geometry on a 1024 grid.
SIZE = 1024
CORNER = 228                    # the rounded square's corner radius
TOP, BOTTOM = "#4C8BF7", "#2556C4"  # background gradient, top-left to bottom-right
INK = "#FFFFFF"
LEFT_X, RIGHT_X = 352, 672      # the u's arm centres
ARM_TOP = 360                   # where the arms start (the nodes' centres)
BOWL_Y = 600                    # the bowl's centre: arms run straight down to it
STROKE = 112                    # the u's stroke
NODE_R = 92                     # the nodes' radius
HOLE_R = 40                     # the nodes' inner dot, in the background colour
HOLE = "#3A73E8"


def svg():
    r = (RIGHT_X - LEFT_X) / 2
    cx = (LEFT_X + RIGHT_X) / 2
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {SIZE} {SIZE}">
  <!-- uConnect: a "u" whose arm tips are two connected devices. -->
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="{TOP}"/>
      <stop offset="1" stop-color="{BOTTOM}"/>
    </linearGradient>
  </defs>
  <rect width="{SIZE}" height="{SIZE}" rx="{CORNER}" fill="url(#bg)"/>
  <path d="M {LEFT_X} {ARM_TOP} V {BOWL_Y} A {r} {r} 0 0 0 {RIGHT_X} {BOWL_Y} V {ARM_TOP}"
        fill="none" stroke="{INK}" stroke-width="{STROKE}" stroke-linecap="round"/>
  <circle cx="{LEFT_X}" cy="{ARM_TOP}" r="{NODE_R}" fill="{INK}"/>
  <circle cx="{RIGHT_X}" cy="{ARM_TOP}" r="{NODE_R}" fill="{INK}"/>
  <circle cx="{LEFT_X}" cy="{ARM_TOP}" r="{HOLE_R}" fill="{HOLE}"/>
  <circle cx="{RIGHT_X}" cy="{ARM_TOP}" r="{HOLE_R}" fill="{HOLE}"/>
</svg>
"""


def rgb(color):
    return tuple(int(color[i:i + 2], 16) for i in (1, 3, 5))


def render(size, inset=0.0):
    """The icon at `size` pixels, drawn 4x larger and scaled down for clean edges.
    `inset` shrinks it inside a transparent margin (macOS icons)."""
    k = 4
    big = SIZE * k
    s = lambda v: int(round(v * k))
    # Background: a diagonal gradient, masked by the rounded square.
    top, bottom = rgb(TOP), rgb(BOTTOM)
    # (x + y) / 2: the mean of a vertical and a horizontal ramp.
    vertical = Image.linear_gradient("L").resize((big, big))
    diagonal = ImageChops.add(vertical, vertical.transpose(Image.Transpose.ROTATE_90).transpose(Image.Transpose.FLIP_TOP_BOTTOM), scale=2.0)
    background = Image.composite(Image.new("RGB", (big, big), bottom), Image.new("RGB", (big, big), top), diagonal)
    mask = Image.new("L", (big, big), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, big - 1, big - 1], radius=s(CORNER), fill=255)
    image = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    image.paste(background, (0, 0), mask)
    draw = ImageDraw.Draw(image)
    ink = rgb(INK)
    half = STROKE / 2
    # The u: two arms, the bowl (a half ring), round end caps.
    for x in (LEFT_X, RIGHT_X):
        draw.rectangle([s(x - half), s(ARM_TOP), s(x + half), s(BOWL_Y)], fill=ink)
    r = (RIGHT_X - LEFT_X) / 2
    cx = (LEFT_X + RIGHT_X) / 2
    draw.arc([s(cx - r - half), s(BOWL_Y - r - half), s(cx + r + half), s(BOWL_Y + r + half)],
             start=0, end=180, fill=ink, width=s(STROKE))
    # The nodes, each with a dot of the background colour.
    for x in (LEFT_X, RIGHT_X):
        draw.ellipse([s(x - NODE_R), s(ARM_TOP - NODE_R), s(x + NODE_R), s(ARM_TOP + NODE_R)], fill=ink)
        draw.ellipse([s(x - HOLE_R), s(ARM_TOP - HOLE_R), s(x + HOLE_R), s(ARM_TOP + HOLE_R)], fill=rgb(HOLE))
    inner = int(round(size * (1 - 2 * inset)))
    icon = image.resize((inner, inner), Image.LANCZOS)
    if not inset:
        return icon
    framed = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    framed.paste(icon, ((size - inner) // 2, (size - inner) // 2), icon)
    return framed


def main():
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else OUT)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "uconnect.svg"), "w", encoding="utf-8") as f:
        f.write(svg())
    pngs = {}
    for size in (16, 24, 32, 48, 64, 128, 256, 512, 1024):
        pngs[size] = render(size)
        pngs[size].save(os.path.join(out, f"uconnect-{size}.png"))
    # Windows: every size up to 256 in one file.
    pngs[256].save(os.path.join(out, "uconnect.ico"), sizes=[(n, n) for n in (16, 24, 32, 48, 64, 128, 256)])
    # macOS: icons sit inside a margin of about 10% (Apple's icon grid).
    render(1024, inset=0.1).save(os.path.join(out, "uconnect.icns"))
    print("wrote", out)


if __name__ == "__main__":
    main()
