#!/usr/bin/env python3
"""
Screenshot inspection helpers - for verifying what actually changed on screen
instead of eyeballing subtle colour differences.

    python inspect_img.py diff a.png b.png       which rows/regions differ
    python inspect_img.py rows img.png [--channel blue]
                                                 per-row intensity profile
    python inspect_img.py bands img.png N        split into N bands, rank them
"""
import argparse
import sys

import numpy as np
from PIL import Image


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def cmd_diff(args):
    a, b = load(args.a), load(args.b)
    if a.shape != b.shape:
        print("different sizes: %s vs %s" % (a.shape, b.shape))
        return 1
    delta = np.abs(a - b).sum(axis=2)
    total = int((delta > args.threshold).sum())
    print("changed pixels: %d / %d (%.2f%%)"
          % (total, delta.size, 100.0 * total / delta.size))
    if total == 0:
        print("images are identical above threshold")
        return 0
    rows = (delta > args.threshold).sum(axis=1)
    cols = (delta > args.threshold).sum(axis=0)
    ys = np.nonzero(rows)[0]
    xs = np.nonzero(cols)[0]
    print("bounding box: x %d..%d, y %d..%d" % (xs[0], xs[-1], ys[0], ys[-1]))
    print("\ntop changed rows:")
    for y in np.argsort(rows)[::-1][:10]:
        if rows[y]:
            print("  y=%-4d %d px" % (y, rows[y]))
    return 0


def channel_index(name):
    return {"red": 0, "green": 1, "blue": 2}.get(name, None)


def profile(img, channel):
    idx = channel_index(channel)
    data = img.mean(axis=2) if idx is None else img[:, :, idx]
    return data.mean(axis=1)


def cmd_rows(args):
    img = load(args.image)
    prof = profile(img, args.channel)
    print("row profile (%s), height %d" % (args.channel, len(prof)))
    for y in np.argsort(prof)[::-1][:args.top]:
        print("  y=%-4d %.1f" % (y, prof[y]))
    return 0


def cmd_bands(args):
    img = load(args.image)
    prof = profile(img, args.channel)
    h = len(prof)
    y0, y1 = args.y0, args.y1 if args.y1 > 0 else h
    band = (y1 - y0) / float(args.n)
    scores = []
    for i in range(args.n):
        lo = int(y0 + i * band)
        hi = int(y0 + (i + 1) * band)
        scores.append((prof[lo:hi].mean(), i, lo, hi))
    print("bands over y %d..%d (%s channel), brightest first:" % (y0, y1, args.channel))
    for score, i, lo, hi in sorted(scores, reverse=True):
        print("  band %-3d y %3d..%-3d  %.1f" % (i, lo, hi, score))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("diff")
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("--threshold", type=int, default=30)
    p.set_defaults(func=cmd_diff)

    p = sub.add_parser("rows")
    p.add_argument("image")
    p.add_argument("--channel", default="all")
    p.add_argument("--top", type=int, default=15)
    p.set_defaults(func=cmd_rows)

    p = sub.add_parser("bands")
    p.add_argument("image")
    p.add_argument("n", type=int)
    p.add_argument("--channel", default="blue")
    p.add_argument("--y0", type=int, default=0)
    p.add_argument("--y1", type=int, default=0)
    p.set_defaults(func=cmd_bands)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
