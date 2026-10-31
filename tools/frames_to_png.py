"""Converts raw ARGB frame dumps (frame_NNNNN_WxH.argb) written by the runtime into PNG files."""
import re
import struct
import sys
import zlib
from pathlib import Path


def write_png(path, w, h, argb):
    rows = bytearray()
    for y in range(h):
        rows.append(0)
        row = argb[y * w * 4:(y + 1) * w * 4]
        for x in range(0, len(row), 4):
            b, g, r, a = row[x], row[x + 1], row[x + 2], row[x + 3]   # little-endian ARGB words
            rows += bytes((r, g, b))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b"")
    Path(path).write_bytes(png)


for f in sorted(Path(sys.argv[1] if len(sys.argv) > 1 else ".").glob("frame_*.argb")):
    m = re.search(r"_(\d+)x(\d+)\.argb$", f.name)
    w, h = int(m.group(1)), int(m.group(2))
    data = f.read_bytes()
    nonblack = sum(1 for i in range(0, len(data), 4) if data[i] | data[i + 1] | data[i + 2])
    write_png(f.with_suffix(".png"), w, h, data)
    print(f"{f.name}: {nonblack} non-black pixels of {w * h}")
