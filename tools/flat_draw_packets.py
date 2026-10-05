#!/usr/bin/env python3
"""Read and validate manually armed flat draw packets without modifying captures.

Packed resource files concatenate logical rows, not driver mapping padding.
Shader hashes use EDVR's raw-byte FNV seed (1469598103934665603).
Exit 0: valid (possibly intentionally partial); 1: corrupt; 2: incomplete
when --require-complete is requested. Payloads are streamed in 1 MiB blocks.
"""
import argparse
import contextlib
import copy
import io
import json
from pathlib import Path, PureWindowsPath
import struct
import sys
import tempfile

META_CAP = 16 << 20
CHUNK_CAP = 64 << 20
BYTE_CAP = 8 << 30
ITEM_CAP = 16384
BLOCK = 1 << 20
FNV_SEED = 1469598103934665603


class CaptureError(ValueError):
    pass


def check(ok, message):
    if not ok:
        raise CaptureError(message)


def number(obj, name, maximum=BYTE_CAP):
    value = obj.get(name)
    check(type(value) is int and 0 <= value <= maximum, f"invalid {name}")
    return value


def array(obj, name):
    value = obj.get(name)
    check(isinstance(value, list) and len(value) <= ITEM_CAP, f"invalid {name} list")
    return value


def leaf(root, name, missing_ok=False):
    check(isinstance(name, str) and name and len(name) <= 240,
          "invalid referenced filename")
    check(not PureWindowsPath(name).drive and not Path(name).is_absolute()
          and '/' not in name and '\\' not in name and ':' not in name
          and name not in ('.', '..'), f"unsafe filename: {name}")
    target = root / name
    check(target.resolve().parent == root.resolve(), f"escaping filename: {name}")
    check(target.is_file() or (missing_ok and not target.exists()), f"missing file: {name}")
    return target


def metadata(path):
    check(path.stat().st_size <= META_CAP, f"metadata too large: {path.name}")
    def unique(pairs):
        result = {}
        for key, value in pairs:
            check(key not in result, f"duplicate JSON key: {key}")
            result[key] = value
        return result
    with path.open('r', encoding='utf-8') as stream:
        value = json.load(stream, object_pairs_hook=unique)
    check(isinstance(value, dict), f"invalid metadata: {path.name}")
    check(value.get('version') == 1, "unsupported metadata version")
    check(type(value.get('complete')) is bool, "missing complete flag")
    return value


def hexhash(value):
    check(isinstance(value, str) and 0 < len(value) <= 16
          and all(c in '0123456789abcdefABCDEF' for c in value), "invalid shader hash")
    return int(value, 16)


class Reader:
    def __init__(self, root):
        self.root = root
        self.bytes = 0
        self.chunks = 0

    def payload(self, names, expected=None, shader=False, failed_write=False):
        check(isinstance(names, list) and len(names) <= ITEM_CAP, "invalid chunk list")
        check(len(set(names)) == len(names), "duplicate chunk file")
        total, digest = 0, FNV_SEED
        for index, name in enumerate(names):
            path = leaf(self.root, name)
            size = path.stat().st_size
            check((0 < size or failed_write) and size <= CHUNK_CAP, f"invalid chunk size: {name}")
            check(index == len(names)-1 or size == CHUNK_CAP,
                  f"short non-final chunk: {name}")
            self.bytes += size
            self.chunks += 1
            check(self.bytes <= BYTE_CAP and self.chunks <= ITEM_CAP, "capture payload exceeds reader bound")
            with path.open('rb') as stream:
                read = 0
                while True:
                    block = stream.read(BLOCK)
                    if not block:
                        break
                    read += len(block)
                    check(read <= size, f"payload changed during read: {name}")
                    if shader:
                        for byte in block:
                            digest = ((digest ^ byte) * 1099511628211) & 0xffffffffffffffff
                check(read == size, f"truncated payload: {name}")
            total += size
        if expected is not None:
            if failed_write and total < expected:
                print(f"  intentional payload omission: bytes={total} declared={expected}")
            else:
                check(total == expected, f"payload bytes {total} != declared {expected}")
        return digest

    def trace(self, name, packet):
        path = leaf(self.root, name)
        check(path.stat().st_size <= 48 + 16 + 65536 * 504, "trace exceeds bound")
        matches = []
        with path.open('rb') as stream:
            header = stream.read(16)
            check(len(header) == 16, "truncated trace header")
            magic, frames, reserved = struct.unpack('<8sII', header)
            check(magic in (b'EDVRFTR3', b'EDVRFTR4') and frames == 1 and reserved == 0,
                  "invalid packet trace header")
            raw = stream.read(48)
            check(len(raw) == 48, "truncated trace frame")
            frame, output, width, height, fmt, count, truncated, produced, contract = struct.unpack('<QQIIIIIIQ', raw)
            check(frame == packet['frame'] and 0 < count <= 65536 and not truncated,
                  "trace frame association or completeness mismatch")
            event_size = 504 if magic == b'EDVRFTR4' else 472
            for _ in range(count):
                event = stream.read(event_size)
                check(len(event) == event_size, "truncated trace event")
                vs, ps = struct.unpack_from('<QQ', event, 120)
                q = struct.unpack_from('<I', event, 196)[0]
                kind = struct.unpack_from('<I', event, 464)[0]
                if kind == 0 and q == packet['q']:
                    matches.append((vs, ps))
            check(not stream.read(1), "trailing trace data")
        check(matches == [(hexhash(packet['vs']), hexhash(packet['ps']))],
              "trace draw q/shader association mismatch")

    def packet(self, path):
        p = metadata(path)
        check(p.get('schema') == 'edvr-flat-draw-packet', "invalid packet schema")
        number(p, 'frame', (1 << 64)-1)
        number(p, 'q', (1 << 32)-1)
        hexhash(p.get('vs')); hexhash(p.get('ps'))
        check(type(p.get('executed')) is bool and isinstance(p.get('reason'), str), "invalid execution/refusal metadata")
        draw = p.get('draw')
        check(isinstance(draw, dict) and draw.get('kind') in ('D', 'N', 'I', 'X', 'A', 'Y', 'Z', '?'), "invalid draw kind")
        for key in ('count', 'start', 'instances', 'start_instance'):
            number(draw, key, (1 << 32)-1)
        check(type(draw.get('base')) is int and -(1 << 31) <= draw['base'] < (1 << 31), "invalid signed base vertex")
        resources = array(p, 'resources')
        ids = set()
        incomplete = not p['executed'] or p.get('trace_status') != 'complete' or draw['kind'] in ('?', 'A')
        for resource in resources:
            check(isinstance(resource, dict), "invalid resource")
            rid = number(resource, 'id', ITEM_CAP)
            check(rid not in ids, "duplicate resource ID")
            ids.add(rid)
            status = resource.get('status')
            check(isinstance(status, str), "invalid resource status")
            incomplete |= status not in ('complete', 'unbound')
            parts = array(resource, 'subresources')
            check(status != 'complete' or parts, "complete resource has no subresources")
            subs = set()
            for part in parts:
                sub = number(part, 'subresource', (1 << 32)-1)
                check(sub not in subs, "duplicate subresource")
                subs.add(sub)
                row = number(part, 'row_pitch')
                pitch = number(part, 'depth_pitch')
                rows = number(part, 'rows')
                depth = number(part, 'depth')
                size = number(part, 'bytes')
                check(rows > 0 and depth > 0 and pitch == row * rows and size == pitch * depth,
                      "invalid packed resource row/depth layout")
                check(status != 'complete' or size > 0, "complete resource has empty bytes")
                part_status = part.get('status', status)
                check(isinstance(part_status, str), 'invalid subresource status')
                incomplete |= part_status != 'complete'
                check(status != 'complete' or part_status == 'complete', 'complete resource has incomplete subresource')
                self.payload(array(part, 'files'), size,
                             failed_write=part_status in ('budget-cap', 'write-failed', 'write-failed-or-cap'))
        check(ids == set(range(len(resources))), "resource IDs are not contiguous")
        for group in ('original', 'executed_pipeline'):
            fields = array(p, group)
            names = {}
            for field in fields:
                check(isinstance(field, dict) and isinstance(field.get('name'), str)
                      and isinstance(field.get('value'), str) and isinstance(field.get('status'), str), "invalid field")
                name = field['name']
                check(name not in names, f"duplicate field: {group}.{name}")
                names[name] = field
                incomplete |= field['status'] != 'complete'
                if field.get('type') == 'resource-id':
                    check(field['value'].isdigit() and int(field['value']) in ids, f"unknown resource reference: {name}")
            for name, field in names.items():
                if name.endswith('.bytecode') and field['status'] == 'complete':
                    hash_field = names.get(name[:-9] + '.hash')
                    check(hash_field is not None, f"shader bytecode has no hash: {name}")
                    names_list = field['value'].split(';') if field['value'] else []
                    check(names_list, f"empty shader bytecode: {name}")
                    digest = self.payload(names_list, shader=True)
                    check(digest == hexhash(hash_field['value']), f"shader hash mismatch: {name}")
            if group == 'original':
                for stage, key in (('VS', 'vs'), ('PS', 'ps')):
                    field = names.get(stage + '.hash')
                    if field:
                        check(hexhash(field['value']) == hexhash(p[key]), "original shader association mismatch")
                    else:
                        incomplete = True
                    incomplete |= stage + '.bytecode' not in names
                if draw['kind'] in ('Y', 'Z'):
                    argument = names.get('draw.indirect_arguments')
                    offset = names.get('draw.indirect_offset')
                    incomplete |= argument is None or offset is None
                    if argument is not None and offset is not None:
                        check(argument.get('type') == 'resource-id', 'invalid indirect argument resource')
                        raw = bytes.fromhex(offset['value'])
                        check(len(raw) == 4, 'invalid indirect argument offset')
                        at = int.from_bytes(raw, 'little')
                        source = resources[int(argument['value'])]
                        desc = bytes.fromhex(source.get('desc_hex', ''))
                        check(source.get('type') == 'D3D11_BUFFER_DESC' and len(desc) == 24,
                              'invalid indirect argument buffer descriptor')
                        width = int.from_bytes(desc[:4], 'little')
                        incomplete |= at % 4 != 0 or at + (20 if draw['kind'] == 'Z' else 16) > width
        if p.get('trace_file'):
            self.trace(p['trace_file'], p)
        else:
            incomplete = True
        check(not p['complete'] or not incomplete, "packet falsely claims complete")
        return p


def inspect(path, field_prefix=None):
    path = Path(path).resolve()
    if path.is_dir():
        path = leaf(path, 'manifest.json')
    check(path.is_file(), "capture path does not exist")
    reader = Reader(path.parent)
    doc = metadata(path)
    if doc.get('schema') == 'edvr-flat-draw-packet':
        packets = [reader.packet(path)]
        complete = packets[0]['complete']
    else:
        check(doc.get('schema') == 'edvr-flat-draw-capture', "invalid capture schema")
        files = array(doc, 'packets')
        check(len(set(files)) == len(files), "duplicate packet reference")
        selected = number(doc, 'selected', ITEM_CAP)
        finished = number(doc, 'finished', ITEM_CAP)
        pending = number(doc, 'pending', ITEM_CAP)
        incomplete = number(doc, 'incomplete', ITEM_CAP)
        check(selected == len(files) and finished + pending == selected and incomplete <= finished,
              "manifest packet accounting mismatch")
        paths = [leaf(path.parent, name, missing_ok=pending > 0) for name in files]
        absent = sum(not target.is_file() for target in paths)
        check(absent == pending, "manifest pending/missing packet accounting mismatch")
        packets = [reader.packet(target) for target in paths if target.is_file()]
        check(sum(not p['complete'] for p in packets) == incomplete, "manifest incomplete packet accounting mismatch")
        associations = [(p['frame'], p['q']) for p in packets]
        check(len(set(associations)) == len(associations), "duplicate frame/q association")
        complete = doc['complete']
        check(not complete or (selected > 0 and pending == 0 and incomplete == 0
              and all(p['complete'] for p in packets)), "manifest falsely claims complete")
        print(f"capture status={doc.get('status')} complete={complete} packets={len(packets)}")
    for p in packets:
        print(f"frame={p['frame']} q={p['q']} VS={p['vs']} PS={p['ps']} executed={p['executed']} complete={p['complete']} reason={p['reason']}")
        print(f"  trace={p.get('trace_file', '')} status={p.get('trace_status', '')}")
        for resource in p['resources']:
            size = sum(part['bytes'] for part in resource['subresources'])
            print(f"  resource {resource['id']} {resource.get('name')} {resource.get('type')} status={resource['status']} bytes={size}")
        if field_prefix is not None:
            for group in ('original', 'executed_pipeline'):
                for field in p[group]:
                    if field['name'].startswith(field_prefix):
                        print(f"  {group}.{field['name']} type={field.get('type')} status={field['status']} value={field['value']}")
    return complete


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', nargs='?', help='capture directory, manifest.json, or packet JSON')
    parser.add_argument('--validate', action='store_true', help='validate referenced metadata and payloads (also performed when displaying)')
    parser.add_argument('--field', metavar='PREFIX', help='display matching original and executed fields')
    parser.add_argument('--require-complete', action='store_true')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args(argv)
    if args.self_test:
        self_test()
        return 0
    if not args.capture:
        parser.error('capture is required unless --self-test is used')
    try:
        complete = inspect(args.capture, args.field)
        return 2 if args.require_complete and not complete else 0
    except (CaptureError, OSError, ValueError, TypeError, KeyError, OverflowError, RecursionError) as error:
        print(f"flat draw packets: {error}", file=sys.stderr)
        return 1


def self_test():
    with tempfile.TemporaryDirectory(prefix='edvr-packets-') as temporary:
        root = Path(temporary)
        shader = b'DXBC fixture raw bytes'
        digest = FNV_SEED
        for byte in shader:
            digest = ((digest ^ byte) * 1099511628211) & 0xffffffffffffffff
        check(digest == 0x31e6ae10fe8a202c, 'raw shader FNV known-vector drift')
        (root / 'shader.bin').write_bytes(shader)
        (root / 'resource.bin').write_bytes(b'12345678')
        event = bytearray(504)
        struct.pack_into('<QQ', event, 120, digest, digest)
        struct.pack_into('<I', event, 196, 7)
        trace = struct.pack('<8sII', b'EDVRFTR4', 1, 0) + struct.pack('<QQIIIIIIQ', 42, 0, 1, 1, 28, 1, 0, 0, 0) + event
        (root / 'trace.bin').write_bytes(trace)
        fields = [{'name': stage + suffix, 'type': 'text', 'status': 'complete', 'value': value}
                  for stage in ('VS', 'PS') for suffix, value in (('.hash', f'{digest:x}'), ('.bytecode', 'shader.bin'))]
        packet = dict(schema='edvr-flat-draw-packet', version=1, frame=42, q=7,
                      vs=f'{digest:x}', ps=f'{digest:x}', complete=True, executed=True,
                      reason='candidate', trace_file='trace.bin', trace_status='complete',
                      draw=dict(kind='D', count=3, start=0, base=0, instances=1, start_instance=0),
                      original=fields + [dict(name='IA.VB0', type='resource-id', status='complete', value='0')],
                      executed_pipeline=[], resources=[dict(id=0, name='IA.VB0', type='D3D11_BUFFER_DESC', status='complete',
                      subresources=[dict(subresource=0, row_pitch=4, depth_pitch=8, rows=2, depth=1, bytes=8, files=['resource.bin'])])])
        manifest = dict(schema='edvr-flat-draw-capture', version=1, complete=True, status='finished',
                        selected=1, finished=1, pending=0, incomplete=0, packets=['packet.json'])
        def save(p=packet, m=manifest):
            (root / 'packet.json').write_text(json.dumps(p), encoding='utf-8')
            (root / 'manifest.json').write_text(json.dumps(m), encoding='utf-8')
        def run(expected, *flags):
            before = {p.name: p.read_bytes() for p in root.iterdir() if p.is_file()}
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                code = main([str(root), *flags])
            check(code == expected, f'self-test exit {code} != {expected}')
            after = {p.name: p.read_bytes() for p in root.iterdir() if p.is_file()}
            check(before == after, 'reader wrote capture files')
        save(); run(0, '--validate', '--field', 'VS', '--require-complete')
        (root / 'packet.json').unlink(); run(1)
        (root / 'manifest.json').write_text(json.dumps(dict(manifest, complete=False, pending=1, finished=0)), encoding='utf-8')
        run(0); run(2, '--require-complete'); save()
        (root / 'resource.bin').unlink(); run(1)
        (root / 'resource.bin').write_bytes(b'123'); run(1)
        (root / 'resource.bin').write_bytes(b'12345678')
        bad = copy.deepcopy(packet); bad['resources'][0]['subresources'][0]['files'] = ['../escape.bin']; save(bad); run(1)
        for unsafe in ('C:\\escape.bin', '/escape.bin'):
            bad['resources'][0]['subresources'][0]['files'] = [unsafe]; save(bad); run(1)
        partial = copy.deepcopy(packet); partial.update(complete=False, executed=False, reason='refused', trace_file='', trace_status='unavailable')
        pm = dict(manifest, complete=False, incomplete=1)
        save(partial, pm); run(0); run(2, '--require-complete')
        save(partial, manifest); run(1)
        partial['complete'] = True; save(partial); run(1)
        save(); (root / 'shader.bin').write_bytes(b'corrupt'); run(1)
        (root / 'shader.bin').write_bytes(shader)
        bad = dict(packet, q=8); save(bad); run(1)
        bad = dict(packet, frame=43); save(bad); run(1)
        save(); altered = bytearray(trace); struct.pack_into('<Q', altered, 64 + 120, digest ^ 1)
        (root / 'trace.bin').write_bytes(altered); run(1)
        (root / 'trace.bin').write_bytes(trace)
        bad = copy.deepcopy(packet); bad['original'][-1]['value'] = '1'; save(bad); run(1)
        bad = copy.deepcopy(packet); bad['resources'][0]['subresources'][0]['depth_pitch'] = 9; save(bad); run(1)
        bad = copy.deepcopy(packet); bad['draw']['base'] = 1 << 31; save(bad); run(1)
        bad = copy.deepcopy(packet); bad['draw']['kind'] = 'A'; save(bad); run(1)
        bad['complete'] = False; save(bad, pm); run(0); run(2, '--require-complete')
        bad = copy.deepcopy(packet); bad['complete'] = False
        bad['resources'][0].update(status='cap', subresources=[]); save(bad, pm); run(0)
        bad['resources'][0].update(status='write-failed-or-cap', subresources=copy.deepcopy(packet['resources'][0]['subresources']))
        (root / 'resource.bin').write_bytes(b'123'); save(bad, pm); run(0)
        bad['resources'][0]['status'] = 'write-failed'
        bad['resources'][0]['subresources'][0]['status'] = 'write-failed'; save(bad, pm); run(0)
        bad['resources'][0]['status'] = 'budget-cap'
        bad['resources'][0]['subresources'][0].update(status='budget-cap', files=[]); save(bad, pm); run(0)
        (root / 'resource.bin').write_bytes(b'12345678')
        save(); (root / 'trace.bin').write_bytes(trace[:-1]); run(1)
        # Some Windows accounts lack symlink privilege. The runtime path check
        # still uses resolve(), so junctions and existing symlinks are rejected.
        with tempfile.TemporaryDirectory(prefix='edvr-packets-outside-') as outside:
            target = Path(outside) / 'outside.bin'
            target.write_bytes(b'12345678')
            try:
                (root / 'escape.bin').symlink_to(target)
            except OSError:
                pass
            else:
                (root / 'trace.bin').write_bytes(trace)
                bad = copy.deepcopy(packet)
                bad['resources'][0]['subresources'][0]['files'] = ['escape.bin']
                save(bad); run(1)
    print('flat draw packet reader self-test passed')


if __name__ == '__main__':
    sys.exit(main())
