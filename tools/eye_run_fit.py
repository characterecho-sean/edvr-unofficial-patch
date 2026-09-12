#!/usr/bin/env python3
"""The turn of a region between consecutive eye-run frames, by a rigid fit:
python eye_run_fit.py eye_HHMMSS_C00.bmp ... --centre X Y --annulus R0 R1 [--blur 2.0] [--label hub]

Each frame is fitted to the one before it over the annulus (R0..R1 about the centre) with three
numbers -- a shift (dx, dy) and a turn about the centre -- by Gauss-Newton on the blurred
intensities (the aliasing of a raw frame does not survive a 2 px blur; the tiles do). The
shift takes the head and the pass's sub-pixel jitter, which move the whole picture; the turn
is the region's own. Summed over the run and fitted by a line, it is the region's rate.

eye_run_spin.py correlates angular profiles, which a jittered, aliased frame throws off by a
tenth of a degree or more a frame at these radii; the fit averages the noise over every pixel
of the region instead, and returns the residual so a bad fit shows.
"""
import argparse, math, sys
import numpy as np
from PIL import Image, ImageFilter

def load(path, blur):
    im = Image.open(path).convert('L')
    if blur > 0:
        im = im.filter(ImageFilter.GaussianBlur(blur))
    return np.asarray(im, dtype=np.float64)

def sample(img, xs, ys):
    """bilinear samples; outside the image = 0"""
    h, w = img.shape
    x0 = np.floor(xs).astype(int); y0 = np.floor(ys).astype(int)
    fx = xs - x0; fy = ys - y0
    ok = (x0 >= 0) & (x0 + 1 < w) & (y0 >= 0) & (y0 + 1 < h)
    x0c = np.clip(x0, 0, w - 2); y0c = np.clip(y0, 0, h - 2)
    v = (img[y0c, x0c] * (1 - fx) * (1 - fy) + img[y0c, x0c + 1] * fx * (1 - fy) +
         img[y0c + 1, x0c] * (1 - fx) * fy + img[y0c + 1, x0c + 1] * fx * fy)
    v[~ok] = 0.0
    return v

def fit(ref, cur, cx, cy, r0, r1, iters=12, keep=None):
    """(dx, dy, theta) such that cur(x) ~= ref(R(-theta)(x - c) + c - d); theta in radians, positive = anticlockwise on the image"""
    h, w = ref.shape
    yy, xx = np.mgrid[0:h, 0:w]
    rr2 = (xx - cx) ** 2 + (yy - cy) ** 2
    m = (rr2 >= r0 * r0) & (rr2 < r1 * r1)
    if keep is not None: m &= keep
    px = xx[m].astype(np.float64); py = yy[m].astype(np.float64)
    gy, gx = np.gradient(ref)
    dx = dy = th = 0.0
    res = None
    for _ in range(iters):
        c, s = math.cos(th), math.sin(th)
        # where each pixel of cur came from in ref: the inverse motion
        rx = px - cx - dx; ry = py - cy - dy
        sx = c * rx + s * ry + cx
        sy = -s * rx + c * ry + cy
        r = sample(cur, px, py) - sample(ref, sx, sy)
        Gx = sample(gx, sx, sy); Gy = sample(gy, sx, sy)
        # d(ref(s))/d(dx) = -Gx*c + Gy*s ... keep it simple: numerical jacobian by the chain rule
        # ds/ddx = (-c, s), ds/ddy = (-s, -c), ds/dth = (-s*rx + c*ry, -c*rx - s*ry)
        J = np.stack([-(Gx * (-c) + Gy * s), -(Gx * (-s) + Gy * (-c)),
                      -(Gx * (-s * rx + c * ry) + Gy * (-c * rx - s * ry))], axis=1)
        A = J.T @ J; b = J.T @ r
        try:
            step = np.linalg.solve(A, -b)
        except np.linalg.LinAlgError:
            break
        dx += step[0]; dy += step[1]; th += step[2]
        res = np.sqrt((r * r).mean())
        if np.abs(step[:2]).max() < 1e-4 and abs(step[2]) < 1e-6:
            break
    return dx, dy, th, res, m.sum()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('frames', nargs='+')
    ap.add_argument('--centre', nargs=2, type=float, required=True)
    ap.add_argument('--annulus', nargs=2, type=float, action='append', required=True)
    ap.add_argument('--blur', type=float, default=2.0)
    ap.add_argument('--label', default='')
    ap.add_argument('--mask', nargs=2, type=float, metavar=('BLUR', 'LEVEL'),
                    help='fit only where the first frame, blurred by BLUR px, is brighter than LEVEL: the '
                         'structure, not the sky (the stars in the gaps hold still and pull the turn toward nought)')
    a = ap.parse_args()
    imgs = [load(f, a.blur) for f in a.frames]
    keep = None
    if a.mask:
        keep = load(a.frames[0], a.mask[0]) > a.mask[1]
    n = len(imgs)
    cx, cy = a.centre
    for r0, r1 in a.annulus:
        turns, shifts, resid = [], [], []
        for k in range(1, n):
            dx, dy, th, res, cnt = fit(imgs[k - 1], imgs[k], cx, cy, r0, r1, keep=keep)
            turns.append(math.degrees(th)); shifts.append((dx, dy)); resid.append(res)
        cum = np.cumsum(turns)
        ks = np.arange(1, n)
        A = np.vstack([ks, np.ones(n - 1)]).T
        rate = np.linalg.lstsq(A, cum, rcond=None)[0][0] if n > 2 else float('nan')
        print(f"{a.label} annulus {r0:.0f}-{r1:.0f} px about ({cx:.0f}, {cy:.0f}), {cnt} px, blur {a.blur}:")
        print("   turn per frame (deg): " + " ".join(f"{t:+.3f}" for t in turns))
        print("   cumulative:           " + " ".join(f"{c:+.3f}" for c in cum))
        print("   shift per frame (px): " + " ".join(f"{dx:+.2f},{dy:+.2f}" for dx, dy in shifts))
        print("   residual rms:         " + " ".join(f"{r:.1f}" for r in resid))
        print(f"   => mean {np.mean(turns):+.4f} deg a frame (sd {np.std(turns):.4f}); the line through the cumulative turns {rate:+.4f} deg a frame")

if __name__ == '__main__':
    sys.exit(main())
