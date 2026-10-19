#!/usr/bin/env python3
"""Draws the uConnect icon and writes every platform's file from one design.

The mark is the name: a bold "u" whose two arm tips are round nodes, two
devices joined by the u. Behind it a blue "C" band echoes the u: its back
crosses the top-left corner, its arms run toward the bottom-right, and it is
mirror-symmetric across that diagonal.

    python scripts/make-icons.py            # writes packaging/icons/

Outputs: uconnect.svg (the master), uconnect-<n>.png (16-1024),
uconnect.ico (Windows, 16-256), uconnect.icns (macOS, with the inset
macOS icons use) and tray-<black|white>-<n>.png (the tray icon: the u
alone, for light and dark taskbars; macOS tints the black one itself).
Needs Pillow.
"""
import math
import os
import sys

from PIL import Image, ImageChops, ImageDraw

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "packaging", "icons")

# Geometry on a 1024 grid.
SIZE = 1024
CORNER = 228                        # the rounded square's corner radius
TOP, BOTTOM = "#FFFFFF", "#FFFFFF"  # background gradient, top-left to bottom-right
C_TOP, C_BOTTOM = "#5B9BFF", "#2F5FD8"  # the C band: the same direction, blue
INK = "#111111"
LEFT_X, RIGHT_X = 352, 672          # the u's arm centres
ARM_TOP = 360                       # where the arms start (the nodes' centres)
BOWL_Y = 600                        # the bowl's centre: arms run straight down to it
STROKE = 112                        # the u's stroke
NODE_R = 92                         # the nodes' radius
HOLE_R = 40                         # the nodes' holes, showing what is behind

# The C band, measured along the top-left to bottom-right diagonal (u, from
# the top-left corner) and across it (v, from the diagonal): its back spans
# u 364-511 within |v| <= 365, and its arms run on with 215 <= |v| <= 365.
# The arms end square where their outer edges meet the tile's sides
# (u = SIZE * sqrt(2) - 365), so the C just touches the edge at its four
# outer corners; it is then scaled about the centre to leave C_PADDING of
# the icon free on every side.
C_BACK = (364, 511)
C_ARMS = (215, 365)
ARM_END = SIZE * math.sqrt(2) - C_ARMS[1]
C_PADDING = 0.04

# A margin around the whole icon, so it sits with others at their size;
# macOS icons use Apple's grid instead (about 10%).
PADDING = 0.05
MAC_INSET = 0.1
# Inside the tile, the C and the u are scaled about its centre to leave this
# much more room to its edge.
INNER_PADDING = 0.08
INNER = 1 - 2 * INNER_PADDING
# The tray icon: the u's height, as a share of the icon's.
TRAY_FILL = 0.9


def c_band():
    """The C's outline as (x, y) points on the 1024 grid."""
    (b0, b1), (inner, outer) = C_BACK, C_ARMS
    uv = [(ARM_END, -outer), (b0, -outer), (b0, outer), (ARM_END, outer),
          (ARM_END, inner), (b1, inner), (b1, -inner), (ARM_END, -inner)]
    r = math.sqrt(0.5)
    scale, centre = (1 - 2 * C_PADDING) * INNER, SIZE / 2
    return [(centre + ((u - v) * r - centre) * scale, centre + ((u + v) * r - centre) * scale) for u, v in uv]


def svg():
    r = (RIGHT_X - LEFT_X) / 2
    ring = (NODE_R + HOLE_R) / 2
    band = " ".join(f"{x:.1f},{y:.1f}" for x, y in c_band())
    nodes = "\n".join(
        f'  <circle cx="{x}" cy="{ARM_TOP}" r="{ring}" fill="none" stroke="{INK}" stroke-width="{NODE_R - HOLE_R}"/>'
        for x in (LEFT_X, RIGHT_X))
    pad = SIZE * PADDING
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="{-pad:g} {-pad:g} {SIZE + 2 * pad:g} {SIZE + 2 * pad:g}">
  <!-- uConnect: a "u" whose arm tips are two connected devices, before a
       blue C that echoes it across the diagonal. -->
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="{TOP}"/>
      <stop offset="1" stop-color="{BOTTOM}"/>
    </linearGradient>
    <linearGradient id="band" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="{C_TOP}"/>
      <stop offset="1" stop-color="{C_BOTTOM}"/>
    </linearGradient>
    <clipPath id="tile"><rect width="{SIZE}" height="{SIZE}" rx="{CORNER}"/></clipPath>
  </defs>
  <g clip-path="url(#tile)">
    <rect width="{SIZE}" height="{SIZE}" fill="url(#bg)"/>
    <polygon points="{band}" fill="url(#band)"/>
  </g>
  <g transform="translate({SIZE / 2:g} {SIZE / 2:g}) scale({INNER:g}) translate({-SIZE / 2:g} {-SIZE / 2:g})">
  <path d="M {LEFT_X} {ARM_TOP + ring} V {BOWL_Y} A {r} {r} 0 0 0 {RIGHT_X} {BOWL_Y} V {ARM_TOP + ring}"
        fill="none" stroke="{INK}" stroke-width="{STROKE}"/>
{nodes}
  </g>
</svg>
"""


def rgb(color):
    return tuple(int(color[i:i + 2], 16) for i in (1, 3, 5))


def gradient(big, start, end):
    """A top-left to bottom-right gradient: (x + y) / 2 of two ramps."""
    vertical = Image.linear_gradient("L").resize((big, big))
    horizontal = vertical.transpose(Image.Transpose.ROTATE_90).transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    diagonal = ImageChops.add(vertical, horizontal, scale=2.0)
    return Image.composite(Image.new("RGB", (big, big), rgb(end)), Image.new("RGB", (big, big), rgb(start)), diagonal)


def mark(k):
    """The u as a mask on the 1024 grid drawn k times larger, holes cut out,
    scaled by INNER about the centre."""
    big = SIZE * k
    s = lambda v: int(round((SIZE / 2 + (v - SIZE / 2) * INNER) * k))  # a position
    w = lambda v: int(round(v * INNER * k))                              # a length
    ink = Image.new("L", (big, big), 0)
    draw = ImageDraw.Draw(ink)
    half = STROKE / 2
    for x in (LEFT_X, RIGHT_X):
        draw.rectangle([s(x - half), s(ARM_TOP), s(x + half), s(BOWL_Y)], fill=255)
    r = (RIGHT_X - LEFT_X) / 2
    cx = (LEFT_X + RIGHT_X) / 2
    draw.arc([s(cx - r - half), s(BOWL_Y - r - half), s(cx + r + half), s(BOWL_Y + r + half)],
             start=0, end=180, fill=255, width=w(STROKE))
    for x in (LEFT_X, RIGHT_X):
        draw.ellipse([s(x - NODE_R), s(ARM_TOP - NODE_R), s(x + NODE_R), s(ARM_TOP + NODE_R)], fill=255)
        draw.ellipse([s(x - HOLE_R), s(ARM_TOP - HOLE_R), s(x + HOLE_R), s(ARM_TOP + HOLE_R)], fill=0)
    return ink


def tray(size, color):
    """The tray icon: the u alone in `color`, TRAY_FILL of the height, centred."""
    ink = mark(4)
    ink = ink.crop(ink.getbbox())
    height = size * TRAY_FILL
    width = height * ink.width / ink.height
    ink = ink.resize((max(1, round(width * 4)), max(1, round(height * 4))), Image.LANCZOS)
    big = Image.new("L", (size * 4, size * 4), 0)
    big.paste(ink, ((big.width - ink.width) // 2, (big.height - ink.height) // 2))
    alpha = big.resize((size, size), Image.LANCZOS)
    icon = Image.new("RGBA", (size, size), rgb(color) + (0,))
    icon.putalpha(alpha)
    return icon


def render(size, inset=PADDING):
    """The icon at `size` pixels, drawn 4x larger and scaled down for clean edges.
    `inset` shrinks it inside a transparent margin on every side."""
    k = 4
    big = SIZE * k
    s = lambda v: int(round(v * k))
    # The background, then the C band over it.
    background = gradient(big, TOP, BOTTOM)
    band = Image.new("L", (big, big), 0)
    ImageDraw.Draw(band).polygon([(x * k, y * k) for x, y in c_band()], fill=255)
    background.paste(gradient(big, C_TOP, C_BOTTOM), (0, 0), band)
    # The mark as a mask, so the nodes' holes show the background and band.
    ink = mark(k)
    background.paste(Image.new("RGB", (big, big), rgb(INK)), (0, 0), ink)
    # Cut to the rounded square.
    tile = Image.new("L", (big, big), 0)
    ImageDraw.Draw(tile).rounded_rectangle([0, 0, big - 1, big - 1], radius=s(CORNER), fill=255)
    image = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    image.paste(background, (0, 0), tile)
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
    render(1024, inset=MAC_INSET).save(os.path.join(out, "uconnect.icns"))
    # The tray. No 40: macOS takes the largest that fits its menu bar (40 px
    # on a Retina one), and 32 gives the u the height of other menu bar icons.
    for size in (16, 20, 24, 32, 48, 64):
        for name, color in (("black", "#000000"), ("white", "#FFFFFF")):
            tray(size, color).save(os.path.join(out, f"tray-{name}-{size}.png"))
    print("wrote", out)


if __name__ == "__main__":
    main()
