"""Compares mean colours of regions in two screenshots (PNG or raw frame_*.argb dumps), e.g. our frame against an
RPCS3 reference. Regions are given in 1280x720 coordinates and scaled to each image's size.
usage: python -I tools/img_compare.py <ours> <reference> [name:x0,y0,x1,y1 ...]"""
import array
import re
import struct
import sys
import zlib


def load_png(path):
    data = open(path, 'rb').read()
    pos, chunks = 8, []
    w = h = ctype = 0
    while pos < len(data):
        n, t = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if t == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
            assert depth == 8, 'only 8-bit PNGs'
        elif t == b'IDAT':
            chunks.append(body)
        pos += 12 + n
    bpp = {2: 3, 6: 4}[ctype]
    raw = zlib.decompress(b''.join(chunks))
    stride = w * bpp
    out = bytearray(h * stride)
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, lambda x, y: tuple(out[(y * w + x) * bpp:(y * w + x) * bpp + 3])


def load_argb(path):
    m = re.search(r'_(\d+)x(\d+)\.argb$', path)
    w, h = int(m.group(1)), int(m.group(2))
    px = array.array('I', open(path, 'rb').read())
    return w, h, lambda x, y: ((px[y * w + x] >> 16) & 255, (px[y * w + x] >> 8) & 255, px[y * w + x] & 255)


def load(path):
    return load_argb(path) if path.endswith('.argb') else load_png(path)


def mean(img, box):
    w, h, get = img
    x0, y0, x1, y1 = [int(v * (w / 1280 if i % 2 == 0 else h / 720)) for i, v in enumerate(box)]
    acc, n = [0, 0, 0], 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            p = get(x, y)
            for k in range(3): acc[k] += p[k]
            n += 1
    return [a / n for a in acc]


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    for spec in sys.argv[3:]:
        name, box = spec.split(':')
        box = [int(v) for v in box.split(',')]
        ma, mb = mean(a, box), mean(b, box)
        print(f'{name:14s} ours {ma[0]:5.0f} {ma[1]:5.0f} {ma[2]:5.0f}   ref {mb[0]:5.0f} {mb[1]:5.0f} {mb[2]:5.0f}')


if __name__ == '__main__':
    main()
