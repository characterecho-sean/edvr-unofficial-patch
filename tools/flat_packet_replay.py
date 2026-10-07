"""Prepare and run read-only source-packet WARP diagnostics; never validates legacy traces."""
import argparse
import json
import pathlib
import struct
import subprocess
import tempfile

MAX_METADATA = 64 << 20

def shader_hash(paths):
    value = 1469598103934665603
    for path in paths:
        with path.open('rb') as source:
            while block := source.read(1 << 20):
                for byte in block:
                    value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value

def leaf(root, name):
    p = pathlib.Path(name)
    if p.is_absolute() or '..' in p.parts or '/' in name or '\\' in name:
        raise ValueError('unsafe capture leaf')
    path = (root / p).resolve()
    if path.parent != root.resolve():
        raise ValueError('capture leaf escapes directory')
    return path

def prepare(packet):
    if packet.stat().st_size > MAX_METADATA:
        raise ValueError('metadata cap')
    p = json.loads(packet.read_text(encoding='utf-8'))
    if p['schema'] != 'edvr-flat-draw-packet' or p['version'] != 1:
        raise ValueError('packet schema')
    fields = {f['name']: f for f in p['original']}
    if any('OM.RTV' + str(i) + '.resource' in fields for i in range(1, 8)):
        raise ValueError('diagnostic currently supports a single original render target')
    # This diagnostic deliberately assesses original state separately from legacy trace q.
    required = {int(f['value']) for f in fields.values() if f['type'] == 'resource-id' and f['value'] != 'null'}
    for name in ('OM.RTV0.after', 'OM.DSV.after'):
        matches = [r['id'] for r in p['resources'] if r['name'] == name]
        if len(matches) != 1:
            raise ValueError('required after snapshot absent or duplicated: ' + name)
        required.add(matches[0])
    resources = []
    total = 0
    for r in p['resources']:
        if r['id'] not in required:
            continue
        if r['status'] != 'complete':
            raise ValueError('required original resource incomplete: ' + r['name'])
        parts = []
        for s in r['subresources']:
            if s['status'] != 'complete':
                raise ValueError('required subresource incomplete')
            paths = [leaf(packet.parent, f) for f in s['files']]
            if sum(f.stat().st_size for f in paths) != s['bytes']:
                raise ValueError('payload size mismatch')
            if s['bytes'] != s['depth_pitch'] * s['depth'] or s['depth_pitch'] != s['row_pitch'] * s['rows']:
                raise ValueError('packed payload layout mismatch')
            total += s['bytes']
            if total > 1536 << 20:
                raise ValueError('resource budget')
            parts.append((s, paths))
        resources.append((r, parts))
    shaders = []
    for stage in ('VS', 'PS'):
        f = fields[stage + '.bytecode']
        if f['status'] != 'complete':
            raise ValueError('original shader bytecode incomplete')
        paths = [leaf(packet.parent, n) for n in f['value'].split(';')]
        if sum(x.stat().st_size for x in paths) > MAX_METADATA:
            raise ValueError('shader cap')
        if shader_hash(paths) != int(p[stage.lower()], 16):
            raise ValueError('original shader hash mismatch')
        shaders.append(paths)
    out = bytearray(b'EDVRRP01')
    def u(v): out.extend(struct.pack('<I', v))
    def string(v):
        b = str(v).encode('utf-8'); u(len(b)); out.extend(b)
    u(p['draw']['count']); u(p['draw']['start']); out.extend(struct.pack('<i', p['draw']['base']))
    u(p['draw']['instances']); u(p['draw']['start_instance'])
    if p['draw']['kind'] != 'X':
        raise ValueError('currently requires indexed-instanced draw')
    u(len(fields))
    for name, f in fields.items(): string(name); string(f['value'])
    u(len(resources))
    for r, parts in resources:
        u(r['id']); string(r['name']); string(r['type']); string(r['desc_hex']); u(len(parts))
        for s, paths in parts:
            for k in ('subresource', 'row_pitch', 'depth_pitch', 'rows', 'depth', 'bytes'): u(s[k])
            u(len(paths))
            for path in paths: string(path)
    for paths in shaders:
        u(len(paths))
        for path in paths: string(path)
    return out, {'frame': p['frame'], 'q': p['q'], 'vs': p['vs'], 'ps': p['ps'],
                 'resource_bytes': total, 'trace_validated': False, 'packet_complete': p['complete']}

def self_test():
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        assert leaf(root, 'a.bin') == root / 'a.bin'
        for name in ('../x', '/x', 'a/b', 'a\\b'):
            try: leaf(root, name)
            except ValueError: pass
            else: raise AssertionError('escape accepted')
        # Dry-run runs preparation but creates no recipe, directory or output.
        capture = root / 'capture'; capture.mkdir()
        packet = capture / 'p.json'; blob = capture / 'b.bin'; blob.write_bytes(b'1234')
        resource = {'id': 0, 'name': 'OM.RTV0.after', 'status': 'complete', 'type': 'D3D11_BUFFER_DESC', 'desc_hex': '',
                    'subresources': [{'subresource': 0, 'status': 'complete', 'row_pitch': 4, 'depth_pitch': 4, 'rows': 1, 'depth': 1, 'bytes': 4, 'files': ['b.bin']}]}
        r2 = dict(resource, id=1, name='OM.DSV.after')
        raw_hash = format(shader_hash([blob]), 'x')
        data = {'schema': 'edvr-flat-draw-packet', 'version': 1, 'frame': 1, 'q': 2, 'vs': raw_hash, 'ps': raw_hash, 'complete': False,
                'draw': dict(kind='X', count=3, start=0, base=0, instances=1, start_instance=0),
                'original': [dict(name=s+'.bytecode', type='text', status='complete', value='b.bin') for s in ('VS','PS')], 'resources': [resource,r2]}
        packet.write_text(json.dumps(data)); before = sorted(root.rglob('*'))
        originals = {p: p.read_bytes() for p in before if p.is_file()}
        run = subprocess.run([__import__('sys').executable, __file__, str(packet), '--output', str(root/'absent'/'result'), '--dry-run'], capture_output=True)
        assert run.returncode == 0 and sorted(root.rglob('*')) == before, run.stderr
        assert all(p.read_bytes() == content for p, content in originals.items())
        blob.write_bytes(b'4321')
        try: prepare(packet)
        except ValueError as e: assert 'shader hash' in str(e)
        else: raise AssertionError('corrupt shader accepted')
        blob.write_bytes(b'1')
        try: prepare(packet)
        except ValueError: pass
        else: raise AssertionError('truncation accepted')
        blob.unlink()
        try: prepare(packet)
        except OSError: pass
        else: raise AssertionError('missing payload accepted')
    print('flat packet replay Python self-test: PASS')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('packet', nargs='?', type=pathlib.Path)
    parser.add_argument('--output', type=pathlib.Path)
    parser.add_argument('--runner', type=pathlib.Path)
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--self-test', action='store_true')
    a = parser.parse_args()
    if a.self_test: self_test(); return 0
    if not a.packet or not a.output: parser.error('packet and --output required')
    recipe, summary = prepare(a.packet.resolve())
    if a.output.resolve().is_relative_to(a.packet.resolve().parent):
        raise ValueError('output must be outside the source capture')
    print(json.dumps(summary))
    if a.dry_run: return 0
    if not a.runner: parser.error('--runner required except --dry-run')
    a.output.mkdir(parents=True, exist_ok=True)
    path = a.output/'replay.recipe'; path.write_bytes(recipe)
    return subprocess.run([str(a.runner.resolve()), str(path.resolve()), str(a.output.resolve())]).returncode

if __name__ == '__main__':
    try: raise SystemExit(main())
    except (ValueError, KeyError, OSError) as e: print('flat packet replay:', e); raise SystemExit(1)
