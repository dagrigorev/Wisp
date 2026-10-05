#!/usr/bin/env python
"""Generate res/wisp.ico, the Wisp application icon.

The mark is an original drawing: a rounded terminal tile with a wisp — a small
flame-like teardrop — rising out of a prompt caret. Nothing here is traced from
or derived from another project's artwork.

Run from the repository root:

    python tools/make_icon.py
"""

import os
from PIL import Image, ImageDraw

SIZES = [256, 128, 64, 48, 32, 24, 16]

BG_TOP = (28, 32, 44)       # tile gradient, top
BG_BOTTOM = (18, 20, 28)    # tile gradient, bottom
FLAME_OUTER = (92, 225, 230)
FLAME_INNER = (226, 252, 255)
CARET = (120, 240, 200)

SS = 8  # supersampling factor, for clean curves at small sizes


def rounded_tile(d, size, radius, fill):
    d.rounded_rectangle([0, 0, size - 1, size - 1], radius=radius, fill=fill)


def vertical_gradient(size, top, bottom):
    """A single-column gradient, stretched — cheaper than per-pixel work."""
    grad = Image.new("RGB", (1, size))
    px = grad.load()
    for y in range(size):
        t = y / max(size - 1, 1)
        px[0, y] = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
    return grad.resize((size, size), Image.BILINEAR)


def teardrop(d, cx, tip_y, r, base_y, fill):
    """A flame: a circle of radius r centred at (cx, base_y), closed off by the
    two tangent lines that run up to the tip at (cx, tip_y). Building it from
    real tangents keeps the silhouette smooth — a polygon patched with an
    ellipse leaves a visible seam where the two shapes meet."""
    import math

    dy = base_y - tip_y
    if dy <= r:
        d.ellipse([cx - r, base_y - r, cx + r, base_y + r], fill=fill)
        return

    # Angle between the axis and each tangent line.
    alpha = math.asin(r / dy)
    # Where each tangent touches the circle, measured from the upward axis.
    theta = math.pi / 2 - alpha

    pts = [(cx, tip_y)]
    steps = 96
    # Sweep the circle from the right tangent point, down around, back to the
    # left one, so the outline closes cleanly through the tip.
    start = -theta
    end = theta - 2 * math.pi
    for i in range(steps + 1):
        a = start + (end - start) * i / steps
        pts.append((cx + r * math.sin(a), base_y - r * math.cos(a)))

    d.polygon(pts, fill=fill)


def draw_icon(size):
    s = size * SS
    tile = vertical_gradient(s, BG_TOP, BG_BOTTOM).convert("RGBA")

    # Mask the gradient into a rounded tile.
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, s - 1, s - 1],
                                           radius=int(s * 0.22), fill=255)
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    img.paste(tile, (0, 0), mask)

    d = ImageDraw.Draw(img)

    # Prompt caret, lower left — the terminal half of the idea.
    cw = s * 0.055
    x0, y0 = s * 0.20, s * 0.56
    arm = s * 0.115
    d.line([(x0, y0 - arm), (x0 + arm * 0.9, y0), (x0, y0 + arm)],
           fill=CARET, width=int(cw), joint="curve")

    # Underscore beside the caret. It stops short of the flame's base circle
    # (left edge ~0.50) so the two marks stay visually separate.
    d.rounded_rectangle([s * 0.375, y0 + arm - cw * 0.5,
                         s * 0.475, y0 + arm + cw * 0.5],
                        radius=cw * 0.5, fill=CARET)

    # The wisp itself, rising from the right.
    teardrop(d, s * 0.655, s * 0.17, s * 0.155, s * 0.565, FLAME_OUTER)
    teardrop(d, s * 0.655, s * 0.40, s * 0.072, s * 0.605, FLAME_INNER)

    return img.resize((size, size), Image.LANCZOS)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "res", "wisp.ico")

    frames = [draw_icon(n) for n in SIZES]
    frames[0].save(out, format="ICO",
                   sizes=[(n, n) for n in SIZES],
                   append_images=frames[1:])

    print("wrote {} ({} bytes)".format(out, os.path.getsize(out)))


if __name__ == "__main__":
    main()
