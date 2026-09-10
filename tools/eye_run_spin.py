#!/usr/bin/env python3
"""How much a rotating thing turned between consecutive eye-run frames, by region:
python eye_run_spin.py eye_HHMMSS_L0.bmp eye_HHMMSS_L1.bmp ... --centre X Y --ring R0 R1 [--ring R0 R1 ...]

Each frame is unwrapped into a polar image about the centre (0.05 deg bins), and each ring's
angular profile is cross-correlated between consecutive frames; the peak, refined by a parabola,
is the turn in degrees (positive = anticlockwise on the image). A station's hub and its ring are
two --ring ranges: whether they turn the same each frame, and whether the turn steps or holds, is
what the pool cannot say."""
import argparse, math, sys
import numpy as np
from PIL import Image

def polar(img, cx, cy, r0, r1, nth, nr):
    th = np.linspace(0.0, 2.0 * math.pi, nth, endpoint=False)
    rs = np.linspace(r0, r1, nr)
    T, R = np.meshgrid(th, rs)
    xs = cx + R * np.cos(T)
    ys = cy - R * np.sin(T)
    x0 = np.floor(xs).astype(int); y0 = np.floor(ys).astype(int)
    fx = xs - x0; fy = ys - y0
    h, w = img.shape
    ok = (x0 >= 0) & (x0 + 1 < w) & (y0 >= 0) & (y0 + 1 < h)
    x0c = np.clip(x0, 0, w - 2); y0c = np.clip(y0, 0, h - 2)
    v = (img[y0c, x0c] * (1 - fx) * (1 - fy) + img[y0c, x0c + 1] * fx * (1 - fy) +
         img[y0c + 1, x0c] * (1 - fx) * fy + img[y0c + 1, x0c + 1] * fx * fy)
    v[~ok] = 0.0
    return v

def turn(pa, pb, nth):
    """the angular shift of pb relative to pa, in bins, by circular cross-correlation over all radii"""
    a = pa - pa.mean(axis=1, keepdims=True)
    b = pb - pb.mean(axis=1, keepdims=True)
    fa = np.fft.rfft(a, axis=1); fb = np.fft.rfft(b, axis=1)
    c = np.fft.irfft(fa.conj() * fb, n=nth, axis=1).sum(axis=0)
    k = int(np.argmax(c))
    y0, y1, y2 = c[(k - 1) % nth], c[k], c[(k + 1) % nth]
    den = (y0 - 2 * y1 + y2)
    frac = 0.5 * (y0 - y2) / den if den != 0 else 0.0
    shift = k + frac
    if shift > nth / 2: shift -= nth
    peak = y1 / (np.sqrt((a * a).sum()) * np.sqrt((b * b).sum()) + 1e-9)
    return shift, peak

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('frames', nargs='+')
    ap.add_argument('--centre', nargs=2, type=float, required=True)
    ap.add_argument('--ring', nargs=2, type=float, action='append', required=True)
    ap.add_argument('--bins', type=int, default=7200)
    a = ap.parse_args()
    imgs = []
    for f in a.frames:
        im = Image.open(f).convert('L')
        imgs.append(np.asarray(im, dtype=np.float32))
        print(f"{f}: {im.size[0]}x{im.size[1]}")
    cx, cy = a.centre
    step = 360.0 / a.bins
    for r0, r1 in a.ring:
        nr = max(8, int(r1 - r0))
        pol = [polar(im, cx, cy, r0, r1, a.bins, nr) for im in imgs]
        print(f"ring {r0:.0f}-{r1:.0f} px about ({cx:.0f}, {cy:.0f}):")
        for k in range(1, len(pol)):
            s, peak = turn(pol[k - 1], pol[k], a.bins)
            print(f"   frame {k-1} -> {k}: turned {s * step:+.4f} deg (correlation {peak:.3f})")
        if len(pol) > 2:
            s, peak = turn(pol[0], pol[-1], a.bins)
            print(f"   frame 0 -> {len(pol)-1}: turned {s * step:+.4f} deg over {len(pol)-1} frames (correlation {peak:.3f})")

if __name__ == '__main__':
    sys.exit(main())
