#!/usr/bin/env python3
"""How much a rotating thing turned between consecutive eye-run frames, by region:
python eye_run_spin.py eye_HHMMSS_C00.bmp eye_HHMMSS_C01.bmp ... --centre X Y --ring R0 R1 [--ring R0 R1 ...]
    [--track X Y HALF]   remove the picture's own shift first (the head), measured by phase
                         correlation on a window of 2*HALF px about (X, Y) -- use the station's centre

Each frame is unwrapped into a polar image about the centre (0.05 deg bins), and each ring's
angular profile is cross-correlated between consecutive frames; the peak, refined by a parabola,
is the turn in degrees (positive = anticlockwise on the image). The summary gives the mean turn a
frame and its scatter, and the straight line through the cumulative turns. A station's hub and
its ring are two --ring ranges: whether they turn the same each frame, and whether the turn steps
or holds, is what the pool cannot say.

The frames must be the game's own consecutive ones (the eye run's raw crops, eye_HHMMSS_C00..15):
NVIDIA's output moves as its history reprojected by the pass's vectors, blended with the new
frame, so a turn read off treated frames is the vectors' as much as the object's. The reading is
biased low on aliased frames when the turn is well under a pixel a frame at the ring's radius
(the peak locks to the pixel grid); the ratio between two rings of the same run still holds.
"""
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

def picture_shift(a, b):
    """the whole-pixel shift of b relative to a, by phase correlation"""
    A = np.fft.fft2(a - a.mean()); B = np.fft.fft2(b - b.mean())
    R = A * np.conj(B); R /= (np.abs(R) + 1e-6)
    c = np.fft.ifft2(R).real
    iy, ix = np.unravel_index(np.argmax(c), c.shape)
    h, w = c.shape
    dy = iy if iy <= h // 2 else iy - h
    dx = ix if ix <= w // 2 else ix - w
    return -dx, -dy

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('frames', nargs='+')
    ap.add_argument('--centre', nargs=2, type=float, required=True)
    ap.add_argument('--ring', nargs=2, type=float, action='append', required=True)
    ap.add_argument('--track', nargs=3, type=float, help='X Y HALF: remove the picture shift measured about (X, Y)')
    ap.add_argument('--bins', type=int, default=7200)
    a = ap.parse_args()
    imgs = []
    for f in a.frames:
        im = Image.open(f).convert('L')
        imgs.append(np.asarray(im, dtype=np.float32))
    n = len(imgs)
    print(f"{n} frames of {imgs[0].shape[1]}x{imgs[0].shape[0]}")
    shifts = [(0.0, 0.0)]
    if a.track and n > 1:
        tx, ty, half = int(a.track[0]), int(a.track[1]), int(a.track[2])
        win = [im[max(0, ty - half):ty + half, max(0, tx - half):tx + half] for im in imgs]
        moved = []
        for k in range(1, n):
            dx, dy = picture_shift(win[k - 1], win[k])
            shifts.append((shifts[-1][0] + dx, shifts[-1][1] + dy))
            moved.append(f"{dx:+.0f},{dy:+.0f}")
        print("the picture's own shift, frame to frame (px): " + " ".join(moved))
    else:
        shifts = [(0.0, 0.0)] * n
    cx, cy = a.centre
    step = 360.0 / a.bins
    for r0, r1 in a.ring:
        nr = max(8, int(r1 - r0))
        pol = [polar(imgs[k], cx + shifts[k][0], cy + shifts[k][1], r0, r1, a.bins, nr) for k in range(n)]
        per = []
        for k in range(1, n):
            s, peak = turn(pol[k - 1], pol[k], a.bins)
            per.append(s * step)
        cum = np.cumsum(per)
        print(f"ring {r0:.0f}-{r1:.0f} px about ({cx:.0f}, {cy:.0f}):")
        print("   per frame:  " + " ".join(f"{p:+.3f}" for p in per))
        print("   cumulative: " + " ".join(f"{c:+.3f}" for c in cum))
        if n > 2:
            ks = np.arange(1, n)
            A = np.vstack([ks, np.ones(n - 1)]).T
            rate = np.linalg.lstsq(A, cum, rcond=None)[0][0]
            print(f"   => mean {np.mean(per):+.4f} deg a frame (sd {np.std(per):.3f}); the line through the "
                  f"cumulative turns {rate:+.4f} deg a frame")

if __name__ == '__main__':
    sys.exit(main())
