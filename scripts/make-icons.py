#!/usr/bin/env python3
"""Draws the uConnect icon and writes every platform's file from one design.

The mark is the name: a bold "u" whose two arm tips are round nodes, two
devices joined by the u. Behind it a darker "C" band echoes the u: its back
crosses the top-left corner, its arms run toward the bottom-right, and it is
mirror-symmetric across that diagonal. The background is colours stirred
like dye in water, into streaks, eddies and glowing veins; the C is the
same colours, darker.

    python scripts/make-icons.py            # writes packaging/icons/

Outputs: uconnect.svg (the master), uconnect-<n>.png (16-1024),
uconnect.ico (Windows, 16-256) and uconnect.icns (macOS, with the inset
macOS icons use). Needs Pillow and numpy.
"""
import base64
import io
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "packaging", "icons")

# Geometry on a 1024 grid.
SIZE = 1024
CORNER = 228                        # the rounded square's corner radius
# The background: colours anchored at points (as fractions of the tile),
# blended softly, then stirred like dye in water into streaks and eddies.
FLOW = [
    (0.08, 0.06, "#6C4DF6"),  # violet, top-left
    (0.92, 0.12, "#11B5E8"),  # cyan, top-right
    (0.50, 0.48, "#2D63E8"),  # blue, the middle
    (0.10, 0.94, "#E2468F"),  # pink, bottom-left
    (0.94, 0.92, "#2A1C8F"),  # indigo, bottom-right
]
FLOW_SPREAD = 0.34   # how far each colour reaches
FLOW_SEED = 7        # which stirring: change it for another pattern
FLOW_STIR = 0.5      # how far the dye is carried (fractions of the tile)
FLOW_STEPS = 64      # steps along the way (more: smoother curls)
FLOW_VEIN = 0.06     # the bright veins' width
FLOW_BODY = 0.45     # the colours' brightness between the veins
FLOW_WHITE = 0.35    # how near white the veins' cores glow
C_SHADE = 0.66       # the C band: the background beneath it, this bright
INK = "#FFFFFF"
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


def c_band():
    """The C's outline as (x, y) points on the 1024 grid."""
    (b0, b1), (inner, outer) = C_BACK, C_ARMS
    uv = [(ARM_END, -outer), (b0, -outer), (b0, outer), (ARM_END, outer),
          (ARM_END, inner), (b1, inner), (b1, -inner), (ARM_END, -inner)]
    r = math.sqrt(0.5)
    scale, centre = 1 - 2 * C_PADDING, SIZE / 2
    return [(centre + ((u - v) * r - centre) * scale, centre + ((u + v) * r - centre) * scale) for u, v in uv]


def svg():
    """The master as SVG. The stirred background has no vector form, so it
    is embedded as an image; the C and the mark over it are vectors."""
    r = (RIGHT_X - LEFT_X) / 2
    ring = (NODE_R + HOLE_R) / 2
    band = " ".join(f"{x:.1f},{y:.1f}" for x, y in c_band())
    jpeg = io.BytesIO()
    flow(SIZE).save(jpeg, "JPEG", quality=90)
    background = base64.b64encode(jpeg.getvalue()).decode("ascii")
    nodes = "\n".join(
        f'  <circle cx="{x}" cy="{ARM_TOP}" r="{ring}" fill="none" stroke="{INK}" stroke-width="{NODE_R - HOLE_R}"/>'
        for x in (LEFT_X, RIGHT_X))
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {SIZE} {SIZE}">
  <!-- uConnect: a "u" whose arm tips are two connected devices, before a
       darker C that echoes it across the diagonal. -->
  <defs>
    <clipPath id="tile"><rect width="{SIZE}" height="{SIZE}" rx="{CORNER}"/></clipPath>
  </defs>
  <g clip-path="url(#tile)">
    <image width="{SIZE}" height="{SIZE}" href="data:image/jpeg;base64,{background}"/>
    <polygon points="{band}" fill="#000" fill-opacity="{1 - C_SHADE:.2f}"/>
  </g>
  <path d="M {LEFT_X} {ARM_TOP + ring} V {BOWL_Y} A {r} {r} 0 0 0 {RIGHT_X} {BOWL_Y} V {ARM_TOP + ring}"
        fill="none" stroke="{INK}" stroke-width="{STROKE}"/>
{nodes}
</svg>
"""


def rgb(color):
    return tuple(int(color[i:i + 2], 16) for i in (1, 3, 5))


def perlin(rng, cells):
    """Gradient noise over the unit square, tiling, with `cells` lattice
    cells per side: a function of x and y arrays, about -0.7 to 0.7."""
    angles = rng.random((cells, cells)) * 2 * np.pi
    gx, gy = np.cos(angles), np.sin(angles)

    def sample(x, y):
        x, y = (x % 1) * cells, (y % 1) * cells
        x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
        fx, fy = x - x0, y - y0

        def corner(dx, dy):
            i, j = (y0 + dy) % cells, (x0 + dx) % cells
            return gx[i, j] * (fx - dx) + gy[i, j] * (fy - dy)

        sx = fx ** 3 * (fx * (fx * 6 - 15) + 10)
        sy = fy ** 3 * (fy * (fy * 6 - 15) + 10)
        top = corner(0, 0) + sx * (corner(1, 0) - corner(0, 0))
        bottom = corner(0, 1) + sx * (corner(1, 1) - corner(0, 1))
        return top + sy * (bottom - top)
    return sample


def fbm(rng, cells, octaves):
    """Fractal noise: octaves of gradient noise, each twice as fine and half as strong."""
    layers = [perlin(rng, cells * 2 ** i) for i in range(octaves)]
    return lambda x, y: sum(0.5 ** i * layer(x, y) for i, layer in enumerate(layers))


def lookup(field, x, y):
    """Bilinear lookup of an n x n field over the unit square, wrapping."""
    n = field.shape[0]
    x, y = (x % 1) * n, (y % 1) * n
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = x - x0, y - y0
    x1, y1 = (x0 + 1) % n, (y0 + 1) % n
    x0, y0 = x0 % n, y0 % n
    top = field[y0, x0] * (1 - fx) + field[y0, x1] * fx
    bottom = field[y1, x0] * (1 - fx) + field[y1, x1] * fx
    return top * (1 - fy) + bottom * fy


_flow = {}


def flow(n):
    """The marbled background, n x n, like dye stirred into water: the colour
    field is carried back along a swirling flow (the curl of fractal noise,
    so it neither gathers nor thins), which draws it into streaks and eddies;
    bright veins of the same colours run through it."""
    if n in _flow:
        return _flow[n]
    rng = np.random.default_rng(FLOW_SEED)
    y, x = np.mgrid[0:n, 0:n] / n
    # The stirring: broad currents plus fine eddies.
    psi = fbm(rng, 2, 2)(x, y) + 0.3 * fbm(rng, 6, 3)(x, y)
    dpsi_dy, dpsi_dx = np.gradient(psi)
    vx, vy = dpsi_dy, -dpsi_dx
    speed = np.sqrt(vx ** 2 + vy ** 2).max()
    vx, vy = vx / speed, vy / speed
    # Trace each pixel back along the flow to the dye it carries.
    px, py = x.copy(), y.copy()
    step = FLOW_STIR / FLOW_STEPS
    for _ in range(FLOW_STEPS):
        ux, uy = lookup(vx, px, py), lookup(vy, px, py)
        px, py = px - step * ux, py - step * uy
    # The dye: the anchored colours, blended softly.
    cx, cy = np.clip(px, 0, 1), np.clip(py, 0, 1)
    total = np.zeros((n, n, 3)); weights = np.zeros((n, n))
    for ax, ay, color in FLOW:
        w = np.exp(-((cx - ax) ** 2 + (cy - ay) ** 2) / FLOW_SPREAD ** 2)
        total += w[..., None] * np.array(rgb(color), dtype=float)
        weights += w
    dye = total / weights[..., None]
    # The veins: thin bright filaments where a carried noise crosses zero,
    # over a dim body, so the colours glow out of the dark.
    vein = fbm(rng, 2, 4)(px, py)
    glow = np.exp(-(vein / FLOW_VEIN) ** 2)
    light = FLOW_BODY + (1 - FLOW_BODY) * glow
    pixels = dye * light[..., None] + 255 * FLOW_WHITE * glow[..., None] ** 4
    _flow[n] = Image.fromarray(np.clip(pixels, 0, 255).astype(np.uint8), "RGB")
    return _flow[n]


def render(size, inset=0.0):
    """The icon at `size` pixels, drawn 4x larger and scaled down for clean edges.
    `inset` shrinks it inside a transparent margin (macOS icons)."""
    k = 4
    big = SIZE * k
    s = lambda v: int(round(v * k))
    # The background (smooth, so drawn at 1024 and scaled), then the C band:
    # the same colours, darker.
    background = flow(SIZE).resize((big, big), Image.BICUBIC)
    band = Image.new("L", (big, big), 0)
    ImageDraw.Draw(band).polygon([(x * k, y * k) for x, y in c_band()], fill=255)
    background.paste(background.point(lambda v: int(v * C_SHADE)), (0, 0), band)
    # The mark as a mask, so the nodes' holes show the background and band.
    ink = Image.new("L", (big, big), 0)
    draw = ImageDraw.Draw(ink)
    half = STROKE / 2
    for x in (LEFT_X, RIGHT_X):
        draw.rectangle([s(x - half), s(ARM_TOP), s(x + half), s(BOWL_Y)], fill=255)
    r = (RIGHT_X - LEFT_X) / 2
    cx = (LEFT_X + RIGHT_X) / 2
    draw.arc([s(cx - r - half), s(BOWL_Y - r - half), s(cx + r + half), s(BOWL_Y + r + half)],
             start=0, end=180, fill=255, width=s(STROKE))
    for x in (LEFT_X, RIGHT_X):
        draw.ellipse([s(x - NODE_R), s(ARM_TOP - NODE_R), s(x + NODE_R), s(ARM_TOP + NODE_R)], fill=255)
        draw.ellipse([s(x - HOLE_R), s(ARM_TOP - HOLE_R), s(x + HOLE_R), s(ARM_TOP + HOLE_R)], fill=0)
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
    render(1024, inset=0.1).save(os.path.join(out, "uconnect.icns"))
    print("wrote", out)


if __name__ == "__main__":
    main()
