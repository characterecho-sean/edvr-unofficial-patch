"""Validate full-plane prebackend evidence and summarize private coverage masks (read-only)."""
import argparse
import json
import math
from pathlib import Path
import tempfile
import zlib

CAP = 384 << 20
CHUNK = 64 << 20
ROLES = {'color', 'depth', 'clean_color', 'overlay_coverage', 'untrusted_coverage', 'slots'}
BUFFERS = {'pool', 'scene_now', 'scene_previous'}
MODES = {'taa', 'dlaa', 'dlss', 'fsr'}
PARTIAL = {'queued', 'cancelled', 'timeout', 'frame-gap', 'expired-arm', 'budget-cap', 'invalid-input', 'staging-failed', 'map-failed', 'write-failed', 'metadata-write-failed'}

def leaf(root, name):
    p = Path(name)
    if not name or p.is_absolute() or p.name != name or '/' in name or '\\' in name or ':' in name or name in ('.', '..'):
        raise ValueError('unsafe evidence leaf')
    resolved = (root / name).resolve()
    if resolved.parent != root.resolve():
        raise ValueError('evidence leaf escapes capture')
    return resolved

def metadata(path):
    if path.stat().st_size > 4 << 20:
        raise ValueError('metadata cap')
    return json.loads(path.read_text(encoding='utf-8'))

def integer(value, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ValueError('invalid integer bound')
    return value

def sample(path):
    m = metadata(path)
    if m.get('schema') != 'edvr-flat-resolve-inputs' or m.get('version') != 1 or m.get('stage') != 'prebackend' or m.get('backend_status') != 'not-run':
        raise ValueError('prebackend schema/stage mismatch')
    if m['mode'] not in MODES or m['status'] not in PARTIAL | {'complete'} or type(m['complete']) is not bool:
        raise ValueError('mode/status mismatch')
    if m['complete'] != (m['status'] == 'complete'):
        raise ValueError('false completeness')
    integer(m['frame'], 0, (1 << 64)-1)
    w, h = m['render_size'];integer(w, 1, 16384);integer(h, 1, 16384)
    ow, oh = m['output_size'];integer(ow, 1, 16384);integer(oh, 1, 16384)
    if type(m['reset']) is not bool: raise ValueError('invalid reset metadata')
    if type(m['static_scene']) is not bool or type(m['steady_detail']) is not bool: raise ValueError('invalid prep flags')
    integer(m['slot'], 0, 2)
    if type(m['delta_ms']) not in (int, float) or not math.isfinite(m['delta_ms']) or m['delta_ms'] < 0: raise ValueError('invalid frame delta')
    if m['hdr'] is not True:
        raise ValueError('expected HDR input evidence')
    for key in ('jitter', 'rows_jitter', 'previous_jitter', 'previous_rows_jitter'):
        if len(m[key]) != 2 or any(type(v) not in (int, float) or not math.isfinite(v) for v in m[key]):
            raise ValueError('invalid phase metadata')
    if len(m['camera']) != 6 or any(len(row) != 4 or any(type(v) not in (int, float) or not math.isfinite(v) for v in row) for row in m['camera']):
        raise ValueError('invalid current camera metadata')
    if len(m['previous_camera']) != 6 or any(len(row) != 4 or any((v is None and not m['reset']) or (v is not None and (type(v) not in (int, float) or not math.isfinite(v))) for v in row) for row in m['previous_camera']):
        raise ValueError('invalid previous camera metadata')
    textures = m['textures']
    if len(textures) != 6 or {t['name'] for t in textures} != ROLES:
        raise ValueError('texture roles duplicated/missing')
    buffers = m['buffers']
    if len(buffers) != 3 or {b['name'] for b in buffers} != BUFFERS: raise ValueError('engine buffer roles duplicated/missing')
    total = 0; available_total = 0; summaries = []; seen = set()
    for t in textures + buffers:
        name = t['name']; present = t['present']; status = t['status']
        if type(present) is not bool:
            raise ValueError('invalid presence')
        if not present:
            if status != 'absent' or t['files'] or t['byte_size'] != 0 or (m['complete'] and name in ('color', 'depth')):
                raise ValueError('false absent/completeness')
            summaries.append({'name': name, 'status': 'absent'});continue
        if m['complete'] and status != 'complete':
            raise ValueError('false complete subresource')
        if status not in PARTIAL | {'complete', 'invalid-view'}:
            raise ValueError('unknown texture status')
        size = integer(t['byte_size'], 0, (1 << 31));total += size
        if total > CAP and m['status'] in ('complete', 'queued'):
            raise ValueError('sample budget')
        if name in BUFFERS and status in ('complete', 'queued', 'budget-cap'):
            if name == 'pool':
                stride = integer(t['structure_stride'], 1, 2048)
                if t['srv_format'] != 0 or t['srv_dimension'] != 1 or not t['misc_flags'] & 64 or size%stride or t['num_elements'] <= 0 or t['first_element'] < 0 or t['first_element']+t['num_elements'] > size//stride:
                    raise ValueError('structured engine pool view mismatch')
            elif not t['bind_flags'] & 4 or size < 276*16 or size%16:
                raise ValueError('engine scene buffer mismatch')
        elif name not in BUFFERS and status in ('complete', 'queued', 'budget-cap'):
            fmt = t['resource_format']
            allowed = {16} if name == 'slots' else {61} if 'coverage' in name else {19, 39, 44} if name == 'depth' else {26}
            bpp = 1 if fmt == 61 else 8 if fmt in (19,16) else 4
            srv = {19: 21, 39: 41, 44: 46}.get(fmt, fmt)
            if fmt not in allowed or t['srv_format'] != srv or (t['width'], t['height']) != (w, h) or t['row_stride'] != w*bpp or size != w*h*bpp or t['srv_dimension'] != 4:
                raise ValueError('full-plane file layout/format mismatch')
            if (t['mip_levels'], t['array_size'], t['sample_count'], t['most_detailed_mip']) != (1,1,1,0) or t['srv_mip_levels'] not in (1, (1 << 32)-1):
                raise ValueError('one-plane view layout mismatch')
        files = t['files']
        if len(files) > 6 or len(set(files)) != len(files) or any(f in seen for f in files):
            raise ValueError('duplicate/chunk cap')
        seen.update(files);paths = [leaf(path.parent, f) for f in files]
        sizes = [p.stat().st_size for p in paths]
        available_total += sum(sizes)
        if available_total > CAP: raise ValueError('available payload budget')
        if any(n > CHUNK for n in sizes):
            raise ValueError('chunk size cap')
        if status == 'complete' and (not files or sum(sizes) != size):
            raise ValueError('missing/truncated complete payload')
        if sum(sizes) > size:
            raise ValueError('payload excess')
        entry = {'name': name, 'status': status, 'bytes': size, 'available_bytes': sum(sizes)}
        if status == 'complete':
            crc = 0;offset = 0;marked = 0;bounds = [w, h, -1, -1]
            for p in paths:
                with p.open('rb') as source:
                    while data := source.read(1 << 20):
                        crc = zlib.crc32(data, crc)
                        if 'coverage' in name:
                            for i, value in enumerate(data):
                                if value:
                                    x = (offset+i)%w;y = (offset+i)//w;marked += 1
                                    bounds[0] = min(bounds[0], x);bounds[1] = min(bounds[1], y);bounds[2] = max(bounds[2], x);bounds[3] = max(bounds[3], y)
                        offset += len(data)
            if crc != integer(t['crc32'], 0, (1 << 32)-1):
                raise ValueError('payload CRC mismatch')
            if 'coverage' in name:
                entry.update(marked=marked, total=w*h, fraction=marked/(w*h), bounds_inclusive=bounds if marked else None)
        summaries.append(entry)
    return {'file': path.name, 'frame': m['frame'], 'mode': m['mode'], 'stage': m['stage'], 'backend_status': m['backend_status'], 'complete': m['complete'], 'status': m['status'], 'bytes': total, 'textures': summaries}

def inspect(path):
    path = path.resolve()
    if path.is_dir(): path = path/'manifest.json'
    m = metadata(path)
    if m.get('schema') == 'edvr-flat-resolve-inputs': return [sample(path)]
    if m.get('schema') != 'edvr-flat-resolve-input-session' or m.get('version') != 1 or m.get('stage') != 'prebackend' or m.get('backend_status') != 'not-run':
        raise ValueError('session schema')
    integer(m['attempts'], 0, 2);integer(m['pending'], 0, 2)
    if m['status'] not in PARTIAL | {'armed', 'ready', 'partial', 'finished'} or m['pending'] > m['attempts'] or (m['status'] == 'finished' and m['pending']):
        raise ValueError('session status/pending accounting')
    integer(m['byte_cap'], 1, CAP);integer(m['reserved_bytes'], 0, m['byte_cap'])
    if len(m['samples']) != m['attempts'] or len(set(m['samples'])) != len(m['samples']):
        raise ValueError('session accounting')
    rows = [sample(leaf(path.parent, name)) for name in m['samples']]
    if sum(r['bytes'] for r in rows if r['status'] not in ('budget-cap', 'invalid-input', 'staging-failed')) > m['reserved_bytes']:
        raise ValueError('reserved session byte accounting')
    if len({r['frame'] for r in rows}) != len(rows): raise ValueError('duplicate frame')
    return rows

def self_test():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp);data = b'\0\xff\0\xff';(root/'mask.bin').write_bytes(data)
        def texture(name):
            fmt = 16 if name == 'slots' else 61 if 'coverage' in name else 39 if name == 'depth' else 26;bpp = 8 if fmt == 16 else 1 if fmt == 61 else 4
            blob = data if bpp == 1 else b'\0'*(4*bpp);filename = name+'.bin';(root/filename).write_bytes(blob)
            return dict(name=name, present=True, status='complete', resource_format=fmt, srv_format=41 if fmt == 39 else fmt, srv_dimension=4, mip_levels=1, array_size=1, sample_count=1, most_detailed_mip=0, srv_mip_levels=(1 << 32)-1, width=2, height=2, row_stride=2*bpp, byte_size=4*bpp, crc32=zlib.crc32(blob), files=[filename])
        m = dict(schema='edvr-flat-resolve-inputs', version=1, stage='prebackend', backend_status='not-run', frame=1, mode='fsr', status='complete', complete=True, hdr=True, reset=True, static_scene=False, steady_detail=False, slot=0, delta_ms=16, render_size=[2,2], output_size=[2,2], jitter=[0,0], rows_jitter=[0,0], previous_jitter=[0,0], previous_rows_jitter=[0,0], camera=[[0]*4 for _ in range(6)], previous_camera=[[0]*4 for _ in range(6)], textures=[texture(n) for n in sorted(ROLES)], buffers=[dict(name=n, present=False, status='absent', byte_size=0, files=[]) for n in sorted(BUFFERS)])
        p = root/'inputs_frame_1.json';p.write_text(json.dumps(m));before = {f: f.read_bytes() for f in root.iterdir()}
        result = inspect(p);mask = next(t for t in result[0]['textures'] if t['name'] == 'overlay_coverage')
        assert mask['marked'] == 2 and mask['bounds_inclusive'] == [1,0,1,1]
        assert before == {f: f.read_bytes() for f in root.iterdir()}
        session = dict(schema='edvr-flat-resolve-input-session', version=1, stage='prebackend', backend_status='not-run', status='ready', byte_cap=CAP, reserved_bytes=sum(t['byte_size'] for t in m['textures']), attempts=1, pending=0, samples=[p.name])
        sp = root/'manifest.json';sp.write_text(json.dumps(session));assert inspect(sp)[0]['complete']
        session['reserved_bytes'] = 0;sp.write_text(json.dumps(session))
        try: inspect(sp)
        except ValueError: pass
        else: raise AssertionError('false session accounting accepted')
        saved = json.dumps(m)
        def reject():
            p.write_text(json.dumps(m))
            try: inspect(p)
            except (ValueError, OSError): pass
            else: raise AssertionError('corrupt evidence accepted')
        m['textures'][0]['files'] = ['../escape'];reject();m = json.loads(saved)
        m['textures'][0]['status'] = 'queued';reject();m = json.loads(saved)
        f = root/m['textures'][0]['files'][0];original = f.read_bytes();f.write_bytes(b'1'*len(original));reject();f.write_bytes(original)
        f = root/m['textures'][0]['files'][0];f.write_bytes(b'x');reject()
        f.unlink();reject();m = json.loads(saved)
        m['complete'] = False;m['status'] = 'queued'
        for t in m['textures']: t['status'] = 'queued';t['files'] = []
        p.write_text(json.dumps(m));assert not inspect(p)[0]['complete']
        m['backend_status'] = 'success';reject()
    print('flat resolve input reader self-test: PASS')

def main():
    a = argparse.ArgumentParser(description=__doc__);a.add_argument('path', nargs='?', type=Path);a.add_argument('--self-test', action='store_true');a.add_argument('--validate', action='store_true');a.add_argument('--require-complete', action='store_true');a.add_argument('--verify-fixture', type=Path);args = a.parse_args()
    if args.self_test: self_test();return 0
    if args.verify_fixture:
        root = args.verify_fixture.resolve()
        pointer = root/'flat_resolve_inputs_fixture.txt'
        if pointer.stat().st_size > 16384: raise ValueError('fixture pointer cap')
        directory = Path(pointer.read_text(encoding='utf-8')).resolve()
        if not directory.is_relative_to(root/'flat_pixels'): raise ValueError('fixture escaped root')
        rows = inspect(directory)
        if len(rows) != 2 or any(not r['complete'] or r['mode'] != 'dlss' for r in rows): raise ValueError('WARP fixture incomplete')
        for name in ('overlay_coverage', 'untrusted_coverage'):
            mask = next(t for t in rows[0]['textures'] if t['name'] == name)
            if mask['marked'] != 3 or mask['bounds_inclusive'] != [0,0,15,11]: raise ValueError('WARP full-plane mask mismatch')
        print('flat resolve inputs WARP fixture: PASS');return 0
    if not args.path: a.error('capture directory or manifest required')
    rows = inspect(args.path);print(json.dumps(rows, indent=2))
    return 2 if args.require_complete and (not rows or any(not r['complete'] for r in rows)) else 0

if __name__ == '__main__':
    try: raise SystemExit(main())
    except (ValueError, KeyError, OSError, TypeError) as e: print('flat resolve inputs:', e);raise SystemExit(1)
