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

The sign is the opposite of eye_run_spin.py's: here a positive turn is CLOCKWISE on the picture
(a region turned anticlockwise by 3 deg fits as -3 deg), there anticlockwise is positive.

python eye_run_fit.py --self-test   rotates and shifts a synthetic picture by known amounts and checks
                                    the fit gets them back (nothing is written but a temp folder)
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
    """(dx, dy, theta) such that cur(x) ~= ref(R(-theta)(x - c) + c - d); theta in radians, positive = CLOCKWISE on the image (x right, y down)"""
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

def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    if '--self-test' in argv:
        return self_test()
    ap = argparse.ArgumentParser()
    ap.add_argument('frames', nargs='+')
    ap.add_argument('--centre', nargs=2, type=float, required=True)
    ap.add_argument('--annulus', nargs=2, type=float, action='append', required=True)
    ap.add_argument('--blur', type=float, default=2.0)
    ap.add_argument('--label', default='')
    ap.add_argument('--mask', nargs=2, type=float, metavar=('BLUR', 'LEVEL'),
                    help='fit only where the first frame, blurred by BLUR px, is brighter than LEVEL: the '
                         'structure, not the sky (the stars in the gaps hold still and pull the turn toward nought)')
    a = ap.parse_args(argv)
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

def self_test():
    """Synthetic pictures moved by known amounts: the sampler, the fit's shift and turn
    (and its sign), the mask, and the printed report over PNG frames in the temp folder."""
    import contextlib, io, os, re, shutil, tempfile
    failures = []

    def expect(name, condition, what=''):
        if not condition:
            failures.append('%s: %s' % (name, what))

    def texture(h, w, seed, cutoff=0.06):
        """A smooth random field, 0..255: low-passed noise, so it has gradient everywhere."""
        rng = np.random.default_rng(seed)
        spec = np.fft.fft2(rng.standard_normal((h, w)))
        fy = np.fft.fftfreq(h)[:, None]; fx = np.fft.fftfreq(w)[None, :]
        spec[np.hypot(fx, fy) > cutoff] = 0
        f = np.fft.ifft2(spec).real
        return 255.0 * (f - f.min()) / (f.max() - f.min())

    H = W = 160
    cx = cy = 80.0
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float64)

    def moved(ref, dx, dy, deg):
        """The picture `cur` with cur(x) = ref(R(-theta)(x - c - d) + c): what fit() inverts."""
        th = math.radians(deg); c, s = math.cos(th), math.sin(th)
        rx = xx - cx - dx; ry = yy - cy - dy
        return sample(ref, c * rx + s * ry + cx, -s * rx + c * ry + cy)

    # The sampler: exact on the grid, bilinear between, nought outside the picture.
    img = np.arange(16, dtype=np.float64).reshape(4, 4)          # img[y, x] = 4 y + x
    v = sample(img, np.array([1.0, 1.5, 2.25, -0.5, 100.0]), np.array([2.0, 2.0, 1.0, 1.0, 1.0]))
    expect('sample', np.allclose(v[:3], [9.0, 9.5, 6.25]) and v[3] == 0.0 and v[4] == 0.0, repr(v))

    ref = texture(H, W, 1)
    rr2 = (xx - cx) ** 2 + (yy - cy) ** 2
    ring_px = int(((rr2 >= 20 * 20) & (rr2 < 60 * 60)).sum())

    # The fit gets a shift and a turn back, together and apart, either way round.
    for dx, dy, deg in ((1.5, -0.8, 1.0), (0.0, 0.0, 0.5), (-2.0, 1.0, -1.5), (0.4, 0.3, 0.0)):
        got = fit(ref, moved(ref, dx, dy, deg), cx, cy, 20, 60)
        name = 'fit(%.1f, %.1f, %.2f deg)' % (dx, dy, deg)
        expect(name, abs(got[0] - dx) < 0.01 and abs(got[1] - dy) < 0.01 and abs(math.degrees(got[2]) - deg) < 0.01,
               'got (%.4f, %.4f, %.4f deg)' % (got[0], got[1], math.degrees(got[2])))
        expect(name, got[3] < 0.5 and got[4] == ring_px, 'residual %r over %r px, expected %d' % (got[3], got[4], ring_px))

    # The sign, against something you can see: two markers about the centre, turned ANTICLOCKWISE
    # on the picture by 3 degrees. Positive here is clockwise, so it fits as -3.
    def blobs(points):
        return sum(255.0 * k * np.exp(-(((xx - bx) ** 2 + (yy - by) ** 2) / (2 * 6.0 ** 2))) for bx, by, k in points)

    def anticlockwise(bx, by, deg):
        r = math.hypot(bx - cx, by - cy)
        a = math.atan2(-(by - cy), bx - cx) + math.radians(deg)
        return cx + r * math.cos(a), cy - r * math.sin(a)
    marks = [(cx + 30, cy, 1.0), (cx + 20, cy + 22, 0.5)]
    turned = [anticlockwise(bx, by, 3.0) + (k,) for bx, by, k in marks]
    got = fit(blobs(marks), blobs(turned), cx, cy, 10, 45)
    expect('sign', abs(math.degrees(got[2]) + 3.0) < 0.1, 'anticlockwise 3 deg fitted as %.3f deg' % math.degrees(got[2]))

    # A frame with nothing to fit does not raise: the singular system leaves the start where it was.
    flat = np.full((H, W), 100.0)
    got = fit(flat, flat, cx, cy, 20, 60)
    expect('flat', got[:3] == (0.0, 0.0, 0.0), repr(got))

    # The mask keeps the fit to where the first frame is bright.
    keep = ref > 128.0
    unmasked = fit(ref, moved(ref, 1.0, 0.0, 1.0), cx, cy, 20, 60)
    masked = fit(ref, moved(ref, 1.0, 0.0, 1.0), cx, cy, 20, 60, keep=keep)
    expect('mask', 0 < masked[4] < unmasked[4] and abs(math.degrees(masked[2]) - 1.0) < 0.02,
           'masked %d of %d px, %.4f deg' % (masked[4], unmasked[4], math.degrees(masked[2])))

    base = tempfile.mkdtemp(prefix='edvr-eye-run-fit-')
    try:
        # Three frames turned a degree apart (the tool's sign), written as 8-bit PNGs:
        # load() reads them back as float, blurring on request, and the report gives the turn.
        from PIL import Image
        frames = []
        for k in range(3):
            path = os.path.join(base, 'eye_120000_C%02d.png' % k)
            Image.fromarray(np.uint8(np.round(moved(ref, 0.0, 0.0, 1.0 * k)))).save(path)
            frames.append(path)
        raw = load(frames[0], 0.0)
        expect('load', raw.shape == (H, W) and raw.dtype == np.float64
               and abs(raw[:-1, :-1] - ref[:-1, :-1]).max() <= 0.5 + 1e-9, 'the loaded frame is not the PNG')
        expect('load-blur', load(frames[0], 3.0).std() < raw.std(), 'blurring did not smooth')

        def report(*extra):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = main(frames + ['--centre', '80', '80', '--annulus', '20', '60', '--blur', '1.0',
                                      '--label', 'fixture'] + list(extra))
            return code, out.getvalue()

        code, said = report()
        turns = re.search(r'turn per frame \(deg\): (\S+) (\S+)', said)
        rate = re.search(r'the line through the cumulative turns ([+-]\d+\.\d+) deg a frame', said)
        expect('report', code in (0, None) and 'fixture annulus 20-60 px about (80, 80)' in said and turns and rate,
               '%r\n%s' % (code, said))
        if turns and rate:
            expect('report', all(abs(float(t) - 1.0) < 0.05 for t in turns.groups()) and abs(float(rate.group(1)) - 1.0) < 0.05,
                   'turns %r, line %s\n%s' % (turns.groups(), rate.group(1), said))
        n_all = int(re.search(r'(\d+) px, blur', said).group(1))
        code, said = report('--mask', '2', '128')
        n_masked = int(re.search(r'(\d+) px, blur', said).group(1))
        expect('report-mask', 0 < n_masked < n_all, 'masked %d of %d px' % (n_masked, n_all))
    finally:
        shutil.rmtree(base, ignore_errors=True)

    if failures:
        print('eye_run_fit: self-test FAILED')
        for f in failures:
            print('  ' + f.replace('\n', '\n    '))
        return 1
    print('eye_run_fit: self-test OK')
    return 0

if __name__ == '__main__':
    sys.exit(main())
