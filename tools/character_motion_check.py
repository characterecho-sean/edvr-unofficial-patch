#!/usr/bin/env python3
"""How wrong is a skinned character's motion in one eye dump?

    python tools\\character_motion_check.py --target frontier [--stamp 125144765001]
    python tools\\character_motion_check.py --logs <edvr_logs> --stamp 125144765001 [--draw 01DA82F8:16122] [--first 4]
    python tools\\character_motion_check.py --self-test

The newest dump is taken when no --stamp is given. It needs a paired eye run (a manual eye dump is one): the raw input (C), the motion handed
to the upscaler (D) and motion.csv. The depth writer map in edvr_logs\\pool, when the run wrote one, separates the regions.

For each frame from --first (default 4: the capture's own hitch fills the frames before it) and each region, it warps the previous raw input
through the frame's own motion, takes out the jitter difference from motion.csv, and finds the extra offset that best matches this input.
Zero means the motion is right. No upscaler response is in this measure, because C is what the upscaler is handed.

  face    engine-path pixels the skin draw (vs D99AFDC2) owns
  draw    the pixels --draw owns (vs prefix, then optionally :index count), dilated 2 px, less the skin: by default the helmet and headset
  body    head- or engine-path pixels under the face

Errors are in native px. The jitter is taken from motion.csv and not from a control region because an avatar's body moves on its own: a control
once reported 3.4 px on a face that was exact. `jump` is the largest motion change between neighbouring face pixels, in input px, and `n` counts
the pairs past 0.25: a rigid surface cannot step, so a jump the size of `step` (one half-float step of the frame's camera travel, in cm) is rounding.
docs/vr-holo-me-head-flick-2026-11-05.md has the baselines, from before its fixes.

Exit 0 when every frame is inside the limits (--face-limit 0.6, --draw-limit 0.5), 3 when one is not, 2 when the dump cannot be read or has no
skinned pixels to measure. Read-only: this tool writes nothing.
"""
import argparse
import csv
import json
import math
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

PATH_HEAD, PATH_WORLD, PATH_ENGINE = 1, 2, 11   # eye_decisions.py PATHS
KIND_SHIFT, KIND_MASK, KIND_JOINED, KIND_MASKED = 12, 7, 1, 2   # eye_decisions.py ENGINE_KIND_SHIFT, ENGINE_KINDS
FACE_VS = 'D99AFDC2'
DEFAULT_DRAW = '01DA82F8:16122'
FACE_LIMIT, DRAW_LIMIT = 0.6, 0.5
BODY_GAP, BODY_HEIGHT = 40, 200   # input px under the face's box


class DumpError(Exception):
    pass


def bilinear(img, x, y):
    import numpy as np
    x0 = np.floor(x).astype(np.int32)
    y0 = np.floor(y).astype(np.int32)
    ax = x - x0
    ay = y - y0
    x0 = np.clip(x0, 0, img.shape[1] - 2)
    y0 = np.clip(y0, 0, img.shape[0] - 2)
    return ((1 - ax) * (1 - ay) * img[y0, x0] + ax * (1 - ay) * img[y0, x0 + 1]
            + (1 - ax) * ay * img[y0 + 1, x0] + ax * ay * img[y0 + 1, x0 + 1])


def best_offset(prev, cur, motion, mask, jitter):
    """(residual at the jitter alone, x, y, residual at the best offset) of one region, input px. Coarse on a sample of the pixels, then fine on all."""
    import numpy as np
    ys, xs = np.where(mask)
    px = xs.astype(np.float32) + motion[ys, xs, 0] + jitter[0]
    py = ys.astype(np.float32) + motion[ys, xs, 1] + jitter[1]
    want = cur[ys, xs]

    def residual(dx, dy, keep=slice(None)):
        return float(np.abs(bilinear(prev, px[keep] + dx, py[keep] + dy) - want[keep]).mean())

    sample = slice(None, None, max(1, len(xs) // 4000))
    coarse = (residual(0.0, 0.0, sample), 0.0, 0.0)
    for dy in np.arange(-6, 6.01, .5):
        for dx in np.arange(-6, 6.01, .5):
            r = residual(dx, dy, sample)
            if r < coarse[0]:
                coarse = (r, dx, dy)
    best = (residual(0.0, 0.0), 0.0, 0.0)
    at_jitter = best[0]
    for dy in np.arange(coarse[2] - .5, coarse[2] + .51, .125):
        for dx in np.arange(coarse[1] - .5, coarse[1] + .51, .125):
            r = residual(dx, dy)
            if r < best[0]:
                best = (r, dx, dy)
    return at_jitter, float(best[1]), float(best[2]), best[0]


def interior_jump(motion, mask):
    """(largest change between neighbouring masked pixels in either motion component, pairs past 0.25 input px)."""
    import numpy as np
    biggest, count = 0.0, 0
    for comp in (0, 1):
        for axis in (0, 1):
            step = np.abs(np.diff(motion[..., comp], axis=axis))
            both = (mask[1:, :] & mask[:-1, :]) if axis == 0 else (mask[:, 1:] & mask[:, :-1])
            step = step[both]
            if step.size:
                biggest = max(biggest, float(step.max()))
                count += int((step > .25).sum())
    return biggest, count


def dilate(mask, radius):
    out = mask.copy()
    for _ in range(radius):
        grown = out.copy()
        grown[1:, :] |= out[:-1, :]
        grown[:-1, :] |= out[1:, :]
        grown[:, 1:] |= out[:, :-1]
        grown[:, :-1] |= out[:, 1:]
        out = grown
    return out


def half_float_step(value):
    return 2.0 ** (math.floor(math.log2(abs(value))) - 10) if value else 0.0


def newest_stamp(logs):
    eyes = os.path.join(logs, 'eyes')
    try:
        names = [n for n in os.listdir(eyes) if n.startswith('eye_') and n.endswith('_decisions.json')]
    except OSError as e:
        raise DumpError('cannot read %s: %s' % (eyes, e))
    if not names:
        raise DumpError('no paired eye run in %s' % eyes)
    names.sort(key=lambda n: os.path.getmtime(os.path.join(eyes, n)))
    return names[-1][len('eye_'):-len('_decisions.json')]


def owner_regions(logs, stamp, crop, draw_vs, draw_count):
    """(skin mask, draw mask) over the decision crop from the depth writer map, or (None, None) when the run wrote none."""
    import numpy as np
    import eye_inputs
    table = os.path.join(logs, 'pool', 'depthwriter_%s.json' % stamp)
    writer = os.path.join(logs, 'pool', 'depthwriter_%s_e0.bin' % stamp)
    if not os.path.exists(table) or not os.path.exists(writer):
        return None, None
    with open(table, 'r', encoding='utf-8') as f:
        rows = [r for r in json.load(f)['draws'] if r['eye'] == 0]
    _, owners = eye_inputs.read(writer)
    x, y, w, h = crop
    owners = owners[y:y + h, x:x + w]
    skin = np.isin(owners, [r['id'] for r in rows if r['vs'].startswith(FACE_VS)])
    draw = np.isin(owners, [r['id'] for r in rows if r['vs'].startswith(draw_vs) and (draw_count is None or r['count'] == draw_count)])
    return skin, dilate(draw, 2) & ~skin


def measure(logs, stamp, first=4, draw=DEFAULT_DRAW):
    """Everything the report prints, as data: {'stamp', 'scale', 'skin_e', 'rows': [per frame]}. Raises DumpError."""
    import numpy as np
    from PIL import Image
    import eye_inputs
    eyes = os.path.join(logs, 'eyes')
    try:
        with open(os.path.join(eyes, 'eye_%s_decisions.json' % stamp), 'r', encoding='utf-8') as f:
            frames = json.load(f)['frames']
        with open(os.path.join(eyes, 'eye_%s_motion.csv' % stamp), 'r', encoding='utf-8', newline='') as f:
            motion_rows = {int(r['crop']): r for r in csv.DictReader(f) if r['eye'] == '0'}
    except (OSError, KeyError, ValueError) as e:
        raise DumpError('cannot read run %s: %s' % (stamp, e))
    usable = [f for f in frames if f.get('decision_file') and f.get('raw_file')]
    if len(usable) < first + 1:
        raise DumpError('run %s has %d frames with motion and input, none past frame %d' % (stamp, len(usable), first))
    crop = usable[0]['decision_crop']
    scale = usable[0]['output_size'][0] / usable[0]['input_size'][0]
    draw_vs, _, draw_count = draw.partition(':')
    skin0, draw0 = owner_regions(logs, stamp, crop, draw_vs.upper(), int(draw_count) if draw_count else None)

    def decisions(frame):
        return eye_inputs.read(os.path.join(eyes, frame['decision_file']))[1]

    def raw(frame):
        return np.asarray(Image.open(os.path.join(eyes, frame['raw_file'])).convert('L'), dtype=np.float32)

    probe_path = decisions(usable[first])[..., 3].astype(np.int32) & 15
    if not (probe_path == PATH_ENGINE).any():
        raise DumpError('run %s has no engine-path pixels: no skinned character in the crop' % stamp)
    if skin0 is not None and not (skin0 & (probe_path == PATH_ENGINE)).any():
        skin0 = None
    box = skin0 if skin0 is not None else probe_path == PATH_ENGINE
    ys, xs = np.where(box)
    yy, xx = np.mgrid[0:box.shape[0], 0:box.shape[1]]
    below = (yy > ys.max() + BODY_GAP) & (yy < ys.max() + BODY_GAP + BODY_HEIGHT) & (xx > xs.min()) & (xx < xs.max())

    skin_e = None
    skin_path = os.path.join(eyes, 'eye_%s_SkinE.bin' % stamp)
    if os.path.exists(skin_path):
        meta, e = eye_inputs.read(skin_path)
        valid = e[..., 3] > 0
        skin_e = dict(format=meta['format'], pixels=int(valid.sum()),
                      median_cm=float(np.median(np.linalg.norm(e[valid][:, :3].astype(np.float64), axis=1))) if valid.any() else 0.0)

    rows = []
    previous = raw(usable[first - 1])
    for k in range(first, len(usable)):
        d = decisions(usable[k])
        current = raw(usable[k])
        row = motion_rows.get(k)
        if row is None:
            raise DumpError('run %s: motion.csv has no row for frame %d' % (stamp, k))
        before = motion_rows.get(k - 1)
        if before is None:
            raise DumpError('run %s: motion.csv has no row for frame %d' % (stamp, k - 1))
        jitter = (-(float(row['jitter0']) - float(before['jitter0'])), -(float(row['jitter1']) - float(before['jitter1'])))
        travel = [(float(row['prev%d' % i]) - float(row['now%d' % i])) * 100.0 for i in (3, 7, 11)]
        flags = d[..., 3].astype(np.int32)
        path = flags & 15
        character = (path == PATH_HEAD) | (path == PATH_ENGINE)
        face = (path == PATH_ENGINE) & skin0 if skin0 is not None else path == PATH_ENGINE
        regions = {'face': face, 'body': character & below}
        if draw0 is not None and draw0.any():
            regions['draw'] = character & draw0
        out = dict(frame=k, travel_cm=math.sqrt(sum(v * v for v in travel)), step_cm=half_float_step(max(travel, key=abs)))
        out['jump'], out['pairs'] = interior_jump(d[..., :2], face)
        for name, mask in regions.items():
            if mask.sum() < 50:
                continue
            r0, x, y, r = best_offset(previous, current, d, mask, jitter)
            out[name] = dict(pixels=int(mask.sum()), x=x * scale, y=y * scale, error=math.hypot(x, y) * scale, residual_at_zero=r0, residual=r)
        if 'draw' in regions and regions['draw'].any():
            kinds = (flags[regions['draw']] >> KIND_SHIFT) & KIND_MASK
            on_path = path[regions['draw']]
            out['draw_paths'] = dict(head=float((on_path == PATH_HEAD).mean()), engine=float((on_path == PATH_ENGINE).mean()),
                                     joined=float((kinds == KIND_JOINED).mean()), masked=float((kinds == KIND_MASKED).mean()))
        if 'body' in out and face.any() and regions['body'].any():
            out['head_motion'] = tuple(float(np.median(d[..., c][face]) - np.median(d[..., c][regions['body']])) * scale for c in (0, 1))
        rows.append(out)
        previous = current
    return dict(stamp=stamp, scale=scale, draw=draw, skin_e=skin_e, has_owners=skin0 is not None, rows=rows)


def verdict(result, face_limit, draw_limit):
    """(exit code, the lines that say why)."""
    lines, code = [], 0
    for name, limit in (('face', face_limit), ('draw', draw_limit)):
        seen = [r[name]['error'] for r in result['rows'] if name in r]
        if not seen:
            lines.append('%s: no pixels to measure' % name)
            continue
        worst = max(seen)
        past = sum(1 for e in seen if e > limit)
        lines.append('%s: worst %.2f native px, %d of %d frames past the limit of %.2f' % (name, worst, past, len(seen), limit))
        if past:
            code = 3
    return code, lines


def report(result, face_limit, draw_limit, out=None):
    out = out or sys.stdout

    def cell(row, name):
        c = row.get(name)
        return '%-26s' % 'none' if c is None else '%+6.2f %+6.2f %5.2f res %5.2f' % (c['x'], c['y'], c['error'], c['residual_at_zero'])
    print('run %s, %g native px per input px; draw %s%s' % (result['stamp'], result['scale'], result['draw'],
                                                            '' if result['has_owners'] else '; no depth writer map: face is every engine-path pixel, no draw region'), file=out)
    skin = result['skin_e']
    if skin:
        print('SkinE: DXGI format %d, %d valid pixels, median |E| %.3f cm on the run\'s first frame' % (skin['format'], skin['pixels'], skin['median_cm']), file=out)
    print(' k  travel cm  step cm  jump px     n | face x, y, |err|, res       | draw x, y, |err|, res       | body x, y, |err|, res       | head motion   | draw path', file=out)
    for r in result['rows']:
        motion = '%+6.2f %+6.2f' % r['head_motion'] if 'head_motion' in r else '%13s' % '-'
        paths = r.get('draw_paths')
        share = 'head %.2f engine %.2f masked %.2f' % (paths['head'], paths['engine'], paths['masked']) if paths else '-'
        print('%2d %10.1f %8.4f %8.2f %5d | %s | %s | %s | %s | %s' % (r['frame'], r['travel_cm'], r['step_cm'], r['jump'], r['pairs'],
                                                                     cell(r, 'face'), cell(r, 'draw'), cell(r, 'body'), motion, share), file=out)
    code, lines = verdict(result, face_limit, draw_limit)
    for line in lines:
        print(line, file=out)
    return code


# ---- the self-test: a synthetic dump with the errors planted, in the file formats the real dumps use -----------------------------------------------
class _Fixture:
    WIDTH, HEIGHT = 170, 170
    ORIGIN = (20, 10)
    INPUT = (210, 190)
    FACE = (70, 40, 110, 90)     # x0, y0, x1, y1 in the crop
    RING = (58, 28, 122, 102)
    BODY = (50, 135, 130, 168)
    IDS = dict(skin=13, helmet=5, suit=9)

    def __init__(self, root, stamp='111111111001', helmet_fixed=False, planted=None, step_frame=None, owners=True, engine=True, skin_format=2):
        import numpy as np
        from PIL import Image
        self.root, self.stamp = root, stamp
        eyes = os.path.join(root, 'eyes')
        pool = os.path.join(root, 'pool')
        os.makedirs(eyes)
        os.makedirs(pool)
        planted = planted or {}
        rng = np.random.default_rng(7)
        base = rng.random((420, 420))
        for _ in range(2):   # smooth enough to warp, sharp enough to lock on
            base = (base + np.roll(base, 1, 0) + np.roll(base, 1, 1) + np.roll(base, -1, 0) + np.roll(base, -1, 1)) / 5
        texture = (base - base.min()) / (base.max() - base.min()) * 255.0
        h, w = self.HEIGHT, self.WIDTH
        yy, xx = np.mgrid[0:h, 0:w]

        def inside(r):
            return (xx >= r[0]) & (xx < r[2]) & (yy >= r[1]) & (yy < r[3])

        face, body = inside(self.FACE), inside(self.BODY)
        ring = inside(self.RING) & ~face
        rows, frames = [], []
        offsets = dict(face=np.zeros(2), ring=np.zeros(2), body=np.zeros(2), back=np.zeros(2))
        jitter_x = [0, 1, 0, 2, 1, 0, 2, 0, 1, 2, 0, 1, 0, 2, 1, 0]
        jitter_y = [1, 0, 2, 0, 1, 2, 0, 1, 0, 1, 2, 0, 1, 0, 2, 1]
        for k in range(16):
            j = np.zeros(2) if k == 0 else np.array([-(jitter_x[k] - jitter_x[k - 1]), -(jitter_y[k] - jitter_y[k - 1])], dtype=float)
            head = np.array([1.0, 0.0])   # the head and helmet move a pixel a frame on their own; the body and the room do not
            for name, true in (('face', head), ('ring', head), ('body', np.zeros(2)), ('back', np.zeros(2))):
                offsets[name] = offsets[name] + true + j
            colour = np.zeros((h, w), np.float32)
            corner = dict(face=(10, 10), ring=(150, 20), body=(20, 200), back=(180, 180))
            for name, mask in (('back', ~(face | ring | body)), ('ring', ring), ('body', body), ('face', face)):
                sy = (yy[mask] + int(offsets[name][1]) + corner[name][1]) % 420
                sx = (xx[mask] + int(offsets[name][0]) + corner[name][0]) % 420
                colour[mask] = texture[sy, sx]
            raw_name = 'eye_%s_C%02d.bmp' % (stamp, k)
            Image.fromarray(colour.astype(np.uint8)).convert('RGB').save(os.path.join(eyes, raw_name), 'BMP')
            d = np.zeros((h, w, 4), np.float32)
            d[..., 3] = PATH_WORLD
            d[body, 3] = PATH_HEAD
            d[ring, 3] = (PATH_ENGINE | (KIND_JOINED << KIND_SHIFT)) if helmet_fixed else PATH_HEAD
            d[ring, 0] = 1.0 if helmet_fixed else 0.0
            if engine:
                d[face, 3] = PATH_ENGINE | (KIND_JOINED << KIND_SHIFT)
                error = planted.get(k, (0.0, 0.0))
                d[face, 0] = 1.0 - error[0]
                d[face, 1] = -error[1]
                if k == step_frame:
                    d[face & (yy >= 65), 0] += 1.0
            else:
                d[face, 3] = PATH_HEAD
            decision_name = 'eye_%s_D%02d.bin' % (stamp, k)
            self._texture(os.path.join(eyes, decision_name), d, 2, k)
            frames.append(dict(index=k, frame=k, decision_file=decision_name, raw_file=raw_name, decision_crop=[self.ORIGIN[0], self.ORIGIN[1], w, h],
                               input_size=list(self.INPUT), output_size=[self.INPUT[0] * 2, self.INPUT[1] * 2]))
            row = dict(frame=k, eye=0, crop=k, jitter0=jitter_x[k], jitter1=jitter_y[k])
            for i in range(12):
                row['prev%d' % i] = 0.0
                row['now%d' % i] = 0.0
            row['prev3'], row['now3'] = 3.0, 0.0   # 3 m of camera travel, 300 cm: half float's step is 0.25 cm
            rows.append(row)
        with open(os.path.join(eyes, 'eye_%s_decisions.json' % stamp), 'w', encoding='utf-8') as f:
            json.dump(dict(frames=frames), f)
        with open(os.path.join(eyes, 'eye_%s_motion.csv' % stamp), 'w', encoding='utf-8', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            writer.writerows(rows)
        e = np.zeros((h, w, 4), np.float32 if skin_format == 2 else np.float16)
        e[face] = (-300.0, 0.0, 0.0, 1.0)
        self._texture(os.path.join(eyes, 'eye_%s_SkinE.bin' % stamp), e, skin_format, 0)
        if owners:
            owner = np.zeros((self.INPUT[1], self.INPUT[0]), np.uint32)
            ox, oy = self.ORIGIN
            owner[oy:oy + h, ox:ox + w][inside((self.RING[0] + 2, self.RING[1] + 2, self.RING[2] - 2, self.RING[3] - 2)) & ~face] = self.IDS['helmet']
            owner[oy:oy + h, ox:ox + w][body] = self.IDS['suit']
            owner[oy:oy + h, ox:ox + w][face] = self.IDS['skin']
            self._texture(os.path.join(pool, 'depthwriter_%s_e0.bin' % stamp), owner, 42, 1)
            with open(os.path.join(pool, 'depthwriter_%s.json' % stamp), 'w', encoding='utf-8') as f:
                json.dump(dict(version=1, draws=[
                    dict(id=self.IDS['skin'], eye=0, vs='D99AFDC250D19A3F', count=22434),
                    dict(id=self.IDS['helmet'], eye=0, vs='01DA82F8D0EA0C99', count=16122),
                    dict(id=self.IDS['suit'], eye=0, vs='01DA82F8D0EA0C99', count=23430),
                    dict(id=self.IDS['helmet'] + 100, eye=1, vs='01DA82F8D0EA0C99', count=16122)]), f)

    @staticmethod
    def _texture(path, array, fmt, frame):
        import struct
        h, w = array.shape[:2]
        pixel = array.dtype.itemsize * (array.shape[2] if array.ndim == 3 else 1)
        with open(path, 'wb') as f:
            f.write(b'EDVRTEX1' + struct.pack('<9I', 1, w, h, fmt, w * pixel, frame, 0, 0, 0) + array.tobytes())


def _files_state(root):
    state = {}
    for folder, _, names in os.walk(root):
        for name in names:
            p = os.path.join(folder, name)
            s = os.stat(p)
            state[p] = (s.st_size, s.st_mtime_ns)
    return state


def self_test():
    checks = []

    def expect(condition, what):
        checks.append((bool(condition), what))
        if not condition:
            print('FAIL: %s' % what)

    def near(a, b, tol=0.3):
        return abs(a - b) <= tol

    with tempfile.TemporaryDirectory() as tmp:
        # Before the helmet fix: the face exact, one frame's face wrong by a planted error, the helmet missing the head's own motion.
        root = os.path.join(tmp, 'before')
        _Fixture(root, planted={7: (2.0, -1.0)}, step_frame=9)
        state = _files_state(root)
        result = measure(root, '111111111001')
        by_frame = {r['frame']: r for r in result['rows']}
        expect(result['scale'] == 2.0, 'the scale comes from the manifest: output over input')
        expect(result['has_owners'], 'the depth writer map separates the regions')
        expect(sorted(by_frame) == list(range(4, 16)), 'the frames from --first to the end are measured')
        for k in (4, 5, 6, 8, 10, 15):
            expect(by_frame[k]['face']['error'] <= 0.3, 'frame %d: a right motion reads about zero, with the jitter taken out (read %.2f)' % (k, by_frame[k]['face']['error']))
        planted = by_frame[7]['face']
        expect(near(planted['x'], 4.0) and near(planted['y'], -2.0), 'frame 7: the planted error reads back with its sign, in native px (read %+.2f %+.2f)' % (planted['x'], planted['y']))
        for k in (5, 8, 12):
            helmet = by_frame[k]['draw']
            expect(near(helmet['x'], 2.0) and near(helmet['y'], 0.0), 'frame %d: the helmet misses the head\'s own pixel a frame, 2 native px (read %+.2f %+.2f)' % (k, helmet['x'], helmet['y']))
            expect(near(by_frame[k]['head_motion'][0], 2.0), 'frame %d: the head motion the face\'s vectors claim' % k)
            expect(by_frame[k]['body']['error'] <= 0.3, 'frame %d: the still body reads zero' % k)
            expect(by_frame[k]['draw_paths']['head'] > 0.99 and by_frame[k]['draw_paths']['masked'] == 0.0, 'frame %d: the helmet is on the head path' % k)
        expect(near(by_frame[9]['jump'], 1.0, 0.01) and by_frame[9]['pairs'] > 0, 'frame 9: a step inside the rigid face is found (read %.2f)' % by_frame[9]['jump'])
        expect(by_frame[8]['jump'] < 0.01 and by_frame[8]['pairs'] == 0, 'frame 8: no step on a rigid face')
        expect(near(by_frame[8]['travel_cm'], 300.0, 0.01) and by_frame[8]['step_cm'] == 0.25, 'the camera travel and its half-float step')
        expect(result['skin_e']['format'] == 2 and near(result['skin_e']['median_cm'], 300.0, 0.01), 'SkinE is read at 32 bits')
        code, lines = verdict(result, FACE_LIMIT, DRAW_LIMIT)
        expect(code == 3 and any('face: worst' in s for s in lines) and any('draw: worst' in s for s in lines), 'errors past the limits exit 3 and name the regions')
        expect(_files_state(root) == state, 'measuring writes nothing')

        # After the helmet fix: everything right, the helmet on the engine path.
        root = os.path.join(tmp, 'after')
        _Fixture(root, helmet_fixed=True)
        result = measure(root, '111111111001')
        code, lines = verdict(result, FACE_LIMIT, DRAW_LIMIT)
        expect(code == 0, 'a right dump exits 0: %s' % lines)
        expect(all(r['draw_paths']['engine'] > 0.99 for r in result['rows']), 'the fixed helmet is on the engine path')
        expect(all(r['draw']['error'] <= 0.3 for r in result['rows']), 'the fixed helmet reads zero')

        # A dump from before target 7 went to 32 bits still reads.
        root = os.path.join(tmp, 'old')
        _Fixture(root, skin_format=10)
        expect(measure(root, '111111111001')['skin_e']['format'] == 10, 'an old dump\'s 16-bit SkinE reads')

        # No depth writer map: the face is every engine-path pixel and there is no draw region; --draw that matches nothing is the same.
        root = os.path.join(tmp, 'plain')
        _Fixture(root, owners=False)
        result = measure(root, '111111111001')
        expect(not result['has_owners'] and all('draw' not in r for r in result['rows']), 'no map: no draw region')
        expect(all(r['face']['error'] <= 0.3 for r in result['rows']), 'no map: the face is still measured')
        root = os.path.join(tmp, 'other')
        _Fixture(root)
        expect(all('draw' not in r for r in measure(root, '111111111001', draw='ABCDEF00')['rows']), 'a --draw that matches nothing has no region')
        expect(all('draw' in r for r in measure(root, '111111111001', draw='01da82f8:16122')['rows']), '--draw takes either case, and an index count')

        # Nothing to measure, and nothing to read.
        root = os.path.join(tmp, 'empty')
        _Fixture(root, engine=False, owners=False)
        try:
            measure(root, '111111111001')
            expect(False, 'no engine-path pixels is an error')
        except DumpError as e:
            expect('no skinned character' in str(e), 'no engine-path pixels says so')
        try:
            newest_stamp(os.path.join(tmp, 'absent'))
            expect(False, 'a missing folder is an error')
        except DumpError:
            expect(True, 'a missing folder is an error')
        try:
            measure(os.path.join(tmp, 'after'), '999')
            expect(False, 'an unknown stamp is an error')
        except DumpError:
            expect(True, 'an unknown stamp is an error')
        expect(newest_stamp(os.path.join(tmp, 'after')) == '111111111001', 'the newest run is found')

        # The whole path through main and its exit codes.
        import contextlib
        import io
        sink = io.StringIO()
        with contextlib.redirect_stdout(sink):
            expect(main(['--logs', os.path.join(tmp, 'before'), '--stamp', '111111111001']) == 3, 'main exits 3 past a limit')
            expect(main(['--logs', os.path.join(tmp, 'after')]) == 0, 'main exits 0 inside the limits, the newest run by default')
            expect(main(['--logs', os.path.join(tmp, 'empty')]) == 2, 'main exits 2 with nothing to measure')
        printed = sink.getvalue()
        expect('head 1.00 engine 0.00' in printed and 'face: worst' in printed, 'the report prints the helmet\'s path and the verdict')
        expect(half_float_step(703.0) == 0.5 and half_float_step(-300.0) == 0.25 and half_float_step(0.0) == 0.0, 'half float\'s step')
    failed = sum(1 for ok, _ in checks if not ok)
    if failed:
        print('character_motion_check self-test: %d of %d checks failed' % (failed, len(checks)))
        return 1
    print('character_motion_check self-test: %d checks passed' % len(checks))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--target', default=None, help='steam, frontier or a game directory; the logs folder is found the way edvr_log.py finds it')
    ap.add_argument('--logs', default=None, help='an edvr_logs folder, instead of --target')
    ap.add_argument('--stamp', default=None, help='the eye run; the newest when not given')
    ap.add_argument('--draw', default=DEFAULT_DRAW, help='vs hash prefix, then optionally :index count (default %(default)s, the helmet and headset)')
    ap.add_argument('--first', type=int, default=4, help='the first frame to measure (default %(default)s)')
    ap.add_argument('--face-limit', type=float, default=FACE_LIMIT, help='native px (default %(default)s)')
    ap.add_argument('--draw-limit', type=float, default=DRAW_LIMIT, help='native px (default %(default)s)')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.first < 1:
        ap.error('--first must be at least 1: frame n is measured against frame n-1')
    logs = args.logs
    if not logs:
        import edvr_log
        logs = edvr_log.log_dir_for(edvr_log.resolve_target(args.target or 'steam'))
    try:
        stamp = args.stamp or newest_stamp(logs)
        return report(measure(logs, stamp, args.first, args.draw.upper()), args.face_limit, args.draw_limit)
    except DumpError as e:
        print('[edvr] %s' % e)
        return 2


if __name__ == '__main__':
    sys.exit(main())
