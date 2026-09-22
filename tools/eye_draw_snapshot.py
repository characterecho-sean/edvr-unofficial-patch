#!/usr/bin/env python3
"""Read drawstate_HHMMSS.bin from an eye run (eye_draw_snapshot.h).

The E508/63ABD targeting sprite source has a bounded 32 MiB per-texture
capture exception within the unchanged 64 MiB normal texture budget.

Each draw preserves VS b0/b1/b2 and PS b2 at the draw, before buffer reuse.
Solar surface/corona/arc families also retain their VS/PS bytecode and
bounded unpacked geometry/layout for the first three watched frames.
For night vision (FCF7BD2896751D96/F786D34B5E118D5E), buffer 1 holds
PS b1 instead of the unused VS b1: its actual screen/camera constants.
Version 2 preserves draw start/base and bounded HUD/sprite VB0/VB1/IB
payloads. Version 3 copies around the draw's base/start and records each
capture_offset separately from its binding offset, for the first three
watched frames. Earlier captures remain readable.
Version 4 adds source mesh records (ordinal UINT32_MAX-1), first-frame
geometry, and references to frame-local t33/t38/VB0 copies. Each buffer's
first_draw states when it was copied; later references are not new copies.
Source effect records (ordinal UINT32_MAX-2) retain b0/b1/b2 and the
bounded VB0/VB1/IB binding windows throughout the run. They have no mesh
references: billboard/flare placement is independent of the instance pool.
Version 5 adds each effect draw's original input layout so its separate
vertex and instance streams can be decoded without guessing their packing.
Version 6 appends before/after source-effect colour crops, keyed to the
exact draw. These are native pixels from the lower right (up to 1024 square),
not whole images. Original target size, crop origin and HDR format are kept.
Night-vision crops use the same records with a central terrain crop;
always use the saved origin rather than assuming a corner.
Version 7 adds night-vision PS texture inputs as effect phases 2..6 (t0..t4),
with typed formats and native crop origins. BC4 rows contain 8-byte blocks.
Each first-frame night draw also preserves its geometry, input layout,
two sampler descriptors (13 raw uint32 words each) and original viewport.

On-foot source records use ordinal UINT32_MAX. Their DSV is copied when
the screen composite runs, alongside its source colour; depth surfaces
use DSV format IDs 20/40/45/55. Only colour surfaces export to PNG.
The explicit on-foot dump permits 128 MiB per surface / 256 MiB total.
Frames/ordinals join draws_HHMMSS.bin, whose crop map joins the eye images.
Holo t2 surfaces are copied once per resource; their alpha is diagnostic,
not a per-frame history. No resource address is a persistent object ID.
Version 8 adds bounded paired before/after central crops for exact
E508/63ABD sprite draws, plus live PS b1, t0 SRV, sampler, blend,
depth/stencil, raster, viewport/scissor, RT and input-layout descriptors.
The sprite image budget is separate from ordinary effect images.
"""
import argparse
from collections import Counter
import io
import json
from pathlib import Path
import struct
import tempfile


def read(path):
    data = Path(path).read_bytes()
    stream = io.BytesIO(data)

    def take(n):
        b = stream.read(n)
        if len(b) != n:
            raise ValueError('Truncated draw snapshot')
        return b

    def unpack(fmt):
        return struct.unpack(fmt, take(struct.calcsize(fmt)))

    if take(8) != b'EDVRDRW1':
        raise ValueError('Not an EDVRDRW1 snapshot')
    version, nd, ns, dropped = unpack('<4I')
    if version not in (1, 2, 3, 4, 5, 6, 7, 8, 9) or nd > 4096 or ns > 24:
        raise ValueError('Unsupported version or invalid counts')
    draws, surfaces = [], []
    vertex_bytes = 0
    for _ in range(nd):
        d = dict(zip(('vs', 'ps', 'target'), unpack('<3Q')))
        d.update(zip(('frame', 'ordinal', 'kind', 'count', 'instances', 'start_instance',
                      'width', 'height', 'texture'), unpack('<9I')))
        if d['texture'] != 0xffffffff and d['texture'] >= ns:
            raise ValueError('Invalid surface index')
        d['buffers'] = []
        for slot in range(4):
            identity, whole, size = unpack('<QII')
            if size > min(whole, 1024 if slot == 0 else 8192) or size % 16:
                raise ValueError('Invalid constant-buffer payload')
            d['buffers'].append(dict(identity=identity, whole=whole, data=take(size)))
        d['streams'] = []
        if version >= 2:
            d['start'], d['base'] = unpack('<Ii')
            for slot in range(3):
                offset, stride, whole = unpack('<3I')
                capture_offset = unpack('<I')[0] if version >= 3 else offset
                size, = unpack('<I')
                if size > min(max(0, whole-capture_offset), 256*1024):
                    raise ValueError('Invalid vertex payload')
                vertex_bytes += size
                if vertex_bytes > 32*1024*1024:
                    raise ValueError('Vertex payload budget exceeded')
                d['streams'].append(dict(offset=offset, stride=stride, whole=whole, capture_offset=capture_offset, data=take(size)))
        d['mesh'] = list(unpack('<3I')) if version >= 4 else [0xffffffff]*3
        d['layout'] = []
        if version >= 5:
            elements, = unpack('<I')
            if elements > 32:
                raise ValueError('Invalid input layout size')
            for _ in range(elements):
                semantic = take(64)
                if b'\0' not in semantic:
                    raise ValueError('Unterminated input semantic')
                e = dict(zip(('index', 'format', 'slot', 'offset', 'classification', 'step'), unpack('<6I')))
                e['semantic'] = semantic.split(b'\0', 1)[0].decode('ascii')
                if e['slot'] >= 32 or e['classification'] > 1:
                    raise ValueError('Invalid input layout element')
                d['layout'].append(e)
        draws.append(d)
    total = 0
    for _ in range(ns):
        s = dict(zip(('frame', 'width', 'height', 'format', 'size'), unpack('<5I')))
        bpp = {28: 4, 29: 4, 87: 4, 91: 4, 10: 8, 20: 8, 40: 4, 45: 4, 55: 2}.get(s['format'])
        expected = s['width'] * s['height'] * (bpp or 0)
        total += expected
        if not bpp or not s['width'] or not s['height'] or expected > 128*1024*1024 or total > 256*1024*1024:
            raise ValueError('Invalid surface descriptor')
        if s['size'] not in (0, expected):
            raise ValueError('Invalid surface payload')
        s['data'] = take(s['size'])
        surfaces.append(s)
    mesh_buffers, mesh_declined = [], 0
    if version >= 4:
        nb, mesh_declined = unpack('<2I')
        if nb > nd*3:
            raise ValueError('Invalid source buffer count')
        total = 0
        for _ in range(nb):
            b = dict(zip(('frame', 'first_draw', 'whole', 'stride', 'size'), unpack('<5I')))
            total += b['whole']
            if not 0 < b['whole'] <= 16*1024*1024 or total > 256*1024*1024 or b['size'] not in (0,b['whole']) or b['first_draw'] >= nd or b['stride'] not in (8,48,336):
                raise ValueError('Invalid source buffer descriptor')
            b['data'] = take(b['size'])
            mesh_buffers.append(b)
        for i,d in enumerate(draws):
            for role,ref in enumerate(d['mesh']):
                if ref == 0xffffffff:
                    continue
                if ref >= nb:
                    raise ValueError('Invalid source buffer reference')
                b = mesh_buffers[ref]
                if b['frame'] != d['frame'] or b['first_draw'] > i or b['stride'] != (336,48,8)[role]:
                    raise ValueError('Source buffer belongs to a different frame, role, or later draw')
    effect_images, effect_image_declined = [], 0
    if version >= 6:
        ni, effect_image_declined = unpack('<2I')
        if ni > 32:
            raise ValueError('Invalid effect image count')
        total = 0
        for _ in range(ni):
            e = dict(zip(('draw', 'after', 'x', 'y', 'width', 'height', 'source_width', 'source_height', 'format', 'size'), unpack('<10I')))
            night = e['draw'] < nd and (draws[e['draw']]['vs'], draws[e['draw']]['ps']) == (0xFCF7BD2896751D96, 0xF786D34B5E118D5E)
            inputs = version >= 7 and night and 2 <= e['after'] <= 6
            bc4 = inputs and e['format'] == 80
            formats = {10: 8, 26: 4, 28: 4, 29: 4}
            if inputs:
                formats.update({24: 4, 41: 4})
            bpp = formats.get(e['format'], 0)
            expected = ((e['width']+3)//4)*((e['height']+3)//4)*8 if bc4 else e['width']*e['height']*bpp
            total += expected
            x, y = (e['source_width']-e['width'])//2, (e['source_height']-e['height'])//3
            if bc4:
                x, y = x & ~3, y & ~3
            origin = ((e['x'],e['y']) == (x,y) if night else
                      (e['x']+e['width'],e['y']+e['height']) == (e['source_width'],e['source_height']))
            if (e['draw'] >= nd or (not night and draws[e['draw']]['ordinal'] != 0xfffffffd) or (e['after'] > 1 and not inputs) or
                    not 0 < e['width'] <= 1024 or not 0 < e['height'] <= 1024 or
                    not origin or e['x']+e['width']>e['source_width'] or e['y']+e['height']>e['source_height'] or
                    (not bpp and not bc4) or total > 64*1024*1024 or e['size'] not in (0, expected)):
                raise ValueError('Invalid effect image descriptor')
            e['data'] = take(e['size'])
            effect_images.append(e)
    night_sampling = []
    if version >= 7:
        count, = unpack('<I')
        if count > 8:
            raise ValueError('Invalid night sampling count')
        for _ in range(count):
            draw, mask, viewports = unpack('<3I')
            if (draw >= nd or (draws[draw]['vs'],draws[draw]['ps']) != (0xFCF7BD2896751D96,0xF786D34B5E118D5E) or
                    mask > 3 or viewports > 16 or any(n['draw'] == draw for n in night_sampling)):
                raise ValueError('Invalid night sampling descriptor')
            samplers = [list(unpack('<13I')) for _ in range(2)]
            viewport = list(unpack('<6f'))
            night_sampling.append(dict(draw=draw, mask=mask, viewport_count=viewports, samplers=samplers, viewport=viewport))
    sprite_diagnostics, sprite_declined = [], 0
    if version >= 8:
        count, sprite_declined = unpack('<2I')
        if count > 10:
            raise ValueError('Invalid sprite diagnostic count')
        sprite_color_bytes = sprite_depth_bytes = 0
        def blob(max_size, exact=None):
            n, = unpack('<I')
            if n > max_size or (exact is not None and n != exact):
                raise ValueError('Invalid sprite descriptor blob')
            return take(n)
        for _ in range(count):
            vals = unpack('<8I')
            (frame, draw, x, y, width, height, source_width, source_height) = vals
            vals = unpack('<8I')
            (rt_format, srv_format, srv_mip, srv_levels, srv_width, srv_height,
             srv_array, srv_mip_count) = vals
            if draw >= nd or not width or not height or width > 1400 or height > 1400 or not source_width or not source_height:
                raise ValueError('Invalid sprite crop')
            if x + width > source_width or y + height > source_height:
                raise ValueError('Sprite crop outside source')
            if (source_width > 16384 or source_height > 16384 or rt_format not in (10, 26, 27, 28, 29) or
                    draws[draw]['frame'] != frame or (draws[draw]['vs'], draws[draw]['ps']) != (0xE508648660A352B2, 0x63ABD86359B57D01) or
                    any(s['draw'] == draw for s in sprite_diagnostics)):
                raise ValueError('Invalid sprite draw association or format')
            ps_b1, srv_id, rt_id, depth_id, ps_b1_whole = unpack('<4QI')
            depth_format, depth_resource_format = unpack('<2I')
            if depth_resource_format not in (19, 20, 39, 40, 44, 45, 53, 55):
                raise ValueError('Invalid sprite depth format')
            if rt_id != draws[draw]['target']:
                raise ValueError('Invalid sprite target identity')
            mesh_refs = list(unpack('<2I'))
            for role, ref in enumerate(mesh_refs):
                if ref != 0xffffffff:
                    if ref >= len(mesh_buffers) or mesh_buffers[ref]['frame'] != frame or mesh_buffers[ref]['first_draw'] != draw or mesh_buffers[ref]['stride'] != (336, 48)[role]:
                        raise ValueError('Invalid sprite structured-buffer reference')
            blend_factor = list(unpack('<4I'))
            sample_mask, stencil_ref, depth_format_again, raster_fill, raster_cull, state_mask = unpack('<6I')
            topology, dsv_flags = unpack('<2I')
            if depth_format_again != depth_format or state_mask > 127 or dsv_flags > 3:
                raise ValueError('Invalid sprite state')
            scissor_count, = unpack('<I')
            if scissor_count > 16:
                raise ValueError('Invalid sprite scissor count')
            scissors = [list(unpack('<4i')) for _ in range(scissor_count)]
            viewport_count, = unpack('<I')
            if viewport_count > 16:
                raise ValueError('Invalid sprite viewport count')
            viewports = []
            for _ in range(viewport_count):
                viewports.append(list(struct.unpack('<6f', blob(24, 24))))
            sampler = blob(52, 52)
            blend = blob(264, 264)
            depth = blob(52, 52)
            raster = blob(40, 40)
            elements, = unpack('<I')
            if elements > 32:
                raise ValueError('Invalid sprite input layout size')
            layout = []
            for _ in range(elements):
                semantic = blob(64, 64)
                if b'\0' not in semantic:
                    raise ValueError('Unterminated sprite input semantic')
                index, fmt, slot, offset, classification, step = unpack('<6I')
                if slot >= 32 or classification > 1:
                    raise ValueError('Invalid sprite input layout element')
                layout.append(dict(semantic=semantic.split(b'\0', 1)[0].decode('ascii'),
                                   index=index, format=fmt, slot=slot, offset=offset,
                                   classification=classification, step=step))
            before_expected = width * height * (8 if rt_format == 10 else 4)
            before_declared, = unpack('<I')
            if before_declared != before_expected:
                raise ValueError('Invalid sprite before size')
            before_size, = unpack('<I')
            if before_size not in (0, before_expected):
                raise ValueError('Invalid sprite before payload')
            before_data = take(before_size)
            after_expected = before_expected
            after_declared, = unpack('<I')
            if after_declared != after_expected:
                raise ValueError('Invalid sprite after size')
            after_size, = unpack('<I')
            if after_size not in (0, after_expected):
                raise ValueError('Invalid sprite after payload')
            after_data = take(after_size)
            depth_expected = width * height * (8 if depth_resource_format in (19, 20) else 2 if depth_resource_format in (53, 55) else 4)
            sprite_color_bytes += before_expected * 2
            sprite_depth_bytes += depth_expected
            if max(sprite_color_bytes, sprite_depth_bytes) > 160 * 1024 * 1024:
                raise ValueError('Sprite payload budget exceeded')
            depth_declared, = unpack('<I')
            if depth_declared != depth_expected:
                raise ValueError('Invalid sprite depth size')
            depth_size, = unpack('<I')
            if depth_size not in (0, depth_expected):
                raise ValueError('Invalid sprite depth payload')
            depth_data = take(depth_size)
            ps_b1_size, = unpack('<I')
            if ps_b1_size > min(ps_b1_whole, 8192) or ps_b1_size % 16:
                raise ValueError('Invalid sprite PS b1 payload')
            ps_b1_data = take(ps_b1_size)
            sprite_diagnostics.append(dict(frame=frame, draw=draw, x=x, y=y, width=width,
                height=height, source_width=source_width, source_height=source_height,
                rt_format=rt_format, srv_format=srv_format, srv_mip=srv_mip,
                srv_levels=srv_levels, srv_width=srv_width, srv_height=srv_height,
                srv_array=srv_array, srv_mip_count=srv_mip_count, ps_b1=ps_b1,
                srv_id=srv_id, rt_id=rt_id, depth_id=depth_id, ps_b1_whole=ps_b1_whole,
                mesh=mesh_refs,
                depth_format=depth_format, depth_resource_format=depth_resource_format,
                blend_factor=blend_factor, sample_mask=sample_mask, stencil_ref=stencil_ref,
                raster_fill=raster_fill, raster_cull=raster_cull, state_mask=state_mask, topology=topology, dsv_flags=dsv_flags,
                scissors=scissors, viewports=viewports, sampler=sampler, blend=blend,
                depth_desc=depth, raster=raster, layout=layout,
                before=before_data, after=after_data, depth=depth_data, ps_b1_data=ps_b1_data))
    geometry = read_geometry(unpack, take) if version >= 9 else empty_geometry()
    failures, = unpack('<I')
    if stream.read(1):
        raise ValueError('Trailing snapshot data')
    return dict(version=version, dropped=dropped, failures=failures, draws=draws, surfaces=surfaces,
                mesh_buffers=mesh_buffers, mesh_declined=mesh_declined,
                effect_images=effect_images, effect_image_declined=effect_image_declined, night_sampling=night_sampling,
                sprite_diagnostics=sprite_diagnostics, sprite_declined=sprite_declined, geometry=geometry)


def empty_geometry():
    return dict(frame=0, meshes=[], declined=0, draws_dropped=0, states=[], draws=[])


GEO_MAX_MESHES = 20000
GEO_MAX_DRAWS = 40000
GEO_BUDGET = 96 * 1024 * 1024


def read_geometry(unpack, take):
    """Version 9's trailing section: the cull gate probe's armed-frame geometry
    (eye_draw_snapshot.h, captureGeometry). Meshes carry the vertex window
    (from the base vertex) and the index window (from the start index) as the
    GPU held them, with the input layout that decodes the vertices; states the
    native D3D11 blend / depth-stencil / rasterizer descriptors; draws map
    each ledger ordinal of the armed frame to a mesh and a state
    (0xFFFFFFFF = declined)."""
    frame, nm, declined, dropped = unpack('<4I')
    if nm > GEO_MAX_MESHES:
        raise ValueError('Invalid geometry mesh count')
    meshes, total = [], 0
    for _ in range(nm):
        m = dict(zip(('vb', 'vb_slot', 'vb_offset', 'vb_stride', 'vb_whole'), unpack('<Q4I')))
        m.update(zip(('ib', 'ib_offset', 'ib_format', 'ib_whole'), unpack('<Q3I')))
        m.update(zip(('start', 'base', 'count', 'kind', 'topology'), unpack('<Ii3I')))
        m['v_capture'], = unpack('<I')
        vbytes, = unpack('<I')
        m['vertices'] = take(vbytes)
        m['i_capture'], = unpack('<I')
        ibytes, = unpack('<I')
        m['indices'] = take(ibytes)
        total += vbytes + ibytes
        if total > GEO_BUDGET or m['vb_slot'] > 1:
            raise ValueError('Invalid geometry mesh')
        elements, = unpack('<I')
        if elements > 32:
            raise ValueError('Invalid geometry layout size')
        m['layout'] = []
        for _ in range(elements):
            semantic = take(64)
            if b'\0' not in semantic:
                raise ValueError('Unterminated geometry input semantic')
            e = dict(zip(('index', 'format', 'slot', 'offset', 'classification', 'step'), unpack('<6I')))
            e['semantic'] = semantic.split(b'\0', 1)[0].decode('ascii')
            m['layout'].append(e)
        meshes.append(m)
    ns, = unpack('<I')
    if ns > 4096:
        raise ValueError('Invalid geometry state count')
    states = []

    def blob(exact):
        n, = unpack('<I')
        if n != exact:
            raise ValueError('Invalid geometry state descriptor')
        return take(n)
    for _ in range(ns):
        flags, = unpack('<I')
        blend = blob(264)
        factor = list(unpack('<4f'))
        sample_mask, = unpack('<I')
        depth = blob(52)
        stencil_ref, = unpack('<I')
        raster = blob(40)
        states.append(dict(flags=flags, blend=blend, factor=factor, sample_mask=sample_mask,
                           depth=depth, stencil_ref=stencil_ref, raster=raster))
    nd, = unpack('<I')
    if nd > GEO_MAX_DRAWS:
        raise ValueError('Invalid geometry draw count')
    draws = []
    for _ in range(nd):
        g = dict(zip(('frame', 'ordinal', 'mesh', 'state'), unpack('<4I')))
        g['vs'], g['ps'] = unpack('<2Q')
        g['instances'], g['start_instance'] = unpack('<2I')
        if (g['mesh'] != 0xffffffff and g['mesh'] >= nm) or (g['state'] != 0xffffffff and g['state'] >= ns):
            raise ValueError('Invalid geometry draw reference')
        draws.append(g)
    return dict(frame=frame, meshes=meshes, declined=declined, draws_dropped=dropped, states=states, draws=draws)


def blend_opaque(state):
    """True when render target 0 does not blend (D3D11_BLEND_DESC: AlphaToCoverage,
    IndependentBlend, then RenderTarget[0].BlendEnable first) or no blend state
    is bound (the default is opaque)."""
    if not state['flags'] & 1:
        return True
    alpha_to_coverage, independent, rt0_enable = struct.unpack_from('<3i', state['blend'], 0)
    return not rt0_enable and not alpha_to_coverage


def depth_writes(state):
    """True when depth testing is on and the write mask is ALL (D3D11_DEPTH_STENCIL_DESC:
    DepthEnable, DepthWriteMask first); an unbound state is the default (on, ALL)."""
    if not state['flags'] & 2:
        return True
    enable, write_mask = struct.unpack_from('<2i', state['depth'], 0)
    return bool(enable) and write_mask == 1


def triangles(mesh):
    """Triangles the draw rasterises: list = count/3, strip = count-2 (restart
    indices and degenerates are not subtracted), anything else 0."""
    t = mesh['topology']
    if t == 4:      # D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST
        return mesh['count'] // 3
    if t == 5:      # TRIANGLESTRIP
        return max(0, mesh['count'] - 2)
    return 0


def packed_float_channel(value, mantissa_bits):
    """Unsigned R11/G11/B10 float to a display byte; raw HDR stays in the dump."""
    import math
    exponent, mantissa = value >> mantissa_bits, value & ((1 << mantissa_bits)-1)
    if exponent == 31:
        return 0 if mantissa else 255
    linear = (math.ldexp(mantissa, 1-15-mantissa_bits) if exponent == 0 else
              math.ldexp(1 + mantissa / (1 << mantissa_bits), exponent-15))
    return max(0, min(255, round(linear * 255)))


def export_effect_images(capture, directory, dry_run=False):
    directory = Path(directory)
    paths = [directory / f"effect_{e['draw']:04d}_{'after' if e['after'] else 'before'}_x{e['x']}_y{e['y']}.png"
             for e in capture['effect_images'] if e['data'] and e['after'] <= 1]
    if dry_run:
        return paths
    from PIL import Image
    if paths:
        directory.mkdir(parents=True, exist_ok=True)
    lut11 = [packed_float_channel(i, 6) for i in range(2048)]
    lut10 = [packed_float_channel(i, 5) for i in range(1024)]
    for e, path in zip((e for e in capture['effect_images'] if e['data'] and e['after'] <= 1), paths):
        if e['format'] == 26:
            rgba = bytearray()
            for (pixel,) in struct.iter_unpack('<I', e['data']):
                rgba.extend((lut11[pixel & 2047], lut11[(pixel >> 11) & 2047], lut10[pixel >> 22], 255))
            data = bytes(rgba)
        elif e['format'] == 10:
            data = bytes(max(0, min(255, round(v[0] * 255))) for v in struct.iter_unpack('<e', e['data']))
        else:
            data = e['data']
        Image.frombytes('RGBA', (e['width'], e['height']), data).save(path)
    return paths


def export_surfaces(capture, directory, dry_run=False):
    directory = Path(directory)
    colour_formats = (28, 29, 87, 91, 10)
    paths = [directory / f'surface_{i:02d}.png' for i, s in enumerate(capture['surfaces']) if s['data'] and s['format'] in colour_formats]
    if dry_run:
        return paths
    from PIL import Image
    if paths:
        directory.mkdir(parents=True, exist_ok=True)
    for i, s in enumerate(capture['surfaces']):
        if not s['data'] or s['format'] not in colour_formats:
            continue
        if s['format'] == 10:
            values = struct.iter_unpack('<e', s['data'])
            rgba = bytes(max(0, min(255, round(v[0] * 255))) for v in values)
        else:
            rgba = s['data']
        decoder = 'BGRA' if s['format'] in (87, 91) else 'RGBA'
        im = Image.frombytes('RGBA', (s['width'], s['height']), rgba, 'raw', decoder)
        im.save(directory / f'surface_{i:02d}.png')
    return paths


def verify_fixture(capture):
    assert len(capture['draws']) == 7 and len(capture['surfaces']) == 1
    assert capture['dropped'] == 0 and capture['failures'] == 0
    for i, d in enumerate(capture['draws'][:3]):
        assert (d['frame'], d['ordinal'], d['width'], d['height']) == (100+i, i*2, 8, 8)
        assert d['buffers'][0]['whole'] == 192
        vals = struct.unpack('<48f', d['buffers'][0]['data'])
        assert all(v == 12+i*3 for v in vals), 'Buffer was captured after a later write'
        assert not d['buffers'][1]['data'] and not d['buffers'][2]['data']
        assert d['buffers'][3]['data'] == d['buffers'][0]['data']
    s = capture['surfaces'][0]
    assert (s['width'], s['height'], s['format']) == (3, 2, 28)
    assert s['data'] == bytes(range(24)), 'Texture rows or first-draw timing differ'
    for i, d in enumerate(capture['draws'][3:5]):
        assert (d['start'], d['base']) == (2, -3)
        v = d['streams'][0]
        assert (v['offset'], v['stride'], v['whole']) == (8, 16, 64)
        assert v['data'] == bytes(range(8+i*64, 64+i*64)), 'Vertex buffer captured after reuse'
    for i, d in enumerate(capture['draws'][5:]):
        assert (d['start'], d['base']) == (150000, 8000)
        for s, binding, start, length in [(d['streams'][1], 16, 320016, 184), (d['streams'][2], 4, 300004, 12)]:
            assert (s['offset'], s['capture_offset']) == (binding, start)
            assert s['data'] == bytes((k+i*37) & 255 for k in range(start, start+length)), 'Shared-buffer draw window lost or overwritten'


def verify_sprite_fixture(capture):
    assert capture['version'] == 9 and capture['failures'] == 0
    assert len(capture['sprite_diagnostics']) == 1 and capture['sprite_declined'] >= 2
    d = capture['sprite_diagnostics'][0]
    assert d['draw'] == 2 and (d['width'], d['height']) == (1400, 1400)
    assert len(d['before']) == len(d['after']) == 1400 * 1400 * 4
    assert d['before'] != d['after'], 'before/after paired crop did not retain distinct pixels'
    assert d['before'] == bytes([0x1a, 0x33, 0x4c, 0xff]) * (1400 * 1400)
    assert d['after'] == bytes([0xb2, 0x33, 0x1a, 0xff]) * (1400 * 1400)
    assert len(d['depth']) == 1400 * 1400 * 4
    assert d['depth'] == bytes([0x00, 0x00, 0x40, 0x01]) * (1400 * 1400)
    assert d['state_mask'] & (1 << 3) and d['state_mask'] & (1 << 6)
    assert d['layout'][0]['semantic'] == 'POSITION'
    assert d['mesh'][0] != 0xffffffff and d['mesh'][1] != 0xffffffff
    assert capture['mesh_buffers'][d['mesh'][0]]['data'] == bytes([0x31]) * 672
    assert capture['mesh_buffers'][d['mesh'][1]]['data'] == bytes([0x42]) * 96
    assert d['ps_b1_data'] == struct.pack('<f', 31.0) * 48


def self_test():
    head = b'EDVRDRW1' + struct.pack('<4I', 1, 0, 0, 0)
    with tempfile.TemporaryDirectory() as td:
        p = Path(td)/'sample.bin'
        p.write_bytes(head + struct.pack('<I', 0))
        c = read(p)
        assert c['draws'] == [] and c['surfaces'] == []
        h4 = b'EDVRDRW1' + struct.pack('<4I',4,1,0,0)
        draw = struct.pack('<3Q9I',1,2,3,7,0xfffffffe,ord('X'),6,1,0,8,8,0xffffffff)
        draw += struct.pack('<QII',0,0,0)*4 + struct.pack('<Ii',0,0) + struct.pack('<5I',0,0,0,0,0)*3
        refs = struct.pack('<3I',0,0xffffffff,0xffffffff)
        table = struct.pack('<2I',1,0)
        blob = struct.pack('<5I',7,0,336,336,336) + bytes(336) + struct.pack('<I',0)
        good = h4+draw+refs+table+blob
        p.write_bytes(good)
        assert len(read(p)['mesh_buffers'][0]['data']) == 336
        h5 = b'EDVRDRW1' + struct.pack('<4I',5,1,0,0)
        layout = b'POSITION'.ljust(64,b'\0')+struct.pack('<6I',0,2,1,16,1,1)
        v5 = h5+draw+refs+struct.pack('<I',1)+layout+table+blob
        p.write_bytes(v5)
        assert read(p)['draws'][0]['layout'] == [dict(semantic='POSITION',index=0,format=2,slot=1,offset=16,classification=1,step=1)]
        for bad in (v5[:-1],h5+draw+refs+struct.pack('<I',33),h5+draw+refs+struct.pack('<I',1)+b'X'*64+layout[64:]+table+blob):
            p.write_bytes(bad)
            try:
                read(p)
            except ValueError:
                pass
            else:
                raise AssertionError('Invalid effect layout accepted')
        malformed_mesh = [good[:-1], h4+draw+struct.pack('<3I',1,0xffffffff,0xffffffff)+table+blob]
        for frame,first,whole,stride,size in ((8,0,336,336,336),(7,1,336,336,336),(7,0,336,48,336),(7,0,336,336,335),(7,0,17*1024*1024,336,0)):
            malformed_mesh.append(h4+draw+refs+table+struct.pack('<5I',frame,first,whole,stride,size)+bytes(336)+struct.pack('<I',0))
        for bad in malformed_mesh:
            p.write_bytes(bad)
            try:
                read(p)
            except ValueError:
                pass
            else:
                raise AssertionError('Invalid source buffer accepted')
        for bad in (b'', head, head+b'\0'*5, head.replace(b'EDVRDRW1', b'EDVRBAD1'),
                    b'EDVRDRW1'+struct.pack('<4I', 5, 0, 0, 0),
                    b'EDVRDRW1'+struct.pack('<4I', 1, 4097, 0, 0)):
            p.write_bytes(bad)
            try:
                read(p)
            except ValueError:
                pass
            else:
                raise AssertionError('Invalid snapshot accepted')
        c['surfaces'] = [dict(data=bytes(range(24)), format=28, width=3, height=2)]
        before = sorted(Path(td).rglob('*'))
        assert export_surfaces(c, Path(td)/'absent', True) == [Path(td)/'absent/surface_00.png']
        assert before == sorted(Path(td).rglob('*')), '--dry-run wrote files'
        h6 = b'EDVRDRW1' + struct.pack('<4I',6,0,0,0)
        p.write_bytes(h6 + struct.pack('<5I',0,0,0,0,0))
        assert read(p)['effect_images'] == []
        for bad in (h6 + struct.pack('<4I',0,0,33,0), h6 + struct.pack('<4I',0,0,1,0) + bytes(40)):
            p.write_bytes(bad)
            try:
                read(p)
            except ValueError:
                pass
            else:
                raise AssertionError('Invalid effect image accepted')
        h7 = b'EDVRDRW1' + struct.pack('<4I',7,0,0,0)
        p.write_bytes(h7 + struct.pack('<6I',0,0,0,0,0,0))
        assert read(p)['night_sampling'] == []
        h8 = b'EDVRDRW1' + struct.pack('<4I',8,0,0,0)
        p.write_bytes(h8 + struct.pack('<2I',0,0) + struct.pack('<2I',0,0) + struct.pack('<I',0) + struct.pack('<2I',0,0) + struct.pack('<I',0))
        assert read(p)['sprite_diagnostics'] == []
        p.write_bytes(h8 + struct.pack('<2I',0,0) + struct.pack('<2I',0,0) + struct.pack('<I',0) + struct.pack('<2I',11,0) + struct.pack('<I',0))
        try:
            read(p)
        except ValueError:
            pass
        else:
            raise AssertionError('Invalid sprite diagnostic count accepted')
        # Version 9: the geometry section, empty and with one mesh/state/draw.
        h9 = b'EDVRDRW1' + struct.pack('<4I',9,0,0,0)
        v8_tail = struct.pack('<2I',0,0) + struct.pack('<2I',0,0) + struct.pack('<I',0) + struct.pack('<2I',0,0)
        p.write_bytes(h9 + v8_tail + struct.pack('<4I',0,0,0,0) + struct.pack('<I',0) + struct.pack('<I',0) + struct.pack('<I',0))
        g = read(p)['geometry']
        assert g['frame'] == 0 and g['meshes'] == [] and g['draws'] == [], 'empty v9 geometry section'
        verts, idx = bytes(range(80)), struct.pack('<6H',0,1,2,0,2,1)
        mesh = struct.pack('<Q4I',0x1234,1,16,40,4096) + struct.pack('<Q3I',0x5678,4,57,1024)
        mesh += struct.pack('<Ii3I',3,8000,6,ord('X'),4) + struct.pack('<2I',16+8000*40,len(verts)) + verts
        mesh += struct.pack('<2I',4+3*2,len(idx)) + idx + struct.pack('<I',1) + b'PACKEDVERTEXDATAA'.ljust(64,b'\0') + struct.pack('<6I',0,3,1,0,0,0)
        state = struct.pack('<I',7) + struct.pack('<I',264) + bytes(264) + struct.pack('<4f',1,1,1,1) + struct.pack('<I',0xffffffff)
        state += struct.pack('<I',52) + struct.pack('<2i',1,1) + bytes(44) + struct.pack('<I',0) + struct.pack('<I',40) + bytes(40)
        draw = struct.pack('<4I',2,123,0,0) + struct.pack('<2Q',0xEB5234DB6ADB491D,0x1) + struct.pack('<2I',5,29178)
        body = struct.pack('<4I',2,1,0,0) + mesh + struct.pack('<I',1) + state + struct.pack('<I',1) + draw
        p.write_bytes(h9 + v8_tail + body + struct.pack('<I',0))
        g = read(p)['geometry']
        m = g['meshes'][0]
        assert g['frame'] == 2 and m['count'] == 6 and m['indices'] == idx and m['vertices'] == verts, 'v9 mesh payloads'
        assert m['layout'][0]['semantic'] == 'PACKEDVERTEXDATAA' and triangles(m) == 2, 'v9 layout and triangle count'
        assert g['draws'][0]['ordinal'] == 123 and g['draws'][0]['mesh'] == 0, 'v9 draw map'
        assert blend_opaque(g['states'][0]) and depth_writes(g['states'][0]), 'v9 state decode'
        bad = body[:-40] + struct.pack('<4I',2,123,1,0) + body[-24:]   # mesh 1 of 1
        p.write_bytes(h9 + v8_tail + bad + struct.pack('<I',0))
        try:
            read(p)
        except ValueError:
            pass
        else:
            raise AssertionError('Dangling geometry mesh reference accepted')
        def night_file(phase=6, size=128, mask=0, reference=0, vs=0xFCF7BD2896751D96):
            d = struct.pack('<3Q9I',vs,0xF786D34B5E118D5E,3,7,17,ord('X'),6,1,0,16,16,0xffffffff)
            d += struct.pack('<QII',0,0,0)*4 + struct.pack('<Ii',0,0) + struct.pack('<5I',0,0,0,0,0)*3
            d += struct.pack('<4I',0xffffffff,0xffffffff,0xffffffff,0)
            header = b'EDVRDRW1'+struct.pack('<4I',7,1,0,0)
            image = struct.pack('<10I',0,phase,0,0,16,16,16,16,80,size)+bytes(size)
            sampling = struct.pack('<4I',1,reference,mask,1)+bytes(104)+struct.pack('<6f',0,0,16,16,0,1)
            return header+d+struct.pack('<4I',0,0,1,0)+image+sampling+struct.pack('<I',0)
        good_night = night_file()
        p.write_bytes(good_night)
        n = read(p)
        assert n['effect_images'][0]['size'] == 128 and n['night_sampling'][0]['mask'] == 0
        before = sorted(Path(td).rglob('*'))
        assert export_effect_images(n, Path(td)/'raw-inputs') == [], 'Raw inputs exported as before/after colour'
        assert before == sorted(Path(td).rglob('*')), 'Raw input export created a directory'
        for bad in (good_night[:-1], night_file(phase=1), night_file(phase=7), night_file(size=127),
                    night_file(mask=4), night_file(reference=1), night_file(vs=1),
                    h7+struct.pack('<5I',0,0,0,0,9)):
            p.write_bytes(bad)
            try:
                read(p)
            except ValueError:
                pass
            else:
                raise AssertionError('Invalid night sampling accepted')
        c['effect_images'] = [dict(draw=0, after=1, x=4, y=5, width=1, height=1, format=26, data=bytes(4))]
        before = sorted(Path(td).rglob('*'))
        assert export_effect_images(c, Path(td)/'missing', True)
        assert before == sorted(Path(td).rglob('*')), 'Effect image dry-run wrote files'
        for bits in (5,6):
            assert packed_float_channel(0,bits) == 0
            assert packed_float_channel(15 << bits,bits) == 255
            assert packed_float_channel(14 << bits,bits) == 128
            assert packed_float_channel((31 << bits)+1,bits) == 0
    print('eye draw snapshot self-test passed')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('path', nargs='?')
    ap.add_argument('--surfaces', help='Export original RGBA panel textures to this directory')
    ap.add_argument('--effects', help='Export before/after effect colour crops to this directory')
    ap.add_argument('--dry-run', action='store_true', help='Report exports without writing anything')
    ap.add_argument('--self-test', action='store_true')
    ap.add_argument('--verify-fixture', action='store_true', help=argparse.SUPPRESS)
    a = ap.parse_args()
    if a.self_test:
        self_test()
        return
    if not a.path:
        ap.error('Name a drawstate capture')
    c = read(a.path)
    if a.verify_fixture:
        verify_fixture(c)
        sprite_path = Path(str(a.path) + '.sprite')
        assert sprite_path.exists(), 'sprite diagnostic fixture is required'
        verify_sprite_fixture(read(sprite_path))
        effects = read(str(a.path)+'.effects')
        assert effects['version'] == 9 and len(effects['draws']) == 5 and effects['failures'] == 0
        for i,d in enumerate(effects['draws']):
            assert d['ordinal'] == 0xfffffffd and d['mesh'] == [0xffffffff]*3
            assert d['layout'][1] == dict(semantic='TEXCOORD',index=0,format=2,slot=1,offset=16,classification=1,step=1)
            assert bool(d['streams'][0]['data']) == (i < 4) and bool(d['streams'][1]['data']) == (i < 4)
        mesh = read(str(a.path)+'.mesh')
        assert mesh['failures'] == mesh['mesh_declined'] == 0 and len(mesh['draws']) == 4 and len(mesh['mesh_buffers']) == 6
        for i,d in enumerate(mesh['draws']):
            assert d['ordinal'] == 0xfffffffe and d['mesh'] == [i//2*3+j for j in range(3)]
            for role,ref in enumerate(d['mesh']):
                b = mesh['mesh_buffers'][ref]
                assert b['first_draw'] == i//2*2 and b['frame'] == 300+i//2
                assert b['data'] == bytes([21+role*10+i//2])*b['whole'], 'Source buffer copied after reuse or truncated'
        assert mesh['mesh_buffers'][1]['whole'] > 1024*1024, 'High bone indices lost'
        eye_mesh = read(str(a.path)+'.eyemesh')
        assert eye_mesh['failures'] == eye_mesh['mesh_declined'] == eye_mesh['dropped'] == 0
        assert len(eye_mesh['draws']) == 12 and len(eye_mesh['mesh_buffers']) == 18
        for i, d in enumerate(eye_mesh['draws']):
            group, draw = divmod(i, 2)
            frame, eye = divmod(group, 2)
            assert d['frame'] == 700+frame and d['ordinal'] == eye*100+draw
            assert len(d['layout']) == 1 and d['layout'][0]['semantic'] == 'POSITION'
            assert d['start'] == 150000 and d['base'] == 8000 and d['start_instance'] == 2+draw
            constants = struct.unpack('<48f', d['buffers'][0]['data'])
            assert all(v == (700+frame)*10+eye*2+draw for v in constants), 'Eye draw camera copied after reuse'
            assert d['mesh'] == [group*3+j for j in range(3)]
            for role, ref in enumerate(d['mesh']):
                b = eye_mesh['mesh_buffers'][ref]
                assert b['first_draw'] == group*2 and b['frame'] == d['frame']
                assert b['data'] == bytes([51+group+role*10])*b['whole'], 'Eye pool copy came from the other eye/frame'
        assert len({d['target'] for d in eye_mesh['draws']}) == 2
        # Version 9 geometry (the rig's armed frame 710): two meshes, two states, three map rows.
        geo = eye_mesh['geometry']
        assert geo['frame'] == 710 and len(geo['meshes']) == 2 and len(geo['states']) == 2 and geo['declined'] == 0
        assert [(g['ordinal'], g['mesh'], g['state']) for g in geo['draws']] == [(7, 0, 0), (8, 0, 0), (9, 1, 1)]
        m0, m1 = geo['meshes']
        assert (m0['i_capture'], m0['indices']) == (6, bytes((b+37) & 255 for b in range(6, 18))), 'index window from the start index'
        assert (m0['v_capture'], m0['vertices']) == (16, bytes([76])*16), 'vertex window from the base vertex, clamped'
        assert m1['indices'] == bytes((b+37) & 255 for b in range(4, 10)) and m1['vertices'] == bytes([76])*24
        assert blend_opaque(geo['states'][0]) and not blend_opaque(geo['states'][1]), 'blend state per draw'
        assert m0['layout'][0]['semantic'] == 'POSITION' and triangles(m0) == 2 and triangles(m1) == 1
        v = read(str(a.path)+'.vscreen')
        assert len(v['draws']) == 3 and len(v['surfaces']) == 2 and v['failures'] == 0
        assert v['draws'][0]['ordinal'] == 0xffffffff and v['draws'][0]['texture'] == 0xffffffff
        assert v['draws'][1]['texture'] == 0 and v['draws'][2]['texture'] == 0
        assert all(z == .75 for z in struct.unpack('<64f', v['surfaces'][1]['data'])), 'Source depth was copied before scene completion or after reuse'
        assert export_surfaces(v, Path('unused'), True) == [Path('unused/surface_00.png')], 'Depth was treated as colour'
        crops = read(str(a.path)+'.crops')
        assert crops['version'] == 9 and crops['failures'] == 0 and crops['effect_image_declined'] == 1
        assert len(crops['effect_images']) == 2
        for i,e in enumerate(crops['effect_images']):
            assert (e['draw'], e['after'], e['x'], e['y'], e['width'], e['height'], e['format']) == (0,i,2,3,1024,1024,26)
            expected = (15<<6) | ((14<<6)<<11) | ((13<<5)<<22) if i == 0 else ((15<<5)<<22)
            assert all(v[0] == expected for v in struct.iter_unpack('<I',e['data'])), 'Effect crop timing or HDR copy differs'
        solar = read(str(a.path)+'.solar')
        assert solar['failures'] == solar['dropped'] == 0 and len(solar['draws']) == 28
        for i, d in enumerate(solar['draws']):
            family, frame = divmod(i, 4)
            assert d['frame'] == 1000+frame and d['ordinal'] == family
            assert len(d['layout']) == 1 and d['layout'][0]['semantic'] == 'POSITION'
            assert all(v == 300+frame for v in struct.unpack('<48f', d['buffers'][0]['data'])), 'Solar constants copied after draw-time reuse'
            assert bool(d['streams'][0]['data']) == (frame < 3)
            for stage, shader in (('vs', d['vs']), ('ps', d['ps'])):
                if shader:
                    path = Path(a.path).parent / f'{stage}_{shader:016X}.dxbc'
                    assert path.read_bytes() == b'solar-bytecode\0', 'Solar shader stage was omitted or substituted'
        night = read(str(a.path)+'.night')
        assert night['version'] == 9 and night['failures'] == 0 and len(night['draws']) == 3
        assert len(night['effect_images']) == 14 and night['effect_image_declined'] == 0
        d = night['draws'][0]
        assert d['vs'] == 0xFCF7BD2896751D96 and d['ps'] == 0xF786D34B5E118D5E
        assert len(d['buffers'][1]['data']) == 192 and d['buffers'][1]['data'] == d['buffers'][3]['data'], 'Night-vision PS camera/settings were not copied at the draw'
        assert len(night['night_sampling']) == 2
        for eye in range(2):
            images = {e['after']:e for e in night['effect_images'] if e['draw'] == eye}
            assert set(images) == set(range(7)), 'Missing night-vision input or colour boundary'
            for phase in (0,1):
                e=images[phase]
                assert (e['x'],e['y'],e['width'],e['height'],e['format']) == (1,1,1024,1024,26)
                assert e['data'] == crops['effect_images'][phase]['data'], 'Night-vision before/after HDR pixels differ'
            for slot, (fmt, w, h) in enumerate(((41,6,1),(41,1024,1024),(24,1024,1024),(28,1,1),(80,16,16))):
                e=images[2+slot]
                assert (e['format'],e['width'],e['height']) == (fmt,w,h), 'Typed night input lost'
                assert (e['x'],e['y']) == ((1,1) if slot in (1,2) else (0,0))
                size=128 if fmt==80 else w*h*4
                assert e['data'] == bytes([11+slot*20+eye*3])*size, 'Night input copied after reuse, deduplicated between eyes, or BC4 rows lost'
            d=night['draws'][eye]
            assert len(d['layout']) == 1 and d['layout'][0]['format'] == 6
            assert d['streams'][0]['capture_offset'] == 12 and d['streams'][0]['data'] == bytes([31+eye])*84
            assert not d['streams'][1]['data'], 'Stale unused night VB1 copied'
            assert d['streams'][2]['capture_offset'] == 6 and d['streams'][2]['data'] == bytes([31+eye])*12
            state=night['night_sampling'][eye]
            assert (state['draw'],state['mask'],state['viewport_count']) == (eye,3,1)
            assert state['viewport'] == [eye,eye*2,1026-eye*2,1027-eye*2,0,1]
            for slot, sampler in enumerate(state['samplers']):
                assert sampler[:4] == ([0,3,3,3] if slot else [21,1,1,1]), 'Original filter/addressing lost'
                floats=struct.unpack('<13f',struct.pack('<13I',*sampler))
                assert (floats[4],floats[11],floats[12]) == ((0,0,0) if slot else (.25,-2,3)), 'Sampler LOD values lost'
        assert len(export_effect_images(night, Path('unused'), True)) == 4, 'Raw night inputs treated as colour'
        print('GPU draw snapshot fixture passed')
        return
    print(json.dumps(dict(version=c['version'], draws=len(c['draws']), surfaces=len(c['surfaces']), dropped=c['dropped'],
                          source_buffers=len(c['mesh_buffers']), source_bytes=sum(b['size'] for b in c['mesh_buffers']), source_declined=c['mesh_declined'],
                          vertex_draws=sum(any(s['data'] for s in d['streams']) for d in c['draws']),
                          vertex_bytes=sum(len(s['data']) for d in c['draws'] for s in d['streams']),
                          effect_images=sum(e['after'] <= 1 for e in c['effect_images']), effect_image_declined=c['effect_image_declined'],
                          night_input_images=sum(e['after'] >= 2 for e in c['effect_images']), night_sampling=len(c['night_sampling']),
                          failures=c['failures'], shaders=Counter(f"{d['vs']:016X}" for d in c['draws'])), indent=2))
    if a.surfaces:
        for path in export_surfaces(c, a.surfaces, a.dry_run):
            print(('Would write ' if a.dry_run else 'Wrote ') + str(path))
    if a.effects:
        for path in export_effect_images(c, a.effects, a.dry_run):
            print(('Would write ' if a.dry_run else 'Wrote ') + str(path))


if __name__ == '__main__':
    main()
