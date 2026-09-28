#!/usr/bin/env python3
"""Writes the fuzzer's dictionaries from the sources, so they follow the code.

  keys.inc   every key the core looks up with json_get()
  strs.inc   every string it compares with json_str_eq()
  seeds.inc  every JSON document written in tests/*_test.c, as seeds to mutate
  apng.inc   the animated PNG of tests/apng_test.c

Usage: gen_dict.py <repository root> <output directory>
"""
import json
import re
import sys
from pathlib import Path


def c_bytes(data):
    return '"' + "".join(f"\\x{b:02x}" for b in data) + '"'


def literals(src):
    """C string literals, adjacent ones joined, decoded to bytes."""
    for m in re.finditer(r'((?:"(?:[^"\\\n]|\\.)*"\s*)+)', src):
        text = "".join(re.findall(r'"((?:[^"\\\n]|\\.)*)"', m.group(1)))
        try:
            yield text.encode("latin-1", "backslashreplace").decode("unicode_escape").encode("latin-1")
        except (UnicodeError, ValueError):
            continue


def main():
    root, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    sources = "".join(p.read_text(encoding="utf-8") for p in sorted((root / "src").glob("*.c")))
    keys = sorted(set(re.findall(r'json_get\([^,]+, "([A-Za-z0-9_]+)"', sources)))
    strs = sorted(set(re.findall(r'json_str_eq\([^,]+, "([^"\\]+)"', sources)))
    (out / "keys.inc").write_text("".join(f'"{k}",\n' for k in keys))
    (out / "strs.inc").write_text("".join(f'"{s}",\n' for s in strs))

    seeds = []
    for test in sorted((root / "tests").glob("*_test.c")):
        for data in literals(test.read_text(encoding="utf-8")):
            if data[:1] in (b"{", b"[") and data not in seeds:
                try:
                    json.loads(data.decode("utf-8"))
                except ValueError:
                    continue
                seeds.append(data)
    (out / "seeds.inc").write_text("".join(f"{{{c_bytes(d)}, {len(d)}}},\n" for d in seeds))

    apng = (root / "tests" / "apng_test.c").read_text(encoding="utf-8")
    body = re.search(r"k_apng\[\] = \{(.*?)\};", apng, re.S).group(1)
    (out / "apng.inc").write_text(body.strip() + "\n")
    print(f"gen_dict: {len(keys)} keys, {len(strs)} strings, {len(seeds)} seeds")


if __name__ == "__main__":
    main()
