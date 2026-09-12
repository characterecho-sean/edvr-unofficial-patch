#!/usr/bin/env python3
"""How much a region of the TREATED picture changes from frame to frame -- the shimmer, and any flash:
python eye_run_shimmer.py eye_HHMMSS_T00.bmp ... --centre X Y [--region NAME X0 Y0 X1 Y1 ...] [--annulus NAME R0 R1 ...]

Reads the eye run's treated crops (advanced.eye_run_treated; the raw C00.. work the same way), removes
the picture's own sub-pixel shift between consecutive frames (the head, by phase correlation on a window
about the centre) and prints, per consecutive pair and per region: the mean absolute change of the
pixels, as a fraction of the region's contrast (its standard deviation in the first frame). A steady
region changes by a few percent; a shimmering one by tens; a flash (a history reset, or one frame drawn
from another camera) changes EVERY region at once and stands out in the "whole" column. The last line
gives each region's per-pixel standard deviation over the run against its contrast: the shimmer's
amplitude.
"""
import argparse, math, sys
import numpy as np
from PIL import Image, ImageFilter

def shift_est(a, b):
    A = np.fft.fft2(a - a.mean()); B = np.fft.fft2(b - b.mean()); R = A * np.conj(B); R /= np.abs(R) + 1e-6
    c = np.fft.ifft2(R).real; iy, ix = np.unravel_index(np.argmax(c), c.shape); h, w = c.shape
    def par(m, p, n): d = m - 2 * p + n; return 0.5 * (m - n) / d if d else 0.0
    dy = iy + par(c[(iy - 1) % h, ix], c[iy, ix], c[(iy + 1) % h, ix]); dx = ix + par(c[iy, (ix - 1) % w], c[iy, ix], c[iy, (ix + 1) % w])
    if dy > h / 2: dy -= h
    if dx > w / 2: dx -= w
    return -dx, -dy

def shifted(im, dx, dy):
    return im.transform(im.size, Image.AFFINE, (1, 0, dx, 0, 1, dy), resample=Image.BICUBIC)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('frames', nargs='+')
    ap.add_argument('--centre', nargs=2, type=float, required=True)
    ap.add_argument('--region', nargs=5, action='append', default=[], metavar=('NAME', 'X0', 'Y0', 'X1', 'Y1'))
    ap.add_argument('--annulus', nargs=3, action='append', default=[], metavar=('NAME', 'R0', 'R1'))
    ap.add_argument('--window', type=int, default=350, help='half-size of the shift window about the centre')
    a = ap.parse_args()
    ims = [Image.open(f).convert('L') for f in a.frames]
    n = len(ims)
    cx, cy = a.centre
    W, H = ims[0].size
    yy, xx = np.mgrid[0:H, 0:W]
    masks = []
    for name, x0, y0, x1, y1 in a.region:
        m = np.zeros((H, W), bool); m[int(y0):int(y1), int(x0):int(x1)] = True; masks.append((name, m))
    rr = np.hypot(xx - cx, yy - cy)
    for name, r0, r1 in a.annulus:
        masks.append((name, (rr >= float(r0)) & (rr < float(r1))))
    masks.append(("whole", np.ones((H, W), bool)))
    # align every frame to the first, by the accumulated shift
    x0, y0 = int(cx) - a.window, int(cy) - a.window
    def win(im): return np.asarray(im.filter(ImageFilter.GaussianBlur(1.5)), dtype=np.float64)[max(0, y0):y0 + 2 * a.window, max(0, x0):x0 + 2 * a.window]
    aligned = [np.asarray(ims[0], dtype=np.float64)]
    shifts = []
    for k in range(1, n):
        dx, dy = shift_est(win(ims[0]), win(ims[k]))
        shifts.append((dx, dy))
        aligned.append(np.asarray(shifted(ims[k], dx, dy), dtype=np.float64))
    print(f"{n} frames of {W}x{H}; the picture's shift from frame 0: " + " ".join(f"{dx:+.1f},{dy:+.1f}" for dx, dy in shifts))
    contrast = {name: max(1.0, aligned[0][m].std()) for name, m in masks}
    print("mean |change| between consecutive frames, as % of the region's contrast:")
    print(f"{'pair':>8s} " + " ".join(f"{name:>10s}" for name, _ in masks))
    for k in range(1, n):
        d = np.abs(aligned[k] - aligned[k - 1])
        # the edge the shift dragged in is left out (a 4-px border)
        d[:4, :] = 0; d[-4:, :] = 0; d[:, :4] = 0; d[:, -4:] = 0
        print(f"{k-1:>3d}->{k:<3d} " + " ".join(f"{100 * d[m].mean() / contrast[name]:10.1f}" for name, m in masks))
    stack = np.stack(aligned)
    sd = stack.std(axis=0)
    print("per-pixel standard deviation over the run, as % of the region's contrast (the shimmer's amplitude):")
    print(f"{'':>8s} " + " ".join(f"{100 * sd[m].mean() / contrast[name]:10.1f}" for name, m in masks))

if __name__ == '__main__':
    sys.exit(main())
