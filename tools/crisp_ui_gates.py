#!/usr/bin/env python3
"""Read the crisp-UI gates off a draw census (docs/crisp-ui-handoff.md).

    python tools/crisp_ui_gates.py <edvr_gfx_*.log> [more logs...]

For every census in the log(s), with advanced.census_offscreen = 1 so the
offscreen builders were recorded, this answers what the gates ask:

  * which render targets the GUI renderer's three shader families drew into
    (the UI SURFACES), their sizes, formats and depth partners;
  * which EYE draws sample those surfaces (the UI COMPOSITES), by vertex and
    pixel shader hash, with their render target's format and VIEW format
    (G1), their depth view and depth state (G2), their blend equation
    decoded (G3), and which sampler slot carries the surface;
  * whether a GUI family ever draws straight into an eye target (G6);
  * the direct-list families -- the flight HUD and the target indicator --
    with the same columns, plus what the flight HUD samples at slot 0 (G7's
    input);
  * the distinct eye targets the UI lands in and any copy whose source is
    one of them (G5);
  * per frame, WHERE the UI draws sit and everything after them that reads
    a UI target -- draws, dispatches, copies (G8: the post-UI chain, which
    is where a tonemap or an SMAA pass shows up);
  * per frame, everything after the UI draws that touches the UI draws'
    DEPTH view: a later draw testing against it, a sample of it, a copy of
    it (G10: whether the UI may write depth into the scene's own buffer).

The census line grammar is the emitter's in src/d3d11/draw_census.cpp. Every
tail is optional here, the way tools/diff_draw_census.py reads them: a log
captured before a field existed must still parse, and a field that is merely
new must never cost a session again.

This reader is NOT a model of src/d3d11/ui_depth.cpp (the review of
2026-09-06): its DIRECT list carries the target indicator quad, which the
DLL's built-in direct list does not, so the G10 section can report a depth
toucher the classifier never binds; it sees only the three frames of one
census where the DLL remembers surfaces for the session; and it parses no
DCL depth-clear lines, so "nothing clears the depth mid-frame" is not a
thing it can say. Read it as the gates' evidence, not as the fix's log.

Exit 0 when at least one census was read, 1 otherwise.
"""

import re
import sys
from collections import defaultdict

# The GUI renderer's three families (game build 332753). Vector = the
# textureless widget shader, text = the glyph-atlas shader, icons = the BC7
# icon pages. Anything they draw into is a UI surface.
GUI_FAMILIES = {
    '666EF0C4C616F67E': 'gui-vector',
    '1012E00B3CB44469': 'gui-text',
    'A3E5D3FCBC1165F8': 'gui-icons',
}
# Families the handoff names, for labelling only: the surface rule finds the
# composites without them, and a label that disagrees with the rule is news.
KNOWN = {
    '81216C77F90DEDD6': 'holo-panel composite (24/frame)',
    'E508648660A352B2': 'interface-surface composite',
    'B7790CBFC6554097': 'flight HUD vectors (direct)',
    '5DA53D8B0133341E': 'target indicator quad (direct)',
    '2D78DC3FD2C0C543': 'tonemap (build-332753.md)',
    'E861F611375E7ECC': 'tonemap/accum (build-332753.md)',
    'F1FB2EEFB662F3AA': 'exposure CS (exposure_fix)',
    '68842760565CC3BA': 'SMAA edges? (R8G8 out)',
    '03D186CE0EC031E3': 'SMAA weights? (160x560 area + 64x16 search)',
    '98E6F9986FDC9A53': 'SMAA blend? (two colour reads)',
    'A888D51024D9798E': 'FSS colour/prepass family (build-332753.md)',
}
DIRECT = ('B7790CBFC6554097', '5DA53D8B0133341E')

DXGI = {
    0: 'UNKNOWN', 2: 'RGBA32F', 10: 'RGBA16F', 11: 'RGBA16_UNORM',
    16: 'RG32F', 19: 'R32G8X24_TYPELESS', 20: 'D32F_S8X24',
    21: 'R32F_X8X24_TYPELESS', 23: 'RGB10A2_TYPELESS', 24: 'RGB10A2_UNORM',
    26: 'R11G11B10F', 27: 'RGBA8_TYPELESS', 28: 'RGBA8_UNORM',
    29: 'RGBA8_UNORM_SRGB', 34: 'RG16F', 39: 'R32_TYPELESS', 40: 'D32F',
    41: 'R32F', 44: 'R24G8_TYPELESS', 45: 'D24S8', 46: 'R24_X8_TYPELESS',
    48: 'RG8_TYPELESS', 49: 'RG8_UNORM', 53: 'R16_TYPELESS', 54: 'R16F',
    55: 'D16', 56: 'R16_UNORM', 60: 'R8_TYPELESS', 61: 'R8_UNORM',
    65: 'A8_UNORM', 70: 'BC1_TYPELESS', 71: 'BC1', 72: 'BC1_SRGB', 77: 'BC3',
    78: 'BC3_SRGB', 80: 'BC4', 83: 'BC5', 87: 'BGRA8_UNORM',
    90: 'BGRA8_TYPELESS', 91: 'BGRA8_UNORM_SRGB', 95: 'BC6H_UF16',
    97: 'BC7_TYPELESS', 98: 'BC7', 99: 'BC7_SRGB',
}
BLEND = {
    1: 'ZERO', 2: 'ONE', 3: 'SRC_COLOR', 4: 'INV_SRC_COLOR', 5: 'SRC_ALPHA',
    6: 'INV_SRC_ALPHA', 7: 'DEST_ALPHA', 8: 'INV_DEST_ALPHA', 9: 'DEST_COLOR',
    10: 'INV_DEST_COLOR', 11: 'SRC_ALPHA_SAT', 14: 'BLEND_FACTOR',
    15: 'INV_BLEND_FACTOR', 16: 'SRC1_COLOR', 17: 'INV_SRC1_COLOR',
    18: 'SRC1_ALPHA', 19: 'INV_SRC1_ALPHA',
}
BLEND_OP = {1: 'ADD', 2: 'SUB', 3: 'REV_SUB', 4: 'MIN', 5: 'MAX'}
CMP = {1: 'NEVER', 2: 'LESS', 3: 'EQUAL', 4: 'LEQUAL', 5: 'GREATER',
       6: 'NOTEQUAL', 7: 'GEQUAL', 8: 'ALWAYS'}
# The layer's premultiplied table (handoff, A3): the RGB shapes it can carry.
ACCEPTED = {(2, 1), (5, 6), (2, 6), (2, 2), (5, 2)}

LINE_RE = re.compile(r'^\[\d{2}:\d{2}:\d{2}\.\d{3}\] (DC\S* .*)$')
HEAD_RE = re.compile(r'^(DC|DCO) (\d+) #(\d+) ([A-Z]) n=(\d+) i=(\d+) '
                     r'r=(\S+) d=(\S+) c=(\S+) s=(\S+),(\S+),(\S+),(\S+)(.*)$')
DISP_RE = re.compile(r'^DCX (\d+) #(\d+) n=(\d+),(\d+),(\d+) ch=([0-9A-Fa-f]+) '
                     r'u=(\S+)(.*)$')
ID_TEX_RE = re.compile(r'^DC id @(\d+) tex (\d+)x(\d+) fmt=(\d+)'
                       r'(?: res=(\S+))?(?: vf=(\d+))?$')
ID_BUF_RE = re.compile(r'^DC id @(\d+) buf (\d+)(?: res=(\S+))?'
                       r'(?: stride=(\d+))?$')
COPY_RE = re.compile(r'^DCC (\d+) #(\d+) ([A-Z]) dst=(\S+) sub=(\d+) '
                     r'at=(\d+),(\d+) src=(\S+) sub=(\d+)(.*)$')
BEGIN_RE = re.compile(r'^DC begin census=(\d+) frames=(\d+) frame=(\d+)')
END_RE = re.compile(r'^DC end census=(\d+)(.*)$')
KV_RE = re.compile(r'(\w+)=(\S+)')


class Ev(object):
    """One recorded event: a draw (DC eye / DCO offscreen), a dispatch (DCX)
    or a copy (DCC). Fields absent for a kind stay at their defaults."""
    __slots__ = ('tag', 'frame', 'idx', 'kind', 'n', 'i', 'r', 'd', 'c',
                 's', 'x', 'vh', 'ph', 'ch', 'u', 'ds', 'st', 'bm', 'bl',
                 'so', 'vt', 'q', 'src', 'dst', 'at')

    def __init__(self):
        self.s = []
        self.x = []
        self.u = []
        self.vt = []
        self.r = self.d = self.c = '-'
        self.vh = self.ph = self.ch = None
        self.ds = self.st = self.bm = self.bl = self.so = None
        self.q = -1
        self.src = self.dst = None
        self.at = (0, 0)
        self.n = self.i = 0
        self.kind = '?'


class Census(object):
    def __init__(self, no, frames, first_frame):
        self.no = no
        self.frames = frames
        self.first_frame = first_frame
        self.events = []
        self.ids = {}       # '@N' -> dict(kind, w, h, fmt, res, vf)
        self.end = ''


def parse(path):
    censuses = []
    cur = None
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for raw in f:
            m = LINE_RE.match(raw.rstrip('\r\n'))
            if not m:
                continue
            line = m.group(1)
            b = BEGIN_RE.match(line)
            if b:
                cur = Census(int(b.group(1)), int(b.group(2)), int(b.group(3)))
                censuses.append(cur)
                continue
            if cur is None:
                continue
            e = END_RE.match(line)
            if e:
                cur.end = e.group(2).strip()
                cur = None
                continue
            h = HEAD_RE.match(line)
            if h:
                ev = Ev()
                ev.tag = h.group(1)
                ev.frame = int(h.group(2))
                ev.idx = int(h.group(3))
                ev.kind = h.group(4)
                ev.n = int(h.group(5))
                ev.i = int(h.group(6))
                ev.r, ev.d, ev.c = h.group(7), h.group(8), h.group(9)
                ev.s = [h.group(10), h.group(11), h.group(12), h.group(13)]
                for k, v in KV_RE.findall(h.group(14)):
                    if k == 'x':
                        ev.x = v.split(',')[:4]
                    elif k == 'vh':
                        ev.vh = v.upper()
                    elif k == 'ph':
                        ev.ph = v.upper()
                    elif k == 'ds':
                        ev.ds = v
                    elif k == 'st':
                        ev.st = v
                    elif k == 'bm':
                        ev.bm = v
                    elif k == 'bl':
                        ev.bl = v
                    elif k == 'so':
                        # The stencil's masks and ops (2026-09-07). Kept
                        # rather than used: the gates ask nothing of it, and
                        # tools/stencil_census.py is what decodes it. It is
                        # stored so this reader's picture of a draw is the
                        # whole line, not the part it happens to want.
                        ev.so = v
                    elif k == 'vt':
                        # VS resource slots 32-39 (2026-09-07), where the
                        # instanced-mesh pool lives. Same bargain as so=:
                        # stored so the picture is whole.
                        ev.vt = v.split(',')
                    elif k == 'q':
                        ev.q = int(v)
                cur.events.append(ev)
                continue
            dsp = DISP_RE.match(line)
            if dsp:
                ev = Ev()
                ev.tag = 'DCX'
                ev.frame = int(dsp.group(1))
                ev.idx = int(dsp.group(2))
                ev.ch = dsp.group(6).upper()
                ev.u = dsp.group(7).split(',')
                for k, v in KV_RE.findall(dsp.group(8)):
                    if k == 's':
                        ev.s = v.split(',')
                    elif k == 'q':
                        ev.q = int(v)
                cur.events.append(ev)
                continue
            t = ID_TEX_RE.match(line)
            if t:
                cur.ids['@' + t.group(1)] = dict(
                    kind='tex', w=int(t.group(2)), h=int(t.group(3)),
                    fmt=int(t.group(4)), res=t.group(5),
                    vf=int(t.group(6)) if t.group(6) else None)
                continue
            u = ID_BUF_RE.match(line)
            if u:
                # h carries the STRUCTURE stride for a buffer, which is the
                # same slot binding_shadow's ResourceInfo puts it in. It is
                # what tells a 336-byte instanced-mesh pool from any other
                # buffer of the same size.
                cur.ids['@' + u.group(1)] = dict(
                    kind='buf', w=int(u.group(2)), h=int(u.group(4) or 0),
                    fmt=0, res=u.group(3), vf=None)
                continue
            c = COPY_RE.match(line)
            if c:
                ev = Ev()
                ev.tag = 'DCC'
                ev.frame = int(c.group(1))
                ev.idx = int(c.group(2))
                ev.kind = c.group(3)
                ev.dst, ev.src = c.group(4), c.group(8)
                ev.at = (int(c.group(6)), int(c.group(7)))
                for k, v in KV_RE.findall(c.group(10)):
                    if k == 'q':
                        ev.q = int(v)
                cur.events.append(ev)
    return censuses


def fmt_name(n):
    if n is None:
        return '?'
    return '%s(%d)' % (DXGI.get(n, 'fmt'), n)


def tok_desc(census, tok):
    """'@3' -> '1952x1597 RGBA8_TYPELESS(27) view RGBA8_UNORM_SRGB(29)'."""
    if tok in ('-', '?'):
        return tok
    e = census.ids.get(tok)
    if e is None:
        m = re.match(r'^tex(\d+)x(\d+)f(\d+)$', tok)
        if m:
            return '%sx%s %s' % (m.group(1), m.group(2),
                                 fmt_name(int(m.group(3))))
        m = re.match(r'^buf(\d+)$', tok)
        if m:
            return 'buf %s' % m.group(1)
        return tok
    if e['kind'] == 'buf':
        if e['h']:
            return 'buf %d stride %d' % (e['w'], e['h'])
        return 'buf %d' % e['w']
    s = '%dx%d %s' % (e['w'], e['h'], fmt_name(e['fmt']))
    if e['vf'] is not None and e['vf'] != e['fmt']:
        s += ' view %s' % fmt_name(e['vf'])
    elif e['vf'] is not None:
        s += ' view=same'
    return s


def res_of(census, tok):
    """The resource identity behind a token. An inline token (the table was
    full) has none, so its own text stands in: two inline tokens of the same
    size and format then compare equal, which over-connects rather than
    under-connects, and the report says 'inline' where that matters."""
    e = census.ids.get(tok)
    if e and e['res']:
        return e['res']
    return tok


def shape_of(census, tok):
    """(w, h, fmt) for a texture token, interned or inline; None otherwise."""
    e = census.ids.get(tok)
    if e:
        return (e['w'], e['h'], e['fmt']) if e['kind'] == 'tex' else None
    m = re.match(r'^tex(\d+)x(\d+)f(\d+)$', tok)
    if m:
        return (int(m.group(1)), int(m.group(2)), int(m.group(3)))
    return None


class TargetSet(object):
    """A set of textures known by identity where the table had room and by
    shape where it did not. A 30-frame census overflows the 512-entry table
    by thousands of tokens (2026-09-03), and the tonemap's read of the HDR
    target then appears as an inline 'tex2064x2208f26' that no identity can
    match. Matching such a token by shape over-connects (two same-shaped
    textures read as one) rather than under-connects, and the report marks
    every shape match so the reader knows which kind it is looking at."""

    def __init__(self, census, toks):
        self.census = census
        self.res = set()
        self.shapes = set()
        for t in toks:
            if t in ('-', '?'):
                continue
            e = census.ids.get(t)
            if e and e['res']:
                self.res.add(e['res'])
            s = shape_of(census, t)
            if s:
                self.shapes.add(s)

    def match(self, tok):
        """'id' for an identity match, 'shape' for a shape-only one, None."""
        if tok in ('-', '?'):
            return None
        e = self.census.ids.get(tok)
        if e and e['res'] and e['res'] in self.res:
            return 'id'
        if not e:
            s = shape_of(self.census, tok)
            if s and s in self.shapes:
                return 'shape'
        return None


def decode_bl(bl):
    if not bl:
        return '(no blend tail)'
    m = re.match(r'^([01])(\d+),(\d+),(\d+)/(\d+),(\d+),(\d+)(,a2c)?$', bl)
    if not m:
        return bl
    if m.group(1) == '0':
        return 'off'
    src, dst, op = int(m.group(2)), int(m.group(3)), int(m.group(4))
    sa, da, oa = int(m.group(5)), int(m.group(6)), int(m.group(7))
    txt = '%s,%s %s / alpha %s,%s %s' % (
        BLEND.get(src, src), BLEND.get(dst, dst), BLEND_OP.get(op, op),
        BLEND.get(sa, sa), BLEND.get(da, da), BLEND_OP.get(oa, oa))
    ok = op == 1 and (src, dst) in ACCEPTED
    return txt + (' [layer: ok]' if ok else ' [layer: REFUSE]') + \
        (' a2c' if m.group(8) else '')


def decode_ds(ds):
    if not ds:
        return '(no depth tail)'
    m = re.match(r'^([01?])(\d+)w([AZ?])$', ds)
    if not m:
        return ds
    if m.group(1) == '0':
        return 'test off'
    return 'test %s, write %s' % (CMP.get(int(m.group(2)), m.group(2)),
                                  'on' if m.group(3) == 'A' else 'off')


def depth_tests(ds):
    return bool(ds) and ds[0] == '1'


def counts_line(census, evs, attr, fn):
    vals = defaultdict(int)
    for d in evs:
        vals[getattr(d, attr)] += 1
    return '; '.join('%s x%d' % (fn(v), n)
                     for v, n in sorted(vals.items(), key=lambda kv: -kv[1]))


def label(h):
    return KNOWN.get(h, '')


def report(census, out):
    w = out.write
    draws = [e for e in census.events if e.tag in ('DC', 'DCO')]
    eye = [e for e in draws if e.tag == 'DC']
    off = [e for e in draws if e.tag == 'DCO']
    w('=== census %d: %d frames from frame %d, %d draws (%d eye, %d offscreen), '
      '%d dispatches, %d copies; end: %s ===\n' % (
          census.no, census.frames, census.first_frame, len(draws), len(eye),
          len(off), sum(1 for e in census.events if e.tag == 'DCX'),
          sum(1 for e in census.events if e.tag == 'DCC'), census.end))
    if not off:
        w('  no offscreen draws recorded: set advanced.census_offscreen = 1 '
          '(the surface rule needs the builders)\n')
    if not any(e['vf'] is not None for e in census.ids.values()):
        w('  no vf= on the id lines: a DLL before 2026-09-06 -- G1\'s view '
          'format is not in this log\n')
    if 'overflow=' in census.end and not census.end.endswith('overflow=0 truncated=0'):
        w('  NOTE: %s -- inline tokens past the table are matched by size and '
          'format only\n' % census.end)

    # 1. UI surfaces: what the GUI families drew into (offscreen).
    surfaces = {}
    gui_into_eye = []
    for d in draws:
        if d.vh not in GUI_FAMILIES:
            continue
        if d.tag == 'DC':
            gui_into_eye.append(d)
            continue
        if d.r in ('-', '?'):
            continue
        key = res_of(census, d.r)
        s = surfaces.setdefault(key, dict(toks=set(), draws=0, families=set(),
                                          depth=set()))
        s['toks'].add(d.r)
        s['draws'] += 1
        s['families'].add(GUI_FAMILIES[d.vh])
        if d.d not in ('-', '?'):
            s['depth'].add(d.d)
    w('\n-- UI surfaces (targets the GUI families drew into, offscreen) --\n')
    if not surfaces:
        w('  none\n')
    for key, s in sorted(surfaces.items(), key=lambda kv: -kv[1]['draws']):
        tok = sorted(s['toks'])[0]
        w('  %-16s %-36s %4d draws  %-32s depth: %s\n' % (
            tok, tok_desc(census, tok), s['draws'],
            ','.join(sorted(s['families'])),
            ', '.join(tok_desc(census, t) for t in sorted(s['depth'])) or 'none'))
    surf_set = TargetSet(census, [t for s in surfaces.values() for t in s['toks']])

    # 2. UI composites: eye draws sampling a UI surface.
    comps = defaultdict(list)
    slot_of = {}
    ui_draws = []
    for d in eye:
        hit = None
        for i, tok in enumerate(d.s + d.x):
            if surf_set.match(tok):
                hit = i
                break
        if hit is None:
            continue
        comps[(d.vh, d.ph)].append(d)
        slot_of[(d.vh, d.ph)] = hit
        ui_draws.append(d)
    w('\n-- UI composites (eye draws sampling a UI surface) --\n')
    if not comps:
        w('  none\n')
    for (vh, ph), ds in sorted(comps.items(), key=lambda kv: -len(kv[1])):
        per_frame = defaultdict(int)
        for d in ds:
            per_frame[d.frame] += 1
        w('  vs %s ps %s  %s\n' % (vh, ph, label(vh)))
        w('     draws/frame %s; samples the surface at slot %d\n' % (
            ','.join(str(per_frame[f]) for f in sorted(per_frame)),
            slot_of[(vh, ph)]))
        w('     target:     %s\n' % counts_line(
            census, ds, 'r', lambda v: '%s %s' % (v, tok_desc(census, v))))
        w('     depth view: %s\n' % counts_line(
            census, ds, 'd', lambda v: '%s %s' % (v, tok_desc(census, v))))
        w('     depth state: %s\n' % counts_line(census, ds, 'ds', decode_ds))
        w('     blend:      %s\n' % counts_line(census, ds, 'bl', decode_bl))
        w('     write mask: %s\n' % counts_line(census, ds, 'bm',
                                                lambda v: v or '?'))
        kinds = defaultdict(int)
        for d in ds:
            kinds['%s n=%d i=%d' % (d.kind, d.n, d.i)] += 1
        top = sorted(kinds.items(), key=lambda kv: -kv[1])[:4]
        w('     shapes:     %s\n' % '; '.join('%s x%d' % kv for kv in top))

    # 3. G6: GUI families drawing straight into an eye target.
    w('\n-- G6: GUI-family draws recorded as EYE draws --\n')
    if not gui_into_eye:
        w('  none (every GUI draw went through a surface)\n')
    else:
        vals = defaultdict(int)
        for d in gui_into_eye:
            vals[(d.vh, d.r)] += 1
        for (vh, r), n in sorted(vals.items(), key=lambda kv: -kv[1]):
            w('  %s into %s %s x%d\n' % (GUI_FAMILIES[vh], r,
                                          tok_desc(census, r), n))
        ui_draws.extend(gui_into_eye)

    # 4. The direct list.
    w('\n-- direct-list families --\n')
    for vh in DIRECT:
        ds = [d for d in draws if d.vh == vh]
        if not ds:
            w('  vs %s %s: not in this census\n' % (vh, KNOWN[vh]))
            continue
        eyed = [d for d in ds if d.tag == 'DC']
        w('  vs %s %s: %d draws (%d eye)\n' % (vh, KNOWN[vh], len(ds), len(eyed)))
        w('     target:     %s\n' % counts_line(
            census, eyed, 'r', lambda v: tok_desc(census, v)))
        w('     depth view: %s\n' % counts_line(
            census, eyed, 'd', lambda v: tok_desc(census, v)))
        w('     depth state: %s\n' % counts_line(census, eyed, 'ds', decode_ds))
        w('     blend:      %s\n' % counts_line(census, eyed, 'bl', decode_bl))
        slots = defaultdict(int)
        for d in eyed:
            slots[','.join(tok_desc(census, t) for t in d.s)] += 1
        for sig, n in sorted(slots.items(), key=lambda kv: -kv[1])[:3]:
            w('     samples:    %s x%d\n' % (sig, n))
        w('     ps:         %s\n' % counts_line(census, eyed, 'ph',
                                                lambda v: v or '?'))
        ui_draws.extend(eyed)

    # 5. G5: the eye targets the UI lands in, and copies out of them.
    tgt_set = TargetSet(census, [d.r for d in ui_draws])
    dep_set = TargetSet(census, [d.d for d in ui_draws])
    tok_by_res = defaultdict(set)
    for tok, e in census.ids.items():
        if e['res']:
            tok_by_res[e['res']].add(tok)
    w('\n-- G5: eye targets carrying UI, and copies whose source is one --\n')
    for res in sorted(tgt_set.res):
        toks = sorted(tok_by_res.get(res, {res}))
        w('  %s: tokens %s\n' % (tok_desc(census, toks[0]), ' '.join(toks)))
    inline_only = [s for s in tgt_set.shapes
                   if not any(shape_of(census, t) == s
                              for r in tgt_set.res for t in tok_by_res[r])]
    for s in sorted(inline_only):
        w('  %dx%d %s: inline tokens only (table full), matched by shape\n' % (
            s[0], s[1], fmt_name(s[2])))
    seen = defaultdict(int)
    for c in census.events:
        if c.tag == 'DCC' and tgt_set.match(c.src):
            seen[(c.kind, c.src, c.dst, c.at, tgt_set.match(c.src))] += 1
    if not seen:
        w('  no copies out of a UI target recorded\n')
    for (kind, src, dst, at, how), n in sorted(seen.items(), key=lambda kv: -kv[1]):
        w('  copy %s: %s %s -> %s %s at %d,%d x%d%s\n' % (
            kind, src, tok_desc(census, src), dst, tok_desc(census, dst),
            at[0], at[1], n, ' (by shape)' if how == 'shape' else ''))

    # 6. Per frame: where the UI sits, and the chain after it (G8, G10).
    #
    # Frames the line cap cut short are skipped: their "last UI draw" is
    # wherever the recording stopped, and everything before the real end of
    # the UI then reads as "after" it. A frame is partial when it holds fewer
    # UI draws than the census's modal count.
    w('\n-- G8/G10: per frame, the UI draws\' place and what follows them --\n')
    by_frame = defaultdict(list)
    for e in census.events:
        by_frame[e.frame].append(e)
    ui_q = defaultdict(list)
    for d in ui_draws:
        ui_q[d.frame].append(d.q)
    counts = defaultdict(int)
    for f, qs in ui_q.items():
        counts[len(qs)] += 1
    mode = max(counts.items(), key=lambda kv: (kv[1], kv[0]))[0] if counts else 0
    after_readers = defaultdict(int)     # who reads a UI colour target later
    depth_touch = defaultdict(int)       # who touches the UI's depth later
    judged = 0
    for f in sorted(by_frame):
        qs = ui_q.get(f)
        evs = by_frame[f]
        qmax = max(e.q for e in evs)
        if not qs:
            w('  frame %d: no UI draws (q up to %d)\n' % (f, qmax))
            continue
        lo, hi = min(qs), max(qs)
        partial = len(qs) < mode
        w('  frame %d: %d UI draws at q %d..%d of %d%s\n' % (
            f, len(qs), lo, hi, qmax, '  (partial: skipped)' if partial else ''))
        if partial:
            continue
        judged += 1
        for e in evs:
            if e.q <= hi:
                continue
            if e.tag == 'DC':
                for tok in e.s + e.x:
                    how = tgt_set.match(tok)
                    if how:
                        after_readers[('draw', e.vh, e.ph, tok, e.r, how)] += 1
                        break
                how = dep_set.match(e.d)
                if how:
                    depth_touch[('draw binds depth, test %s' % (
                        'ON' if depth_tests(e.ds) else 'off'), e.vh, e.d, how)] += 1
                for tok in e.s + e.x:
                    how = dep_set.match(tok)
                    if how:
                        depth_touch[('draw samples depth', e.vh, tok, how)] += 1
                        break
            elif e.tag == 'DCX':
                for tok in e.s:
                    how = tgt_set.match(tok)
                    if how:
                        after_readers[('dispatch', e.ch, '', tok,
                                       ','.join(t for t in e.u if t != '-'),
                                       how)] += 1
                        break
                for tok in e.s + e.u:
                    how = dep_set.match(tok)
                    if how:
                        depth_touch[('dispatch touches depth', e.ch, tok, how)] += 1
                        break
            elif e.tag == 'DCC':
                how = tgt_set.match(e.src)
                if how:
                    after_readers[('copy', e.kind, '', e.src, e.dst, how)] += 1
                how = dep_set.match(e.src)
                if how:
                    depth_touch[('copy from depth', e.kind, e.src, how)] += 1
                how = dep_set.match(e.dst)
                if how:
                    depth_touch[('copy INTO depth', e.kind, e.dst, how)] += 1
    tagged = lambda how: '' if how == 'id' else ' (by shape)'
    w('\n  G8 -- readers of a UI colour target after the last UI draw '
      '(%d complete frames):\n' % judged)
    if not after_readers:
        w('    none\n')
    for (what, a, b, src, dst, how), n in sorted(after_readers.items(),
                                                key=lambda kv: -kv[1]):
        if what == 'draw':
            w('    draw vs %s ps %s  reads %s %s -> %s %s  x%d%s  %s\n' % (
                a, b, src, tok_desc(census, src), dst, tok_desc(census, dst), n,
                tagged(how), label(a)))
        elif what == 'dispatch':
            w('    dispatch cs %s  reads %s %s -> uav %s  x%d%s  %s\n' % (
                a, src, tok_desc(census, src), dst, n, tagged(how), label(a)))
        else:
            w('    copy %s  %s %s -> %s %s  x%d%s\n' % (
                a, src, tok_desc(census, src), dst, tok_desc(census, dst), n,
                tagged(how)))
    w('\n  G10 -- touches of the UI draws\' DEPTH view after the last UI draw '
      '(%d complete frames):\n' % judged)
    if not depth_touch:
        w('    none: the UI could write into that depth with nothing '
          'downstream reading it\n')
    for (what, a, b, how), n in sorted(depth_touch.items(), key=lambda kv: -kv[1]):
        w('    %s: %s %s  x%d%s  %s\n' % (what, a, tok_desc(census, b) if b else '',
                                           n, tagged(how), label(a)))
    w('\n')


def self_test():
    """That this reader still parses a census line, in both vintages.

    Added 2026-09-07 with the so= column. This tool had no self-test, and it
    reads the same emitter tools/diff_draw_census.py does -- whose own
    comment records what a drifted regex costs: a current log parsed as zero
    censuses, and a report that said "no difference" in the exact words it
    would have used had the effect genuinely been absent. The gates
    themselves are not asserted here; the PARSE is, because that is the half
    that fails silently."""
    old = ('[12:00:00.000] DC 0 #1 X n=360 i=1 r=@1 d=@2 c=@3 s=@4,-,-,- '
           'vh=A888D51024D9798E ds=02wA st=14 bm=F pr=- q=804')
    # ...with ia=/ib= (2026-09-08), the draw's own arguments and its index
    # buffer, on the IA tail: two more pairs to parse past.
    new = ('[12:00:00.000] DC 0 #2 X n=360 i=1 r=@1 d=@2 c=@3 s=@4,-,-,- '
           'vs=@6 vh=81216C77F90DEDD6 vb=@7 sd=32 of=0 tp=4 ia=1536,-12,0 '
           'ib=@8+64 vt=-,@5,-,-,-,-,-,- ds=17wZ st=14 bm=F pr=- '
           'bl=1,5,6,1/2,1,1 sm=FFFFFFFF so=rFF/w18/f7/1,1,3 q=805')
    lines = ['[12:00:00.000] DC begin census=1 frames=1 frame=1', old, new,
             '[12:00:00.000] DC id @1 tex 2818x2784 fmt=87 res=00000000AA00',
             '[12:00:00.000] DC id @2 tex 2818x2784 fmt=19 res=00000000AB00',
             '[12:00:00.000] DC id @3 buf 256 res=00000000AC00',
             '[12:00:00.000] DC id @4 tex 2212x1244 fmt=28 res=00000000AD00',
             # The structured form: a buffer line may now carry a stride, and
             # one without it must still parse (every log before 2026-09-07).
             '[12:00:00.000] DC id @5 buf 3010560 res=00000000AE00 stride=336',
             '[12:00:00.000] DC end census=1 draws=2 lines=2 interned=4 '
             'overflow=0 truncated=0']
    censuses = []
    cur = None
    for raw in lines:
        m = LINE_RE.match(raw)
        if not m:
            continue
        line = m.group(1)
        b = BEGIN_RE.match(line)
        if b:
            cur = Census(int(b.group(1)), int(b.group(2)), int(b.group(3)))
            censuses.append(cur)
            continue
        if cur is None:
            continue
        e = END_RE.match(line)
        if e:
            cur.end = e.group(2).strip()
            cur = None
            continue
        h = HEAD_RE.match(line)
        if h:
            ev = Ev()
            ev.tag, ev.frame, ev.idx = h.group(1), int(h.group(2)), int(h.group(3))
            for k, v in KV_RE.findall(h.group(14)):
                if k == 'vh':
                    ev.vh = v.upper()
                elif k == 'ds':
                    ev.ds = v
                elif k == 'st':
                    ev.st = v
                elif k == 'so':
                    ev.so = v
                elif k == 'vt':
                    ev.vt = v.split(',')
                elif k == 'q':
                    ev.q = int(v)
            cur.events.append(ev)
            continue
        t = ID_TEX_RE.match(line)
        if t:
            cur.ids['@' + t.group(1)] = dict(
                kind='tex', w=int(t.group(2)), h=int(t.group(3)),
                fmt=int(t.group(4)), res=t.group(5),
                vf=int(t.group(6)) if t.group(6) else None)
            continue
        u = ID_BUF_RE.match(line)
        if u:
            cur.ids['@' + u.group(1)] = dict(
                kind='buf', w=int(u.group(2)), h=int(u.group(4) or 0),
                fmt=0, res=u.group(3), vf=None)
    if len(censuses) != 1 or len(censuses[0].events) != 2:
        print('self-test: expected 1 census with 2 draws, got %d/%s'
              % (len(censuses),
                 len(censuses[0].events) if censuses else '-'))
        return 1
    a, b = censuses[0].events
    if a.q != 804 or a.st != '14' or a.so is not None:
        print('self-test: the pre-2026-09-07 line parsed wrong: %r'
              % ((a.q, a.st, a.so),))
        return 1
    if b.q != 805 or b.st != '14' or b.so != 'rFF/w18/f7/1,1,3':
        print('self-test: the so= line parsed wrong: %r'
              % ((b.q, b.st, b.so),))
        return 1
    if b.vt != ['-', '@5', '-', '-', '-', '-', '-', '-'] or a.vt != []:
        print('self-test: the vt= window parsed wrong: %r / %r' % (b.vt, a.vt))
        return 1
    # A structured buffer must carry its stride through to the description,
    # which is what tells the instanced-mesh pool from any other buffer.
    if tok_desc(censuses[0], '@5') != 'buf 3010560 stride 336':
        print('self-test: the buffer stride did not survive: %r'
              % tok_desc(censuses[0], '@5'))
        return 1
    if tok_desc(censuses[0], '@3') != 'buf 256':
        print('self-test: a strideless buffer should not grow a stride: %r'
              % tok_desc(censuses[0], '@3'))
        return 1
    # The whole point: a NEW trailing field must not swallow q=. That is the
    # failure mode, not a rejected line -- q= is last, and a greedy tail
    # takes it with no error of any kind.
    if tok_desc(censuses[0], '@2') != '2818x2784 R32G8X24_TYPELESS(19)':
        print('self-test: the depth token described wrong: %r'
              % tok_desc(censuses[0], '@2'))
        return 1
    print('self-test: ok')
    return 0


def main(argv):
    if '--self-test' in argv[1:]:
        return self_test()
    if len(argv) < 2:
        print(__doc__)
        return 1
    total = 0
    for path in argv[1:]:
        censuses = parse(path)
        sys.stdout.write('%s: %d census(es)\n' % (path, len(censuses)))
        for c in censuses:
            report(c, sys.stdout)
        total += len(censuses)
    return 0 if total else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
