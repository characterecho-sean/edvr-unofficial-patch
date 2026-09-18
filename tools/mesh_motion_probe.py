#!/usr/bin/env python3
"""Measure conservative mesh-motion optimisation opportunities offline.

The current and previous files are EDVRMSH1 buffers: an eight-byte magic,
little-endian uint32 record count and stride, then 240-byte records.  Extended
raw fields are used only when a version-1 MeshProbe.json marker authenticates
the pair.  This distinction matters because those words were unused in older
dumps and can contain zeroes without carrying raw-pose meaning.

The tool never writes analysis files.  Its report (or --json document) goes to
stdout.  Coverage and scene depth use the existing EDVRTEX1 eye-input format.

Examples:
  python tools/mesh_motion_probe.py --mesh eye_120000_Mesh.bin
  python tools/mesh_motion_probe.py --mesh eye_120000_Mesh.bin --json
  python tools/mesh_motion_probe.py --mesh current.bin --previous old.bin \
      --probe MeshProbe.json --coverage MeshCoverage.bin --scene-depth SceneZ.bin
  python tools/mesh_motion_probe.py --self-test
"""
from __future__ import annotations

import argparse
from array import array
from collections import defaultdict
import json
import math
from pathlib import Path
import struct
import sys
import tempfile


MESH_MAGIC = b'EDVRMSH1'
MESH_STRIDE = 240
MESH_HEADER = struct.Struct('<8sII')
EYE_MAGIC = b'EDVRTEX1'
EYE_HEADER = struct.Struct('<8s9I')
MAX_RECORDS = 1_000_000


class ProbeError(ValueError):
    pass


def _u32(value, name):
    if type(value) is not int or not 0 <= value <= 0xffffffff:
        raise ProbeError(f'MeshProbe.json: {name} is not a uint32')
    return value


def read_mesh(path):
    path = Path(path)
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise ProbeError(f'{path}: {exc}') from exc
    if len(data) < MESH_HEADER.size:
        raise ProbeError(f'{path}: truncated EDVRMSH1 header')
    magic, count, stride = MESH_HEADER.unpack_from(data)
    if magic != MESH_MAGIC:
        raise ProbeError(f'{path}: not an EDVRMSH1 dump')
    if stride != MESH_STRIDE:
        raise ProbeError(f'{path}: record stride {stride}, expected {MESH_STRIDE}')
    if count > MAX_RECORDS:
        raise ProbeError(f'{path}: implausible record count {count}')
    expected = MESH_HEADER.size + count * stride
    if len(data) != expected:
        kind = 'truncated' if len(data) < expected else 'has trailing bytes'
        raise ProbeError(f'{path}: {kind} (expected {expected}, found {len(data)})')
    records = []
    for i in range(count):
        raw = data[MESH_HEADER.size + i * stride:MESH_HEADER.size + (i + 1) * stride]
        words = struct.unpack_from('<32I', raw)
        meta = struct.unpack_from('<4f', raw, 224)
        records.append({
            'raw': raw,
            'key': raw[:80],                 # uint[0..19], persistent compatibility key
            'pose': raw[80:104],             # uint[20..25], raw scale/quaternion/position
            'origin': raw[104:116],           # uint[26..28], scene[275].xyz bits
            'first': words[29],
            'instances': words[30],
            'pool': words[31],                # diagnostic only; never persistent identity
            'clip': raw[128:176],
            'valid': meta[0] == 1.0,
            'meta': meta,
        })
    return {'path': path, 'count': count, 'records': records}


def _auto_sibling(mesh_path, suffix):
    name = mesh_path.name
    if name.endswith('_Mesh.bin'):
        return mesh_path.with_name(name[:-9] + suffix)
    return mesh_path.with_name(suffix.lstrip('_'))


def _optional_path(value, automatic):
    if value == 'auto':
        return automatic, True
    if value is None:
        return None, False
    return Path(value), False


def read_probe_marker(path, current_path):
    try:
        marker = json.loads(Path(path).read_text(encoding='utf-8'))
    except OSError as exc:
        raise ProbeError(f'{path}: {exc}') from exc
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise ProbeError(f'{path}: invalid JSON: {exc}') from exc
    if not isinstance(marker, dict) or marker.get('schema') != 1:
        raise ProbeError(f'{path}: unsupported MeshProbe schema')
    if marker.get('recordFormat') != 'EDVRMSH1' or marker.get('recordStride') != MESH_STRIDE:
        raise ProbeError(f'{path}: incompatible record format or stride')
    capture = marker.get('capture')
    current = marker.get('current')
    previous = marker.get('previous')
    if not all(isinstance(v, dict) for v in (capture, current, previous)):
        raise ProbeError(f'{path}: missing capture/current/previous object')
    if capture.get('frame') is not None:
        _u32(capture.get('frame'), 'capture.frame')
    for key in ('meshFrame', 'width', 'height', 'writeSlot'):
        _u32(capture.get(key), f'capture.{key}')
    eye = _u32(capture.get('eye'), 'capture.eye')
    if eye > 1 or capture.get('matched') is not True or not capture['width'] or not capture['height']:
        raise ProbeError(f'{path}: invalid capture eye, dimensions, or matched flag')
    if capture['writeSlot'] > 1:
        raise ProbeError(f'{path}: invalid capture.writeSlot')
    for section, value in (('current', current), ('previous', previous)):
        _u32(value.get('count'), f'{section}.count')
        filename = value.get('file')
        if not isinstance(filename, str) or not filename or Path(filename).name != filename:
            raise ProbeError(f'{path}: {section}.file must be a leaf filename')
    if current['file'] != Path(current_path).name:
        raise ProbeError(f'{path}: marker current.file does not name --mesh')
    return marker


def validate_groups(records):
    """Return exact draw ranges carried in authenticated raw words."""
    n = len(records)
    groups = {}
    for i, record in enumerate(records):
        first, count = record['first'], record['instances']
        if not count or first >= n or count > n - first:
            raise ProbeError(f'record {i}: invalid draw range first={first}, count={count}, records={n}')
        if not first <= i < first + count:
            raise ProbeError(f'record {i}: does not lie in its draw range [{first}, {first + count})')
        prior = groups.setdefault(first, count)
        if prior != count:
            raise ProbeError(f'record {i}: inconsistent count for draw starting at {first}')
    ordered = sorted(groups.items())
    cursor = 0
    for first, count in ordered:
        if first != cursor:
            why = 'overlapping' if first < cursor else 'gap before'
            raise ProbeError(f'draw grouping has {why} record {first}')
        for i in range(first, first + count):
            if records[i]['first'] != first or records[i]['instances'] != count:
                raise ProbeError(f'draw starting at {first} has inconsistent member {i}')
        cursor += count
    if cursor != n:
        raise ProbeError(f'draw grouping ends at {cursor}, record count is {n}')
    return [{'first': first, 'count': count} for first, count in ordered]


def compare_records(current, previous, groups):
    current_valid = [i for i, r in enumerate(current) if r['valid']]
    result = {
        'current_total': len(current),
        'current_valid': len(current_valid),
        'current_invalid': len(current) - len(current_valid),
        'comparison_state': 'available',
        'exact_unchanged_unique': 0,
        'changed_or_unmatched_unique': 0,
        'no_previous': 0,
        'duplicate_ambiguous': 0,
        'duplicate_exact_equivalent': 0,
        'duplicate_previous_clip_disagreement_keys': 0,
        'raw_origin_changed_unique': 0,
        'raw_static_candidate_unique': 0,
    }
    state = ['unclassified' if record['valid'] else 'invalid' for record in current]
    if previous is None:
        result['comparison_state'] = 'unavailable'
        result['no_previous'] = len(current_valid)
        for i in current_valid:
            state[i] = 'no_previous'
    elif not previous:
        result['comparison_state'] = 'first_frame'
        result['no_previous'] = len(current_valid)
        for i in current_valid:
            state[i] = 'no_previous'
    else:
        curr_by_key, prev_by_key = defaultdict(list), defaultdict(list)
        for i in current_valid:
            curr_by_key[current[i]['key']].append(i)
        for i, record in enumerate(previous):
            if record['valid']:
                prev_by_key[record['key']].append(i)
        for key, indices in curr_by_key.items():
            old = prev_by_key.get(key, [])
            now_by_pose, old_by_pose = defaultdict(list), defaultdict(list)
            for i in indices:
                now_by_pose[current[i]['pose']].append(i)
            for i in old:
                old_by_pose[previous[i]['pose']].append(i)
            for pose, pose_indices in now_by_pose.items():
                old_pose_indices = old_by_pose.get(pose, [])
                # Repeated geometry keys at distinct poses are useful exact
                # evidence. Ambiguity begins only when the complete key+raw
                # pose repeats, because then no record has persistent identity.
                if len(pose_indices) != 1 or len(old_pose_indices) > 1:
                    result['duplicate_ambiguous'] += len(pose_indices)
                    result['duplicate_exact_equivalent'] += min(len(pose_indices), len(old_pose_indices))
                    for i in pose_indices:
                        state[i] = 'duplicate'
                    if len(old_pose_indices) > 1 and len({previous[i]['clip'] for i in old_pose_indices}) > 1:
                        result['duplicate_previous_clip_disagreement_keys'] += 1
                    continue
                i = pose_indices[0]
                if len(old_pose_indices) == 1:
                    prior = previous[old_pose_indices[0]]
                    origin_same = current[i]['origin'] == prior['origin']
                    result['exact_unchanged_unique'] += 1
                    if not origin_same:
                        result['raw_origin_changed_unique'] += 1
                        state[i] = 'raw_pose_candidate_origin_changed'
                    else:
                        result['raw_static_candidate_unique'] += 1
                        state[i] = 'raw_static_candidate'
                else:
                    result['changed_or_unmatched_unique'] += 1
                    state[i] = 'unmatched'

    draw_counts = {'total': len(groups),
                   'fully_exact_raw_pose_candidate': 0, 'mixed_exact_raw_pose_candidate': 0,
                   'no_exact_raw_pose_candidate': 0,
                   'fully_raw_static_candidate': 0, 'mixed': 0,
                   'no_raw_static_candidate': 0, 'with_invalid_records': 0}
    reissue_records = 0
    for group in groups:
        statuses = state[group['first']:group['first'] + group['count']]
        static = statuses.count('raw_static_candidate')
        raw_pose = static + statuses.count('raw_pose_candidate_origin_changed')
        invalid = statuses.count('invalid')
        if invalid:
            draw_counts['with_invalid_records'] += 1
        if raw_pose == group['count']:
            draw_counts['fully_exact_raw_pose_candidate'] += 1
        elif raw_pose:
            draw_counts['mixed_exact_raw_pose_candidate'] += 1
        else:
            draw_counts['no_exact_raw_pose_candidate'] += 1
        if static == group['count']:
            draw_counts['fully_raw_static_candidate'] += 1
            reissue_records += group['count']
        elif static:
            draw_counts['mixed'] += 1
        else:
            draw_counts['no_raw_static_candidate'] += 1
    draw_counts['exact_raw_pose_record_upper_bound'] = sum(
        value in ('raw_static_candidate', 'raw_pose_candidate_origin_changed') for value in state)
    draw_counts['record_work_upper_bound'] = state.count('raw_static_candidate')
    draw_counts['reissue_draw_upper_bound'] = draw_counts['fully_raw_static_candidate']
    draw_counts['reissue_record_upper_bound'] = reissue_records
    return result, draw_counts, state


def read_eye(path):
    path = Path(path)
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise ProbeError(f'{path}: {exc}') from exc
    if len(data) < EYE_HEADER.size:
        raise ProbeError(f'{path}: truncated EDVRTEX1 header')
    fields = EYE_HEADER.unpack_from(data)
    magic, values = fields[0], fields[1:]
    if magic != EYE_MAGIC:
        raise ProbeError(f'{path}: not an EDVRTEX1 capture')
    keys = ('version', 'width', 'height', 'format', 'row_bytes', 'frame', 'eye', 'ui_bound', 'ui_flags')
    meta = dict(zip(keys, values))
    pixel_bytes = {10: 8, 16: 8, 27: 4, 28: 4, 29: 4, 34: 4, 39: 4, 40: 4,
                   41: 4, 42: 4, 19: 8, 20: 8, 44: 4, 45: 4, 53: 2, 55: 2, 61: 1}
    bpp = pixel_bytes.get(meta['format'])
    if meta['version'] != 1 or not meta['width'] or not meta['height'] or bpp is None:
        raise ProbeError(f'{path}: unsupported version, dimensions, or DXGI format')
    if meta['row_bytes'] != meta['width'] * bpp:
        raise ProbeError(f'{path}: invalid packed row extent')
    expected = EYE_HEADER.size + meta['row_bytes'] * meta['height']
    if len(data) != expected:
        raise ProbeError(f'{path}: incomplete or trailing EDVRTEX1 payload')
    return meta, memoryview(data)[EYE_HEADER.size:]


def _native_array(typecode, payload):
    values = array(typecode)
    values.frombytes(payload)
    if sys.byteorder != 'little':
        values.byteswap()
    return values


def read_coverage(path):
    meta, payload = read_eye(path)
    if meta['format'] != 16:
        raise ProbeError(f'{path}: MeshCoverage must be DXGI_FORMAT_R32G32_FLOAT (16)')
    values = _native_array('f', payload)
    for i in range(0, len(values), 2):
        if not math.isfinite(values[i]) or not math.isfinite(values[i + 1]):
            raise ProbeError(f'{path}: non-finite coverage sample at pixel {i // 2}')
    return meta, values


def read_depth(path):
    meta, payload = read_eye(path)
    fmt = meta['format']
    if fmt in (39, 40, 41):
        values = _native_array('f', payload)
    elif fmt in (19, 20):
        pairs = _native_array('f', payload)
        values = array('f', (pairs[i] for i in range(0, len(pairs), 2)))
    elif fmt in (44, 45):
        packed = _native_array('I', payload)
        values = array('f', ((v & 0xffffff) / 16777215.0 for v in packed))
    elif fmt in (53, 55):
        packed = _native_array('H', payload)
        values = array('f', (v / 65535.0 for v in packed))
    else:
        raise ProbeError(f'{path}: SceneZ has unsupported depth format {fmt}')
    if any(not math.isfinite(v) for v in values):
        raise ProbeError(f'{path}: SceneZ contains non-finite depth')
    return meta, values


def analyse_coverage(coverage, record_count, scene=None):
    meta, values = coverage
    depth_meta, depth = scene if scene else (None, None)
    if scene:
        for key in ('width', 'height', 'frame', 'eye'):
            if depth_meta[key] != meta[key]:
                raise ProbeError(f'SceneZ {key} does not match MeshCoverage')
    counts = [0] * record_count
    agrees = [0] * record_count if scene else None
    covered = eligible = 0
    for pixel in range(meta['width'] * meta['height']):
        raw_index, cov_z = values[2 * pixel], values[2 * pixel + 1]
        index = int(raw_index + .5)
        if abs(raw_index - index) > 1e-5 or index < 0 or index > record_count:
            raise ProbeError(f'MeshCoverage pixel {pixel}: invalid record index {raw_index!r}')
        if not index:
            continue
        covered += 1
        counts[index - 1] += 1
        if scene and abs(depth[pixel] - cov_z) <= abs(cov_z) * 1e-6:
            eligible += 1
            agrees[index - 1] += 1
    records = []
    for i, pixels in enumerate(counts):
        item = {'index': i, 'pixels': pixels}
        if scene:
            item['depth_agreeing_pixels'] = agrees[i]
        records.append(item)
    return {
        'available': True,
        'width': meta['width'], 'height': meta['height'], 'frame': meta['frame'], 'eye': meta['eye'],
        'covered_pixels': covered,
        'covered_records': sum(v != 0 for v in counts),
        'zero_coverage_records': sum(v == 0 for v in counts),
        'depth_agreement_available': bool(scene),
        'depth_agreeing_pixels': eligible if scene else None,
        'depth_disagreeing_pixels': covered - eligible if scene else None,
        'records': records,
    }


def _vector(value, size, name, nullable=False):
    if not isinstance(value, list) or len(value) != size:
        raise ProbeError(f'MeshFallback.json: {name} must have {size} values')
    out = []
    for v in value:
        if v is None and nullable:
            out.append(None)
        elif type(v) not in (int, float) or not math.isfinite(v):
            raise ProbeError(f'MeshFallback.json: {name} contains an invalid number')
        else:
            out.append(float(v))
    return out


def _int_vector(value, size, name):
    if not isinstance(value, list) or len(value) != size or any(type(v) is not int for v in value):
        raise ProbeError(f'MeshFallback.json: {name} must have {size} integers')
    return list(value)


def _float4_array(value, count, name):
    if not isinstance(value, list) or len(value) != count:
        raise ProbeError(f'MeshFallback.json: {name} must have {count} float4 values')
    return [_vector(row, 4, f'{name}[{i}]', nullable=True) for i, row in enumerate(value)]


def read_fallback(path):
    try:
        value = json.loads(Path(path).read_text(encoding='utf-8'))
    except OSError as exc:
        raise ProbeError(f'{path}: {exc}') from exc
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise ProbeError(f'{path}: invalid JSON: {exc}') from exc
    if not isinstance(value, dict) or value.get('schema') != 1 or type(value.get('available')) is not bool:
        raise ProbeError(f'{path}: unsupported MeshFallback schema')
    if not value['available']:
        if not isinstance(value.get('reason'), str):
            raise ProbeError(f'{path}: unavailable marker has no reason')
        return value
    for key in ('frame', 'eye', 'flags'):
        _u32(value.get(key), key)
    value['region'] = _int_vector(value.get('region'), 4, 'region')
    value['size'] = _int_vector(value.get('size'), 2, 'size')
    value['texSize'] = _int_vector(value.get('texSize'), 2, 'texSize')
    for key, size in (('jit', 4), ('knobs', 4), ('tvUsed', 4), ('tvCam', 4), ('split', 4), ('objects', 4),
                      ('ships', 4), ('probe', 4), ('tvSt', 4)):
        value[key] = _vector(value.get(key), size, key, nullable=True)
    if any(v is None for key in ('jit', 'knobs', 'tvUsed', 'tvCam', 'split', 'objects', 'ships', 'probe', 'tvSt')
           for v in value[key]):
        raise ProbeError(f'{path}: required fallback constants are non-finite')
    for key in ('tanNow', 'wR0', 'wR1', 'wR2'):
        value[key] = _vector(value.get(key), 4, key, nullable=True)
        if any(v is None for v in value[key]):
            raise ProbeError(f'{path}: {key} is non-finite')
    ship_count = value['ships'][0]
    if ship_count != int(ship_count) or not 0 <= ship_count <= 8:
        raise ProbeError(f'{path}: ships.x is not a count from 0 through 8')
    if ship_count:
        value['shBox0'] = _float4_array(value.get('shBox0'), 8, 'shBox0')
        value['shBox1'] = _float4_array(value.get('shBox1'), 8, 'shBox1')
        if any(v is None for rows in (value['shBox0'], value['shBox1'])
               for row in rows[:int(ship_count)] for v in row[:3]):
            raise ProbeError(f'{path}: active ship boxes contain non-finite bounds')
    return value


def analyse_fallback(marker, fallback, coverage, scene, static_state):
    if not fallback.get('available'):
        return {'available': False, 'reason': fallback['reason']}
    cmeta, cov = coverage
    zmeta, depth = scene
    if fallback['frame'] != cmeta['frame'] or fallback['eye'] != cmeta['eye']:
        raise ProbeError('MeshFallback frame/eye does not match MeshCoverage')
    if marker and marker['capture']['frame'] is None:
        return {'available': False, 'reason': 'MeshProbe capture.frame is null; exact cross-file pairing is unavailable'}
    if marker and (fallback['frame'] != marker['capture']['frame'] or fallback['eye'] != marker['capture']['eye']):
        raise ProbeError('MeshFallback frame/eye does not match MeshProbe capture')
    tex_w, tex_h = map(int, fallback['texSize'])
    size_w, size_h = map(int, fallback['size'])
    rx, ry = map(int, fallback['region'][:2])
    if (tex_w, tex_h) != (cmeta['width'], cmeta['height']) or (tex_w, tex_h) != (zmeta['width'], zmeta['height']):
        raise ProbeError('MeshFallback texSize does not match coverage/depth')
    if min(size_w, size_h) <= 0 or rx < 0 or ry < 0 or rx + size_w > tex_w or ry + size_h > tex_h:
        raise ProbeError('MeshFallback region/size lies outside its textures')
    knobs, tv_cam, split = fallback['knobs'], fallback['tvCam'], fallback['split']
    objects, ships, probe, tv_st = fallback['objects'], fallback['ships'], fallback['probe'], fallback['tvSt']
    tan_now = fallback['tanNow']
    world_rows = (fallback['wR0'], fallback['wR1'], fallback['wR2'])
    world_on = knobs[1] != 0.0 and tv_cam[3] != 0.0 and split[0] > 0.0
    body_enabled = tv_st[3] != 0.0
    ship_count = int(ships[0])
    body_ship_enabled = body_enabled or ship_count != 0
    scanner = (int(probe[3] + .5) & 128) != 0
    probe_bits = int(probe[3] + .5)
    terrain_holo_enabled = (probe_bits & (8 | 16)) != 0
    screen_enabled = (probe_bits & 32) != 0
    counts = {'static_candidate_pixels': 0, 'static_world_base': 0, 'head_base': 0,
              'menu_assumed_depth_base': 0, 'head_rotation_only_base': 0,
              'body_or_ship_membership_unknown': 0, 'world_with_body_or_ship_membership_unknown': 0,
              'body_grid_membership_unknown': 0, 'ship_box_membership_unknown': 0,
              'ship_box_excluded': 0, 'ui_coverage_membership_unknown': 0,
              'ui_mesh_rejection_membership_unknown': 0,
              'terrain_or_holo_override_unknown': 0, 'screen_override_unknown': 0,
              'later_layer_override_unknown': 0, 'scanner_ui_membership_unknown': 0}
    by_record = defaultdict(lambda: {'static_world_base': 0, 'head_base': 0,
                                     'body_or_ship_membership_unknown': 0})

    def scene_at(x, y):
        return depth[y * tex_w + x]

    # This is the DLSS-preparation mv() path. It calls meshPixel(p, 0), so
    # coverage is fetched at region.xy + integer p. The jittered fetchHistoryT
    # path is intentionally not used for these eye-input dumps.
    for py in range(size_h):
        for px in range(size_w):
            qx, qy = rx + px, ry + py
            at = qy * tex_w + qx
            index = int(cov[2 * at] + .5)
            if not index or index > len(static_state) or static_state[index - 1] != 'raw_static_candidate':
                continue
            cov_z = cov[2 * at + 1]
            if cov_z <= knobs[0] or abs(scene_at(qx, qy) - cov_z) > abs(cov_z) * 1e-6:
                continue
            counts['static_candidate_pixels'] += 1
            if probe[2] != 0.0:
                counts['ui_mesh_rejection_membership_unknown'] += 1
            zr = 0.0
            for oy in (-1, 0, 1):
                sy = min(ry + size_h - 1, max(ry, ry + py + oy))
                for ox in (-1, 0, 1):
                    sx = min(rx + size_w - 1, max(rx, rx + px + ox))
                    zr = max(zr, scene_at(sx, sy))
            den = zr - knobs[0]
            far = zr <= 0.0 or den <= 0.0
            z = 0.0 if far else knobs[2] / den
            potential_world = world_on and (far or z > split[0])
            if scanner and potential_world:
                base = 'scanner_ui_membership_unknown'
            elif potential_world:
                base = 'static_world_base'
            elif knobs[1] != 0.0 and not far:
                base = 'head_base'
            elif knobs[1] != 0.0 and far and split[3] != 0.0 and split[2] > 0.0:
                base = 'menu_assumed_depth_base'
            else:
                base = 'head_rotation_only_base'
            counts[base] += 1
            if terrain_holo_enabled:
                counts['terrain_or_holo_override_unknown'] += 1
            if screen_enabled:
                counts['screen_override_unknown'] += 1
            if terrain_holo_enabled or screen_enabled:
                counts['later_layer_override_unknown'] += 1
            if base in ('static_world_base', 'head_base'):
                by_record[index - 1][base] += 1
            potential_override = base != 'scanner_ui_membership_unknown' and body_ship_enabled and not far and (
                base == 'static_world_base' or (base == 'head_base' and z > objects[1]))
            if potential_override:
                body_unknown = body_enabled
                ship_unknown = False
                if ship_count:
                    d = (tan_now[0] + (px + .5) / size_w * (tan_now[1] - tan_now[0]),
                         tan_now[3] - (py + .5) / size_h * (tan_now[3] - tan_now[2]), -1.0)
                    vg = (d[0] * z, d[1] * z, -d[2] * z)
                    world_point = tuple(sum(world_rows[row][axis] * vg[axis] for axis in range(3)) +
                                        world_rows[row][3] for row in range(3))
                    for ship in range(ship_count):
                        low, high = fallback['shBox0'][ship], fallback['shBox1'][ship]
                        if all(low[axis] <= world_point[axis] <= high[axis] for axis in range(3)):
                            ship_unknown = True
                            break
                    if ship_unknown:
                        counts['ship_box_membership_unknown'] += 1
                    else:
                        counts['ship_box_excluded'] += 1
                if body_unknown:
                    counts['body_grid_membership_unknown'] += 1
                if probe[2] != 0.0:
                    counts['ui_coverage_membership_unknown'] += 1
                if body_unknown or ship_unknown:
                    counts['body_or_ship_membership_unknown'] += 1
                    if base == 'static_world_base':
                        counts['world_with_body_or_ship_membership_unknown'] += 1
                    by_record[index - 1]['body_or_ship_membership_unknown'] += 1
    counts['world_base_without_body_ship_uncertainty_upper_bound'] = max(
        0, counts['static_world_base'] - counts['world_with_body_or_ship_membership_unknown'])
    counts['available'] = True
    counts['method'] = 'DLSS mv'
    counts['world_on'] = world_on
    counts['body_or_ship_enabled'] = body_ship_enabled
    counts['records'] = [{'index': i, **values} for i, values in sorted(by_record.items())]
    return counts


def analyse(mesh, previous, marker, coverage=None, scene=None, fallback=None):
    groups = validate_groups(mesh['records']) if marker else []
    if marker:
        if marker['current']['count'] != mesh['count']:
            raise ProbeError('MeshProbe current.count does not match EDVRMSH1')
        if previous is None:
            raise ProbeError('MeshProbe marker exists but its previous dump is unavailable')
        if marker['previous']['count'] != previous['count']:
            raise ProbeError('MeshProbe previous.count does not match EDVRMSH1')
        if previous['records']:
            validate_groups(previous['records'])
        comparison, draws, state = compare_records(mesh['records'], previous['records'], groups)
    else:
        valid = sum(r['valid'] for r in mesh['records'])
        comparison = {'current_total': mesh['count'], 'current_valid': valid,
                      'current_invalid': mesh['count'] - valid, 'comparison_state': 'marker_unavailable',
                      'exact_unchanged_unique': None, 'changed_or_unmatched_unique': None,
                      'no_previous': None, 'duplicate_ambiguous': None,
                      'raw_origin_changed_unique': None, 'raw_static_candidate_unique': None}
        draws, state = {'available': False, 'reason': 'schema marker unavailable; raw words not interpreted'}, []
    report = {
        'schema': 1,
        'limitations': [
            'Counts cover admitted records only; objects rejected by the 512-record cap are absent.',
            'Raw-static and saved-work figures are candidates and upper bounds, not proof that skipping is safe.',
            'Duplicate compatible keys are equivalent/ambiguous and are never reported as persistent identities.',
            'Coverage is exact raster-sample evidence; it is not expanded to rectangles or subpixel area.',
            'Fallback world/head counts describe the DLSS-mv base branch; UI, body-grid, in-box ship, terrain, hologram, and screen membership can leave the actual final vector unknown.',
        ],
        'mesh': {'path': str(mesh['path']), 'records': comparison},
        'probe': {'available': bool(marker), 'capture': marker.get('capture') if marker else None},
        'draws': draws,
        'coverage': {'available': False, 'reason': 'not supplied'},
        'fallback': {'available': False, 'reason': 'not supplied'},
    }
    if marker:
        report['draws']['available'] = True
    if coverage:
        if marker:
            for key in ('width', 'height', 'frame', 'eye'):
                if key == 'frame' and marker['capture'][key] is None:
                    continue
                if coverage[0][key] != marker['capture'][key]:
                    raise ProbeError(f'MeshCoverage {key} does not match MeshProbe capture')
        report['coverage'] = analyse_coverage(coverage, mesh['count'], scene)
    elif scene:
        raise ProbeError('--scene-depth requires --coverage')
    if fallback:
        if not coverage or not scene:
            report['fallback'] = {'available': False, 'reason': 'MeshFallback analysis requires coverage and SceneZ'}
        elif not marker:
            report['fallback'] = {'available': False, 'reason': 'MeshProbe marker unavailable; raw-static candidates unknown'}
        else:
            report['fallback'] = analyse_fallback(marker, fallback, coverage, scene, state)
    return report


def print_report(report):
    rec = report['mesh']['records']
    print(f"mesh: {rec['current_valid']} valid, {rec['current_invalid']} invalid, {rec['current_total']} admitted records")
    if report['probe']['available']:
        print(f"comparison: {rec['comparison_state']}; exact unchanged unique {rec['exact_unchanged_unique']}, "
              f"changed/unmatched unique {rec['changed_or_unmatched_unique']}, no previous {rec['no_previous']}, "
              f"duplicate/ambiguous {rec['duplicate_ambiguous']}")
        print(f"raw origin changed on {rec['raw_origin_changed_unique']} unique compatible pairs; "
              f"raw-static candidates {rec['raw_static_candidate_unique']}")
        if rec['duplicate_ambiguous']:
            print(f"duplicates: {rec['duplicate_exact_equivalent']} current records have an equivalent old raw pose; "
                  f"{rec['duplicate_previous_clip_disagreement_keys']} duplicate keys disagree in previous clip rows")
        draws = report['draws']
        print(f"draws: exact raw pose in every instance {draws['fully_exact_raw_pose_candidate']}, "
              f"mixed {draws['mixed_exact_raw_pose_candidate']}; stricter same-origin whole draws "
              f"{draws['fully_raw_static_candidate']}, mixed {draws['mixed']}")
        print(f"work upper bounds: exact-pose records {draws['exact_raw_pose_record_upper_bound']}; "
              f"same-origin records {draws['record_work_upper_bound']} and whole-draw reissues "
              f"{draws['reissue_draw_upper_bound']}")
    else:
        print('comparison: unavailable (no schema-1 MeshProbe marker; extended raw words were not interpreted)')
    cov = report['coverage']
    if cov['available']:
        print(f"coverage: {cov['covered_records']} records nonzero, {cov['zero_coverage_records']} zero; "
              f"{cov['covered_pixels']} raster pixels")
        if cov['depth_agreement_available']:
            print(f"depth: {cov['depth_agreeing_pixels']} covered pixels agree, "
                  f"{cov['depth_disagreeing_pixels']} do not")
        else:
            print('depth: unavailable (coverage pixels are not called eligible)')
    else:
        print(f"coverage: unavailable ({cov['reason']})")
    fb = report['fallback']
    if fb['available']:
        print(f"fallback among depth-eligible raw-static candidate pixels: world base {fb['static_world_base']}, "
              f"head base {fb['head_base']}, menu-depth {fb['menu_assumed_depth_base']}, "
              f"head rotation only {fb['head_rotation_only_base']}")
        print(f"fallback uncertainty: body/ship membership unknown {fb['body_or_ship_membership_unknown']}, "
              f"scanner UI membership unknown {fb['scanner_ui_membership_unknown']}, "
              f"UI mesh rejection unknown {fb['ui_mesh_rejection_membership_unknown']}, "
              f"later-layer override unknown {fb['later_layer_override_unknown']}")
    else:
        print(f"fallback: unavailable ({fb['reason']})")
    print('scope: admitted records only; raw-static and work saved are candidate upper bounds, never a skip-safe claim')


def _record(key, pose, origin, first, count, pool, valid=True, clip_seed=0.0):
    words = [0] * 32
    words[:20] = key
    words[20:26] = pose
    words[26:29] = origin
    words[29:32] = (first, count, pool)
    clip = [clip_seed + i for i in range(12)]
    mapped = [0.0] * 12
    meta = [1.0 if valid else 0.0, 4.0, 2.0, 0.0]
    return struct.pack('<32I28f', *(words + clip + mapped + meta))


def _mesh_bytes(records):
    return MESH_HEADER.pack(MESH_MAGIC, len(records), MESH_STRIDE) + b''.join(records)


def _eye_bytes(width, height, fmt, frame, eye, payload):
    bpp = len(payload) // (width * height)
    return EYE_HEADER.pack(EYE_MAGIC, 1, width, height, fmt, width * bpp, frame, eye, 0, 0) + payload


def self_test():
    key_a, key_b, key_c = [1] * 20, [2] * 20, [3] * 20
    pose_a, pose_b, pose_changed = [10] * 6, [20] * 6, [21] * 6
    origin, moved_origin = [30] * 3, [31] * 3
    previous_records = [
        _record(key_a, pose_a, origin, 0, 2, 90),
        _record(key_b, pose_b, origin, 0, 2, 91),
        _record(key_c, pose_a, origin, 2, 2, 92, clip_seed=1),
        _record(key_c, pose_a, origin, 2, 2, 93, clip_seed=2),
        _record([4] * 20, pose_a, origin, 4, 1, 94, valid=False),
    ]
    current_records = [
        _record(key_a, pose_a, origin, 0, 2, 7),       # pool repack: still exact
        _record(key_b, pose_changed, origin, 0, 2, 8), # one changing member -> mixed
        _record(key_c, pose_a, origin, 2, 2, 9),
        _record(key_c, pose_a, origin, 2, 2, 10),      # coincident duplicates
        _record([4] * 20, pose_a, origin, 4, 1, 11, valid=False),
        _record([5] * 20, pose_a, moved_origin, 5, 1, 12),
    ]
    previous_records.append(_record([5] * 20, pose_a, origin, 5, 1, 95))
    with tempfile.TemporaryDirectory() as temp_name:
        temp = Path(temp_name)
        mesh_path = temp / 'eye_test_Mesh.bin'
        prev_path = temp / 'eye_test_MeshPrev.bin'
        probe_path = temp / 'eye_test_MeshProbe.json'
        mesh_path.write_bytes(_mesh_bytes(current_records))
        prev_path.write_bytes(_mesh_bytes(previous_records))
        marker = {'schema': 1, 'recordFormat': 'EDVRMSH1', 'recordStride': 240,
                  'capture': {'frame': 17, 'meshFrame': 9, 'eye': 0, 'width': 4, 'height': 2,
                              'writeSlot': 1, 'matched': True},
                  'current': {'file': mesh_path.name, 'count': 6},
                  'previous': {'file': prev_path.name, 'count': 6}}
        probe_path.write_text(json.dumps(marker), encoding='utf-8')
        mesh, prev = read_mesh(mesh_path), read_mesh(prev_path)
        loaded_marker = read_probe_marker(probe_path, mesh_path)
        report = analyse(mesh, prev, loaded_marker)
        rec, draws = report['mesh']['records'], report['draws']
        assert rec['exact_unchanged_unique'] == 2 and rec['raw_static_candidate_unique'] == 1
        assert rec['raw_origin_changed_unique'] == 1 and rec['changed_or_unmatched_unique'] == 1
        assert rec['duplicate_ambiguous'] == 2 and rec['duplicate_exact_equivalent'] == 2
        assert rec['duplicate_previous_clip_disagreement_keys'] == 1
        assert draws['mixed'] == 1 and draws['fully_raw_static_candidate'] == 0
        assert draws['fully_exact_raw_pose_candidate'] == 1  # exact pose, changed origin
        assert draws['with_invalid_records'] == 1

        # A shared geometry key does not make distinct raw poses ambiguous.
        # Pool indices may repack freely because they are coverage lookup only.
        shared = [8] * 20
        distinct_previous = [_record(shared, [30 + i] * 6, origin, 0, 3, 100 + i) for i in range(3)]
        distinct_current = [_record(shared, [30 + i] * 6, origin, 0, 3, 9 - i) for i in range(3)]
        distinct_groups = validate_groups(read_mesh_bytes(_mesh_bytes(distinct_current))['records'])
        dcomp, ddraws, _ = compare_records(
            read_mesh_bytes(_mesh_bytes(distinct_current))['records'],
            read_mesh_bytes(_mesh_bytes(distinct_previous))['records'], distinct_groups)
        assert dcomp['exact_unchanged_unique'] == 3 and dcomp['duplicate_ambiguous'] == 0
        assert ddraws['fully_exact_raw_pose_candidate'] == 1
        distinct_current[2] = _record(shared, [99] * 6, origin, 0, 3, 22)
        ccomp, cdraws, _ = compare_records(
            read_mesh_bytes(_mesh_bytes(distinct_current))['records'],
            read_mesh_bytes(_mesh_bytes(distinct_previous))['records'], distinct_groups)
        assert ccomp['exact_unchanged_unique'] == 2 and ccomp['changed_or_unmatched_unique'] == 1
        assert cdraws['mixed_exact_raw_pose_candidate'] == 1

        # First frame is available evidence with an explicit empty previous file,
        # distinct from a missing/unavailable previous dump.
        empty = temp / 'empty.bin'
        empty.write_bytes(_mesh_bytes([]))
        first_marker = json.loads(json.dumps(marker))
        first_marker['previous']['count'] = 0
        first = analyse(mesh, read_mesh(empty), first_marker)
        assert first['mesh']['records']['comparison_state'] == 'first_frame'
        assert first['mesh']['records']['no_previous'] == 5

        # Coverage IDs are record+1.  One sample is hidden by a different exact
        # scene depth; zero coverage remains different from unavailable coverage.
        cov_pairs = [(1.0, .5), (1.0, .5), (0.0, 0.0), (2.0, .25),
                     (3.0, .125), (0.0, 0.0), (0.0, 0.0), (6.0, .0625)]
        cov_payload = b''.join(struct.pack('<2f', *p) for p in cov_pairs)
        z_values = [.5, .4, 0, .25, .125, 0, 0, .03125]
        coverage_path, scene_path = temp / 'cov.bin', temp / 'z.bin'
        coverage_path.write_bytes(_eye_bytes(4, 2, 16, 17, 0, cov_payload))
        scene_path.write_bytes(_eye_bytes(4, 2, 40, 17, 0, struct.pack('<8f', *z_values)))
        coverage, scene = read_coverage(coverage_path), read_depth(scene_path)
        cov_report = analyse_coverage(coverage, 6, scene)
        assert cov_report['covered_pixels'] == 5 and cov_report['depth_agreeing_pixels'] == 3
        assert cov_report['covered_records'] == 4 and cov_report['zero_coverage_records'] == 2

        fallback_value = {
            'schema': 1, 'available': True, 'frame': 17, 'eye': 0, 'flags': 0,
            'region': [0, 0, 4, 2], 'size': [4, 2], 'texSize': [4, 2],
            'jit': [.75, .75, 0, 0], 'knobs': [0, 1, 1, 0],
            'tvUsed': [0, 0, 0, 0], 'tvCam': [0, 0, 0, 1],
            'split': [10, 0, 0, 0], 'objects': [0, 1, 0, 0],
            'ships': [0, 0, 0, 0], 'probe': [0, 0, 1, 32], 'tvSt': [0, 0, 0, 1],
            'tanNow': [-1, 1, -1, 1], 'wR0': [1, 0, 0, 0],
            'wR1': [0, 1, 0, 0], 'wR2': [0, 0, 1, 0],
        }
        fallback_path = temp / 'fallback.json'
        fallback_path.write_text(json.dumps(fallback_value), encoding='utf-8')
        fallback = read_fallback(fallback_path)
        full = analyse(mesh, prev, loaded_marker, coverage, scene, fallback)
        assert full['fallback']['static_candidate_pixels'] == 1
        assert full['fallback']['head_base'] == 1  # tvUsed.w does not gate mv depth
        assert full['fallback']['body_or_ship_membership_unknown'] == 1
        assert full['fallback']['ui_mesh_rejection_membership_unknown'] == 1
        assert full['fallback']['screen_override_unknown'] == 1
        fallback['split'][0] = 1
        static_state = compare_records(mesh['records'], prev['records'], validate_groups(mesh['records']))[2]
        world = analyse_fallback(loaded_marker, fallback, coverage, scene, static_state)
        assert world['static_world_base'] == 1
        ship_value = json.loads(json.dumps(fallback_value))
        ship_value['split'][0] = 1
        ship_value['tvSt'][3] = 0
        ship_value['ships'][0] = 1
        ship_value['shBox0'] = [[100, 100, 100, 0] for _ in range(8)]
        ship_value['shBox1'] = [[101, 101, 101, 0] for _ in range(8)]
        fallback_path.write_text(json.dumps(ship_value), encoding='utf-8')
        outside_ship = analyse_fallback(loaded_marker, read_fallback(fallback_path), coverage, scene, static_state)
        assert outside_ship['ship_box_excluded'] == 1
        assert outside_ship['body_or_ship_membership_unknown'] == 0

        # Marker absence never activates the raw fields, even though this fixture
        # happens to contain them.
        old = analyse(mesh, None, None)
        assert old['mesh']['records']['comparison_state'] == 'marker_unavailable'
        assert old['mesh']['records']['exact_unchanged_unique'] is None

        malformed = [b'', _mesh_bytes(current_records)[:-1], _mesh_bytes(current_records) + b'x',
                     MESH_HEADER.pack(MESH_MAGIC, 6, 239)]
        for i, data in enumerate(malformed):
            path = temp / f'bad{i}.bin'
            path.write_bytes(data)
            try:
                read_mesh(path)
            except ProbeError:
                pass
            else:
                raise AssertionError('malformed mesh accepted')
        coverage_path.write_bytes(_eye_bytes(4, 2, 16, 17, 0, cov_payload)[:-1])
        try:
            read_coverage(coverage_path)
        except ProbeError:
            pass
        else:
            raise AssertionError('truncated coverage accepted')
        bad_marker = temp / 'bad_marker.json'
        bad_marker.write_text('{"schema":2}', encoding='utf-8')
        try:
            read_probe_marker(bad_marker, mesh_path)
        except ProbeError:
            pass
        else:
            raise AssertionError('unsupported marker accepted')
        broken = list(current_records)
        broken[1] = _record(key_b, pose_b, origin, 1, 2, 0)
        try:
            validate_groups(read_mesh_bytes(_mesh_bytes(broken))['records'])
        except ProbeError:
            pass
        else:
            raise AssertionError('inconsistent group accepted')
    print('Mesh motion probe self-test passed')


def read_mesh_bytes(data):
    """Self-test helper using the production parser without a persistent file."""
    with tempfile.TemporaryDirectory() as temp:
        path = Path(temp) / 'mesh.bin'
        path.write_bytes(data)
        return read_mesh(path)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mesh', help='current EDVRMSH1 Mesh.bin')
    parser.add_argument('--previous', nargs='?', const='auto', default='auto',
                        help='previous EDVRMSH1 dump (default: marker-named sibling; bare flag means auto)')
    parser.add_argument('--probe', nargs='?', const='auto', default='auto',
                        help='schema-1 MeshProbe.json (default/bare flag: auto sibling)')
    parser.add_argument('--coverage', help='optional EDVRTEX1 MeshCoverage.bin')
    parser.add_argument('--scene-depth', help='optional EDVRTEX1 SceneZ.bin (requires --coverage)')
    parser.add_argument('--fallback', nargs='?', const='auto', default='auto',
                        help='schema-1 MeshFallback.json (default/bare flag: auto sibling)')
    parser.add_argument('--json', action='store_true', help='write machine-readable JSON to stdout')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args(argv)
    if args.self_test:
        self_test()
        return 0
    if not args.mesh:
        parser.error('--mesh is required unless --self-test is used')
    try:
        mesh_path = Path(args.mesh)
        mesh = read_mesh(mesh_path)
        probe_path, probe_auto = _optional_path(args.probe, _auto_sibling(mesh_path, '_MeshProbe.json'))
        marker = None
        if probe_path and probe_path.exists():
            marker = read_probe_marker(probe_path, mesh_path)
        elif probe_path and not probe_auto:
            raise ProbeError(f'{probe_path}: explicitly requested MeshProbe marker does not exist')

        previous = None
        if marker:
            automatic_previous = mesh_path.with_name(marker['previous']['file'])
        else:
            automatic_previous = _auto_sibling(mesh_path, '_MeshPrev.bin')
        previous_path, previous_auto = _optional_path(args.previous, automatic_previous)
        if previous_path and previous_path.exists():
            previous = read_mesh(previous_path)
            if marker and previous_path.name != marker['previous']['file']:
                raise ProbeError('explicit previous filename does not match MeshProbe previous.file')
        elif previous_path and not previous_auto:
            raise ProbeError(f'{previous_path}: explicitly requested previous dump does not exist')

        coverage = read_coverage(args.coverage) if args.coverage else None
        scene = read_depth(args.scene_depth) if args.scene_depth else None
        fallback_path, fallback_auto = _optional_path(args.fallback, _auto_sibling(mesh_path, '_MeshFallback.json'))
        fallback = None
        if fallback_path and fallback_path.exists():
            fallback = read_fallback(fallback_path)
        elif fallback_path and not fallback_auto:
            raise ProbeError(f'{fallback_path}: explicitly requested MeshFallback marker does not exist')
        report = analyse(mesh, previous, marker, coverage, scene, fallback)
    except ProbeError as exc:
        print(f'error: {exc}', file=sys.stderr)
        return 2
    if args.json:
        json.dump(report, sys.stdout, indent=2, sort_keys=True)
        print()
    else:
        print_report(report)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
