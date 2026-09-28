#!/usr/bin/env python3
"""Builds assets/silicord.ico from assets/logo.svg, with the standard library only.

The logo is a 16 x 16 grid of square cells drawn as axis-aligned rectangles
(shape-rendering="crispEdges"), so each icon size, a multiple of 16, is an
exact nearest-neighbour rendering. Every size is stored as a PNG entry
(Windows Vista and later), which keeps the file a few kilobytes.

Run from the repository root after changing the logo:  python tools/make_icon.py
"""
import re
import struct
import sys
import zlib
from pathlib import Path

SIZES = (16, 32, 48, 64, 128, 256)
ROOT = Path(__file__).resolve().parent.parent


def parse_logo(svg):
    """Returns (viewbox size, background RGB, mark RGB, rectangles as (x, y, w, h))."""
    view = float(re.search(r'viewBox="0 0 ([\d.]+) [\d.]+"', svg).group(1))
    background = re.search(r'<rect [^>]*fill="#([0-9A-Fa-f]{6})"', svg).group(1)
    mark, path = re.search(r'<path fill="#([0-9A-Fa-f]{6})" d="([^"]+)"', svg).groups()
    rects = []
    for m in re.finditer(r'M([\d.]+) ([\d.]+)h([\d.]+)v([\d.]+)h-?[\d.]+z', path):
        x, y, w, h = map(float, m.groups())
        rects.append((x, y, w, h))
    if not rects:
        sys.exit("make_icon: no rectangles found in the logo path")
    rgb = lambda s: tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))
    return view, rgb(background), rgb(mark), rects


def render(size, view, background, mark, rects):
    """RGBA rows: the background, then every rectangle, sampled at pixel centres."""
    scale = view / size
    rows = []
    for py in range(size):
        y = (py + 0.5) * scale
        row = bytearray()
        for px in range(size):
            x = (px + 0.5) * scale
            inside = any(rx <= x < rx + rw and ry <= y < ry + rh for rx, ry, rw, rh in rects)
            row += bytes(mark if inside else background) + b"\xff"
        rows.append(bytes(row))
    return rows


def png(size, rows):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + r for r in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def ico(images):
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = len(header) + 16 * len(images)
    entries, data = b"", b""
    for size, blob in images:
        dim = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(blob), offset + len(data))
        data += blob
    return header + entries + data


def main():
    logo = parse_logo((ROOT / "assets" / "logo.svg").read_text(encoding="utf-8"))
    images = [(s, png(s, render(s, *logo))) for s in SIZES]
    out = ROOT / "assets" / "silicord.ico"
    out.write_bytes(ico(images))
    print(f"{out.relative_to(ROOT)}: {out.stat().st_size} bytes, sizes {', '.join(map(str, SIZES))}")


if __name__ == "__main__":
    main()
