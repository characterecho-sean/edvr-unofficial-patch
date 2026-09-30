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

python eye_run_shimmer.py --self-test   shifted copies of a synthetic picture (a steady run) and a run with a
                                        flash: the shift it finds, the change it reports, in a temp folder
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

def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    if '--self-test' in argv:
        return self_test()
    ap = argparse.ArgumentParser()
    ap.add_argument('frames', nargs='+')
    ap.add_argument('--centre', nargs=2, type=float, required=True)
    ap.add_argument('--region', nargs=5, action='append', default=[], metavar=('NAME', 'X0', 'Y0', 'X1', 'Y1'))
    ap.add_argument('--annulus', nargs=3, action='append', default=[], metavar=('NAME', 'R0', 'R1'))
    ap.add_argument('--window', type=int, default=350, help='half-size of the shift window about the centre')
    a = ap.parse_args(argv)
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

def self_test():
    """The phase-correlation shift, the re-alignment, and the printed report, on synthetic frames
    made in a temp folder: a steady run that only moves, and a run with a flash in the middle."""
    import contextlib, io, os, re, shutil, tempfile
    failures = []

    def expect(name, condition, what=''):
        if not condition:
            failures.append('%s: %s' % (name, what))

    def texture(h, w, seed, cutoff=0.4):
        """Broadband noise, 0..255: a picture with detail at every scale, as a station's is."""
        rng = np.random.default_rng(seed)
        spec = np.fft.fft2(rng.standard_normal((h, w)))
        fy = np.fft.fftfreq(h)[:, None]; fx = np.fft.fftfreq(w)[None, :]
        spec[np.hypot(fx, fy) > cutoff] = 0
        f = np.fft.ifft2(spec).real
        return 255.0 * (f - f.min()) / (f.max() - f.min())

    def png(base, name, array):
        path = os.path.join(base, name)
        Image.fromarray(np.uint8(np.round(np.clip(array, 0, 255)))).save(path)
        return path

    tex = texture(128, 128, 5)

    # shift_est: whole-pixel rolls come back exactly, x first, and shifted() undoes them.
    for sy, sx in ((0, 3), (2, 0), (-3, 4), (5, -6)):
        got = shift_est(tex, np.roll(tex, (sy, sx), axis=(0, 1)))
        expect('shift_est(%d, %d)' % (sx, sy), abs(got[0] - sx) < 1e-6 and abs(got[1] - sy) < 1e-6, repr(got))
    for sy, sx in ((1, 3), (2, -3)):
        moved = Image.fromarray(np.uint8(np.round(np.roll(tex, (sy, sx), axis=(0, 1)))))
        back = np.asarray(shifted(moved, sx, sy), dtype=np.float64)
        want = np.round(tex)
        expect('shifted(%d, %d)' % (sx, sy), np.abs(back[8:-8, 8:-8] - want[8:-8, 8:-8]).max() <= 1.0,
               'the picture was not put back')

    def report(frames, *extra):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            main(frames + ['--centre', '64', '64', '--window', '40'] + list(extra))
        said = out.getvalue()
        rows = [[float(x) for x in rest.split()] for _a, _b, rest in re.findall(r'^\s*(\d+)->(\d+)\s+(.*)$', said, re.M)]
        return said, rows

    base = tempfile.mkdtemp(prefix='edvr-eye-run-shimmer-')
    try:
        # A run that only moves: found, put back, and nearly still after it.
        rolls = ((0, 0), (1, 3), (2, -3))
        frames = [png(base, 'eye_120000_T%02d.png' % k, np.roll(tex, r, axis=(0, 1))) for k, r in enumerate(rolls)]
        said, rows = report(frames, '--region', 'core', '20', '20', '100', '100', '--annulus', 'ring', '15', '40')
        shifts = re.search(r'3 frames of 128x128; the picture\'s shift from frame 0: (\S+) (\S+)\n', said)
        expect('steady', shifts is not None, said)
        if shifts:
            got = [tuple(float(v) for v in s.split(',')) for s in shifts.groups()]
            expect('steady', all(abs(g[0] - r[1]) < 0.3 and abs(g[1] - r[0]) < 0.3 for g, r in zip(got, rolls[1:])),
                   'shifts %r for rolls %r' % (got, rolls[1:]))
        expect('steady', 'pair       core       ring      whole' in said, said)
        expect('steady', len(rows) == 2 and all(len(r) == 3 and max(r) < 25.0 for r in rows),
               'a steady run should change by a few percent:\n' + said)
        sd = [float(x) for x in said.strip().splitlines()[-1].split()]
        expect('steady', len(sd) == 3 and sd[0] < 25.0, 'the amplitude line: %r' % (sd,))

        # A flash in the middle frame changes EVERY region at once, whole included.
        flashed = [png(base, 'eye_120000_F%02d.png' % k, tex * scale) for k, scale in enumerate((1.0, 0.5, 1.0))]
        said, rows = report(flashed, '--region', 'core', '20', '20', '100', '100')
        expect('flash', len(rows) == 2 and all(len(r) == 2 and min(r) > 100.0 for r in rows),
               'both pairs should change every region by more than its contrast:\n' + said)
    finally:
        shutil.rmtree(base, ignore_errors=True)

    if failures:
        print('eye_run_shimmer: self-test FAILED')
        for f in failures:
            print('  ' + f.replace('\n', '\n    '))
        return 1
    print('eye_run_shimmer: self-test OK')
    return 0

if __name__ == '__main__':
    sys.exit(main())
