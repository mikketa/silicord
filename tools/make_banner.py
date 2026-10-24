#!/usr/bin/env python3
"""Builds assets/banner-dark.svg and assets/banner-light.svg from assets/logo.svg.

Each banner is the logo tile followed by "silicord", drawn with round-capped
strokes in the logo's own geometry: the s repeats the S of the mark, the dots
of the i take its blue. On the dark banner the tile is a shade lighter than in
the logo, so it stays visible on GitHub's dark background.

Run from the repository root after changing the logo:  python tools/make_banner.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BLUE = "#4C8DFF"
TILE_FILL = "#111318"  # the logo's background
STROKE = 24
GAP = 28               # between the outer edges of two letters
TILE = 200             # banner height, and the tile's size
X_TOP = 64             # top of the x-height; the baseline is 120 below
VARIANTS = (           # name, letters, tile
    ("dark", "#E8EAF0", "#1C2029"),
    ("light", "#111318", TILE_FILL),
)

# Letter: (outer left, outer right, stroked paths, dots as (x, y, r)), with the top of the x-height at y = 0.
LETTERS = {
    "s": (-12, 84, ["M72 12H24A24 24 0 0 0 24 60H48A24 24 0 0 1 48 108H0"], []),
    "i": (-12, 12, ["M0 12V108"], [(0, -34, 15)]),
    "l": (-12, 12, ["M0 -48V108"], []),
    "c": (-12, 92, ["M80 12H48A48 48 0 0 0 48 108H80"], []),
    "o": (-12, 108, ["M48 12A48 48 0 0 1 48 108A48 48 0 0 1 48 12"], []),
    "r": (-12, 76, ["M0 108V60A48 48 0 0 1 48 12H64"], []),
    "d": (-12, 108, ["M48 12A48 48 0 0 0 48 108A48 48 0 0 0 48 12", "M96 -48V108"], []),
}


def shift(d, dx, dy):
    """Moves absolute path data (M L H V A) by (dx, dy)."""
    out = []
    for cmd, args in re.findall(r"([MLHVA])([^MLHVA]*)", d):
        n = [float(v) for v in re.findall(r"-?[\d.]+", args)]
        if cmd in "ML":
            n = [n[0] + dx, n[1] + dy]
        elif cmd == "H":
            n = [n[0] + dx]
        elif cmd == "V":
            n = [n[0] + dy]
        else:
            n = n[:5] + [n[5] + dx, n[6] + dy]
        out.append(cmd + " ".join(f"{v:g}" for v in n))
    return "".join(out)


def wordmark(text, x, ink):
    """Returns the SVG of text starting at x, and where it ends."""
    paths, dots = [], []
    for ch in text:
        left, right, strokes, ds = LETTERS[ch]
        ox = x - left
        paths += [shift(d, ox, X_TOP) for d in strokes]
        dots += [f'<circle cx="{cx + ox:g}" cy="{cy + X_TOP:g}" r="{r}" fill="{BLUE}"/>' for cx, cy, r in ds]
        x = ox + right + GAP
    body = (f'<path d="{"".join(paths)}" fill="none" stroke="{ink}" stroke-width="{STROKE}" '
            f'stroke-linecap="round" stroke-linejoin="round"/>' + "".join(dots))
    return body, x - GAP


def main():
    logo = (ROOT / "assets" / "logo.svg").read_text(encoding="utf-8")
    shapes = "\n".join(line.strip() for line in logo.splitlines() if re.match(r"\s*<(rect|circle|path)\b", line))
    for name, ink, tile in VARIANTS:
        body, end = wordmark("silicord", TILE + 64, ink)
        width = round(end + 8)
        svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {TILE}" width="{width}" height="{TILE}">\n'
               f'  <title>silicord</title>\n'
               f'  <g transform="scale({TILE / 512:g})">\n    ' + shapes.replace(TILE_FILL, tile).replace("\n", "\n    ") +
               f"\n  </g>\n  {body}\n</svg>\n")
        out = ROOT / "assets" / f"banner-{name}.svg"
        out.write_text(svg, encoding="utf-8", newline="\n")
        print(f"{out.relative_to(ROOT)}: {len(svg)} bytes")


if __name__ == "__main__":
    main()
