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

python eye_run_spin.py --self-test   frames of a pattern turned by known angles, and a picture shifted by known
                                     pixels, made in a temp folder: the polar unwrap, the turn (and its sign),
                                     the picture shift and the printed report
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

def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    if '--self-test' in argv:
        return self_test()
    ap = argparse.ArgumentParser()
    ap.add_argument('frames', nargs='+')
    ap.add_argument('--centre', nargs=2, type=float, required=True)
    ap.add_argument('--ring', nargs=2, type=float, action='append', required=True)
    ap.add_argument('--track', nargs=3, type=float, help='X Y HALF: remove the picture shift measured about (X, Y)')
    ap.add_argument('--bins', type=int, default=7200)
    a = ap.parse_args(argv)
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

def self_test():
    """The polar unwrap, the turn and its sign, the whole-pixel picture shift and the report, on
    synthetic frames: a pattern turned by known angles, and a texture shifted by known pixels."""
    import contextlib, io, os, re, shutil, tempfile
    failures = []

    def expect(name, condition, what=''):
        if not condition:
            failures.append('%s: %s' % (name, what))

    S = 200
    cx = cy = 100.0
    yy, xx = np.mgrid[0:S, 0:S].astype(np.float64)

    def turned(deg):
        """A pattern about the centre turned ANTICLOCKWISE on the picture by deg (x right, y down)."""
        a = np.arctan2(-(yy - cy), xx - cx) - math.radians(deg)
        rr = np.hypot(xx - cx, yy - cy)
        return 128 + 100 * np.cos(4 * a) * np.exp(-((rr - 60) / 30) ** 2) + 20 * np.cos(7 * a + 0.5)

    def texture(h, w, seed, cutoff=0.4):
        rng = np.random.default_rng(seed)
        spec = np.fft.fft2(rng.standard_normal((h, w)))
        fy = np.fft.fftfreq(h)[:, None]; fx = np.fft.fftfreq(w)[None, :]
        spec[np.hypot(fx, fy) > cutoff] = 0
        f = np.fft.ifft2(spec).real
        return 255.0 * (f - f.min()) / (f.max() - f.min())

    # polar(): rows are rings at their own radius; nothing outside the picture.
    cone = np.hypot(xx - cx, yy - cy)
    p = polar(cone, cx, cy, 40, 80, 720, 5)
    expect('polar', p.shape == (5, 720) and np.allclose(p.mean(axis=1), [40, 50, 60, 70, 80], atol=0.05)
           and p.std(axis=1).max() < 0.01, repr(p.mean(axis=1)))
    expect('polar-outside', not polar(cone, cx, cy, 150, 160, 720, 5).any(), 'a ring off the picture is not nought')
    # Angle runs anticlockwise from the +x axis, on the picture: a bright pixel above the centre is at 90 deg.
    spot = np.zeros((S, S)); spot[60, 100] = 1000.0
    ring = polar(spot, cx, cy, 40, 40, 360, 8)
    expect('polar-angle', abs(int(np.argmax(ring[0])) - 90) <= 1, 'the spot above the centre is at bin %d of 360' % np.argmax(ring[0]))

    # turn(): whole and fractional bins, either way round, and the peak says how sure.
    nth = 720
    th = np.linspace(0, 2 * math.pi, nth, endpoint=False)
    pattern = lambda t: np.cos(3 * t) + 0.5 * np.cos(5 * t + 1.0) + 0.3 * np.cos(11 * t)
    pa = np.tile(pattern(th), (12, 1))
    for k in (0, 7, -13, 100):
        shift, peak = turn(pa, np.roll(pa, k, axis=1), nth)
        expect('turn(%d bins)' % k, abs(shift - k) < 1e-6 and peak > 0.999, '%r %r' % (shift, peak))
    for frac in (0.5, 2.5, -1.3):
        shift, _ = turn(pa, np.tile(pattern(th - frac * 2 * math.pi / nth), (12, 1)), nth)
        expect('turn(%.1f bins)' % frac, abs(shift - frac) < 0.01, repr(shift))
    noise = np.random.default_rng(3).standard_normal((12, nth))
    expect('turn-noise', turn(pa, noise, nth)[1] < 0.5, 'unrelated profiles should not peak')

    # ... and read off pictures: one degree anticlockwise is +1 with bins to spare.
    bins = 3600
    p0 = polar(turned(0.0), cx, cy, 40, 80, bins, 40)
    for deg in (1.0, 2.5, -1.0):
        shift, peak = turn(p0, polar(turned(deg), cx, cy, 40, 80, bins, 40), bins)
        expect('turn(%.1f deg)' % deg, abs(shift * 360.0 / bins - deg) < 0.01 and peak > 0.99,
               '%.4f deg, peak %.3f' % (shift * 360.0 / bins, peak))

    # picture_shift(): whole pixels, x first, of b relative to a.
    tex = texture(128, 128, 2)
    for sy, sx in ((0, 3), (2, 0), (-3, 4), (5, -6)):
        got = picture_shift(tex, np.roll(tex, (sy, sx), axis=(0, 1)))
        expect('picture_shift(%d, %d)' % (sx, sy), (int(got[0]), int(got[1])) == (sx, sy), repr(got))

    def report(frames, *extra):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            main(frames + ['--centre', '100', '100'] + list(extra))
        return out.getvalue()

    def numbers(said, label):
        return [[float(x) for x in m.split()] for m in re.findall(r'^\s*%s\s*(.*)$' % label, said, re.M)]

    def png(base, name, array):
        path = os.path.join(base, name)
        Image.fromarray(np.uint8(np.round(np.clip(array, 0, 255)))).save(path)
        return path

    base = tempfile.mkdtemp(prefix='edvr-eye-run-spin-')
    try:
        # Frames turned a degree apart, read through two rings: each ring turns a degree a frame.
        frames = [png(base, 'eye_120000_C%02d.png' % k, turned(1.0 * k)) for k in range(4)]
        said = report(frames, '--ring', '40', '80', '--ring', '50', '70', '--bins', '3600')
        expect('report', said.startswith('4 frames of 200x200\n') and said.count(' px about (100, 100):') == 2, said)
        per = numbers(said, r'per frame:')
        expect('report', len(per) == 2 and all(len(r) == 3 and all(abs(t - 1.0) < 0.02 for t in r) for r in per),
               'turns %r\n%s' % (per, said))
        cum = numbers(said, r'cumulative:')
        expect('report', len(cum) == 2 and all(abs(c - k) < 0.05 for r in cum for c, k in zip(r, (1.0, 2.0, 3.0))),
               'cumulative %r' % (cum,))
        rates = re.findall(r'the line through the cumulative turns ([+-]\d+\.\d+) deg a frame', said)
        expect('report', said.count('deg a frame (sd') == 2 and len(rates) == 2
               and all(abs(float(r) - 1.0) < 0.02 for r in rates), 'the summary lines: %r\n%s' % (rates, said))

        # A texture shifted (2, -1) a frame and turned not at all: --track finds the shift, and with it
        # removed the ring's turn is nought.
        moving = [png(base, 'eye_120000_T%02d.png' % k, np.roll(texture(200, 200, 7), (-1 * k, 2 * k), axis=(0, 1)))
                  for k in range(4)]
        said = report(moving, '--ring', '30', '70', '--bins', '1800', '--track', '100', '100', '60')
        expect('track', "the picture's own shift, frame to frame (px): +2,-1 +2,-1 +2,-1" in said, said)
        per = numbers(said, r'per frame:')
        expect('track', len(per) == 1 and all(abs(t) < 0.02 for t in per[0]), 'turns %r\n%s' % (per, said))
        # Two frames have no line to fit, so no summary line either.
        said = report(moving[:2], '--ring', '30', '70', '--bins', '1800')
        expect('two-frames', 'per frame:' in said and '=>' not in said, said)
    finally:
        shutil.rmtree(base, ignore_errors=True)

    if failures:
        print('eye_run_spin: self-test FAILED')
        for f in failures:
            print('  ' + f.replace('\n', '\n    '))
        return 1
    print('eye_run_spin: self-test OK')
    return 0

if __name__ == '__main__':
    sys.exit(main())
