#!/usr/bin/env python3
"""Rasterizes the application icon (resources/icons/*.svg) into PNGs, a Windows .ico and a macOS .icns.

Needs only Pillow. It understands the small SVG subset the icons are written in: rect (with rx),
circle, ellipse and polygon, each with a flat fill and an optional "rotate(angle cx cy)" transform.
Shapes are drawn at a high resolution and box-filtered down, which antialiases the edges.
deskflower-small.svg (drawn on a 16 px grid) is used up to 48 px, deskflower.svg above that.

Usage: python scripts/make-icons.py   (rewrites the files in resources/icons; commit them)
"""

import math
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

from PIL import Image, ImageDraw

ICONS = Path(__file__).resolve().parent.parent / "resources" / "icons"
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256, 512]
SMALL_MAX = 48
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
WORK = 2048  # supersampled canvas size

NS = "{http://www.w3.org/2000/svg}"


def rotation(element):
    """Returns (angle in radians, cx, cy) of a rotate() transform, or None."""
    transform = element.get("transform")
    if not transform:
        return None
    m = re.fullmatch(r"\s*rotate\(\s*([-\d.]+)[\s,]+([-\d.]+)[\s,]+([-\d.]+)\s*\)\s*", transform)
    if not m:
        sys.exit(f"unsupported transform: {transform}")
    a, cx, cy = map(float, m.groups())
    return math.radians(a), cx, cy


def rotate(points, rot):
    if rot is None:
        return points
    a, cx, cy = rot
    c, s = math.cos(a), math.sin(a)
    return [(cx + (x - cx) * c - (y - cy) * s, cy + (x - cx) * s + (y - cy) * c) for x, y in points]


def render(svg, size):
    root = ET.parse(svg).getroot()
    _, _, vw, vh = map(float, root.get("viewBox").split())
    work = WORK - WORK % size if size <= WORK else size
    k = work / vw
    image = Image.new("RGBA", (work, round(vh * k)), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    def num(element, name, default=0.0):
        return float(element.get(name, default)) * k

    for el in root.iter():
        tag = el.tag.removeprefix(NS)
        if tag in ("svg", "title", "desc"):
            continue
        fill = el.get("fill")
        rot = rotation(el)
        if tag == "rect":
            if rot:
                sys.exit("rotated rect is not supported")
            x, y = num(el, "x"), num(el, "y")
            box = (x, y, x + num(el, "width") - 1, y + num(el, "height") - 1)
            draw.rounded_rectangle(box, radius=num(el, "rx"), fill=fill)
        elif tag in ("circle", "ellipse"):
            cx, cy = num(el, "cx"), num(el, "cy")
            rx = num(el, "r") if tag == "circle" else num(el, "rx")
            ry = num(el, "r") if tag == "circle" else num(el, "ry")
            if rot:
                a, rcx, rcy = rot
                rot = (a, rcx * k, rcy * k)
                steps = 256
                outline = [(cx + rx * math.cos(2 * math.pi * i / steps), cy + ry * math.sin(2 * math.pi * i / steps))
                           for i in range(steps)]
                draw.polygon(rotate(outline, rot), fill=fill)
            else:
                draw.ellipse((cx - rx, cy - ry, cx + rx - 1, cy + ry - 1), fill=fill)
        elif tag == "polygon":
            values = [float(v) * k for v in re.split(r"[\s,]+", el.get("points").strip())]
            points = list(zip(values[::2], values[1::2]))
            if rot:
                a, rcx, rcy = rot
                points = rotate(points, (a, rcx * k, rcy * k))
            draw.polygon(points, fill=fill)
        else:
            sys.exit(f"unsupported element: {tag}")
    return image.resize((size, size), Image.Resampling.BOX)


def main():
    images = {}
    for size in SIZES:
        svg = ICONS / ("deskflower-small.svg" if size <= SMALL_MAX else "deskflower.svg")
        images[size] = render(svg, size)
        out = ICONS / "png" / f"deskflower-{size}.png"
        out.parent.mkdir(exist_ok=True)
        images[size].save(out, optimize=True)

    # Each size is stored as rendered (append_images), not rescaled from the largest one.
    largest = images[ICO_SIZES[-1]]
    largest.save(ICONS / "deskflower.ico", sizes=[(s, s) for s in ICO_SIZES],
                 append_images=[images[s] for s in ICO_SIZES[:-1]])
    icns_sizes = [16, 32, 64, 128, 256, 512]
    images[512].save(ICONS / "deskflower.icns", append_images=[images[s] for s in icns_sizes[:-1]])
    print(f"Wrote {len(SIZES)} PNGs, deskflower.ico and deskflower.icns to {ICONS}")


if __name__ == "__main__":
    main()
