#!/usr/bin/env python3
"""Read a paired eye run's per-frame DLSS decisions; never writes files.

Pass eye_HHMMSS_decisions.json, optionally --roi X Y WIDTH HEIGHT in D-crop
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

PATHS = {0: 'invalid', 1: 'head', 2: 'world', 3: 'ship', 4: 'body',
         5: 'body2', 6: 'stepped', 7: 'terrain', 8: 'holo', 9: 'mesh', 10: 'screen',
         11: 'engine'}
FLAGS = {16: 'hidden_history', 32: 'screen_invalid_history', 64: 'ui_here',
         128: 'world_available', 256: 'depth_valid', 512: 'tracked_foreground',
         1024: 'projection_valid', 2048: 'static_confirmed'}
KNOWN = 15 | sum(FLAGS)


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
    counts, bits = Counter(), Counter()
    motion_sq = motion_max = 0.0
    invalid = motion_samples = 0
    labels = bytearray()
    for row in range(y, y + h):
        start = 44 + (row * extent[0] + x) * 16
        for mx, my, depth, packed in struct.iter_unpack('<4f', memoryview(data)[start:start + w * 16]):
            if (not all(math.isfinite(v) for v in (mx, my, depth, packed)) or
                    packed < 0 or packed > KNOWN or packed != int(packed) or
                    int(packed) & ~KNOWN or (int(packed) & 15) not in PATHS):
                invalid += 1
                labels.append(255)
                continue
            value = int(packed)
            counts[PATHS[value & 15]] += 1
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
    if not isinstance(stamp, str) or len(stamp) != 6 or not stamp.isascii() or not stamp.isdigit():
        raise ValueError('invalid capture stamp')
    if manifest_path.name != f'eye_{stamp}_decisions.json':
        raise ValueError('manifest filename/stamp mismatch')
    frames = manifest.get('frames')
    if not isinstance(frames, list) or len(frames) > 16 or any(not isinstance(f, dict) for f in frames):
        raise ValueError('invalid manifest frame list')
    result = {'schema': 1, 'stamp': stamp, 'complete': len(frames) == 16, 'frames': [],
              'interpretation': 'Fixed-raster comparisons are not object tracking. P/T differences identify post-DLSS modification, not whether it is erroneous. Capture timing is not a performance benchmark.'}
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
        first = result['frames'][0]
        assert first['path_fraction']['world'] == .25
        assert first['flag_fraction']['hidden_history'] == .25
        assert first['flag_fraction']['static_confirmed'] == .25
        assert first['flag_fraction']['screen_invalid_history'] == .25
        assert first['physical_motion_samples'] == 3
        assert first['physical_motion_rms_px'] == math.sqrt(25 / 3)
        assert first['post_ui_difference']['mean_abs_rgb_255'] == 2
        assert result['frames'][1]['fixed_raster_path_change_fraction'] == 0
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
                        original[:56] + struct.pack('<f', 4096) + original[60:],
                        original[:44] + struct.pack('<f', float('nan')) + original[48:]):
            dpath.write_bytes(payload)
            assert not run()['complete']
        dpath.write_bytes(original)
        assert mapped_output_roi([0, 0, 1, 1], (1, 1, 2, 2), [5, 5], [8, 8], (1, 1, 5, 5)) == (1, 1, 1, 1)
    print('eye_decisions: self-test passed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', nargs='?')
    parser.add_argument('--roi', nargs=4, type=int, metavar=('X', 'Y', 'WIDTH', 'HEIGHT'))
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not args.manifest:
        parser.error('a decision manifest is required')
    try:
        report = analyze(args.manifest, args.roi)
        print(json.dumps(report, indent=2, allow_nan=False))
        return 0 if report['complete'] else 2
    except (ValueError, KeyError, OSError, TypeError) as exc:
        print(f'eye_decisions: {exc}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
