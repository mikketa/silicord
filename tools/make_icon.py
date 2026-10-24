#!/usr/bin/env python3
"""Builds assets/silicord.ico from assets/logo.svg, with the standard library only.

The logo uses a small subset of SVG, drawn in document order:
  <rect x y width height rx fill>   a filled (rounded) rectangle
  <circle cx cy r fill>             a filled circle
  <path d stroke stroke-width>      a stroked path of lines and circular arcs
                                    (M L H V A and their relative forms), round caps
Each shape is rendered from its signed distance, so every pixel gets its exact
coverage: edges stay smooth and sharp at each size, and the rounded corners
are transparent. Every size is stored as a PNG entry (Windows Vista and
later), which keeps the file small.

Run from the repository root after changing the logo:  python tools/make_icon.py
"""
import math
import re
import struct
import sys
import zlib
from pathlib import Path

SIZES = (16, 24, 32, 48, 64, 128, 256)
ROOT = Path(__file__).resolve().parent.parent


def attrs(tag):
    return dict(re.findall(r'([\w-]+)="([^"]*)"', tag))


def color(value):
    m = re.fullmatch(r"#([0-9A-Fa-f]{6})", value)
    if not m:
        sys.exit(f"make_icon: unsupported color {value!r}")
    return tuple(int(m.group(1)[i:i + 2], 16) for i in (0, 2, 4))


def segments(d):
    """Splits path data into ('line', ax, ay, bx, by) and ('arc', cx, cy, r, start, span)."""
    out, x, y = [], 0.0, 0.0
    for cmd, args in re.findall(r"([MLHVAmlhva])([^MLHVAmlhva]*)", d):
        n = [float(v) for v in re.findall(r"-?[\d.]+(?:e-?\d+)?", args)]
        rel = cmd.islower()
        cmd = cmd.upper()
        step = {"M": 2, "L": 2, "H": 1, "V": 1, "A": 7}[cmd]
        for i in range(0, len(n), step):
            v = n[i:i + step]
            if cmd == "H":
                v = [v[0] + (x if rel else 0), y]
            elif cmd == "V":
                v = [x, v[0] + (y if rel else 0)]
            elif rel:
                v[-2:] = [v[-2] + x, v[-1] + y]
            nx, ny = v[-2], v[-1]
            if cmd == "M" and i == 0:
                pass
            elif cmd == "A":
                out.append(arc(x, y, nx, ny, v[0], int(v[3]), int(v[4])))
            else:
                out.append(("line", x, y, nx, ny))
            x, y = nx, ny
    return out


def arc(x0, y0, x1, y1, r, large, sweep):
    hx, hy = (x1 - x0) / 2, (y1 - y0) / 2
    half = math.hypot(hx, hy)
    r = max(r, half)
    h = math.sqrt(r * r - half * half)
    s = 1 if large != sweep else -1
    cx, cy = x0 + hx - s * h * hy / half, y0 + hy + s * h * hx / half
    start = math.atan2(y0 - cy, x0 - cx)
    span = math.atan2(y1 - cy, x1 - cx) - start
    if sweep and span < 0:
        span += 2 * math.pi
    elif not sweep and span > 0:
        span -= 2 * math.pi
    return ("arc", cx, cy, r, start, span)


def stroke_distance(segs, hw):
    """Distance to the path's centre line, less half the stroke width: round caps and joins."""
    def dist(px, py):
        best = math.inf
        for s in segs:
            if s[0] == "line":
                _, ax, ay, bx, by = s
                dx, dy = bx - ax, by - ay
                t = min(max(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy), 0.0), 1.0)
                d = math.hypot(px - ax - t * dx, py - ay - t * dy)
            else:
                _, cx, cy, r, start, span = s
                rel = (math.atan2(py - cy, px - cx) - start) % (2 * math.pi)
                if span < 0:
                    rel -= 2 * math.pi
                if (0 <= rel <= span) if span > 0 else (span <= rel <= 0):
                    d = abs(math.hypot(px - cx, py - cy) - r)
                else:
                    d = min(math.hypot(px - cx - r * math.cos(a), py - cy - r * math.sin(a))
                            for a in (start, start + span))
            best = min(best, d)
        return best - hw
    return dist


def rect_distance(x, y, w, h, rx):
    rx = min(rx, w / 2, h / 2)
    cx, cy = x + w / 2, y + h / 2

    def dist(px, py):
        qx, qy = abs(px - cx) - w / 2 + rx, abs(py - cy) - h / 2 + rx
        return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - rx
    return dist


def parse_logo(svg):
    """Returns (viewbox size, [(signed distance function, RGB)]) in drawing order."""
    view = float(re.search(r'viewBox="0 0 ([\d.]+) [\d.]+"', svg).group(1))
    shapes = []
    for kind, body in re.findall(r"<(rect|circle|path)\b([^>]*)>", svg):
        a = attrs(body)
        f = lambda k: float(a.get(k, 0))
        if kind == "rect":
            shapes.append((rect_distance(f("x"), f("y"), f("width"), f("height"), f("rx")), color(a["fill"])))
        elif kind == "circle":
            cx, cy, r = f("cx"), f("cy"), f("r")
            shapes.append((lambda px, py, cx=cx, cy=cy, r=r: math.hypot(px - cx, py - cy) - r, color(a["fill"])))
        else:
            if a.get("fill") != "none" or "stroke" not in a:
                sys.exit("make_icon: paths must be stroked, with fill=\"none\"")
            shapes.append((stroke_distance(segments(a["d"]), f("stroke-width") / 2), color(a["stroke"])))
    if not shapes:
        sys.exit("make_icon: no shapes found in the logo")
    return view, shapes


def render(size, view, shapes):
    """RGBA rows: each shape composited over the last, with its coverage at each pixel."""
    scale = view / size
    rows = []
    for py in range(size):
        y = (py + 0.5) * scale
        row = bytearray()
        for px in range(size):
            x = (px + 0.5) * scale
            r = g = b = a = 0.0  # premultiplied
            for dist, (sr, sg, sb) in shapes:
                cover = min(max(0.5 - dist(x, y) / scale, 0.0), 1.0)
                if cover:
                    r, g, b = (sr * cover + r * (1 - cover), sg * cover + g * (1 - cover), sb * cover + b * (1 - cover))
                    a = cover + a * (1 - cover)
            if a:
                row += bytes((round(r / a), round(g / a), round(b / a), round(a * 255)))
            else:
                row += b"\0\0\0\0"
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
