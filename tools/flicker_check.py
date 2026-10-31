"""Finds flicker in frame dumps (F1CE_DUMP_FRAMES=1, optionally F1CE_DUMP_FROM / F1CE_DUMP_COUNT): reports frames
that differ strongly from the previous one but not from the one before it (back-and-forth changes).
usage: python -I tools/flicker_check.py <dir with frame_*.argb>"""
import array
import glob
import os
import sys


def sample(path):
    return array.array('I', open(path, 'rb').read())[::97]


def changed(a, b):   # percentage of sampled pixels whose green channel moved by more than 40
    return sum(1 for x, y in zip(a, b) if abs(((x >> 8) & 255) - ((y >> 8) & 255)) > 40) / len(a) * 100


def main():
    files = sorted(glob.glob(os.path.join(sys.argv[1] if len(sys.argv) > 1 else '.', 'frame_*.argb')))
    prev = pprev = None
    hits = []
    for f in files:
        s = sample(f)
        if pprev is not None:
            d1, d2 = changed(s, prev), changed(s, pprev)
            if d1 > 5 and d2 < d1 / 3:
                hits.append('%s:%.0f/%.0f' % (os.path.basename(f)[6:11], d1, d2))
        pprev, prev = prev, s
    print(f'{len(files)} frames, {len(hits)} flicker-like (change vs previous / vs two back):', ' '.join(hits[:40]))


if __name__ == '__main__':
    main()
