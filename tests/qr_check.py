"""Decodes the QR codes printed by qr_test with zxing-cpp and compares them with the inputs."""
import subprocess
import sys

import numpy as np
import zxingcpp

SCALE = 8
QUIET = 4


def cases(text):
    name, rows = None, []
    for line in text.splitlines():
        if line.startswith("CASE "):
            if name is not None:
                yield name, rows
            name, rows = bytes.fromhex(line[5:]), []
        elif line and set(line) <= {"0", "1"}:
            rows.append(line)
    if name is not None:
        yield name, rows


def image(rows):
    size = len(rows) + 2 * QUIET
    img = np.full((size, size), 255, dtype=np.uint8)
    for y, row in enumerate(rows):
        for x, c in enumerate(row):
            if c == "1":
                img[y + QUIET, x + QUIET] = 0
    return np.kron(img, np.ones((SCALE, SCALE), dtype=np.uint8))


def main():
    run = subprocess.run([sys.argv[1]], capture_output=True)
    text = run.stdout.decode("ascii", "replace")
    failed = run.returncode != 0
    count = 0
    for data, rows in cases(text):
        count += 1
        found = zxingcpp.read_barcodes(image(rows), formats=zxingcpp.BarcodeFormat.QRCode)
        got = found[0].bytes if found else None
        ok = got == data and all(len(r) == len(rows) for r in rows)
        failed |= not ok
        version = (len(rows) - 17) // 4
        print(f"{'ok  ' if ok else 'FAIL'} v{version:<2} {len(data):3} bytes  {data[:40]!r}")
    if count == 0:
        print("FAIL no cases printed")
        failed = True
    print(text[text.find("ok "):] if "ok " in text else "", end="")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
