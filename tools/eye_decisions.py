#!/usr/bin/env python3
"""Read a paired eye run's per-frame DLSS decisions; never writes files.

Pass eye_<stamp>_decisions.json, optionally --roi X Y WIDTH HEIGHT in D-crop
pixels. D contains pre-hidden-rejection motion XY, predicted prior eye depth
Z (zero when unavailable, including screen motion), and integer flags W. P is pre-UI, T is submitted
colour; both use the identical output crop. Changes at a fixed raster pixel
are NOT object tracking: camera movement and jitter also change membership.
"""
import argparse
from collections import Counter
import json
import math
from pathlib import Path
import struct
import sys

# 4-7 and 9 are retired paths (body, body2, stepped, terrain -- 7 went 2026-10-01 with terrain
# motion -- and mesh), kept so an older dump still reads; the shader writes none of them now. 7 is
# deliberately not reused. 12 is the planet patch motion (2026-10-06): a world-path pixel with a depth
# inside a body's patch volume takes that body's own rigid motion (src/d3d11/celestial_motion.h).
PATHS = {0: 'invalid', 1: 'head', 2: 'world', 3: 'ship', 4: 'body',
         5: 'body2', 6: 'stepped', 7: 'terrain', 8: 'holo', 9: 'mesh', 10: 'screen',
         11: 'engine', 12: 'celestial'}
FLAGS = {16: 'hidden_history', 32: 'screen_invalid_history', 64: 'ui_here',
         128: 'world_available', 256: 'depth_valid', 512: 'tracked_foreground',
         1024: 'projection_valid', 2048: 'static_confirmed'}
ENGINE_KINDS = {0: 'unavailable', 1: 'joined', 2: 'masked', 3: 'nonrig',
                4: 'stale_slot', 5: 'corrupt_slot', 6: 'stale_stamp'}
ENGINE_KIND_SHIFT = 12
ENGINE_KIND_MASK = 7
KNOWN = 15 | sum(FLAGS) | (ENGINE_KIND_MASK << ENGINE_KIND_SHIFT)


def integers(value, count, label, positive=False):
    if not isinstance(value, list) or len(value) != count or any(
            type(v) is not int or v < (1 if positive else 0) for v in value):
        raise ValueError(f'invalid {label}')
    return value


def crop_rect(value, size, label):
    x, y, w, h = integers(value, 4, label)
    if not w or not h or x + w > size[0] or y + h > size[1]:
        raise ValueError(f'{label} outside extent')
    return x, y, w, h


def capture_file(base, name, expected):
    if name != expected:
        raise ValueError(f'wrong capture filename: expected {expected}')
    return base / name


def read_decisions(path, frame, extent):
    data = path.read_bytes()
    if len(data) < 44 or data[:8] != b'EDVRTEX1':
        raise ValueError('invalid decision texture header')
    version, w, h, fmt, row, scene, eye, _, _ = struct.unpack_from('<9I', data, 8)
    if (version, fmt, scene, eye) != (1, 2, frame, 0):
        raise ValueError('decision version/format/frame/eye mismatch')
    if [w, h] != list(extent) or not w or not h or row != w * 16:
        raise ValueError('decision crop/row mismatch')
    if len(data) != 44 + row * h:
        raise ValueError('incomplete decision texture')
    return data


def read_bmp(path, extent):
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b'BM':
        raise ValueError('invalid BMP header')
    offset = struct.unpack_from('<I', data, 10)[0]
    dib, w, h, planes, bits, compression = struct.unpack_from('<IiiHHI', data, 14)
    if dib != 40 or planes != 1 or bits != 24 or compression != 0:
        raise ValueError('expected EDVR 24-bit uncompressed BMP')
    if [w, abs(h)] != list(extent) or w <= 0 or h == 0 or offset < 54:
        raise ValueError('BMP crop mismatch')
    row = (w * 3 + 3) & ~3
    if len(data) != offset + row * abs(h):
        raise ValueError('incomplete BMP')
    return data, offset, row, h


def mapped_output_roi(roi, decision_crop, input_size, output_size, output_crop):
    """Select output pixel centres inside the exact input ROI angular bounds."""
    x, y, w, h = roi
    dx, dy, _, _ = decision_crop
    ox, oy, ow, oh = output_crop
    left = max(0, math.ceil((dx + x) * output_size[0] / input_size[0] - ox - .5))
    top = max(0, math.ceil((dy + y) * output_size[1] / input_size[1] - oy - .5))
    right = min(ow, math.ceil((dx + x + w) * output_size[0] / input_size[0] - ox - .5))
    bottom = min(oh, math.ceil((dy + y + h) * output_size[1] / input_size[1] - oy - .5))
    if right <= left or bottom <= top:
        raise ValueError('ROI has no captured output pixels')
    return left, top, right - left, bottom - top


def colour_difference(before, after, roi):
    x, y, w, h = roi
    total = changed = largest = 0
    for row_y in range(y, y + h):
        rows = []
        for data, offset, stride, signed_height in (before, after):
            source_y = abs(signed_height) - 1 - row_y if signed_height > 0 else row_y
            start = offset + source_y * stride + x * 3
            rows.append(memoryview(data)[start:start + w * 3])
        for pixel in range(w):
            ds = [abs(rows[0][pixel * 3 + c] - rows[1][pixel * 3 + c]) for c in range(3)]
            total += sum(ds)
            changed += max(ds) > 1
            largest = max(largest, max(ds))
    return {'pixels': w * h, 'mean_abs_rgb_255': total / (w * h * 3),
            'changed_over_1_fraction': changed / (w * h), 'max_channel_delta_255': largest}


def summarize(data, extent, roi):
    x, y, w, h = roi
    counts, bits, engine = Counter(), Counter(), Counter()
    motion_sq = motion_max = 0.0
    invalid = motion_samples = 0
    labels = bytearray()
    for row in range(y, y + h):
        start = 44 + (row * extent[0] + x) * 16
        for mx, my, depth, packed in struct.iter_unpack('<4f', memoryview(data)[start:start + w * 16]):
            if (not all(math.isfinite(v) for v in (mx, my, depth, packed)) or
                    packed < 0 or packed > KNOWN or packed != int(packed) or
                    int(packed) & ~KNOWN or (int(packed) & 15) not in PATHS or
                    ((int(packed) >> ENGINE_KIND_SHIFT) & ENGINE_KIND_MASK) not in ENGINE_KINDS):
                invalid += 1
                labels.append(255)
                continue
            value = int(packed)
            counts[PATHS[value & 15]] += 1
            engine[ENGINE_KINDS[(value >> ENGINE_KIND_SHIFT) & ENGINE_KIND_MASK]] += 1
            for bit, label in FLAGS.items():
                if value & bit:
                    bits[label] += 1
            labels.append(value & 15)
            if value & 1024:
                motion_samples += 1
                square = mx * mx + my * my
                motion_sq += square
                motion_max = max(motion_max, math.sqrt(square))
    n = w * h
    return {'pixels': n, 'invalid_pixels': invalid,
            'path_fraction': {label: counts[label] / n for label in PATHS.values()},
            'flag_fraction': {label: bits[label] / n for label in FLAGS.values()},
            'engine_kind_fraction': {label: engine[label] / n for label in ENGINE_KINDS.values()},
            'physical_motion_samples': motion_samples,
            'physical_motion_rms_px': math.sqrt(motion_sq / motion_samples) if motion_samples else None,
            'physical_motion_max_px': motion_max if motion_samples else None}, labels


def analyze(manifest_path, requested_roi=None):
    manifest_path = Path(manifest_path)
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    if (not isinstance(manifest, dict) or type(manifest.get('schema')) is not int or
            manifest.get('schema') != 1 or manifest.get('requested') != 16):
        raise ValueError('unsupported decision manifest schema/requested count')
    stamp = manifest.get('stamp')
    if not isinstance(stamp, str) or len(stamp) not in (6, 12) or not stamp.isascii() or not stamp.isdigit():
        raise ValueError('invalid capture stamp')
    if manifest_path.name != f'eye_{stamp}_decisions.json':
        raise ValueError('manifest filename/stamp mismatch')
    frames = manifest.get('frames')
    if not isinstance(frames, list) or len(frames) > 16 or any(not isinstance(f, dict) for f in frames):
        raise ValueError('invalid manifest frame list')
    result = {'schema': 1, 'stamp': stamp, 'complete': len(frames) == 16, 'frames': [],
              'interpretation': 'Fixed-raster comparisons are not object tracking. P/T differences identify post-DLSS modification, not whether it is erroneous. Capture timing is not a performance benchmark.'}
    encoding = (manifest.get('engine_kind_shift'), manifest.get('engine_kind_mask'))
    if encoding not in ((None, None), (ENGINE_KIND_SHIFT, ENGINE_KIND_MASK)):
        raise ValueError('unsupported engine-kind encoding')
    result['engine_kind_captured'] = encoding == (ENGINE_KIND_SHIFT, ENGINE_KIND_MASK)
    prior_labels = prior_key = None
    seen_frames = set()
    last_frame = 0
    for index, entry in enumerate(frames):
        report = {'index': index, 'frame': entry.get('frame'), 'error': entry.get('error', '')}
        result['frames'].append(report)
        try:
            if entry.get('index') != index or type(entry.get('frame')) is not int or entry['frame'] <= 0:
                raise ValueError('invalid frame index/scene frame')
            if entry['frame'] in seen_frames:
                raise ValueError('duplicate scene frame')
            if entry['frame'] <= last_frame:
                raise ValueError('scene frames out of order')
            if last_frame:
                report['scene_frame_delta'] = entry['frame'] - last_frame
                if report['scene_frame_delta'] != 1:
                    prior_labels = prior_key = None
            seen_frames.add(entry['frame'])
            last_frame = entry['frame']
            for field in ('dlss_success', 'dlss_history', 'dlss_reset'):
                if type(entry.get(field)) is not bool:
                    raise ValueError(f'invalid {field}')
                report[field] = entry[field]
            if entry.get('ui_mode') not in ('none', 'legacy', 'separated', 'deferred'):
                raise ValueError('invalid UI mode')
            report['ui_mode'] = entry['ui_mode']
            if not entry['dlss_success'] or report['error']:
                raise ValueError(report['error'] or 'DLSS treatment unavailable')
            input_size = integers(entry['input_size'], 2, 'input size', True)
            output_size = integers(entry['output_size'], 2, 'output size', True)
            dc = crop_rect(entry['decision_crop'], input_size, 'decision crop')
            oc = crop_rect(entry['output_crop'], output_size, 'output crop')
            roi = crop_rect(list(requested_roi or (0, 0, dc[2], dc[3])), dc[2:], 'ROI')
            if entry.get('diagnostic_frame') != entry['frame']:
                raise ValueError('stale diagnostic frame')
            prefix = f'eye_{stamp}_'
            decision_path = capture_file(manifest_path.parent, entry.get('decision_file'), f'{prefix}D{index:02}.bin')
            raw_path = capture_file(manifest_path.parent, entry.get('raw_file'), f'{prefix}C{index:02}.bmp')
            before_path = capture_file(manifest_path.parent, entry.get('pre_ui_file'), f'{prefix}P{index:02}.bmp')
            after_path = capture_file(manifest_path.parent, entry.get('treated_file'), f'{prefix}T{index:02}.bmp')
            data = read_decisions(decision_path, entry['frame'], dc[2:])
            read_bmp(raw_path, dc[2:])
            summary, labels = summarize(data, dc[2:], roi)
            report.update(summary)
            if not result['engine_kind_captured']:
                report['engine_kind_fraction'] = None  # absent legacy bits are not measured unavailability
            if summary['invalid_pixels']:
                raise ValueError('invalid decision pixels')
            before = read_bmp(before_path, oc[2:])
            after = read_bmp(after_path, oc[2:])
            output_roi = mapped_output_roi(roi, dc, input_size, output_size, oc)
            report['input_roi'] = roi
            report['output_roi'] = output_roi
            report['post_ui_difference'] = colour_difference(before, after, output_roi)
            key = (input_size, dc, roi)
            if prior_labels is not None and key == prior_key:
                report['fixed_raster_path_change_fraction'] = sum(a != b for a, b in zip(labels, prior_labels)) / len(labels)
            prior_labels, prior_key = labels, key
        except (ValueError, KeyError, OSError, TypeError) as exc:
            report['error'] = str(exc)
            result['complete'] = False
            prior_labels = prior_key = None
    return result


# --rim: what DLSS did at a tracked foreground's silhouette (the held weapon on foot), from the same run.
# Needs numpy; the plain report and its self-test stay stdlib. For each consecutive pair it asks which
# history best explains NVIDIA's output P_k at background texels beside the foreground, against four
# candidates: the current input C_k upsampled (what an honoured history-invalid sentinel leaves), P_{k-1}
# fetched along the nearest foreground texel's vector (what nearest-depth vector dilation hands DLSS),
# P_{k-1} unmoved, and P_{k-1} along the texel's own vector. Only informative output pixels count: where the
# foreground fetch differs from both the current input and the unmoved history by more than RIM_CONTRAST.
RIM_FOREGROUND = 11          # decision path: engine-record motion
RIM_BACKGROUND = 2           # decision path: world (camera rows through depth)
RIM_CONTRAST = 25.0          # luma levels (0-255) two candidates must differ by for a pixel to discriminate
RIM_WINDOW = 40              # texels per side of a jitter-residual window


def _rim_luma(np, path, extent):
    data, offset, stride, signed_height = read_bmp(path, extent)
    w, h = extent
    rows = np.frombuffer(data, np.uint8, count=stride * h, offset=offset).reshape(h, stride)[:, :w * 3]
    bgr = rows.reshape(h, w, 3).astype(np.float32)
    if signed_height > 0:
        bgr = bgr[::-1]
    return bgr[..., 2] * 0.299 + bgr[..., 1] * 0.587 + bgr[..., 0] * 0.114


def _rim_bilinear(np, image, x, y):
    h, w = image.shape
    x = np.clip(x, 0, w - 1.001)
    y = np.clip(y, 0, h - 1.001)
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    fx, fy = x - x0, y - y0
    return (image[y0, x0] * (1 - fx) * (1 - fy) + image[y0, x0 + 1] * fx * (1 - fy) +
            image[y0 + 1, x0] * (1 - fx) * fy + image[y0 + 1, x0 + 1] * fx * fy)


def _rim_nearest(np, mask, reach):
    """Chebyshev distance to the nearest True texel (reach + 1 past reach), and that texel's coordinates."""
    h, w = mask.shape
    dist = np.full((h, w), reach + 1, np.int32)
    ny, nx = np.mgrid[0:h, 0:w]
    ys, xs = ny.copy(), nx.copy()
    dist[mask] = 0
    padded = np.pad(mask, reach, constant_values=False)
    for d in range(1, reach + 1):
        for dy in range(-d, d + 1):
            for dx in range(-d, d + 1):
                if max(abs(dx), abs(dy)) != d:
                    continue
                take = padded[reach + dy:reach + dy + h, reach + dx:reach + dx + w] & (dist > d)
                dist[take] = d
                ny[take] = ys[take] + dy
                nx[take] = xs[take] + dx
    return dist, ny, nx


def _rim_erode(np, mask, r):
    h, w = mask.shape
    padded = np.pad(mask, r, constant_values=False)
    out = mask.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            out &= padded[r + dy:r + dy + h, r + dx:r + dx + w]
    return out


def _rim_residual_shift(np, cur, prev, mvx, mvy, ys, xs):
    """Sub-pixel s with cur(x) ~ prev(x + mv + s): what is left after the vectors, i.e. the jitter step."""
    s = np.zeros(2)
    for _ in range(3):
        wx, wy = xs + mvx + s[0], ys + mvy + s[1]
        gx = _rim_bilinear(np, prev, wx + 0.5, wy) - _rim_bilinear(np, prev, wx - 0.5, wy)
        gy = _rim_bilinear(np, prev, wx, wy + 0.5) - _rim_bilinear(np, prev, wx, wy - 0.5)
        r = cur[ys, xs] - _rim_bilinear(np, prev, wx, wy)
        a = np.array([[np.sum(gx * gx), np.sum(gx * gy)], [np.sum(gx * gy), np.sum(gy * gy)]])
        if np.linalg.cond(a) > 50:
            return None
        s += np.linalg.solve(a, np.array([np.sum(gx * r), np.sum(gy * r)]))
    return s if np.all(np.abs(s) < 2.5) else None


def _rim_texels(np, output_extent, input_size, output_size, dc, oc):
    """Output-crop pixel grid, its centres in D-crop texel coordinates (texel centres on integers), and each
    pixel's nearest texel. The eye runs' output crop starts on an odd row (617), so output rows pair with
    texel rows one row late: rows 0, 1, 2, 3 read texels 0, 1, 1, 2."""
    ow, oh = output_extent
    ox, oy = np.meshgrid(np.arange(ow, dtype=np.float64), np.arange(oh, dtype=np.float64))
    ix = (ox + oc[0] + 0.5) * input_size[0] / output_size[0] - dc[0] - 0.5
    iy = (oy + oc[1] + 0.5) * input_size[1] / output_size[1] - dc[1] - 0.5
    tix = np.clip(np.floor(ix + 0.5).astype(np.int64), 0, dc[2] - 1)
    tiy = np.clip(np.floor(iy + 0.5).astype(np.int64), 0, dc[3] - 1)
    return ox, oy, ix, iy, tix, tiy


def _rim_best(np, errors, informative, labels):
    n = int(informative.sum())
    if not n:
        return {'pixels': 0}
    best = np.argmin(np.stack([e[informative] for e in errors]), axis=0)
    return {'pixels': n, 'best': {label: round(float(np.mean(best == i)), 3) for i, label in enumerate(labels)},
            'median_error': {label: round(float(np.median(e[informative])), 1) for label, e in zip(labels, errors)}}


def rim_pair(np, d, cur, prev, out, prev_out, input_size, output_size, dc, oc):
    """One consecutive pair: d is frame k's decisions (h, w, 4) over the D crop, cur/prev the C luma of k and
    k-1 over the same crop, out/prev_out the P luma of k and k-1 over the output crop."""
    flags = d[..., 3].astype(np.int64)
    path = flags & 15
    fg = (path == RIM_FOREGROUND) & ((flags & 512) != 0)
    bg = path == RIM_BACKGROUND
    hidden = (flags & 16) != 0
    mvx, mvy = d[..., 0], d[..., 1]
    h, w = fg.shape
    report = {'foreground_texels': int(fg.sum()), 'background_texels': int(bg.sum())}
    for label, m in (('foreground', fg), ('background', bg)):
        report[label + '_vector'] = [round(float(np.median(mvx[m])), 2), round(float(np.median(mvy[m])), 2)] if m.any() else None
    # How well each path's vectors explain C_k from C_{k-1}: compensated over uncompensated mean error. Well
    # under 1 when the scene moved and the vectors are right; 1.000 exactly when they are all zero.
    ty, tx = np.mgrid[0:h, 0:w]
    warped = _rim_bilinear(np, prev, tx + mvx, ty + mvy)
    fit = {}
    for label, m in (('foreground', _rim_erode(np, fg, 2)), ('background', _rim_erode(np, bg, 2))):
        plain = float(np.mean(np.abs(cur[m] - prev[m]))) if m.any() else 0.0
        fit[label] = round(float(np.mean(np.abs(cur[m] - warped[m]))) / plain, 3) if plain > 0 else None
    report['vector_fit'] = fit
    # Hypothesis 3's signature: a foreground drawn with its own jitter or projection leaves a residual shift
    # unlike the background's. Same jitter: the two residuals agree to the vectors' own error.
    shifts = {'foreground': [], 'background': []}
    masks = {'foreground': _rim_erode(np, fg, 3), 'background': _rim_erode(np, bg & ~hidden, 3)}
    for y0 in range(0, h - RIM_WINDOW + 1, RIM_WINDOW):
        for x0 in range(0, w - RIM_WINDOW + 1, RIM_WINDOW):
            for label, m in masks.items():
                if not m[y0:y0 + RIM_WINDOW, x0:x0 + RIM_WINDOW].all() or np.std(cur[y0:y0 + RIM_WINDOW, x0:x0 + RIM_WINDOW]) < 6:
                    continue
                ys, xs = np.mgrid[y0:y0 + RIM_WINDOW, x0:x0 + RIM_WINDOW]
                ys, xs = ys.ravel(), xs.ravel()
                s = _rim_residual_shift(np, cur, prev, mvx[ys, xs], mvy[ys, xs], ys, xs)
                if s is not None:
                    shifts[label].append(s)
    report['jitter_residual'] = {label: ({'windows': len(v), 'median': [round(float(x), 3) for x in np.median(np.array(v), axis=0)]}
                                         if v else {'windows': 0}) for label, v in shifts.items()}
    ox, oy, ix, iy, tix, tiy = _rim_texels(np, out.shape[::-1], input_size, output_size, dc, oc)
    sx, sy = input_size[0] / output_size[0], input_size[1] / output_size[1]
    current = _rim_bilinear(np, cur, ix, iy)
    def fetch(vx, vy):
        return _rim_bilinear(np, prev_out, ox + vx / sx, oy + vy / sy)
    dist_fg, fny, fnx = _rim_nearest(np, fg, 3)
    dist_bg, bny, bnx = _rim_nearest(np, bg, 1)
    gy, gx = fny[tiy, tix], fnx[tiy, tix]
    by, bx = bny[tiy, tix], bnx[tiy, tix]
    along_fg = fetch(mvx[gy, gx], mvy[gy, gx])
    along_own = fetch(mvx[tiy, tix], mvy[tiy, tix])
    along_bg = fetch(mvx[by, bx], mvy[by, bx])
    e_cur, e_fg, e_zero, e_own, e_bg = (np.abs(out - c) for c in (current, along_fg, prev_out, along_own, along_bg))
    discriminating = (np.abs(along_fg - current) > RIM_CONTRAST) & (np.abs(along_fg - prev_out) > RIM_CONTRAST)
    d_fg, at_bg, at_fg, at_hidden = dist_fg[tiy, tix], bg[tiy, tix], fg[tiy, tix], hidden[tiy, tix]
    labels = ('current', 'foreground_vector', 'unmoved', 'own_vector')
    errors = (e_cur, e_fg, e_zero, e_own)
    report['background_ring_hidden'] = _rim_best(np, errors, at_bg & at_hidden & (d_fg == 1) & discriminating, labels)
    report['background_ring_kept'] = _rim_best(np, errors, at_bg & ~at_hidden & (d_fg == 1) & discriminating, labels)
    report['background_band_hidden'] = _rim_best(np, errors, at_bg & at_hidden & (d_fg >= 2) & (d_fg <= 3) & discriminating, labels)
    report['foreground_rim'] = _rim_best(np, (e_cur, e_own, e_bg), at_fg & (dist_bg[tiy, tix] == 1) &
                                         (np.abs(along_own - along_bg) > RIM_CONTRAST), ('current', 'own_vector', 'background_vector'))
    return report


def rim_report(manifest_path):
    import numpy as np
    manifest_path = Path(manifest_path)
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    if not isinstance(manifest, dict) or manifest.get('schema') != 1:
        raise ValueError('unsupported decision manifest schema')
    stamp = manifest.get('stamp')
    if not isinstance(stamp, str) or len(stamp) not in (6, 12) or not stamp.isdigit() or manifest_path.name != f'eye_{stamp}_decisions.json':
        raise ValueError('invalid capture stamp or manifest filename')
    frames = manifest.get('frames')
    if not isinstance(frames, list) or not 2 <= len(frames) <= 16:
        raise ValueError('a rim report needs two to sixteen frames')
    prefix = f'eye_{stamp}_'
    result = {'stamp': stamp, 'contrast': RIM_CONTRAST, 'pairs': [],
              'interpretation': 'Background ring texels touch the foreground (Chebyshev 1), the band is 2-3 texels out. '
                                'foreground_vector winning at the ring while current wins in the band: NVIDIA dilated the '
                                'foreground vector over the background texel, defeating its own vector or the sentinel. '
                                'vector_fit near 1.000 with motion in the picture: that path\'s vectors were zero. '
                                'jitter_residual foreground unlike background: the foreground carries its own jitter or projection.'}
    previous = None
    for index, entry in enumerate(frames):
        input_size = integers(entry.get('input_size'), 2, 'input size', True)
        output_size = integers(entry.get('output_size'), 2, 'output size', True)
        dc = crop_rect(entry.get('decision_crop'), input_size, 'decision crop')
        oc = crop_rect(entry.get('output_crop'), output_size, 'output crop')
        data = read_decisions(capture_file(manifest_path.parent, entry.get('decision_file'), f'{prefix}D{index:02}.bin'), entry.get('frame'), dc[2:])
        current = {'frame': entry.get('frame'), 'geometry': (tuple(input_size), tuple(output_size), dc, oc),
                   'd': np.frombuffer(data, np.float32, count=dc[2] * dc[3] * 4, offset=44).reshape(dc[3], dc[2], 4),
                   'c': _rim_luma(np, capture_file(manifest_path.parent, entry.get('raw_file'), f'{prefix}C{index:02}.bmp'), dc[2:]),
                   'p': _rim_luma(np, capture_file(manifest_path.parent, entry.get('pre_ui_file'), f'{prefix}P{index:02}.bmp'), oc[2:])}
        if previous and previous['frame'] + 1 == current['frame'] and previous['geometry'] == current['geometry']:
            pair = {'index': index, 'frame': current['frame']}
            pair.update(rim_pair(np, current['d'], current['c'], previous['c'], current['p'], previous['p'], input_size, output_size, dc, oc))
            result['pairs'].append(pair)
        previous = current
    return result


def rim_self_test():
    import numpy as np
    import tempfile
    rng = np.random.default_rng(7)
    w = h = 48
    flags = np.full((h, w), float(RIM_BACKGROUND | 1024), np.float32)
    fg = np.zeros((h, w), bool)
    fg[10:40, 20:36] = True
    flags[fg] = RIM_FOREGROUND | 512 | 1024
    flags[10:40, 14:20] = RIM_BACKGROUND | 1024 | 16           # the disoccluded band left of the foreground
    d = np.zeros((h, w, 4), np.float32)
    d[..., 0] = np.where(fg, -2.0, 6.0)
    d[..., 3] = flags
    cur, prev = (np.rint(rng.uniform(0, 255, (h, w))).astype(np.float32) for _ in range(2))
    prev_out = np.rint(rng.uniform(0, 255, (2 * h, 2 * w))).astype(np.float32)
    # Build P_k the way the measured DLSS behaves: the background ring takes the foreground's vector, the
    # hidden band the current input, the foreground its own vector; then the same through BMP files.
    geometry = ([w, h], [2 * w, 2 * h], (0, 0, w, h), (0, 0, 2 * w, 2 * h))
    ox, oy, ix, iy, tix, tiy = _rim_texels(np, (2 * w, 2 * h), *geometry)
    rows = _rim_texels(np, (4, 4), [2016, 2017], [4032, 4034], (308, 308, 1400, 1400), (616, 617, 2800, 2800))[5][:, 0]
    assert rows.tolist() == [0, 1, 1, 2], rows
    dist, ny, nx = _rim_nearest(np, fg, 3)
    current = _rim_bilinear(np, cur, ix, iy)
    along_fg = _rim_bilinear(np, prev_out, ox + 2 * d[ny[tiy, tix], nx[tiy, tix], 0], oy)
    along_own = _rim_bilinear(np, prev_out, ox + 2 * d[tiy, tix, 0], oy)
    ring = (dist[tiy, tix] == 1) & ~fg[tiy, tix]
    out = np.where(ring, along_fg, np.where(fg[tiy, tix], along_own, current))
    with tempfile.TemporaryDirectory() as directory:
        base = Path(directory)
        stamp = '654321'
        def bmp(name, luma):
            px = np.clip(np.rint(luma), 0, 255).astype(np.uint8)[::-1]
            hh, ww = px.shape
            stride = (ww * 3 + 3) & ~3
            rows = np.zeros((hh, stride), np.uint8)
            rows[:, :ww * 3] = np.repeat(px, 3, axis=1)
            payload = rows.tobytes()
            (base / name).write_bytes(b'BM' + struct.pack('<IHHI', 54 + len(payload), 0, 0, 54) +
                                      struct.pack('<IiiHHIIiiII', 40, ww, hh, 1, 24, 0, len(payload), 0, 0, 0, 0) + payload)
        frames = []
        for k, (c, p, dd) in enumerate(((prev, prev_out, np.zeros_like(d)), (cur, out, d))):
            names = dict(decision_file=f'eye_{stamp}_D{k:02}.bin', raw_file=f'eye_{stamp}_C{k:02}.bmp', pre_ui_file=f'eye_{stamp}_P{k:02}.bmp')
            (base / names['decision_file']).write_bytes(b'EDVRTEX1' + struct.pack('<9I', 1, w, h, 2, w * 16, 100 + k, 0, 0, 0) + dd.tobytes())
            bmp(names['raw_file'], c)
            bmp(names['pre_ui_file'], p)
            frames.append(dict(index=k, frame=100 + k, input_size=geometry[0], output_size=geometry[1],
                               decision_crop=list(geometry[2]), output_crop=list(geometry[3]), **names))
        path = base / f'eye_{stamp}_decisions.json'
        path.write_text(json.dumps(dict(schema=1, stamp=stamp, requested=16, frames=frames)), encoding='utf-8')
        pair = rim_report(path)['pairs'][0]
        assert pair['foreground_vector'] == [-2.0, 0.0] and pair['background_vector'] == [6.0, 0.0], pair
        assert pair['background_ring_hidden']['best']['foreground_vector'] >= .95, pair['background_ring_hidden']
        assert pair['background_ring_kept']['best']['foreground_vector'] >= .95, pair['background_ring_kept']
        assert pair['background_band_hidden']['best']['current'] >= .95, pair['background_band_hidden']
        assert pair['foreground_rim']['best']['own_vector'] >= .95, pair['foreground_rim']
        # An unrelated picture pair: the vectors explain nothing, so neither fit is under 1 by much.
        assert all(v > .9 for v in pair['vector_fit'].values()), pair['vector_fit']
        assert _rim_nearest(np, np.array([[False, True]]), 1)[0].tolist() == [[1, 0]]
        frames[1]['output_crop'] = [0, 1, 2 * w, 2 * h - 1]
        path.write_text(json.dumps(dict(schema=1, stamp=stamp, requested=16, frames=frames)), encoding='utf-8')
        try:
            rim_report(path)
            raise AssertionError('a BMP that no longer matches its crop must be refused')
        except ValueError:
            pass
    print('eye_decisions: rim self-test passed')


def self_test():
    import copy
    import tempfile
    with tempfile.TemporaryDirectory() as directory:
        base = Path(directory)
        stamp = '123456'
        path = base / f'eye_{stamp}_decisions.json'
        frames = []
        for k in range(16):
            frame = dict(index=k, frame=k + 10, diagnostic_frame=k + 10,
                         input_size=[4, 4], decision_crop=[1, 1, 2, 2],
                         output_size=[8, 8], output_crop=[2, 2, 4, 4],
                         decision_file=f'eye_{stamp}_D{k:02}.bin',
                         raw_file=f'eye_{stamp}_C{k:02}.bmp',
                         pre_ui_file=f'eye_{stamp}_P{k:02}.bmp',
                         treated_file=f'eye_{stamp}_T{k:02}.bmp',
                         ui_mode='legacy', dlss_success=True, dlss_history=True,
                         dlss_reset=False, error='')
            frames.append(frame)
            payload = b'EDVRTEX1' + struct.pack('<9I', 1, 2, 2, 2, 32, k + 10, 0, 0, 0)
            payload += struct.pack('<16f', 3, 4, 1, 2 | 16 | 1024 | 2048, 0, 0, 0, 10 | 32,
                                   0, 0, 3, 9 | 512 | 1024, 0, 0, 2, 1 | 64 | 1024)
            (base / frame['decision_file']).write_bytes(payload)
            for field, value in (('pre_ui_file', 10), ('treated_file', 12), ('raw_file', 10)):
                width = 2 if field == 'raw_file' else 4
                stride = (width * 3 + 3) & ~3
                pixels = bytes([value]) * (stride * width)
                bmp = b'BM' + struct.pack('<IHHI', 54 + len(pixels), 0, 0, 54)
                bmp += struct.pack('<IiiHHIIiiII', 40, width, width, 1, 24, 0, len(pixels), 0, 0, 0, 0)
                (base / frame[field]).write_bytes(bmp + pixels)
        manifest = dict(schema=1, requested=16, stamp=stamp, frames=frames)
        def run(value=manifest, roi=None):
            path.write_text(json.dumps(value), encoding='utf-8')
            return analyze(path, roi)
        result = run()
        assert result['complete']
        assert not result['engine_kind_captured']  # old manifests remain readable
        first = result['frames'][0]
        assert first['engine_kind_fraction'] is None
        assert first['path_fraction']['world'] == .25
        assert first['flag_fraction']['hidden_history'] == .25
        assert first['flag_fraction']['static_confirmed'] == .25
        assert first['flag_fraction']['screen_invalid_history'] == .25
        assert first['physical_motion_samples'] == 3
        assert first['physical_motion_rms_px'] == math.sqrt(25 / 3)
        assert first['post_ui_difference']['mean_abs_rgb_255'] == 2
        assert result['frames'][1]['fixed_raster_path_change_fraction'] == 0
        long_stamp = '123456789012'
        long_path = base / f'eye_{long_stamp}_decisions.json'
        long_manifest = copy.deepcopy(manifest)
        long_manifest['stamp'] = long_stamp
        for frame in long_manifest['frames']:
            for field, suffix in (('decision_file','D'),('raw_file','C'),('pre_ui_file','P'),('treated_file','T')):
                old = base / frame[field]
                new_name = f'eye_{long_stamp}_{suffix}{frame["index"]:02}.bin' if suffix=='D' else f'eye_{long_stamp}_{suffix}{frame["index"]:02}.bmp'
                (base / new_name).write_bytes(old.read_bytes())
                frame[field] = new_name
        long_path.write_text(json.dumps(long_manifest), encoding='utf-8')
        assert analyze(long_path)['complete'], 'millisecond/serial run stamps remain readable'
        first = run(roi=[0, 0, 1, 1])['frames'][0]
        assert first['output_roi'] == (0, 0, 2, 2)
        assert first['flag_fraction']['hidden_history'] == 1
        # A change at the top left must not be attributed to a lower building
        # band when reading the BMP's bottom-up rows.
        after_path = base / frames[0]['treated_file']
        original_colour = after_path.read_bytes()
        localized = bytearray((base / frames[0]['pre_ui_file']).read_bytes())
        localized[54 + 3 * 12] += 12
        after_path.write_bytes(localized)
        assert run(roi=[0, 0, 1, 1])['frames'][0]['post_ui_difference']['mean_abs_rgb_255'] == 1
        assert run(roi=[0, 1, 1, 1])['frames'][0]['post_ui_difference']['mean_abs_rgb_255'] == 0
        after_path.write_bytes(original_colour)
        assert not run(roi=[2, 2, 1, 1])['complete']
        for field, value in (('diagnostic_frame', 9), ('frame', 11),
                             ('decision_file', '../bad.bin'), ('error', 'trace_unavailable'),
                             ('raw_file', None),
                             ('dlss_success', False), ('decision_crop', [3, 3, 2, 2])):
            bad = copy.deepcopy(manifest)
            bad['frames'][0][field] = value
            assert not run(bad)['complete'], field
        short = copy.deepcopy(manifest)
        short['frames'].pop()
        assert not run(short)['complete']
        dpath = base / frames[0]['decision_file']
        original = dpath.read_bytes()
        for payload in (original[:-1], original[:28] + struct.pack('<I', 123) + original[32:],
                        original[:56] + struct.pack('<f', 32768) + original[60:],
                        original[:44] + struct.pack('<f', float('nan')) + original[48:]):
            dpath.write_bytes(payload)
            assert not run()['complete']
        dpath.write_bytes(original)
        # Path 12, `celestial`: the second pixel (path 10, screen, above) takes its body's own motion instead. 13 is still no path.
        celestial = bytearray(original)
        struct.pack_into('<f', celestial, 56 + 16, 12 | 1024)
        dpath.write_bytes(celestial)
        again = run()['frames'][0]
        assert again['path_fraction']['celestial'] == .25
        assert again['path_fraction']['screen'] == 0
        assert again['path_fraction']['world'] == .25
        assert again['physical_motion_samples'] == 4
        assert PATHS[12] == 'celestial' and 7 in PATHS and PATHS[7] == 'terrain'
        struct.pack_into('<f', celestial, 56 + 16, 13 | 1024)
        dpath.write_bytes(celestial)
        assert not run()['complete']
        dpath.write_bytes(original)
        # Ownership result is separate from path membership and history flags.
        encoded = bytearray(original)
        for i, kind in enumerate((1, 3, 4, 6)):
            flag, = struct.unpack_from('<f', encoded, 56 + i * 16)
            struct.pack_into('<f', encoded, 56 + i * 16, int(flag) | (kind << ENGINE_KIND_SHIFT))
        dpath.write_bytes(encoded)
        manifest.update(engine_kind_shift=ENGINE_KIND_SHIFT, engine_kind_mask=ENGINE_KIND_MASK)
        augmented_report = run()
        assert augmented_report['engine_kind_captured']
        augmented = augmented_report['frames'][0]
        for label in ('joined', 'nonrig', 'stale_slot', 'stale_stamp'):
            assert augmented['engine_kind_fraction'][label] == .25
        for field in ('path_fraction', 'flag_fraction', 'physical_motion_rms_px',
                      'physical_motion_max_px', 'physical_motion_samples'):
            assert augmented[field] == result['frames'][0][field], field
        struct.pack_into('<f', encoded, 56, 7 << ENGINE_KIND_SHIFT)
        dpath.write_bytes(encoded)
        assert not run()['complete']
        dpath.write_bytes(original)
        assert mapped_output_roi([0, 0, 1, 1], (1, 1, 2, 2), [5, 5], [8, 8], (1, 1, 5, 5)) == (1, 1, 1, 1)
    print('eye_decisions: self-test passed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', nargs='?')
    parser.add_argument('--roi', nargs=4, type=int, metavar=('X', 'Y', 'WIDTH', 'HEIGHT'))
    parser.add_argument('--rim', action='store_true',
                        help='report what DLSS did beside a tracked foreground (the held weapon): needs numpy; '
                             'with --self-test, test that report instead of the stdlib one')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        rim_self_test() if args.rim else self_test()
        return 0
    if not args.manifest:
        parser.error('a decision manifest is required')
    try:
        if args.rim:
            print(json.dumps(rim_report(args.manifest), indent=1, allow_nan=False))
            return 0
        report = analyze(args.manifest, args.roi)
        print(json.dumps(report, indent=2, allow_nan=False))
        return 0 if report['complete'] else 2
    except (ValueError, KeyError, OSError, TypeError) as exc:
        print(f'eye_decisions: {exc}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
