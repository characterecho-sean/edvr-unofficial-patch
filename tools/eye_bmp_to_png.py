#!/usr/bin/env python3
"""Turn an eye dump (hotkey.dump_eyes: edvr_logs\\eyes\\eye_HHMMSS_L.bmp) into
a PNG small enough to look at, optionally cropped.

    python tools/eye_bmp_to_png.py <eye.bmp> [out.png] [--scale 0.25]
                                   [--crop x y w h]

Needs Pillow. The BMP is 24-bit bottom-up, some 75 MB at the DLAA size;
the PNG at a quarter scale is a few MB and shows what the player saw.
"""
from __future__ import annotations

import argparse
import os
import sys


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('bmp')
    ap.add_argument('out', nargs='?')
    ap.add_argument('--scale', type=float, default=0.25)
    ap.add_argument('--crop', type=int, nargs=4, metavar=('X', 'Y', 'W', 'H'))
    a = ap.parse_args(argv)
    try:
        from PIL import Image
    except ImportError:
        print('Pillow is not installed: pip install pillow', file=sys.stderr)
        return 2
    Image.MAX_IMAGE_PIXELS = None
    im = Image.open(a.bmp)
    if a.crop:
        x, y, w, h = a.crop
        im = im.crop((x, y, x + w, y + h))
    if a.scale != 1.0:
        im = im.resize((max(1, int(im.width * a.scale)), max(1, int(im.height * a.scale))),
                       Image.Resampling.LANCZOS)
    out = a.out or os.path.splitext(a.bmp)[0] + ('_crop' if a.crop else '') + '.png'
    im.save(out)
    print('%s -> %s (%dx%d)' % (a.bmp, out, im.width, im.height))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
