"""Stacks the same region (1280x720 coordinates) of two screenshots (PNG or frame_*.argb) into one PNG, for visual
comparison of our frame with a reference (e.g. RPCS3).
usage: python -I tools/img_side_by_side.py <ours> <reference> <x0,y0,x1,y1> <out.png> [scale]"""
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from img_compare import load  # noqa: E402


def crop(img, box, scale):
    w, h, get = img
    x0, y0, x1, y1 = box
    sx, sy = w / 1280, h / 720
    out = []
    for y in range((y1 - y0) * scale):
        row = []
        for x in range((x1 - x0) * scale):
            row.append(get(min(w - 1, int((x0 + x / scale) * sx)), min(h - 1, int((y0 + y / scale) * sy))))
        out.append(row)
    return out


def write_png(path, rows):
    h, w = len(rows), len(rows[0])
    raw = b''.join(b'\x00' + bytes(c for p in r for c in p) for r in rows)
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def main():
    box = [int(v) for v in sys.argv[3].split(',')]
    scale = int(sys.argv[5]) if len(sys.argv) > 5 else 1
    a = crop(load(sys.argv[1]), box, scale)
    b = crop(load(sys.argv[2]), box, scale)
    write_png(sys.argv[4], a + [[(255, 0, 255)] * len(a[0])] * 2 + b)


if __name__ == '__main__':
    main()
