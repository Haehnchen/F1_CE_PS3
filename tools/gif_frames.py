"""Extracts frames of a GIF as PNG files (for looking at reference animations).
usage: python -I tools/gif_frames.py <in.gif> <out dir> <frame index> [<frame index> ...]   (use 'count' to list)"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from img_side_by_side import write_png  # noqa: E402


def lzw_decode(data, min_size):
    clear, end = 1 << min_size, (1 << min_size) + 1
    size = min_size + 1
    table = [bytes([i]) for i in range(clear)] + [b'', b'']
    out = bytearray()
    bits = nbits = pos = 0
    prev = None
    while True:
        while nbits < size:
            if pos >= len(data): return bytes(out)
            bits |= data[pos] << nbits
            nbits += 8
            pos += 1
        code = bits & ((1 << size) - 1)
        bits >>= size
        nbits -= size
        if code == clear:
            table = table[:end + 1]
            size = min_size + 1
            prev = None
            continue
        if code == end: return bytes(out)
        if code < len(table): entry = table[code]
        elif prev is not None: entry = prev + prev[:1]
        else: return bytes(out)
        out += entry
        if prev is not None and len(table) < 4096:
            table.append(prev + entry[:1])
            if len(table) == (1 << size) and size < 12: size += 1
        prev = entry


def frames(path):
    d = Path(path).read_bytes()
    w, h, flags = struct.unpack('<HHB', d[6:11])
    pos = 13
    gct = None
    if flags & 0x80:
        n = 3 << ((flags & 7) + 1)
        gct = d[pos:pos + n]
        pos += n
    canvas = [[(0, 0, 0)] * w for _ in range(h)]
    transp = None
    while pos < len(d):
        b = d[pos]
        if b == 0x21:   # extension
            label = d[pos + 1]
            pos += 2
            if label == 0xF9 and d[pos] == 4:
                transp = d[pos + 4] if d[pos + 1] & 1 else None
            while d[pos]: pos += d[pos] + 1
            pos += 1
        elif b == 0x2C:   # image
            x, y, iw, ih, f = struct.unpack('<HHHHB', d[pos + 1:pos + 10])
            pos += 10
            ct = gct
            if f & 0x80:
                n = 3 << ((f & 7) + 1)
                ct = d[pos:pos + n]
                pos += n
            min_size = d[pos]
            pos += 1
            data = bytearray()
            while d[pos]:
                data += d[pos + 1:pos + 1 + d[pos]]
                pos += d[pos] + 1
            pos += 1
            px = lzw_decode(bytes(data), min_size)
            interlaced = f & 0x40
            rows = list(range(ih))
            if interlaced:
                rows = list(range(0, ih, 8)) + list(range(4, ih, 8)) + list(range(2, ih, 4)) + list(range(1, ih, 2))
            for i, ry in enumerate(rows):
                for cx in range(iw):
                    k = i * iw + cx
                    if k >= len(px): break
                    c = px[k]
                    if c != transp:
                        canvas[y + ry][x + cx] = (ct[c * 3], ct[c * 3 + 1], ct[c * 3 + 2])
            yield [row[:] for row in canvas]
            transp = None
        elif b == 0x3B:
            return
        else:
            pos += 1


def main():
    if sys.argv[3] == 'count':
        print(sum(1 for _ in frames(sys.argv[1])))
        return
    want = {int(v) for v in sys.argv[3:]}
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    for i, fr in enumerate(frames(sys.argv[1])):
        if i in want:
            write_png(out / f'frame_{i:04d}.png', fr)
        if i >= max(want): break


if __name__ == '__main__':
    main()
