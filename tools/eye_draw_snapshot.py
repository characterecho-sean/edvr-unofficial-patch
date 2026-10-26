#!/usr/bin/env python3
"""Read drawstate_<stamp>.bin from an eye run (eye_draw_snapshot.h).

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
Frames/ordinals join draws_<stamp>.bin, whose crop map joins the eye images.
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
import math

MENU_FAMILIES=(0x5DAD41AA66757CE3,0xC4B4B334B26E81A9)
MENU_SCENES=(0x4435F2E50020E7F3,0xDE545DC8EE4FBB87,0xAACFDCF2FB9AD809,0x61AE8EB05FDC18DD)


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
    if version not in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16) or nd > 4096 or ns > 24:
        raise ValueError('Unsupported version or invalid counts')
    draws, surfaces = [], []
    vertex_bytes = 0
    for _ in range(nd):
        d = dict(zip(('vs', 'ps', 'target'), unpack('<3Q')))
        d.update(zip(('frame', 'ordinal', 'kind', 'count', 'instances', 'start_instance'), unpack('<6I')))
        if version >= 12:
            d['render_frame'], eye_bits, d['mesh_selected'] = unpack('<3I')
            if d['mesh_selected'] not in (0,1,2):raise ValueError('Invalid body mesh selection class')
            d['mesh_selection']='observed_body_family_candidate_not_proven_part' if d['mesh_selected']==2 else 'known_body_selector' if d['mesh_selected']==1 else 'not_body_selected'
            d['eye'] = struct.unpack('<i', struct.pack('<I', eye_bits))[0]
            join = dict(zip(('scene_frame','epoch','scheduled_index','scheduled_render_frame','fp_frame','fp_sequence',
                             'g_game_frame','item_serial','rig_generation'),unpack('<9I')))
            join['arm_joints']=unpack('<6H')
            join['rig'],join['pose_qpc'],join['draw_qpc'],join['capture_sequence']=unpack('<4Q')
            join['valid_flags'],=unpack('<I')
            join['source_job_lookup_status'],=unpack('<I')
            join['source_job_lookup']='offline_t33_palette_base_join' if join['source_job_lookup_status']==1 else 'unknown'
            d['join']=join
        d.update(zip(('width', 'height', 'texture'), unpack('<3I')))
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
        if version >= 12:
            d['mesh'], d['mesh_sources'], d['mesh_views'], d['mesh_range'] = [], [], [], []
            d['mesh_status'] = []
            for _ in range(4):
                ref, identity, view, start, count, status = unpack('<IQQIII')
                d['mesh'].append(ref);d['mesh_sources'].append(identity);d['mesh_views'].append(view);d['mesh_range'].append((start,count));d['mesh_status'].append(status)
            d['mesh_role_names']=['t33_instance_record','t38_current_palette','vb0_instance_ids','other_palette_candidate']
            d['other_palette_relation']='unknown' if d['mesh_sources'][3] else 'unavailable'
        else:
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
        if nb > nd*4:
            raise ValueError('Invalid source buffer count')
        total = 0
        for _ in range(nb):
            if version >= 12:
                b = dict(zip(('frame', 'first_draw', 'whole', 'stride'), unpack('<4I')))
                b['identity'], b['scope'] = unpack('<2Q')
                b['size'], = unpack('<I')
            else:
                b = dict(zip(('frame', 'first_draw', 'whole', 'stride', 'size'), unpack('<5I')))
            total += b['whole']
            mesh_budget=768*1024*1024 if version>=12 else 256*1024*1024
            if not 0 < b['whole'] <= 16*1024*1024 or total > mesh_budget or b['size'] not in (0,b['whole']) or b['first_draw'] >= nd or b['stride'] not in (8,48,336):
                raise ValueError('Invalid source buffer descriptor')
            b['data'] = take(b['size'])
            if version >= 12:
                b['readback_status'], = unpack('<I')
            mesh_buffers.append(b)
        mesh_capture = dict(first_frame=0, frames=0, declined=0, snapshots=0, rejected_source_draws=0)
        if version >= 12:
            (mesh_capture['first_frame'], mesh_capture['frames'], mesh_capture['declined'],
             mesh_capture['snapshots'],mesh_capture['rejected_source_draws']) = unpack('<5I')
            expected=set(range(mesh_capture['first_frame'],mesh_capture['first_frame']+mesh_capture['frames']))
            observed={d['frame'] for d in draws if d.get('mesh_selected',0)}
            mesh_capture['missing_ledger_frames']=sorted(expected-observed)
            mesh_capture['missing_body_ledger_frames']=sorted(expected-{d['frame'] for d in draws if d.get('mesh_selected')==1})
            mesh_capture['candidate_coverage']=[]
            for count in (10050,3216):
                for eye in (0,1):
                    candidates=[d for d in draws if d.get('mesh_selected')==2 and d['count']==count and d['eye']==eye]
                    admitted=[d for d in candidates if d['mesh_status'][:3]==[1,1,1]]
                    copied=[d for d in admitted if candidate_body_payload_available(d,mesh_buffers)]
                    mesh_capture['candidate_coverage'].append(dict(count=count,eye=eye,identity='unproven_body_family_candidate',
                        observed_rows=len(candidates),draw_time_palette_frames=[d['frame'] for d in copied],
                        clip_constant_rows=sum(bool(d['buffers'][3]['data']) for d in candidates),
                        geometry_rows=sum(bool(d['streams'][1]['data']) and bool(d['streams'][2]['data']) for d in candidates),
                        status='draw_time_palettes_copied' if copied else 'staged_readback_unavailable' if admitted else 'metadata_only' if candidates else 'absent'))
        for i,d in enumerate(draws):
            for role,ref in enumerate(d['mesh']):
                if ref == 0xffffffff:
                    continue
                if ref >= nb:
                    raise ValueError('Invalid source buffer reference')
                b = mesh_buffers[ref]
                if b['frame'] != d['frame'] or b['first_draw'] > i or b['stride'] != (336,48,8,48)[role]:
                    raise ValueError('Source buffer belongs to a different frame, role, or later draw')
    else:
        mesh_capture = dict(first_frame=0, frames=0, declined=0, snapshots=0, rejected_source_draws=0)
    effect_images, effect_image_declined = [], 0
    if version >= 6:
        ni, effect_image_declined = unpack('<2I')
        if ni > 32:
            raise ValueError('Invalid effect image count')
        total = 0
        for _ in range(ni):
            e = dict(zip(('draw', 'after', 'x', 'y', 'width', 'height', 'source_width', 'source_height', 'format', 'size'), unpack('<10I')))
            night = e['draw'] < nd and (draws[e['draw']]['vs'], draws[e['draw']]['ps']) == (0xFCF7BD2896751D96, 0xF786D34B5E118D5E)
            roi_family = version >= 10 and e['draw'] < nd and (
                (draws[e['draw']]['vs'], draws[e['draw']]['ps']) == (0x025B4B9FF54622ED, 0x46F92DC71BF8DFA5) or
                (version >= 14 and (draws[e['draw']]['vs'], draws[e['draw']]['ps']) ==
                 (0xB10B032BDFD46700, 0xDB899F4BD577F2E5)))
            inputs = version >= 7 and night and 2 <= e['after'] <= 6
            bc4 = inputs and e['format'] == 80
            formats = {10: 8, 26: 4, 28: 4, 29: 4}
            if inputs:
                formats.update({24: 4, 41: 4})
            bpp = formats.get(e['format'], 0)
            expected = ((e['width']+3)//4)*((e['height']+3)//4)*8 if bc4 else e['width']*e['height']*bpp
            total += expected
            x, y = (e['source_width']-e['width'])//2, (e['source_height']-e['height'])//3
            if roi_family:
                y = (e['source_height']-e['height'])//2
            if bc4:
                x, y = x & ~3, y & ~3
            origin = ((e['x'],e['y']) == (x,y) if night or roi_family else
                      (e['x']+e['width'],e['y']+e['height']) == (e['source_width'],e['source_height']))
            if (e['draw'] >= nd or (not night and not roi_family and draws[e['draw']]['ordinal'] != 0xfffffffd) or
                    (roi_family and e['after'] > 1) or (e['after'] > 1 and not inputs) or
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
    candidate = empty_candidates()
    if version >= 10:
        candidate = read_candidates(unpack,take,draws,effect_images,version)
    failures, = unpack('<I')
    if stream.read(1):
        raise ValueError('Trailing snapshot data')
    return dict(version=version, dropped=dropped, failures=failures, draws=draws, surfaces=surfaces,
                mesh_buffers=mesh_buffers, mesh_declined=mesh_declined, mesh_capture=mesh_capture,
                effect_images=effect_images, effect_image_declined=effect_image_declined, night_sampling=night_sampling,
                sprite_diagnostics=sprite_diagnostics, sprite_declined=sprite_declined, geometry=geometry,
                candidates=candidate)


def empty_candidates():
    return dict(dropped=0,missing_render_frame=0,outside_frame_window=0,input_declined=0,mip_declined=0,input_bytes=0,
                optic_draws=0,roi_incomplete=0,roi_draw=0xffffffff,frames=[],inputs=[],samplers=[])


def read_menu(path, capture=None):
    """Join the bounded menu companion to its v16 binary; missing copies remain explicit."""
    path=Path(path)
    if path.stat().st_size>4*1024*1024:
        raise ValueError('Menu metadata exceeds its bounded schema')
    meta=json.loads(path.read_text(encoding='utf-8'))
    binary=meta.get('binary','')
    if (meta.get('schema')!='edvr_late_menu_v1' or not isinstance(binary,str) or not binary or
            Path(binary).name!=binary or '/' in binary or '\\' in binary or not meta.get('binary_written')):
        raise ValueError('Invalid menu metadata header')
    c=read(path.with_name(binary)) if capture is None else capture
    if (c['version']!=16 or meta.get('mesh_limit')!=128*1024*1024 or meta.get('vertex_limit')!=8*1024*1024 or
            not isinstance(meta.get('selected_frame'),int) or meta['selected_frame']<=0 or
            len(meta.get('rows',[]))>50 or len(meta.get('rows',[]))!=len(c['draws']) or
            [e.get('eye') for e in meta.get('eyes',[])]!=[0,1,-1]):
        raise ValueError('Invalid menu selection or capacity')
    issues={f['draw']:f for f in c['candidates']['frames']}
    seen=set();counts=[[[0,0] for _ in range(2)] for _ in range(3)];scene_ord=[None,None];menu_ord=[None,None]
    family_ids=MENU_FAMILIES
    identities=lambda v:isinstance(v,str) and len(v)==16 and all(z in '0123456789abcdefABCDEF' for z in v)
    for row in meta['rows']:
        n=row.get('draw');role=row.get('role')
        if not isinstance(n,int) or n in seen or n<0 or n>=len(c['draws']) or role not in (1,2):
            raise ValueError('Invalid menu row association')
        seen.add(n);d=c['draws'][n]
        if d['frame']!=meta['selected_frame'] or n not in issues or (role==2)!=(d['vs'] in MENU_SCENES):
            raise ValueError('Menu row does not belong to selected frame/family')
        if role==1:
            if d['vs'] not in family_ids:
                raise ValueError('Unknown menu diagnostic family')
            e=d['eye'] if d['eye'] in (0,1) else 2
            counts[e][family_ids.index(d['vs'])][0 if d['count']==360 else 1]+=1
            if e<2:menu_ord[e]=d['ordinal'] if menu_ord[e] is None else min(menu_ord[e],d['ordinal'])
        elif d['eye'] not in (0,1) or scene_ord[d['eye']] is not None:
            raise ValueError('Menu scene reference has unknown/duplicate eye')
        else:
            scene_ord[d['eye']]=d['ordinal']
        if len(row.get('constants',[]))!=3 or len(row.get('streams',[]))!=3 or len(row.get('ps_inputs',[]))!=16:
            raise ValueError('Menu row binding count differs')
        for slot,binding in enumerate(row['constants']):
            if (not identities(binding.get('identity')) or binding.get('range_known') not in (0,1) or
                    any(not isinstance(binding.get(k),int) or binding[k]<0 or binding[k]>4096 for k in ('first','count'))):
                raise ValueError('Invalid menu constant-buffer binding')
            if int(binding['identity'],16)!=d['buffers'][slot]['identity']:
                raise ValueError('Menu constant identity disagrees with binary')
            payload=d['buffers'][slot]['data']
            binding['status']='absent' if not int(binding['identity'],16) else ('available' if payload else 'copy_unavailable')
            binding['payload_origin_bytes']=binding['first']*16 if binding['range_known'] else 0
            binding['bounded']=bool(payload) and binding['range_known'] and len(payload)<binding['count']*16
        if not all(identities(v) for v in row['streams']):
            raise ValueError('Invalid menu stream identity')
        for slot,input_ in enumerate(row['ps_inputs']):
            if (input_.get('slot')!=slot or not identities(input_.get('view')) or not identities(input_.get('resource')) or
                    len(input_.get('descriptor',[]))!=6 or any(not isinstance(v,int) or not 0<=v<=0xffffffff for v in input_['descriptor'])):
                raise ValueError('Invalid menu texture binding descriptor')
            input_['status']='absent' if not int(input_['view'],16) else 'identity_only'
        row['outcome']=('skipped','issued','unavailable')[issues[n]['issued']]
        avatar=row.get('avatar',dict(status=0,valid_flags=0))
        if avatar.get('status') not in (0,1,2) or not isinstance(avatar.get('valid_flags'),int) or not 0<=avatar['valid_flags']<=63:
            raise ValueError('Invalid avatar camera evidence status')
        if avatar['status']:
            for key in ('requested_frame','source_frame','fp_frame','root_read_render_frame','g_game_frame','item_serial',
                        'sample_qpc','qpc','fp_qpc','root_qpc','ms','root_ms','qpc_frequency'):
                if not isinstance(avatar.get(key),int) or avatar[key]<0:
                    raise ValueError('Invalid avatar camera evidence stamp')
            if avatar['requested_frame']!=d['render_frame'] or (avatar['status']==2 and avatar['valid_flags']&(2|8)):
                raise ValueError('Cached avatar evidence mislabeled exact or pinned')
            for key,size in (('froot',16),('m3p',16),('G',16),('body_offset',3),('pin_eye_target',3),('pin_target',3)):
                values=avatar.get(key,[])
                if len(values)!=size or any(v is not None and (not isinstance(v,(int,float)) or not math.isfinite(v)) for v in values):
                    raise ValueError('Invalid avatar camera numeric payload')
                if avatar['valid_flags']&1 and key in ('froot','m3p','G') and any(v is None for v in values):
                    raise ValueError('Valid avatar matrices contain unavailable numbers')
            for key in ('qpc','fp_qpc','root_qpc'):
                avatar[key+'_age_ms']=(1000*(avatar['sample_qpc']-avatar[key])/avatar['qpc_frequency']
                    if avatar['qpc_frequency'] and avatar[key] and avatar['sample_qpc']>=avatar[key] else None)
        avatar['source_status']=('unavailable','hook_frame_match','cached_not_exact')[avatar['status']]
        avatar['sources_exact']=bool(avatar['valid_flags']&2)
        avatar['times_fresh']=bool(avatar['valid_flags']&4)
        avatar['pin_targets_available']=bool(avatar['valid_flags']&8)
        avatar['coordinate_spaces']='froot/m3p model-to-world; G FP-to-3P; body offset in world metres; pin targets in 3P model metres'
        row['avatar']=avatar
        if row.get('eye_source',0) not in (0,1,2,3):
            raise ValueError('Invalid menu eye mapping source')
        row['eye_mapping']=('unknown','scene_depth','current_registered_ui_target','cached_redirected_ui_target')[row.get('eye_source',0)]
        if role==1:
            try:issues[n]['index_status']=_validate_candidate_indices(d)
            except ValueError as exc:issues[n]['index_status']='invalid: '+str(exc)
        else:issues[n]['index_status']='not_requested_scene_reference'
        row['layout_status']='available' if d['layout'] else 'unavailable'
        row['stream_status']=['absent' if not int(identity,16) else
                              ('available' if stream['data'] else 'copy_unavailable_or_not_requested')
                              for identity,stream in zip(row['streams'],d['streams'])]
        row['structured_status']=[{0:'absent',1:'copy_issued',2:'unsupported_view',3:'unsupported_offset',4:'unsupported_stride',
                                   5:'size_or_quota',6:'allocation_failed',7:'quota',8:'not_selected',9:'unavailable',10:'unknown_eye',11:'instance_stream_window'}.get(s,'unknown')
                                  for s in d['mesh_status'][:3]]
        for slot,ref in enumerate(d['mesh'][:2]):
            if row['structured_status'][slot]=='copy_issued':
                row['structured_status'][slot]='available' if ref<len(c['mesh_buffers']) and c['mesh_buffers'][ref]['data'] else 'copy_unavailable'
    for e,eye in enumerate(meta['eyes']):
        if (not isinstance(eye.get('scene_before_menu'),bool) or len(eye.get('families',[]))!=2 or
                not isinstance(eye.get('mesh_bytes'),int) or not 0<=eye['mesh_bytes']<=(16 if e==2 else 56)*1024*1024 or
                not isinstance(eye.get('vertex_bytes'),int) or not 0<=eye['vertex_bytes']<=(2 if e==2 else 3)*1024*1024):
            raise ValueError('Invalid menu eye reservation')
        if (eye['scene_before_menu']!=(e<2 and scene_ord[e] is not None) or
                (e<2 and scene_ord[e] is not None and menu_ord[e] is not None and scene_ord[e]>=menu_ord[e])):
            raise ValueError('Menu scene reference did not precede its menu')
        for family,recorded in zip(eye['families'],counts[e]):
            for k in ('anchor_seen','siblings_seen','anchor_kept','siblings_kept','anchor_quota','siblings_quota'):
                if not isinstance(family.get(k),int) or family[k]<0:
                    raise ValueError('Invalid menu family counter')
            if ([family['anchor_kept'],family['siblings_kept']]!=recorded or max(recorded)>4 or
                    family['anchor_seen']!=family['anchor_kept']+family['anchor_quota'] or
                    family['siblings_seen']!=family['siblings_kept']+family['siblings_quota']):
                raise ValueError('Menu family count disagrees with retained draws')
        for family,vs in zip(eye['families'],MENU_FAMILIES):family['vs']=f'{vs:016X}'
        eye['anchor_status']='present' if eye['families'][0]['anchor_kept'] else 'absent'
        eye['scene_status']='present_before_menu' if eye['scene_before_menu'] else 'unavailable'
    meta['anchor_status']='both_eyes' if all(e['anchor_status']=='present' for e in meta['eyes'][:2]) else 'absent_or_one_eye'
    meta['camera_context']='bound draw constants plus existing F81/cached IkG avatar reference and freshness; no game camera getter or picking mesh captured'
    meta['avatar_sources']=dict(Counter(r['avatar']['source_status'] for r in meta['rows']))
    meta['outcomes']=dict(Counter(r['outcome'] for r in meta['rows']))
    meta['eye_mappings']=dict(Counter(r['eye_mapping'] for r in meta['rows']))
    meta['menu_geometry']=dict(Counter(issues[r['draw']]['index_status'] for r in meta['rows'] if r['role']==1))
    meta['input_summary']=validate_menu_input(meta.get('input_observer'),meta['selected_frame'])
    c['late_menu']=meta
    return c


def validate_menu_input(data,selected_frame):
    if data is None:return dict(status='not_captured')
    if (data.get('schema')!='edvr_menu_input_v1' or not isinstance(data.get('capture_id'),str) or
            not data['capture_id'].isdigit() or data.get('getter_calls')!=0 or data.get('update_capture_gate') is not False or
            not isinstance(data.get('first_ledger_frame'),int) or not isinstance(data.get('last_ledger_frame'),int) or
            not data['first_ledger_frame']<=selected_frame<=data['last_ledger_frame'] or
            data['last_ledger_frame']-data['first_ledger_frame']>=20 or len(data.get('rows',[]))>160 or len(data.get('meshes',[]))>8):
        raise ValueError('Invalid bounded menu input observer header')
    for field in ('hooks_armed','qpc_frequency','row_quota','mesh_quota','read_faults','contended','placements_begun','in_flight_at_close'):
        if not isinstance(data.get(field),int) or data[field]<0:
            raise ValueError('Invalid menu input observer counter')
    def floats(values,size):
        return isinstance(values,list) and len(values)==size and all(v is None or isinstance(v,(int,float)) and math.isfinite(v) for v in values)
    for mesh in data['meshes']:
        if (mesh.get('ledger_frame')!=data['first_ledger_frame']+1 or mesh.get('space')!='cached_pick_camera_frame' or
                not 1<=len(mesh.get('triangles',[]))<=256):raise ValueError('Invalid menu input mesh frame/capacity')
        for tri in mesh['triangles']:
            if len(tri)!=6 or not all(floats(v,3 if i<3 else 2) for i,v in enumerate(tri)):
                raise ValueError('Invalid cached input triangle/UV payload')
    counts=[0,0];mesh_counts=[0,0]
    for mesh in data['meshes']:mesh_counts[0 if len(mesh['triangles'])==120 else 1]+=1
    if max(mesh_counts)>4:raise ValueError('Menu input mesh reservation overflow')
    for row in data['rows']:
        if (not isinstance(row.get('ledger_frame'),int) or not data['first_ledger_frame']<=row['ledger_frame']<=data['last_ledger_frame'] or
                not isinstance(row.get('before_qpc'),int) or not isinstance(row.get('after_qpc'),int) or row['after_qpc']<row['before_qpc'] or
                row.get('mesh_status') not in range(1,8) or not isinstance(row.get('triangle_count'),int) or row['triangle_count']<0):
            raise ValueError('Invalid menu input row stamps/status')
        counts[0 if row['triangle_count']==120 else 1]+=1
        for field,size in (('mouse',2),('other',2),('direction',3),('uv',2),('ray_origin',3),('ray_direction',3),('ray_t',1),('sprite',3),('owner_world',16),('owner_output',16)):
            if not floats(row.get(field),size):raise ValueError('Invalid menu input numeric payload')
        for field in ('ray_calls','window_calls','owner_calls','accepted_ray','read_flags','button_valid','mode_kind'):
            if not isinstance(row.get(field),int) or row[field]<0:raise ValueError('Invalid menu input callback count')
        if row['mesh_status'] in (6,7):
            if not isinstance(row.get('mesh'),int) or not 0<=row['mesh']<len(data['meshes']):raise ValueError('Invalid menu input mesh association')
            mesh=data['meshes'][row['mesh']]
            if any(row[k]!=mesh[k] for k in ('panel','list','owner','ledger_frame')) or row['triangle_count']!=len(mesh['triangles']):
                raise ValueError('Menu input row disagrees with cached mesh provenance')
        elif row.get('mesh')!=0xffffffff:raise ValueError('Unavailable menu input mesh has a live association')
        row['branch']=('ray_and_2d' if row['ray_calls'] and row['window_calls'] else 'ray' if row['ray_calls'] else '2d_window' if row['window_calls'] else 'no_callback')
        row['callback_payload_status']='multiple_calls_last_payload_not_a_unique_geometric_join' if row['ray_calls']+row['window_calls']>1 else 'single_call' if row['ray_calls']+row['window_calls'] else 'absent'
        row['mode_status']='plain_accessor_read' if row['read_flags']&16 else 'unavailable_without_calling_getter'
        row['owner_transform_status']=('mixed_observed_owners' if row.get('observed_owner_mixed') else
            'matching_panel_owner' if row['read_flags']&1024 and row.get('observed_owner') and row['observed_owner']==row.get('owner') else
            'owner_mismatch' if row['read_flags']&1024 else 'not_observed')
        row['mesh_availability']={1:'absent',2:'triangle_cap',3:'read_fault_or_changed',4:'not_selected_complete_frame',5:'mesh_quota',6:'first_copy_in_same_frame',7:'copied_after_original'}[row['mesh_status']]
    if max(counts)>80 or len(data['rows'])+data['in_flight_at_close']>data['placements_begun']:
        raise ValueError('Menu input row reservation/count mismatch')
    return dict(status='observed' if data['rows'] else 'no_placement_callbacks',rows=len(data['rows']),meshes=len(data['meshes']),
                branches=dict(Counter(r['branch'] for r in data['rows'])),hooks_armed=data['hooks_armed'],
                row_quota=data['row_quota'],mesh_quota=data['mesh_quota'],read_faults=data['read_faults'],
                contended=data['contended'],in_flight_at_close=data['in_flight_at_close'],
                association='time_and_geometry_only_not_proven_GPU_owner')


def read_candidates(unpack,take,draws,effect_images,version=11):
    dropped,missing,outside,input_declined,mip_declined,input_bytes,optic_draws,roi_incomplete,roi_draw=unpack('<9I')
    if version>=13:
        ui_counts=unpack('<48I');ui_dropped=unpack('<48I')
    else:
        ui_counts=ui_dropped=(0,)*48
    ui_image_bytes=unpack('<16I') if version>=15 else (0,)*16
    nf,=unpack('<I')
    max_candidates=1152 if version>=15 else 384
    max_input_bytes=128*1024*1024 if version>=15 else 24*1024*1024
    if nf>max_candidates or input_bytes>max_input_bytes or any(x>6*1024*1024 for x in ui_image_bytes) or (roi_draw!=0xffffffff and roi_draw>=len(draws)):
        raise ValueError('Invalid targeted candidate header')
    frames=[];seen=set()
    for _ in range(nf):
        draw,ledger_frame,render_frame=unpack('<3I');eye,issued,roi_captured,roi_complete=unpack('<i3I')
        issue = None
        if version>=13:
            issue_status, actual_vs, actual_ps, b0_id, target_id, width, height, rtv_format, b0_first, b0_count, b0_range = unpack('<I4Q6I')
            camera_cb_slot,=unpack('<I') if version>=15 else (0,)
            rtv_size,=unpack('<I');rtv=take(rtv_size)
            nv,ns=unpack('<2I')
            if rtv_size not in (0,20) or nv>16 or ns>16:
                raise ValueError('Invalid issued UI draw state descriptors')
            if nv:
                viewport_size,=unpack('<I')
                if viewport_size!=nv*24:
                    raise ValueError('Invalid issued UI viewport blob length')
                viewports=take(viewport_size)
            else:
                viewports=b''
            if ns:
                scissor_size,=unpack('<I')
                if scissor_size!=ns*16:
                    raise ValueError('Invalid issued UI scissor blob length')
                scissors=take(scissor_size)
            else:
                scissors=b''
            b0_expected,=unpack('<I');b0_size,=unpack('<I')
            if issue_status not in (0,1,2,3,4,5) or b0_range not in (0,1) or b0_expected>65536 or b0_size not in (0,b0_expected) or (b0_range and b0_count>4096) or camera_cb_slot not in (0,1):
                raise ValueError('Invalid issued UI draw state status')
            b0=take(b0_size)
            if issue_status==2 and (issued!=1 or not b0_id or not target_id or not width or not height or not b0_expected):
                raise ValueError('Issued UI state lacks its bound b0/target payload')
            if issue_status==2 and version>=15 and camera_cb_slot==1 and (
                    b0_expected < (b0_first+276)*16 or (b0_range and b0_count<276)):
                raise ValueError('025B issued camera b1 does not cover declared cb1[270..275]')
            if issue_status!=2 and (b0_size or b0_expected):
                raise ValueError('Unmatched/refused issued UI state carries b0 bytes')
            issue=dict(status=issue_status,b0_identity=b0_id,camera_cb_identity=b0_id,camera_cb_slot=camera_cb_slot,
                       b0_status='unused' if version>=15 and camera_cb_slot==1 else ('available' if b0_size else 'map_not_ready'),
                       camera_cb_status='available' if b0_size else 'map_not_ready',
                       target_identity=target_id,width=width,height=height,
                       actual_vs=actual_vs,actual_ps=actual_ps,
                       rtv_format=rtv_format,b0_first_constant=b0_first,b0_num_constants=b0_count,
                       b0_range_known=bool(b0_range),rtv=rtv,viewports=viewports,scissors=scissors,
                       b0_expected=b0_expected,b0_payload_status=('available' if b0_size else 'map_not_ready'),b0=b0)
        menu_family=version>=16 and draw<len(draws) and draws[draw]['vs'] in MENU_FAMILIES+MENU_SCENES
        if (draw>=len(draws) or draw in seen or (not render_frame and not menu_family) or draws[draw]['frame']!=ledger_frame or
                eye not in (-1,0,1) or issued not in (0,1,2) or roi_captured not in (0,1) or roi_complete not in (0,1) or
                roi_complete and (issued!=1 or roi_captured!=1) or
                (not menu_family and draws[draw]['vs']!=0x154FB5A453F3D2E9 and
                 (draws[draw]['vs'],draws[draw]['ps']) not in ((0x025B4B9FF54622ED,0x46F92DC71BF8DFA5),
                                                               (0xB10B032BDFD46700,0xDB899F4BD577F2E5)))):
            raise ValueError('Invalid targeted candidate frame association')
        if version>=13:
            expected=(draws[draw]['vs'],draws[draw]['ps'])
            actual=(issue['actual_vs'],issue['actual_ps'])
            if (issue['status']==2 and actual!=expected) or (issue['status']==5 and actual==expected):
                raise ValueError('Issued UI state shader identity disagrees with the candidate')
            if version>=15 and issue['camera_cb_slot']!=(1 if menu_family or expected[0]==0x025B4B9FF54622ED else 0):
                raise ValueError('Issued UI camera constant-buffer slot disagrees with the shader family')
        seen.add(draw);frames.append(dict(draw=draw,ledger_frame=ledger_frame,render_frame=render_frame,eye=eye,
                                          issued=issued,roi_captured=roi_captured,roi_complete=roi_complete,issue=issue))
    ni,=unpack('<I')
    if ni>(4608 if version>=15 else 1536):
        raise ValueError('Invalid targeted candidate input count')
    inputs=[];total=0
    for _ in range(ni):
        fields=unpack('<23I' if version>=11 else '<22I')
        names=('draw','slot','kind','view_dimension','format','width','height','source_width','source_height',
               'source_mip_levels','source_array_size','mip','requested_mip','mip_levels','requested_mip_levels','first_slice',
               'slices','first_element','elements','stride','row_bytes','rows','bytes')
        if version<11:
            fields=fields[:12]+(fields[12],)+fields[13:]
            names=names[:12]+names[13:]
        item=dict(zip(names,fields))
        if version>=14:
            item['source_resource'],item['source_view']=unpack('<2Q')
            if not item['source_resource'] or not item['source_view']:
                raise ValueError('Missing targeted PS source identity')
        if version>=15:
            item['sample_mode'],item['source_format']=unpack('<2I')
            if item['sample_mode'] not in (0,1,2):
                raise ValueError('Invalid targeted texture sample mode')
        ui_family=(draws[item['draw']]['vs'],draws[item['draw']]['ps'])==(0xB10B032BDFD46700,0xDB899F4BD577F2E5) if item['draw'] in seen else False
        optic_family=(draws[item['draw']]['vs'],draws[item['draw']]['ps'])==(0x025B4B9FF54622ED,0x46F92DC71BF8DFA5) if item['draw'] in seen else False
        if (item['draw'] not in seen or not (ui_family or optic_family) or item['slot'] not in ((0,1,2,3) if ui_family else (1,2,3)) or
                item['kind'] not in (0,1) or item['bytes']>max_input_bytes):
            raise ValueError('Invalid targeted PS input descriptor')
        expected=item['bytes']
        total+=expected
        if total>max_input_bytes:
            raise ValueError('Targeted input budget exceeded')
        if item['kind']==0:
            if item['view_dimension'] not in (1,11):
                raise ValueError('Invalid targeted buffer view')
            size,=unpack('<I')
            if size not in (0,expected):
                raise ValueError('Invalid targeted buffer payload')
            item['data']=take(size)
        else:
            if item['view_dimension'] not in (4,5):
                raise ValueError('Invalid targeted texture view')
            if (not item['width'] or not item['height'] or not item['source_width'] or not item['source_height'] or
                    not item['source_mip_levels'] or not item['source_array_size'] or not item['slices'] or
                    not item['mip_levels'] or item['mip_levels']>item['requested_mip_levels'] or
                    item['mip']<item.get('requested_mip',item['mip']) or
                    item.get('requested_mip',item['mip'])+item['requested_mip_levels']>item['source_mip_levels'] or
                    item['mip']+item['mip_levels']>item['source_mip_levels'] or
                    item['first_slice']+item['slices']>item['source_array_size'] or
                    (item.get('sample_mode',0)!=2 and (item['width']!=max(1,item['source_width']>>item['mip']) or
                     item['height']!=max(1,item['source_height']>>item['mip'])))):
                raise ValueError('Invalid targeted texture dimensions')
            if item.get('sample_mode',0)==1 and (version<15 or max(item['width'],item['height'])>384):
                raise ValueError('Invalid whole-image mip sample descriptor')
            ui_texture_slot=2 if optic_family else 0
            if item.get('sample_mode',0)==2 and (version<15 or item['slot']!=ui_texture_slot or item['source_format'] not in (28,29) or
                    item['format']!=28 or item['width']!=384 or item['height']!=384 or item['mip']!=item['requested_mip']):
                raise ValueError('Invalid full-image point sample descriptor')
            if item.get('sample_mode',0)==2:
                item['sample_mapping']='full-image-center-point'
                item['sample_color_semantics']='srgb-decoded-linear-unorm8' if item['source_format']==29 else 'linear-unorm8'
            elif item.get('sample_mode',0)==1:
                item['sample_mapping']='whole-image-source-mip'
                item['sample_color_semantics']='source-format-preserved'
            else:
                item['sample_mapping']='source-mip-chain'
                item['sample_color_semantics']='source-format-preserved'
            subresources=[];actual=0
            for mip in range(item['mip_levels']):
                w=max(1,item['width']>>mip);h=max(1,item['height']>>mip)
                row_bytes,rows=_candidate_texture_layout(item['format'],w,h)
                if not row_bytes:
                    raise ValueError('Unsupported targeted texture format')
                for _ in range(item['slices']):
                    size,=unpack('<I');sub_bytes=row_bytes*rows
                    if size not in (0,sub_bytes):
                        raise ValueError('Invalid targeted texture mip payload')
                    subresources.append(take(size));actual+=sub_bytes
            if actual!=expected or item['row_bytes']*item['rows']>expected:
                raise ValueError('Targeted texture byte count mismatch')
            item['data_subresources']=subresources
        inputs.append(item)
    refusals=[]
    if version>=11:
        refusal_count,=unpack('<I')
        if refusal_count>(4608 if version>=15 else 96):
            raise ValueError('Invalid targeted PS input refusal count')
        for _ in range(refusal_count):
            fields=unpack('<14I')
            names=('draw','slot','kind','view_dimension','format','source_width','source_height','source_mip_levels',
                   'source_array_size','requested_mip','requested_mip_levels','first_slice','slices','reason')
            item=dict(zip(names,fields))
            if version>=14:
                item['source_resource'],item['source_view']=unpack('<2Q')
                if not item['source_resource'] or not item['source_view']:
                    raise ValueError('Missing refused PS source identity')
            family=(draws[item['draw']]['vs'],draws[item['draw']]['ps']) if item['draw'] in seen else None
            if (item['draw'] not in seen or family not in ((0x025B4B9FF54622ED,0x46F92DC71BF8DFA5),(0xB10B032BDFD46700,0xDB899F4BD577F2E5)) or
                    item['slot'] not in ((1,2,3) if family==(0x025B4B9FF54622ED,0x46F92DC71BF8DFA5) else (0,1,2,3)) or
                    item['kind']!=1 or item['reason'] not in (1,2,3,4) or item['view_dimension'] not in (4,5) or
                    not item['source_width'] or not item['source_height'] or not item['source_mip_levels'] or
                    not item['source_array_size'] or not item['requested_mip_levels'] or not item['slices'] or
                    item['requested_mip']+item['requested_mip_levels']>item['source_mip_levels'] or
                    item['first_slice']+item['slices']>item['source_array_size']):
                raise ValueError('Invalid targeted PS input refusal descriptor')
            refusals.append(item)
    sampler_count,=unpack('<I')
    if sampler_count>(1152 if version>=15 else 384):
        raise ValueError('Invalid targeted sampler count')
    samplers=[]
    for _ in range(sampler_count):
        draw,present=unpack('<2I');size,=unpack('<I')
        if draw not in seen or (draws[draw]['vs'],draws[draw]['ps']) not in ((0x025B4B9FF54622ED,0x46F92DC71BF8DFA5),(0xB10B032BDFD46700,0xDB899F4BD577F2E5)) or present>1 or size!=52:
            raise ValueError('Invalid targeted sampler descriptor')
        samplers.append(dict(draw=draw,present=present,descriptor=take(size)))
    optic_rows=[f for f in frames if (draws[f['draw']]['vs'],draws[f['draw']]['ps'])==(0x025B4B9FF54622ED,0x46F92DC71BF8DFA5)]
    roi_rows=[f for f in frames if (draws[f['draw']]['vs'],draws[f['draw']]['ps'])==
        (0x025B4B9FF54622ED,0x46F92DC71BF8DFA5) or
        (version>=14 and (draws[f['draw']]['vs'],draws[f['draw']]['ps'])==
         (0xB10B032BDFD46700,0xDB899F4BD577F2E5))]
    if optic_draws!=len(optic_rows):
        raise ValueError('Targeted optic draw count mismatch')
    index_missing=0
    for frame in optic_rows:
        try:
            frame['index_status']=_validate_candidate_indices(draws[frame['draw']])
        except ValueError as exc:
            # One incomplete candidate must remain inspectable alongside
            # successful captures from the same run.
            frame['index_status']='invalid: '+str(exc)
        index_missing+=frame['index_status']!='validated'
    roi_bytes=0
    roi_payload_missing=0
    for frame in roi_rows:
        if not frame['roi_captured']:
            continue
        roi=[e for e in effect_images if e['draw']==frame['draw'] and e['after']<=1]
        if len(roi)!=2 or {e['after'] for e in roi}!={0,1} or sum(e['width']*e['height']*({10:8,26:4,28:4,29:4}.get(e['format'],0)) for e in roi)>8*1024*1024:
            raise ValueError('Invalid targeted before/after ROI pair')
        for e in roi:
            if (e['width']>512 or e['height']>512 or e['x']!=(e['source_width']-e['width'])//2 or
                    e['y']!=(e['source_height']-e['height'])//2):
                raise ValueError('Targeted ROI is not centered or exceeds its bound')
            roi_bytes+=e['width']*e['height']*({10:8,26:4,28:4,29:4}.get(e['format'],0))
        if frame['roi_complete'] and not next(e for e in roi if e['after']==1)['data']:
            roi_payload_missing+=1
    matched_roi=[e for e in effect_images if e['after']<=1 and (
        (draws[e['draw']]['vs'],draws[e['draw']]['ps'])==(0x025B4B9FF54622ED,0x46F92DC71BF8DFA5) or
        (version>=14 and (draws[e['draw']]['vs'],draws[e['draw']]['ps'])==
         (0xB10B032BDFD46700,0xDB899F4BD577F2E5)))]
    if any(e['draw'] not in {f['draw'] for f in roi_rows if f['roi_captured']} for e in matched_roi) or roi_bytes>8*1024*1024:
        raise ValueError('Unassociated targeted ROI image or budget exceeded')
    if roi_draw!=0xffffffff and not any(f['draw']==roi_draw and f['roi_captured'] for f in optic_rows):
        raise ValueError('Targeted ROI summary draw missing from candidate ledger')
    if version>=15 and total!=input_bytes:
        raise ValueError('Targeted input payload descriptors disagree with the header byte total')
    ui_coverage=[dict(index=i,eye=e if e<2 else None,domain='unknown_eye' if e==2 else 'eye',
                      candidate_count=ui_counts[i*3+e],refused_count=ui_dropped[i*3+e],
                      status=('refused' if ui_dropped[i*3+e] else 'observed' if ui_counts[i*3+e] else 'not_observed'))
                 for i in range(16) for e in range(3)] if version>=13 else []
    return dict(dropped=dropped,missing_render_frame=missing,outside_frame_window=outside,
                input_declined=input_declined,mip_declined=mip_declined,input_bytes=input_bytes,optic_draws=optic_draws,
                roi_incomplete=roi_incomplete,roi_payload_missing=roi_payload_missing,index_missing=index_missing,
                roi_draw=roi_draw,frames=frames,inputs=inputs,refusals=refusals,samplers=samplers,ui_coverage=ui_coverage,
                ui_image_bytes=ui_image_bytes)


def _candidate_texture_layout(fmt,width,height):
    bpp={61:1,65:1,49:2,54:2,56:2,28:4,29:4,87:4,91:4,34:4,35:4,41:4,24:4,26:4,10:8,11:8,16:8,2:16}.get(fmt)
    if bpp:
        return width*bpp,height
    if fmt in (71,72,80):
        return ((width+3)//4)*8,(height+3)//4
    if fmt in (74,75,77,78,83,95,98,99):
        return ((width+3)//4)*16,(height+3)//4
    return 0,0


def _validate_candidate_indices(draw):
    index=draw['streams'][2]
    if draw['kind'] not in (ord('X'),ord('I')) or index['stride'] not in (2,4) or draw['count']<1:
        raise ValueError('Targeted optic draw has no bounded indexed window')
    index_begin=index['offset']+draw['start']*index['stride']-index['capture_offset']
    needed=draw['count']*index['stride']
    if index_begin<0 or index_begin+needed>len(index['data']):
        if not index['data']:
            return 'index_readback_missing'
        raise ValueError('Targeted optic index window does not cover the draw range')
    if not index['data']:
        return 'index_readback_missing'
    code='H' if index['stride']==2 else 'I'
    indices=struct.unpack('<'+code*draw['count'],index['data'][index_begin:index_begin+needed])
    per_vertex=[e for e in draw['layout'] if e['classification']==0]
    if not per_vertex:
        return 'vertex_layout_missing'
    missing=False
    for element in per_vertex:
        stream=draw['streams'][element['slot']]
        if not stream['data'] or not stream['stride']:
            missing=True;continue
        element_bytes={2:16,3:16,6:12,10:8,16:8,17:8,24:4,28:4,34:4,41:4,49:2,54:2,56:2}.get(element['format'])
        if not element_bytes:
            raise ValueError('Unsupported targeted optic vertex format')
        for value in indices:
            vertex=value+draw['base']
            offset=stream['offset']+vertex*stream['stride']+element['offset']-stream['capture_offset']
            if vertex<0 or offset<0 or offset+element_bytes>len(stream['data']):
                raise ValueError('Targeted optic index escapes retained vertex window')
    return 'vertex_readback_missing' if missing else 'validated'


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
    assert capture['version'] == 15 and capture['failures'] == 0
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


def verify_candidate_fixture(capture):
    c=capture['candidates']
    assert capture['version']==15 and capture['failures']==0
    assert c['optic_draws']==1 and len(c['frames'])==1
    frame=c['frames'][0]
    assert (frame['draw'],frame['ledger_frame'],frame['render_frame'],frame['eye'])==(0,101,501,1)
    assert (frame['issued'],frame['roi_captured'],frame['roi_complete'],frame['index_status'])==(1,1,1,'validated')
    assert len(c['inputs'])==1 and c['inputs'][0]['slot']==1 and c['inputs'][0]['data']==bytes([0x5a])*96
    assert len(c['samplers'])==1 and c['samplers'][0]['present']==1 and len(c['samplers'][0]['descriptor'])==52
    roi=[e for e in capture['effect_images'] if e['draw']==0 and e['after']<=1]
    assert len(roi)==2 and {e['after'] for e in roi}=={0,1}
    assert all((e['x'],e['y'],e['width'],e['height'],e['source_width'],e['source_height'])==(0,0,512,512,512,512) for e in roi)
    assert len(roi[0]['data'])==len(roi[1]['data'])==512*512*4 and roi[0]['data']!=roi[1]['data']


def verify_candidate_capacity_fixture(capture):
    c=capture['candidates']
    assert capture['version']==15 and len(capture['draws'])==1152 and c['dropped']==0
    assert len(c['frames'])==1152 and c['optic_draws']==0
    assert sum(row['candidate_count'] for row in c['ui_coverage'])==768
    assert all(row['candidate_count']==16 and row['refused_count']==0 for row in c['ui_coverage'])
    ui_vs=0xB10B032BDFD46700;ui_ps=0xDB899F4BD577F2E5
    assert sum((capture['draws'][row['draw']]['vs'],capture['draws'][row['draw']]['ps'])==(ui_vs,ui_ps) for row in c['frames'])==768


def candidate_body_payload_available(draw,buffers):
    refs=draw.get('mesh',[])[:3]
    return len(refs)==3 and all(isinstance(ref,int) and 0<=ref<len(buffers) and buffers[ref].get('readback_status')==1 and bool(buffers[ref].get('data')) for ref in refs)


def menu_self_test():
    import copy
    c=dict(version=16,draws=[],mesh_buffers=[],candidates=dict(frames=[]))
    meta=dict(schema='edvr_late_menu_v1',binary='menu.bin',binary_written=True,selected_frame=11,
              mesh_limit=128*1024*1024,vertex_limit=8*1024*1024,eyes=[],rows=[])
    for e in (0,1,-1):
        family=dict(anchor_seen=0,siblings_seen=0,anchor_kept=0,siblings_kept=0,anchor_quota=0,siblings_quota=0)
        meta['eyes'].append(dict(eye=e,scene_before_menu=False,mesh_bytes=0,vertex_bytes=0,families=[copy.copy(family),copy.copy(family)]))
    for eye in (0,1):
        c['draws'].append(dict(frame=11,ordinal=eye,vs=0x5DAD41AA66757CE3,eye=eye,count=6,kind=ord('I'),start=0,base=0,
                               buffers=[dict(identity=0,data=b'') for _ in range(3)],layout=[],
                               streams=[dict(data=b'',stride=0) for _ in range(3)],mesh_status=[0,0,0,0],mesh=[0xffffffff]*4))
        c['candidates']['frames'].append(dict(draw=eye,issued=2))
        meta['rows'].append(dict(draw=eye,role=1,constants=[dict(identity='0'*16,first=0,count=0,range_known=1) for _ in range(3)],
            streams=['0'*16]*3,ps_inputs=[dict(slot=i,view='0'*16,resource='0'*16,descriptor=[0]*6) for i in range(16)]))
        meta['eyes'][eye]['families'][0].update(siblings_seen=1,siblings_kept=1)
    with tempfile.TemporaryDirectory() as tmp:
        path=Path(tmp)/'menu.json'
        def check(value,capture=c):
            path.write_text(json.dumps(value),encoding='utf-8');return read_menu(path,copy.deepcopy(capture))
        parsed=check(meta)['late_menu']
        assert parsed['anchor_status']=='absent_or_one_eye' and all(e['anchor_status']=='absent' for e in parsed['eyes'])
        assert parsed['rows'][0]['outcome']=='unavailable' and parsed['rows'][0]['constants'][0]['status']=='absent'
        bad=[]
        x=copy.deepcopy(meta);x['eyes'][0]['families'][0]['anchor_kept']=1;bad.append(x)
        x=copy.deepcopy(meta);x['eyes'][0]['scene_before_menu']=True;bad.append(x)
        x=copy.deepcopy(meta);x['binary']='../escape.bin';bad.append(x)
        x=copy.deepcopy(meta);x['rows'][0]['constants'][0]['identity']='1'*16;bad.append(x)
        x=copy.deepcopy(meta);x['eyes'][0]['mesh_bytes']=57*1024*1024;bad.append(x)
        for x in bad:
            try:check(x)
            except ValueError:pass
            else:raise AssertionError('Malformed menu metadata accepted')
        try:check(meta,dict(c,version=15))
        except ValueError:pass
        else:raise AssertionError('Old binary mislabeled as menu evidence')


def self_test():
    menu_self_test()
    available=[dict(readback_status=1,data=b'x') for _ in range(3)]
    assert candidate_body_payload_available(dict(mesh=[0,1,2]),available)
    available[1]=dict(readback_status=2,data=b'')
    assert not candidate_body_payload_available(dict(mesh=[0,1,2]),available), 'Staging admission incorrectly reported a failed readback as available'
    available[1]=dict(readback_status=1,data=b'')
    assert not candidate_body_payload_available(dict(mesh=[0,1,2]),available), 'Empty retained palette incorrectly reported as available'
    def candidate_reader(blob, draws=None, effects=None, version=11):
        stream=io.BytesIO(blob)
        def take(n):
            value=stream.read(n)
            if len(value)!=n:raise ValueError('Truncated draw snapshot')
            return value
        def unpack(fmt):return struct.unpack(fmt,take(struct.calcsize(fmt)))
        return read_candidates(unpack,take,draws or [],effects or [],version)
    empty_candidate=struct.pack('<8I',0,0,0,0,0,0,0,0)+struct.pack('<I',0xffffffff)+struct.pack('<I',0)+struct.pack('<3I',0,0,0)
    assert candidate_reader(empty_candidate)['frames']==[]
    try:
        malformed_candidate=struct.pack('<8I',0,0,0,0,0,0,1,0)+struct.pack('<I',0xffffffff)+struct.pack('<3I',0,0,0)
        candidate_reader(malformed_candidate)
    except ValueError:
        pass
    else:
        raise AssertionError('Optic draw count without candidate rows accepted')
    # F83 v14's centered B10 ROI and length-prefixed issued-state blobs.
    ui_vs,ui_ps=0xB10B032BDFD46700,0xDB899F4BD577F2E5
    ui_draw=dict(frame=18192,vs=ui_vs,ps=ui_ps,kind=ord('I'),count=2,start=3,base=4,layout=[],
                 streams=[{},dict(offset=0,stride=48,capture_offset=0,data=bytes(96)),
                          dict(offset=0,stride=2,capture_offset=0,data=struct.pack('<2H',0,1))])
    viewport=struct.pack('<6f',0.25,0.5,512,512,0,1)
    scissor=struct.pack('<4i',0,0,512,512)
    b0=bytes(range(16))
    def ui_candidate(viewport_size=24,scissor_size=16):
        out=struct.pack('<9I',0,0,0,0,0,0,0,0,0xffffffff)
        out+=struct.pack('<48I',*([0]*48))+struct.pack('<48I',*([0]*48))
        out+=struct.pack('<I',1)+struct.pack('<3I',0,18192,501)+struct.pack('<i3I',1,1,1,1)
        out+=struct.pack('<I4Q6I',2,ui_vs,ui_ps,0x44,0x101,512,512,26,0,1,1)
        out+=struct.pack('<I',0)+struct.pack('<2I',1,1)
        out+=struct.pack('<I',viewport_size)+viewport+struct.pack('<I',scissor_size)+scissor
        out+=struct.pack('<2I',len(b0),len(b0))+b0
        out+=struct.pack('<I',0)+struct.pack('<I',0)+struct.pack('<I',0)
        return out
    roi=[dict(draw=0,after=phase,x=255,y=255,width=2,height=2,source_width=512,source_height=512,
              format=26,size=16,data=bytes([phase+1])*16) for phase in (0,1)]
    parsed=candidate_reader(ui_candidate(),[ui_draw],roi,14)
    assert parsed['frames'][0]['issue']['viewports']==viewport and parsed['frames'][0]['issue']['scissors']==scissor
    assert parsed['frames'][0]['issue']['b0']==b0 and parsed['frames'][0]['issue']['b0_payload_status']=='available'
    assert parsed['roi_payload_missing']==0
    for bad in (ui_candidate(viewport_size=23),ui_candidate(scissor_size=15)):
        try:
            candidate_reader(bad,[ui_draw],roi,14)
        except ValueError:
            pass
        else:
            raise AssertionError('Mismatched issued-state blob length accepted')
    off_center=[dict(roi[0],x=254),roi[1]]
    try:
        candidate_reader(ui_candidate(),[ui_draw],off_center,14)
    except ValueError:
        pass
    else:
        raise AssertionError('Off-center B10 ROI accepted')
    def ui_candidate_v15():
        vsps=((ui_vs,ui_ps,0),(0x025B4B9FF54622ED,0x46F92DC71BF8DFA5,1))
        out=struct.pack('<9I',0,0,0,0,0,1179648,1,0,0xffffffff)
        out+=struct.pack('<48I',*([0]*48))+struct.pack('<48I',*([0]*48))
        out+=struct.pack('<16I',589824,589824,*([0]*14))+struct.pack('<I',2)
        frames=[];draws=[];inputs=[];samplers=[]
        for i,(vs,ps,camera_slot) in enumerate(vsps):
            frame_no=18192+i;target=0x101+i;camera_id=0x44+i
            draws.append(dict(frame=frame_no,vs=vs,ps=ps,kind=ord('I'),count=2,start=0,base=0,layout=[],
                              streams=[{}, {}, dict(offset=0,stride=2,capture_offset=0,data=struct.pack('<2H',0,1))]))
            frames.append(struct.pack('<3I',i,frame_no,501+i)+struct.pack('<i3I',i,1,0,0))
            cb=bytes(16 if camera_slot==0 else 276*16)
            frames.append(struct.pack('<I4Q6I',2,vs,ps,camera_id,target,512,512,26,0,276 if camera_slot else 1,1))
            frames.append(struct.pack('<I',camera_slot)+struct.pack('<I',20)+bytes(20)+struct.pack('<2I',1,1))
            frames.append(struct.pack('<I',24)+viewport+struct.pack('<I',16)+scissor)
            frames.append(struct.pack('<2I',len(cb),len(cb))+cb)
            slot=2 if camera_slot else 0
            source_format=29
            fields=(i,slot,1,4,28,384,384,1024,768,1,1,0,0,1,1,0,1,0,0,0,1536,384,589824)
            inputs.append(struct.pack('<23I',*fields)+struct.pack('<2Q2I',0x400+i,0x500+i,2,source_format))
            inputs.append(struct.pack('<I',589824)+bytes(589824))
            samplers.append(struct.pack('<2I',i,1)+struct.pack('<I',52)+bytes(52))
        out+=b''.join(frames)+struct.pack('<I',2)+b''.join(inputs)+struct.pack('<I',0)
        out+=struct.pack('<I',2)+b''.join(samplers)
        parsed=candidate_reader(out,draws,[],15)
        assert [f['issue']['camera_cb_slot'] for f in parsed['frames']]==[0,1]
        assert parsed['frames'][1]['issue']['b0_status']=='unused' and parsed['frames'][1]['issue']['camera_cb_status']=='available'
        assert [x['slot'] for x in parsed['inputs']]==[0,2]
        assert all(x['sample_mapping']=='full-image-center-point' for x in parsed['inputs'])
        assert all(x['sample_color_semantics']=='srgb-decoded-linear-unorm8' for x in parsed['inputs'])
        assert parsed['input_bytes']==1179648 and parsed['ui_image_bytes'][:2]==(589824,589824)
        return out,draws
    good_v15,v15_draws=ui_candidate_v15()
    for bad_v15 in (good_v15[:-1],good_v15.replace(struct.pack('<I',16)+scissor,struct.pack('<I',15)+scissor,1)):
        try:
            candidate_reader(bad_v15,v15_draws,[],15)
        except ValueError:
            pass
        else:
            raise AssertionError('Malformed v15 full-source UI trace accepted')
    def runtime_ui_snapshot(x=255,version=14):
        pair=b''.join(struct.pack('<10I',0,phase,x,255,2,2,512,512,26,16)+bytes([phase+1])*16
                      for phase in (0,1))
        draw=struct.pack('<3Q6I',ui_vs,ui_ps,0x101,18192,7,ord('I'),2,1,0)
        draw+=struct.pack('<3I',501,1,0)+struct.pack('<9I',*([0]*9))+struct.pack('<6H',*([0]*6))
        draw+=struct.pack('<4Q2I',0,0,0,0,0,0)+struct.pack('<3I',512,512,0xffffffff)
        draw+=struct.pack('<QII',0,0,0)*4
        draw+=struct.pack('<Ii',3,4)+struct.pack('<5I',0,0,0,0,0)*3
        draw+=struct.pack('<IQQIII',0xffffffff,0,0,0,0,0)*4+struct.pack('<I',0)
        body=draw+struct.pack('<2I',0,0)+struct.pack('<5I',0,0,0,0,0)
        body+=struct.pack('<2I',2,0)+pair+struct.pack('<I',0)+struct.pack('<2I',0,0)
        body+=struct.pack('<4I',0,0,0,0)+struct.pack('<I',0)+struct.pack('<I',0)
        return b'EDVRDRW1'+struct.pack('<4I',version,1,0,0)+body+ui_candidate()+struct.pack('<I',0)
    optic_fixture=dict(kind=ord('I'),count=2,start=3,base=4,layout=[
        dict(classification=0,slot=1,format=3,offset=0),dict(classification=0,slot=1,format=17,offset=16)],
        streams=[{},dict(offset=16,stride=48,capture_offset=208,data=bytes(144)),
                 dict(offset=8,stride=2,capture_offset=14,data=struct.pack('<2H',0,2))])
    assert _validate_candidate_indices(optic_fixture)=='validated', 'Packed optic formats and shifted capture windows'
    large_base=2_955_444
    optic_fixture['base']=large_base
    optic_fixture['streams'][1].update(offset=16,stride=40,capture_offset=16+large_base*40,data=bytes(256*1024))
    optic_fixture['streams'][2].update(offset=8,stride=2,capture_offset=14,data=struct.pack('<2H',0,2))
    assert _validate_candidate_indices(optic_fixture)=='validated', 'Large indexed base vertex is covered by the retained window'
    optic_fixture['base']=-1
    try:
        _validate_candidate_indices(optic_fixture)
    except ValueError:
        pass
    else:
        raise AssertionError('Negative base vertex escaping the binding origin accepted')
    optic_fixture['streams'][1]['capture_offset']=400
    try:
        _validate_candidate_indices(optic_fixture)
    except ValueError:
        pass
    else:
        raise AssertionError('Index outside retained vertex window accepted')
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
        p.write_bytes(runtime_ui_snapshot())
        runtime_capture=read(p)
        assert runtime_capture['version']==14 and runtime_capture['candidates']['frames'][0]['issue']['b0']==b0
        assert len(runtime_capture['effect_images'])==2 and runtime_capture['effect_images'][0]['x']==255
        for bad in (runtime_ui_snapshot(x=254),runtime_ui_snapshot(version=13)):
            p.write_bytes(bad)
            try:
                read(p)
            except ValueError:
                pass
            else:
                raise AssertionError('Invalid or version-incompatible B10 ROI accepted by top-level reader')
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
    c = read_menu(a.path) if Path(a.path).suffix.lower()=='.json' else read(a.path)
    if a.verify_fixture:
        verify_fixture(c)
        sprite_path = Path(str(a.path) + '.sprite')
        assert sprite_path.exists(), 'sprite diagnostic fixture is required'
        verify_sprite_fixture(read(sprite_path))
        candidate_path=Path(str(a.path)+'.candidate')
        assert candidate_path.exists(),'nonempty targeted-candidate fixture is required'
        verify_candidate_fixture(read(candidate_path))
        capacity_path=Path(str(a.path)+'.capacity')
        assert capacity_path.exists(),'full-window targeted-candidate fixture is required'
        verify_candidate_capacity_fixture(read(capacity_path))
        menu=read_menu(str(a.path)+'.menu.json')
        assert len(menu['draws'])==8 and menu['late_menu']['anchor_status']=='both_eyes'
        assert [r['outcome'] for r in menu['late_menu']['rows']]==['issued','skipped','issued','issued']*2
        assert all(e['scene_status']=='present_before_menu' for e in menu['late_menu']['eyes'][:2])
        assert menu['late_menu']['rows'][1]['constants'][1]['payload_origin_bytes']==256
        avatar=menu['late_menu']['rows'][1]['avatar']
        assert avatar['source_status']=='cached_not_exact' and not avatar['sources_exact'] and not avatar['pin_targets_available']
        assert avatar['qpc_age_ms']==100 and avatar['root_qpc_age_ms']==200
        assert menu['late_menu']['input_summary']['branches']=={'2d_window':1}
        assert menu['late_menu']['input_summary']['in_flight_at_close']==1
        assert len(menu['late_menu']['input_observer']['meshes'][0]['triangles'])==120
        assert menu['draws'][1]['buffers'][1]['data']==bytes([0x35])*4608
        assert menu['draws'][1]['streams'][1]['data']==bytes([0xA7])*160
        t33=[menu['mesh_buffers'][d['mesh'][0]]['data'] for d in menu['draws'] if d['mesh'][0]!=0xffffffff]
        assert [z[0] for z in t33]==[0x47,0x48,0x49,0x4a,0x4b,0x4c], 'Menu structured copies were deduplicated across rewritten inputs'
        t2=read(str(a.path)+'.t2')['candidates']['inputs']
        assert len(t2)==2 and all((i['slot'],i['mip'],i['requested_mip'],i['mip_levels'],
                                   i['requested_mip_levels'],i['source_array_size'],i['slices'])==(2,1,0,10,11,8,8) for i in t2)
        assert t2[0]['bytes']==t2[1]['bytes'] and sum(i['bytes'] for i in t2)<=24*1024*1024
        effects = read(str(a.path)+'.effects')
        assert effects['version'] == 15 and len(effects['draws']) == 5 and effects['failures'] == 0
        for i,d in enumerate(effects['draws']):
            assert d['ordinal'] == 0xfffffffd and d['mesh'] == [0xffffffff]*4
            assert d['layout'][1] == dict(semantic='TEXCOORD',index=0,format=2,slot=1,offset=16,classification=1,step=1)
            assert bool(d['streams'][0]['data']) == (i < 4) and bool(d['streams'][1]['data']) == (i < 4)
        mesh = read(str(a.path)+'.mesh')
        assert mesh['failures'] == mesh['mesh_declined'] == 0 and len(mesh['draws']) == 4 and len(mesh['mesh_buffers']) == 6
        for i,d in enumerate(mesh['draws']):
            assert d['ordinal'] == 0xfffffffe and d['mesh'] == [i//2*3+j for j in range(3)]+[0xffffffff]
            for role,ref in enumerate(d['mesh'][:3]):
                b = mesh['mesh_buffers'][ref]
                assert b['first_draw'] == i//2*2 and b['frame'] == 300+i//2
                assert b['data'] == bytes([21+role*10+i//2])*b['whole'], 'Source buffer copied after reuse or truncated'
        assert mesh['mesh_buffers'][1]['whole'] > 1024*1024, 'High bone indices lost'
        eye_mesh = read(str(a.path)+'.eyemesh')
        assert eye_mesh['failures'] == eye_mesh['mesh_declined'] == eye_mesh['dropped'] == 0
        assert len(eye_mesh['draws']) == 64 and len(eye_mesh['mesh_buffers']) == 128
        assert eye_mesh['mesh_capture']['first_frame'] == 700 and eye_mesh['mesh_capture']['frames'] == 16
        assert eye_mesh['mesh_capture']['declined'] == 2 and eye_mesh['mesh_capture']['snapshots'] == 32
        assert eye_mesh['mesh_capture']['rejected_source_draws'] == 1
        assert eye_mesh['mesh_capture']['missing_body_ledger_frames'] == []
        for i, d in enumerate(eye_mesh['draws']):
            group, draw = divmod(i, 2)
            frame, eye = divmod(group, 2)
            assert d['frame'] == 700+frame and d['render_frame'] == 900+frame and d['eye'] == eye and d['ordinal'] == eye*100+draw
            assert d['mesh_selected'] == 1
            j=d['join']
            assert (j['scene_frame'],j['epoch'],j['scheduled_index'],j['scheduled_render_frame'])==(1200+frame,44,draw,900+frame)
            assert (j['fp_frame'],j['fp_sequence'],j['g_game_frame'],j['item_serial'],j['rig_generation'])==(d['frame'],7000+d['frame'],8000+d['frame'],eye+1,9)
            assert j['arm_joints'] == (24,34,35,36,37,38)
            assert (j['rig'],j['pose_qpc'],j['draw_qpc'],j['capture_sequence'],j['valid_flags'])==(0x12340000+eye,100000+d['frame'],200000+d['frame'],0,0xBF)
            assert j['source_job_lookup_status'] == 1 and j['source_job_lookup'] == 'offline_t33_palette_base_join'
            if draw==0:
                assert d['mesh_status'] == [1,1,1,1] and d['mesh'] == [i//2*4+j for j in range(4)]
            else:
                assert d['mesh_status'] == [7,7,7,7] and d['mesh'] == [0xffffffff]*4
            assert d['mesh_range'] == [(0,2),(0,24000),(0,32),(0,24000)]
            assert d['mesh_role_names'][3] == 'other_palette_candidate' and d['other_palette_relation'] == 'unknown'
            assert d['mesh_views'][0] and d['mesh_views'][1] and not d['mesh_views'][2] and not d['mesh_views'][3]
            assert len(d['layout']) == 1 and d['layout'][0]['semantic'] == 'POSITION'
            assert d['start'] == 150000 and d['base'] == 8000 and d['start_instance'] == 2+draw
            constants = struct.unpack('<48f', d['buffers'][0]['data'])
            assert all(v == (700+frame)*10+eye*2+draw for v in constants), 'Eye draw camera copied after reuse'
            if draw==0:
                for role, ref in enumerate(d['mesh']):
                    b = eye_mesh['mesh_buffers'][ref]
                    assert b['first_draw'] == i and b['frame'] == d['frame'] and b['identity'] == d['mesh_sources'][role] and b['readback_status'] == 1
                    value=51+frame*2+eye+(30 if role==3 else role*10)
                    assert b['data'] == bytes([value])*b['whole'], 'Selected body draw snapshot was not exact'
        assert len({d['target'] for d in eye_mesh['draws']}) == 2
        body_candidates=read(str(a.path)+'.bodycandidates')
        assert body_candidates['failures']==body_candidates['mesh_declined']==0
        assert len(body_candidates['draws'])==5
        assert len(body_candidates['mesh_capture']['missing_body_ledger_frames'])==16
        assert all(c['status']=='draw_time_palettes_copied' and c['draw_time_palette_frames']==[951] and c['clip_constant_rows']==c['geometry_rows']==1 for c in body_candidates['mesh_capture']['candidate_coverage'])
        assert body_candidates['draws'][0]['eye']==-1 and body_candidates['draws'][0]['mesh_status'][0]==10
        assert {(d['count'],d['eye']) for d in body_candidates['draws'][1:]}=={(n,e) for n in (10050,3216) for e in (0,1)}
        for d in body_candidates['draws'][1:]:
            assert d['mesh_selection']=='observed_body_family_candidate_not_proven_part' and d['mesh_selected']==2
            assert d['frame']==951 and d['mesh_status']==[1]*4 and all(x!=0xffffffff for x in d['mesh'])
            assert len(d['buffers'][3]['data'])==192, 'Candidate clip PSb2 missing'
            assert d['mesh'][2]!=0xffffffff and all(s['data'] for s in d['streams'][1:]), 'Candidate VB/IB geometry missing'
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
        assert crops['version'] == 15 and crops['failures'] == 0 and crops['effect_image_declined'] == 1
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
        assert night['version'] == 15 and night['failures'] == 0 and len(night['draws']) == 3
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
                          body_evidence=c['mesh_capture'],
                          source_buffers=len(c['mesh_buffers']), source_bytes=sum(b['size'] for b in c['mesh_buffers']), source_declined=c['mesh_declined'],
                          vertex_draws=sum(any(s['data'] for s in d['streams']) for d in c['draws']),
                          vertex_bytes=sum(len(s['data']) for d in c['draws'] for s in d['streams']),
                          effect_images=sum(e['after'] <= 1 for e in c['effect_images']), effect_image_declined=c['effect_image_declined'],
                          night_input_images=sum(e['after'] >= 2 for e in c['effect_images']), night_sampling=len(c['night_sampling']),
                          candidate_evidence=dict(optic_draws=c['candidates']['optic_draws'],
                              frames=[dict(draw=f['draw'],ledger_frame=f['ledger_frame'],render_frame=f['render_frame'],eye=f['eye'],
                                           issued=f['issued'],roi_captured=f['roi_captured'],roi_complete=f['roi_complete'],index_status=f.get('index_status','not_requested'))
                                      for f in c['candidates']['frames']],
                              inputs=[{k:i[k] for k in ('draw','slot','kind','view_dimension','format','width','height','source_width','source_height',
                                  'source_mip_levels','source_array_size','mip','requested_mip','mip_levels','requested_mip_levels',
                                  'first_slice','slices','bytes')} for i in c['candidates']['inputs']],
                              refusals=c['candidates']['refusals'],input_declined=c['candidates']['input_declined'],
                              mip_declined=c['candidates']['mip_declined']),
                          late_menu=({k:c['late_menu'][k] for k in ('selected_frame','anchor_status','eyes','camera_context','outcomes','eye_mappings','menu_geometry','avatar_sources','input_summary')} if 'late_menu' in c else None),
                          failures=c['failures'], shaders=Counter(f"{d['vs']:016X}" for d in c['draws'])), indent=2))
    if a.surfaces:
        for path in export_surfaces(c, a.surfaces, a.dry_run):
            print(('Would write ' if a.dry_run else 'Wrote ') + str(path))
    if a.effects:
        for path in export_effect_images(c, a.effects, a.dry_run):
            print(('Would write ' if a.dry_run else 'Wrote ') + str(path))


if __name__ == '__main__':
    main()
