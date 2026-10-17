#!/usr/bin/env python3
"""Validate an EDVR F83 stride-two SkinE/MV/EngineSlots point lattice."""
import argparse
import json
from pathlib import Path
import struct
import tempfile

COUNT, STRIDE, SAMPLE_BYTES = 16, 2, 20
FORMATS = (10, 34, 16)  # R16G16B16A16_FLOAT, R16G16_FLOAT, R32G32_FLOAT
STATUSES = {'not_reached', 'staged', 'missing_input', 'source_shape', 'source_format',
            'dimensions_changed', 'budget', 'shader_unavailable', 'source_bound_output',
            'scratch_create_failed', 'stage_create_failed', 'capture_fault', 'join_mismatch',
            'map_not_ready', 'write_failed', 'written', 'native_inactive'}


def read(path):
    path = Path(path)
    data = path.read_bytes()
    if len(data) < 44 or data[:8] != b'EDVRJMP1':
        raise ValueError('Invalid jump lattice header')
    version, count, stride, sample_bytes, skin, motion, slots = struct.unpack_from('<7I', data, 8)
    budget = struct.unpack_from('<Q', data, 36)[0]
    if (version, count, stride, sample_bytes) != (1, COUNT, STRIDE, SAMPLE_BYTES) or (skin, motion, slots) != FORMATS:
        raise ValueError('Unsupported jump lattice layout or formats')
    if budget != 320 * 1024 * 1024:
        raise ValueError('Unexpected run budget')
    binary_rows, offset, total = [], 44, 0
    last_index = -1
    while offset < len(data):
        if len(data) - offset < 52:
            raise ValueError('Truncated jump lattice row')
        fields = struct.unpack_from('<9IQQ', data, offset)
        index, scene, render, epoch, width, height, sw, sh, status, sequence, nbytes = fields
        offset += 52
        if (index <= last_index or index >= COUNT or status != 15 or not width or not height or
                (sw, sh) != ((width + 1)//2, (height + 1)//2) or not sequence):
            raise ValueError('Invalid or unordered written-row metadata')
        expected = sw * sh * SAMPLE_BYTES
        if nbytes != expected or total + nbytes > budget or offset + nbytes > len(data):
            raise ValueError('Invalid payload extent or aggregate budget')
        binary_rows.append(dict(index=index, scene_frame=scene, render_frame=render, epoch=epoch,
                                width=width, height=height, sample_width=sw, sample_height=sh,
                                sequence=sequence, payload=data[offset:offset+nbytes]))
        offset += nbytes
        total += nbytes
        last_index = index
    manifest_path = path.with_suffix('.json')
    try:
        manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError('Missing or invalid lattice manifest') from exc
    if (manifest.get('schema') != 1 or manifest.get('stride') != STRIDE or
            manifest.get('sample_record_bytes') != SAMPLE_BYTES or manifest.get('budget_bytes') != budget or
            manifest.get('data_file') != path.name):
        raise ValueError('Manifest header does not match binary')
    descriptors = manifest.get('frames')
    if not isinstance(descriptors, list) or len(descriptors) != COUNT:
        raise ValueError('Manifest must describe all scheduled indices')
    by_index = {}
    for desc in descriptors:
        i = desc.get('index')
        if not isinstance(i, int) or i in by_index or i < 0 or i >= COUNT or desc.get('status') not in STATUSES:
            raise ValueError('Invalid, duplicate, or unknown manifest descriptor')
        by_index[i] = desc
    if set(by_index) != set(range(COUNT)):
        raise ValueError('Manifest indices are incomplete')
    written = {row['index']: row for row in binary_rows}
    if manifest.get('data_status') not in ('complete', 'write_failed'):
        raise ValueError('Invalid manifest data status')
    for i, desc in by_index.items():
        status = desc['status']
        if (desc.get('payload_bytes', 0) < 0 or desc.get('binary_layout') != 'row_header_then_interleaved_skin_mv_slots' or
                desc.get('coordinate_rule') != 'source=(2*x,2*y)'):
            raise ValueError('Invalid manifest row descriptor')
        if status == 'written':
            row = written.get(i)
            if row is None:
                raise ValueError('Written manifest row is absent from binary')
            if (desc.get('scheduled') is not True or tuple(desc.get('source_size', ())) != (row['width'], row['height']) or
                    tuple(desc.get('sample_size', ())) != (row['sample_width'], row['sample_height']) or
                    tuple(desc.get(k) for k in ('scene_frame', 'render_frame', 'capture_epoch', 'capture_sequence')) !=
                    (row['scene_frame'], row['render_frame'], row['epoch'], row['sequence']) or
                    desc.get('payload_bytes') != row['sample_width'] * row['sample_height'] * SAMPLE_BYTES):
                raise ValueError('Manifest and binary row identity differ')
        elif i in written or desc.get('payload_bytes') != 0 or desc.get('capture_sequence') != 0:
            raise ValueError('Refused or unattempted row has binary payload metadata')
    if set(written) != {i for i, d in by_index.items() if d['status'] == 'written'}:
        raise ValueError('Binary and manifest written-row sets differ')
    return dict(version=version, stride=stride, sample_bytes=sample_bytes,
                formats=(skin, motion, slots), budget=budget, rows=binary_rows, manifest=manifest)


def self_test():
    budget = 320 * 1024 * 1024
    header = b'EDVRJMP1' + struct.pack('<7IQ', 1, COUNT, STRIDE, SAMPLE_BYTES, *FORMATS, budget)
    payload = bytes(120)
    row = struct.pack('<9IQQ', 0, 7, 19, 2, 5, 3, 3, 2, 15, 44, len(payload)) + payload
    frames = []
    for i in range(COUNT):
        frames.append(dict(index=i, scheduled=True, scene_frame=7+i, render_frame=19+i,
                           capture_epoch=2, capture_sequence=44 if i == 0 else 0, source_size=[5, 3], sample_size=[3, 2],
                           status='written' if i == 0 else 'budget', payload_bytes=len(payload) if i == 0 else 0,
                           coordinate_rule='source=(2*x,2*y)', binary_layout='row_header_then_interleaved_skin_mv_slots'))
    manifest = dict(schema=1, stamp='TEST', reason='end', data_file='test.bin', stride=STRIDE,
                    sample_record_bytes=SAMPLE_BYTES, budget_bytes=budget, frames=frames, data_status='complete')
    with tempfile.TemporaryDirectory() as td:
        path = Path(td) / 'test.bin'
        meta = path.with_suffix('.json')
        path.write_bytes(header + row)
        meta.write_text(json.dumps(manifest), encoding='utf-8')
        parsed = read(path)
        assert len(parsed['rows']) == 1 and parsed['rows'][0]['payload'] == payload
        cases = []
        cases.append((header[:-1] + row, manifest))
        cases.append((header + row[:-1], manifest))
        cases.append((header + row + row, manifest))
        cases.append((header + struct.pack('<9IQQ', 0, 7, 19, 2, 5, 3, 3, 2, 15, 0, 120) + payload, manifest))
        mismatch = json.loads(json.dumps(manifest)); mismatch['frames'][0]['capture_epoch'] = 3; cases.append((header + row, mismatch))
        incomplete = json.loads(json.dumps(manifest)); incomplete['frames'].pop(); cases.append((header + row, incomplete))
        for bad_data, bad_manifest in cases:
            path.write_bytes(bad_data)
            meta.write_text(json.dumps(bad_manifest), encoding='utf-8')
            try:
                read(path)
            except ValueError:
                pass
            else:
                raise AssertionError('invalid jump lattice accepted')
        path.write_bytes(header)
        refused = json.loads(json.dumps(manifest))
        refused['frames'][0]['status'] = 'join_mismatch'; refused['frames'][0]['payload_bytes'] = 0
        refused['frames'][0]['capture_sequence'] = 0
        meta.write_text(json.dumps(refused), encoding='utf-8')
        assert read(path)['rows'] == []
    print('eye jump lattice reader self-test passed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('file', nargs='?')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
    if args.file:
        result = read(args.file)
        print(json.dumps({k: len(v) if k == 'rows' else v for k, v in result.items()}, indent=2))
    elif not args.self_test:
        parser.error('file or --self-test is required')


if __name__ == '__main__':
    main()
