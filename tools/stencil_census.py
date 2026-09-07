#!/usr/bin/env python3
"""Read the stencil plane's story off a draw census.

    python tools/stencil_census.py <edvr_gfx_*.log> [more logs...]
    python tools/stencil_census.py --self-test

Answers Phase 0 questions 1 and 2 of docs/per-object-motion.md, which the
per-object-motion design turns on:

  1. WHICH STENCIL BITS ARE FREE. The design stamps a per-mover tag into the
     scene depth's stencil plane through a twin depth-stencil state, and the
     tag's write mask must miss every bit the game reads. So per depth
     target: the union of bits any draw READS, the union any draw or clear
     WRITES, and what is left.
  2. WHETHER THE STENCIL SURVIVES TO SUBMIT. The tag is resolved in EDVR's
     own motion-vector dispatch. If something clears or rewrites the plane
     between the target's last draw and that dispatch, the tag has to be
     rescued at the last unbind instead. So per depth target: everything
     that touches it after its last draw, up to the dispatch.

Both answers are per DEPTH TARGET, grouped by the underlying RESOURCE rather
than the view token -- one resource can be bound through several views, and
the review of 2026-09-06 had to match on res= for exactly this reason. The
target EDVR's motion-vector dispatch reads is marked, because that one is
the scene depth and the others are not the design's business.

WHAT AN OLD LOG CANNOT ANSWER. The masks and ops arrived in the census on
2026-09-07 as the so= column. Every census captured before that prints the
stencil ENABLE and the REFERENCE and nothing else, and a reference is not a
mask: ref 4 says a draw compares against 4, not that bit 2 is the only bit it
reads. So for a census with no so= anywhere, this tool reports the enable and
reference inventory -- which is what the coming flight will be checked
against -- and refuses to compute free bits, rather than printing a confident
wrong number. Question 2 needs no masks and is answered on any census.

The census line grammar is the emitter's in src/d3d11/draw_census.cpp. Every
tail is optional here, the way tools/diff_draw_census.py and
tools/crisp_ui_gates.py read them: a log captured before a field existed must
still parse, and a field that is merely new must never cost a session again.

Exit 0 when at least one census was read, 1 otherwise.
"""

import re
import sys
from collections import OrderedDict

# EDVR's own motion-vector compute shader (the reviewed build). It is the
# reader question 2 measures up to; --mv overrides it when a build renumbers.
MV_HASH = '6D94E9C00DCE909F'

DXGI = {
    0: 'UNKNOWN', 19: 'R32G8X24_TYPELESS', 20: 'D32F_S8X24',
    21: 'R32F_X8X24_TYPELESS', 22: 'X32_G8X24_UINT', 39: 'R32_TYPELESS',
    40: 'D32F', 41: 'R32F', 44: 'R24G8_TYPELESS', 45: 'D24S8',
    46: 'R24_X8_TYPELESS', 47: 'X24_G8_UINT', 53: 'R16_TYPELESS', 54: 'R16F',
    55: 'D16', 56: 'R16_UNORM',
}
CMP = {1: 'NEVER', 2: 'LESS', 3: 'EQUAL', 4: 'LEQUAL', 5: 'GREATER',
       6: 'NOTEQUAL', 7: 'GEQUAL', 8: 'ALWAYS'}
OP = {1: 'KEEP', 2: 'ZERO', 3: 'REPLACE', 4: 'INCR_SAT', 5: 'DECR_SAT',
      6: 'INVERT', 7: 'INCR', 8: 'DECR'}
# The two comparisons that read no stencil bits at all: ALWAYS passes and
# NEVER fails without consulting the plane. Everything between them compares
# (value & readmask) against (ref & readmask), which is what "reads" means
# here and what a tag's write mask must avoid.
NON_READING_FUNCS = (1, 8)

LINE_RE = re.compile(r'^\[\d{2}:\d{2}:\d{2}\.\d{3}\] (DC\S* .*)$')
HEAD_RE = re.compile(r'^(DC|DCO) (\d+) #(\d+) ([A-Z]) n=(\d+) i=(\d+) '
                     r'r=(\S+) d=(\S+) c=(\S+) s=(\S+),(\S+),(\S+),(\S+)(.*)$')
CLEAR_D_RE = re.compile(r'^DCL (\d+) #(\d+) D dsv=(\S+) f=(\d+) z=(\S+) '
                        r's=(\d+)(.*)$')
DISP_RE = re.compile(r'^DCX (\d+) #(\d+) n=(\d+),(\d+),(\d+) ch=([0-9A-Fa-f]+) '
                     r'u=(\S+)(.*)$')
COPY_RE = re.compile(r'^DCC (\d+) #(\d+) ([A-Z]) dst=(\S+) sub=(\d+) '
                     r'at=(\d+),(\d+) src=(\S+) sub=(\d+)(.*)$')
ID_TEX_RE = re.compile(r'^DC id @(\d+) tex (\d+)x(\d+) fmt=(\d+)'
                       r'(?: res=(\S+))?(?: vf=(\d+))?$')
ID_BUF_RE = re.compile(r'^DC id @(\d+) buf (\d+)(?: res=(\S+))?$')
BEGIN_RE = re.compile(r'^DC begin census=(\d+) frames=(\d+) frame=(\d+)')
END_RE = re.compile(r'^DC end census=(\d+)(.*)$')
KV_RE = re.compile(r'(\w+)=(\S+)')

# st=<enable><ref>, the column as it has always been: one character of enable
# then the reference in decimal. st=14 is enable 1, reference 4.
ST_RE = re.compile(r'^([01?])(\d+)$')
# so=rRR/wWW/fF/fail,zfail,pass[+fF/fail,zfail,pass] (2026-09-07). Masks in
# hex, everything else decimal; the back face only when it differs.
SO_RE = re.compile(r'^r([0-9A-Fa-f]+)/w([0-9A-Fa-f]+)/f(\d+)/(\d+),(\d+),(\d+)'
                   r'(?:\+f(\d+)/(\d+),(\d+),(\d+))?$')


class Face(object):
    """One face's stencil behaviour."""
    __slots__ = ('func', 'fail', 'zfail', 'ppass')

    def __init__(self, func, fail, zfail, ppass):
        self.func, self.fail, self.zfail, self.ppass = func, fail, zfail, ppass

    def reads(self):
        return self.func not in NON_READING_FUNCS

    def writes(self):
        """True when any op can change the plane. KEEP on all three is a
        draw that tests and never writes, which is most of them."""
        return not (self.fail == 1 and self.zfail == 1 and self.ppass == 1)

    def text(self):
        return '%s %s/%s/%s' % (CMP.get(self.func, self.func),
                                OP.get(self.fail, self.fail),
                                OP.get(self.zfail, self.zfail),
                                OP.get(self.ppass, self.ppass))


class Sten(object):
    """A draw's whole stencil state, as far as the census recorded it.

    enable/ref come from st=, which every census has. read/write/faces come
    from so=, which only censuses from 2026-09-07 have; `full` says which."""
    __slots__ = ('enable', 'ref', 'read', 'write', 'front', 'back', 'full')

    def __init__(self):
        self.enable = None
        self.ref = None
        self.read = self.write = None
        self.front = self.back = None
        self.full = False

    def read_bits(self):
        """Bits this draw compares against, 0 when it compares nothing."""
        if not self.enable or not self.full:
            return 0
        if not (self.front.reads() or self.back.reads()):
            return 0
        return self.read

    def write_bits(self):
        """Bits this draw can change, 0 when it changes nothing. The write
        mask bounds every op -- REPLACE stamps ref & mask, INVERT and the
        increments touch only mask bits -- so the mask IS the answer as soon
        as one op is not KEEP."""
        if not self.enable or not self.full:
            return 0
        if not (self.front.writes() or self.back.writes()):
            return 0
        return self.write

    def text(self):
        if self.enable is None:
            return 'st=?'
        if not self.enable:
            return 'off'
        s = 'on ref=%d' % self.ref
        if self.full:
            s += ' r=%02X w=%02X %s' % (self.read, self.write,
                                        self.front.text())
            if self.back is not self.front:
                s += ' back %s' % self.back.text()
        else:
            s += ' (masks not recorded)'
        return s


def parse_sten(tail_kv):
    """A draw's stencil state from its already-split key/value tail."""
    s = Sten()
    st = tail_kv.get('st')
    if st:
        m = ST_RE.match(st)
        if m:
            s.enable = None if m.group(1) == '?' else bool(int(m.group(1)))
            s.ref = int(m.group(2))
    so = tail_kv.get('so')
    if so:
        m = SO_RE.match(so)
        if m:
            s.read = int(m.group(1), 16)
            s.write = int(m.group(2), 16)
            s.front = Face(int(m.group(3)), int(m.group(4)), int(m.group(5)),
                           int(m.group(6)))
            if m.group(7) is None:
                s.back = s.front
            else:
                s.back = Face(int(m.group(7)), int(m.group(8)),
                              int(m.group(9)), int(m.group(10)))
            s.full = True
    return s


class Ev(object):
    """One recorded event that matters to the stencil: a draw, a depth clear,
    a dispatch, or a copy. Fields absent for a kind stay at their defaults."""
    __slots__ = ('tag', 'frame', 'idx', 'kind', 'n', 'i', 'r', 'd', 'c', 's',
                 'vh', 'ph', 'ch', 'u', 'ds', 'sten', 'q', 'src', 'dst',
                 'flags', 'z', 'sval')

    def __init__(self):
        self.tag = '?'
        self.frame = self.idx = 0
        self.kind = '?'
        self.n = self.i = 0
        self.r = self.d = self.c = '-'
        self.s = []
        self.u = []
        self.vh = self.ph = self.ch = None
        self.ds = None
        self.sten = Sten()
        self.q = -1
        self.src = self.dst = None
        self.flags = 0
        self.z = ''
        self.sval = 0


class Census(object):
    def __init__(self, no, frames, first_frame, source):
        self.no = no
        self.frames = frames
        self.first_frame = first_frame
        self.source = source
        self.events = []
        self.ids = {}       # '@N' -> dict(kind, w, h, fmt, res, vf)
        self.end = ''

    def res_of(self, tok):
        """The resource behind a view token. An inline token (the intern
        table was full) has none, so its own text stands in: two inline
        tokens of the same size and format then compare equal, which
        over-connects rather than under-connects."""
        e = self.ids.get(tok)
        if e and e.get('res'):
            return e['res']
        return tok

    def desc(self, tok):
        if tok in ('-', '?'):
            return tok
        e = self.ids.get(tok)
        if e is None:
            m = re.match(r'^tex(\d+)x(\d+)f(\d+)$', tok)
            if m:
                return '%sx%s %s' % (m.group(1), m.group(2),
                                     fmt_name(int(m.group(3))))
            return tok
        if e['kind'] == 'buf':
            return 'buf %d' % e['w']
        s = '%dx%d %s' % (e['w'], e['h'], fmt_name(e['fmt']))
        if e['vf'] is not None and e['vf'] != e['fmt']:
            s += ' view %s' % fmt_name(e['vf'])
        return s

    def has_so(self):
        return any(e.sten.full for e in self.events if e.tag in ('DC', 'DCO'))


def fmt_name(n):
    if n is None:
        return '?'
    return '%s(%d)' % (DXGI.get(n, 'fmt'), n)


def parse_lines(lines, source):
    censuses = []
    cur = None
    for line in lines:
        b = BEGIN_RE.match(line)
        if b:
            cur = Census(int(b.group(1)), int(b.group(2)), int(b.group(3)),
                         source)
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
            kv = dict(KV_RE.findall(h.group(14)))
            ev.vh = (kv.get('vh') or '').upper() or None
            ev.ph = (kv.get('ph') or '').upper() or None
            ev.ds = kv.get('ds')
            ev.sten = parse_sten(kv)
            ev.q = int(kv['q']) if 'q' in kv else -1
            cur.events.append(ev)
            continue
        cd = CLEAR_D_RE.match(line)
        if cd:
            ev = Ev()
            ev.tag = 'DCL'
            ev.kind = 'D'
            ev.frame = int(cd.group(1))
            ev.idx = int(cd.group(2))
            ev.d = cd.group(3)
            ev.flags = int(cd.group(4))
            ev.z = cd.group(5)
            ev.sval = int(cd.group(6))
            kv = dict(KV_RE.findall(cd.group(7)))
            ev.q = int(kv['q']) if 'q' in kv else -1
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
            kv = dict(KV_RE.findall(dsp.group(8)))
            ev.s = kv['s'].split(',') if 's' in kv else []
            ev.q = int(kv['q']) if 'q' in kv else -1
            cur.events.append(ev)
            continue
        cp = COPY_RE.match(line)
        if cp:
            ev = Ev()
            ev.tag = 'DCC'
            ev.frame = int(cp.group(1))
            ev.idx = int(cp.group(2))
            ev.kind = cp.group(3)
            ev.dst, ev.src = cp.group(4), cp.group(8)
            kv = dict(KV_RE.findall(cp.group(10)))
            ev.q = int(kv['q']) if 'q' in kv else -1
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
            cur.ids['@' + u.group(1)] = dict(kind='buf', w=int(u.group(2)),
                                             h=0, fmt=0, res=u.group(3),
                                             vf=None)
    return censuses


def parse(path):
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        lines = [m.group(1) for m in map(LINE_RE.match,
                                         (r.rstrip('\r\n') for r in f)) if m]
    return parse_lines(lines, path)


def bits(mask):
    """'FF (0-7)' -- the mask in hex and the bit numbers it names."""
    if mask == 0:
        return '00 (none)'
    on = [str(i) for i in range(8) if mask & (1 << i)]
    return '%02X (%s)' % (mask, ','.join(on))


def targets_of(census):
    """Depth resources touched during the census -> the view tokens seen for
    each, in first-seen order. Grouping is by RESOURCE: the same buffer bound
    through two views is one target, and the review of 2026-09-06 had to
    match on res= for exactly this reason."""
    out = OrderedDict()
    for ev in census.events:
        tok = None
        if ev.tag in ('DC', 'DCO') and ev.d not in ('-', '?'):
            tok = ev.d
        elif ev.tag == 'DCL' and ev.kind == 'D' and ev.d not in ('-', '?'):
            tok = ev.d
        if tok is None:
            continue
        res = census.res_of(tok)
        out.setdefault(res, [])
        if tok not in out[res]:
            out[res].append(tok)
    return out


def mv_reads(census):
    """The resources EDVR's motion-vector dispatch SAMPLES, and its q= per
    frame. That dispatch is the deadline question 2 measures up to.

    s= only, not u=: the claim printed against a target is that the dispatch
    READS it, and u= is where the dispatch writes its own MV and depth copy.
    An eye-sized output in u= marked as "the scene depth" would be a label
    that reads as evidence and is not.

    The deadline is PER TARGET, not per frame. There is one dispatch per eye
    and each reads its own eye's depth, so eye B's deadline is its own later
    dispatch; taking the frame's first for both would flag every event
    between the two dispatches against eye B and invent a failure.

    Returns (deadline, seen): deadline[res][frame] = the q of the dispatch
    that reads that resource in that frame, and seen[frame] = the frame's
    first dispatch, for the header line.
    """
    deadline = {}
    seen = {}
    for ev in census.events:
        if ev.tag != 'DCX' or ev.ch != MV_HASH:
            continue
        if ev.frame not in seen or ev.q < seen[ev.frame]:
            seen[ev.frame] = ev.q
        for tok in ev.s:
            if tok in ('-', '?', ''):
                continue
            r = census.res_of(tok)
            per = deadline.setdefault(r, {})
            if ev.frame not in per or ev.q < per[ev.frame]:
                per[ev.frame] = ev.q
    return deadline, seen


def touches(census, ev, res):
    """How this event touches the resource `res`, or None."""
    hit = []
    if ev.tag in ('DC', 'DCO'):
        if census.res_of(ev.d) == res:
            hit.append('draws into')
        if census.res_of(ev.r) == res:
            hit.append('renders to')
        for tok in ev.s:
            if tok not in ('-', '?') and census.res_of(tok) == res:
                hit.append('samples')
                break
    elif ev.tag == 'DCL' and ev.kind == 'D':
        if census.res_of(ev.d) == res:
            hit.append('clears %s' % {1: 'DEPTH', 2: 'STENCIL',
                                      3: 'DEPTH|STENCIL'}.get(ev.flags,
                                                              'f=%d' % ev.flags))
    elif ev.tag == 'DCX':
        for tok in list(ev.s) + list(ev.u):
            if tok not in ('-', '?', '') and census.res_of(tok) == res:
                hit.append('dispatch reads/writes')
                break
    elif ev.tag == 'DCC':
        if ev.dst and census.res_of(ev.dst) == res:
            hit.append('copy INTO')
        if ev.src and census.res_of(ev.src) == res:
            hit.append('copy from')
    return ', '.join(hit) if hit else None


def report(census, w, listing):
    w('\ncensus %d (%s), frames %d from %d\n' % (
        census.no, census.source, census.frames, census.first_frame))
    full = census.has_so()
    if not full:
        w('  NOTE: no so= column anywhere in this census, so it predates\n'
          '  2026-09-07 and the stencil MASKS were never recorded. Question 1\n'
          '  (free bits) cannot be answered from it; the enable/reference\n'
          '  inventory below is what the flight will be checked against.\n')
    mv_deadline, mv_seen = mv_reads(census)
    if mv_seen:
        w('  EDVR motion-vector dispatch %s, first of each frame at q=%s\n' % (
            MV_HASH, ', '.join('f%d:%d' % (f, q)
                               for f, q in sorted(mv_seen.items()))))
    else:
        w('  EDVR motion-vector dispatch %s: NOT PRESENT in this census, so\n'
          '  question 2 has no deadline to measure to here.\n' % MV_HASH)

    for res, toks in targets_of(census).items():
        draws = [e for e in census.events
                 if e.tag in ('DC', 'DCO') and census.res_of(e.d) == res]
        clears = [e for e in census.events
                  if e.tag == 'DCL' and e.kind == 'D'
                  and census.res_of(e.d) == res]
        mark = '  <-- the scene depth: the mv dispatch reads it' \
            if res in mv_deadline else ''
        w('\n  depth target %s  [%s]%s\n' % (
            census.desc(toks[0]), ' '.join(toks), mark))
        w('    %d draws, %d clears over %d frames\n' % (
            len(draws), len(clears), census.frames))

        # --- (a) (b) (c): what is read, what is written, what is left.
        read_mask = write_mask = 0
        readers = OrderedDict()
        writers = OrderedDict()
        # Draws whose state the census could NOT read: st=? (the OMGet
        # faulted or its budget was spent), or a line with no so= in a
        # census that otherwise has it. They contribute nothing to either
        # union, which biases the free set the UNSAFE way -- towards
        # reporting a bit free that some unseen draw reads. Counted so the
        # verdict can say so instead of quietly rounding down.
        unknown = 0
        for e in draws:
            if e.sten.enable is None or (full and not e.sten.full
                                         and e.sten.enable):
                unknown += 1
        for e in draws:
            rb, wb = e.sten.read_bits(), e.sten.write_bits()
            read_mask |= rb
            write_mask |= wb
            if rb:
                readers.setdefault((e.vh, e.sten.text()), 0)
                readers[(e.vh, e.sten.text())] += 1
            if wb:
                writers.setdefault((e.vh, e.sten.text()), 0)
                writers[(e.vh, e.sten.text())] += 1
        clear_all = [c for c in clears if c.flags & 2]
        if clear_all:
            # ClearDepthStencilView ignores the write mask and sets the whole
            # plane, so a stencil clear writes every bit by definition.
            write_mask |= 0xFF

        st_seen = OrderedDict()
        for e in draws:
            st_seen.setdefault(e.sten.text(), 0)
            st_seen[e.sten.text()] += 1
        w('    stencil states seen on its draws:\n')
        for text, n in sorted(st_seen.items(), key=lambda kv: -kv[1]):
            w('      %-52s x%d\n' % (text, n))

        # The clears, always, with their q= and where they sit relative to
        # the target's own draws. A clear BEFORE the first draw costs a tag
        # nothing; one after the last draw is question 2's failure, and the
        # only way to tell them apart is the ordinal.
        if clears:
            first_draw = {}
            last_draw = {}
            for e in draws:
                if e.q < 0:
                    continue
                first_draw[e.frame] = min(first_draw.get(e.frame, e.q), e.q)
                last_draw[e.frame] = max(last_draw.get(e.frame, -1), e.q)
            w('    clears of this target:\n')
            for e in sorted(clears, key=lambda e: (e.frame, e.q)):
                what = {1: 'DEPTH', 2: 'STENCIL',
                        3: 'DEPTH|STENCIL'}.get(e.flags, 'f=%d' % e.flags)
                fd, ld = first_draw.get(e.frame), last_draw.get(e.frame)
                if fd is None:
                    where = 'no draws this frame'
                elif e.q < fd:
                    where = 'before its first draw (q=%d)' % fd
                elif e.q > ld:
                    where = 'AFTER its last draw (q=%d)' % ld
                else:
                    where = 'MID-FRAME, between its draws (q=%d..%d)' % (fd, ld)
                w('      f%d q=%-6d %-13s z=%s s=%d   %s\n' % (
                    e.frame, e.q, what, e.z, e.sval, where))

        if full:
            w('    (a) bits READ   %s\n' % bits(read_mask))
            for (vh, text), n in sorted(readers.items(), key=lambda kv: -kv[1]):
                w('          vs %s  %s  x%d\n' % (vh or '?', text, n))
            w('    (b) bits WRITTEN %s%s\n' % (
                bits(write_mask),
                '  (0xFF because a stencil CLEAR sets the whole plane)'
                if clear_all else ''))
            for (vh, text), n in sorted(writers.items(), key=lambda kv: -kv[1]):
                w('          vs %s  %s  x%d\n' % (vh or '?', text, n))
            free = (~(read_mask | write_mask)) & 0xFF
            w('    (c) bits FREE   %s\n' % bits(free))
            if unknown:
                w('        WARNING: %d of %d draws into this target did not\n'
                  '        record their stencil state, so they are in neither\n'
                  '        union. The free set above is an UPPER BOUND: a bit\n'
                  '        one of those draws reads would be missing from it.\n'
                  % (unknown, len(draws)))
            if clear_all:
                # A clear sets every bit, so counting it as a writer makes the
                # free set empty on any target that is cleared -- which is
                # every depth target. What a tag actually has to avoid is the
                # DRAWS: a clear at the top of the frame runs before any tag
                # is stamped and costs the tag nothing. Both numbers are
                # printed so the reader can see which one their frame is.
                draw_free = (~(read_mask | draw_write_mask(draws))) & 0xFF
                w('        bits free of the DRAWS alone: %s\n'
                  '        (the clear is only a writer if it runs AFTER the\n'
                  '        tags are stamped -- see (d) below)\n'
                  % bits(draw_free))
        else:
            refs = sorted({e.sten.ref for e in draws
                           if e.sten.enable and e.sten.ref is not None})
            w('    (a)(b)(c) not answerable: no masks recorded.\n')
            w('        references seen while stencil enabled: %s\n' % (
                ', '.join(str(r) for r in refs) if refs else 'none'))
            off = sum(1 for e in draws if e.sten.enable is False)
            w('        draws with stencil disabled: %d of %d\n' %
              (off, len(draws)))

        # --- (d): does anything touch it after its last draw, before the mv?
        w('    (d) after this target\'s last draw, before the mv dispatch:\n')
        by_frame = {}
        for e in draws:
            if e.q >= 0:
                by_frame[e.frame] = max(by_frame.get(e.frame, -1), e.q)
        if not by_frame:
            w('        no draws with a q= ordinal; nothing to measure\n')
        for frame in sorted(by_frame):
            last = by_frame[frame]
            end = mv_deadline.get(res, {}).get(frame)
            # STRICTLY before the dispatch. The dispatch is the deadline, not
            # a violation of it: it reads the plane, which is the whole point,
            # and counting its own read as a "toucher" would mean no target
            # could ever come back clean.
            after = [e for e in census.events
                     if e.frame == frame and e.q > last
                     and (end is None or e.q < end)
                     and touches(census, e, res)]
            span = ('q>%d to the dispatch at q=%d' % (last, end)) if end \
                else ('q>%d to the end of the frame (no mv dispatch)' % last)
            if not after:
                why = ('The stencil the draws left is the stencil the '
                       'dispatch reads.' if end else
                       'No EDVR dispatch reads this target, so it is not the '
                       'scene depth and question 2 does not apply to it.')
                w('        frame %d: %s -- NOTHING touches it.\n'
                  '          %s\n' % (frame, span, why))
                continue
            w('        frame %d: %s\n' % (frame, span))
            for e in after:
                what = touches(census, e, res)
                extra = ''
                if e.tag in ('DC', 'DCO'):
                    extra = '  vs %s  %s' % (e.vh or '?', e.sten.text())
                    if e.sten.write_bits():
                        extra += '  WRITES STENCIL %s' % bits(
                            e.sten.write_bits())
                elif e.tag == 'DCL':
                    extra = '  to s=%d' % e.sval
                w('          q=%-6d %-4s %-28s%s\n' % (e.q, e.tag, what, extra))

        if listing:
            w('    full listing by q=:\n')
            evs = sorted([e for e in draws + clears if e.q >= 0],
                         key=lambda e: (e.frame, e.q))
            for e in evs:
                if e.tag == 'DCL':
                    w('      f%d q=%-6d CLEAR f=%d z=%s s=%d\n' % (
                        e.frame, e.q, e.flags, e.z, e.sval))
                else:
                    w('      f%d q=%-6d %s n=%-6d vs %-16s ds=%-6s %s\n' % (
                        e.frame, e.q, e.kind, e.n, e.vh or '?',
                        e.ds or '?', e.sten.text()))


def draw_write_mask(draws):
    m = 0
    for e in draws:
        m |= e.sten.write_bits()
    return m


# --------------------------------------------------------------- self-test
def self_test():
    def dc(lines):
        return ['[12:00:00.000] %s' % l for l in lines]

    # Census 1, the MODERN format: a scene depth pair bound through two view
    # tokens over ONE resource (which the grouping must merge), a stencil
    # clear at the top of the frame, scene draws that TEST against bit 2 and
    # never write, a lighting draw that WRITES bits 3 and 4, the UI after it,
    # and EDVR's motion-vector dispatch last.
    a = ['DC begin census=1 frames=1 frame=100 offscreen=yes']
    a += [
        # The clear, first thing in the frame.
        'DCL 0 #0 D dsv=@10 f=3 z=0.000 s=0 q=1',
        # Scene draws: stencil on, ref 4, reads through mask FF, all KEEP.
        'DC 0 #1 X n=900 i=1 r=@1 d=@10 c=@9 s=@3,-,-,- vh=AAAA000000000001 '
        'ds=17wA st=14 bm=F pr=- bl=0,2,1,1/2,1,1 sm=FFFFFFFF '
        'so=rFF/wFF/f7/1,1,1 q=2',
        # ...the same target through its OTHER view token: one target.
        'DC 0 #2 X n=800 i=1 r=@1 d=@11 c=@9 s=@3,-,-,- vh=AAAA000000000001 '
        'ds=17wA st=14 bm=F pr=- bl=0,2,1,1/2,1,1 sm=FFFFFFFF '
        'so=rFF/wFF/f7/1,1,1 q=3',
        # The lighting draw: ALWAYS (so it reads nothing) but REPLACE through
        # write mask 18 -- bits 3 and 4.
        'DC 0 #3 X n=6 i=1 r=@1 d=@10 c=@9 s=@3,-,-,- vh=BBBB000000000002 '
        'ds=02wZ st=18 bm=F pr=- bl=0,2,1,1/2,1,1 sm=FFFFFFFF '
        'so=rFF/w18/f8/1,1,3 q=4',
        # A draw with stencil OFF: neither reads nor writes, whatever its
        # masks say. This is the case a naive union would get wrong.
        'DC 0 #4 X n=6 i=1 r=@1 d=@10 c=@9 s=@3,-,-,- vh=CCCC000000000003 '
        'ds=17wZ st=00 bm=F pr=- bl=1,5,6,1/2,1,1 sm=FFFFFFFF '
        'so=rFF/wFF/f8/2,2,2 q=5',
        # A draw into a DIFFERENT depth, which must not pollute the pair.
        'DC 0 #5 X n=6 i=1 r=@2 d=@20 c=@9 s=@3,-,-,- vh=DDDD000000000004 '
        'ds=17wZ st=17 bm=F pr=- bl=1,5,6,1/2,1,1 sm=FFFFFFFF '
        'so=rFF/wFF/f3/1,1,3 q=6',
        # A draw whose state the census could NOT read: st=?0, no so=. It
        # must land in neither union AND must be counted, because a bit it
        # reads would otherwise be reported free.
        'DC 0 #6 X n=6 i=1 r=@1 d=@10 c=@9 s=@3,-,-,- vh=EEEE000000000005 '
        'ds=?0w? st=?0 bm=F pr=- q=7',
        # The motion-vector dispatch reads the pair: the deadline.
        'DCX 0 #0 n=353,348,1 ch=6D94E9C00DCE909F u=-,-,@40,-,-,-,-,- '
        's=@41,@10,-,-,-,-,-,- q=8',
        'DC id @1 tex 1832x1920 fmt=87 res=00000000AAAA0000',
        'DC id @2 tex 1832x1920 fmt=87 res=00000000AAAA1000',
        'DC id @3 tex 4096x4096 fmt=98 res=00000000AAAA2000',
        'DC id @9 buf 256 res=00000000AAAA3000',
        'DC id @10 tex 1832x1920 fmt=19 res=00000000DEAD0000 vf=20',
        'DC id @11 tex 1832x1920 fmt=19 res=00000000DEAD0000 vf=20',
        'DC id @20 tex 512x512 fmt=45 res=00000000DEAD8000',
        'DC id @40 tex 1832x1920 fmt=10 res=00000000AAAA4000',
        'DC id @41 tex 1832x1920 fmt=10 res=00000000AAAA5000',
        'DC end census=1 draws=5 lines=13 interned=9 overflow=0 truncated=0']

    # Census 2, the OLD format: no so= anywhere, and a UI draw that clears
    # the pair's stencil AFTER the last scene draw and BEFORE the dispatch --
    # the failure question 2 exists to catch.
    b = ['DC begin census=2 frames=1 frame=200']
    b += [
        'DC 0 #1 X n=900 i=1 r=@1 d=@10 c=@9 s=@3,-,-,- vh=AAAA000000000001 '
        'ds=17wA st=14 bm=F pr=- q=1',
        'DCL 0 #0 D dsv=@10 f=3 z=0.000 s=0 q=2',
        'DCX 0 #0 n=353,348,1 ch=6D94E9C00DCE909F u=-,-,-,-,-,-,-,- '
        's=@10,-,-,-,-,-,-,- q=3',
        'DC id @1 tex 1832x1920 fmt=87 res=00000000BBBB0000',
        'DC id @3 tex 4096x4096 fmt=98 res=00000000BBBB2000',
        'DC id @9 buf 256 res=00000000BBBB3000',
        'DC id @10 tex 1832x1920 fmt=19 res=00000000BEEF0000 vf=20',
        'DC end census=2 draws=1 lines=4 interned=4 overflow=0 truncated=0']

    # Through LINE_RE, exactly as a file would be read -- the timestamp
    # prefix is part of what is under test, and stripping it by hand here is
    # how tools/diff_draw_census.py once hid a LINE_RE that matched nothing.
    prefixed = dc(a + b) + ['[12:00:00.000] vScreen: unrelated line',
                            'no prefix at all']
    lines = [m.group(1) for m in map(LINE_RE.match, prefixed) if m]
    cs = parse_lines(lines, 'self-test')
    if len(cs) != 2:
        print('self-test: expected 2 censuses, parsed %d' % len(cs))
        return 1

    c1, c2 = cs
    if not c1.has_so():
        print('self-test: census 1 should carry so=')
        return 1
    if c2.has_so():
        print('self-test: census 2 must NOT carry so=')
        return 1

    # The two view tokens over one resource must merge into ONE target, and
    # the unrelated 512x512 depth must stay separate.
    t1 = targets_of(c1)
    if len(t1) != 2:
        print('self-test: expected 2 depth targets in census 1, got %d: %r'
              % (len(t1), list(t1)))
        return 1
    pair_res = '00000000DEAD0000'
    if pair_res not in t1 or sorted(t1[pair_res]) != ['@10', '@11']:
        print('self-test: the pair did not merge its two views: %r' % (t1,))
        return 1

    draws = [e for e in c1.events
             if e.tag in ('DC', 'DCO') and c1.res_of(e.d) == pair_res]
    if len(draws) != 5:
        print('self-test: expected 5 draws on the pair, got %d' % len(draws))
        return 1
    unk = [e for e in draws if e.sten.enable is None]
    if len(unk) != 1 or unk[0].sten.read_bits() or unk[0].sten.write_bits():
        print('self-test: the unreadable-state draw was mishandled')
        return 1
    rm = 0
    wm = 0
    for e in draws:
        rm |= e.sten.read_bits()
        wm |= e.sten.write_bits()
    # Reads: only the two GEQUAL(7) draws, through mask FF.
    if rm != 0xFF:
        print('self-test: read mask expected FF, got %02X' % rm)
        return 1
    # Writes by DRAWS: only the lighting draw's 0x18. The ALWAYS draw reads
    # nothing but does write; the stencil-OFF draw writes nothing despite a
    # write mask of FF and three ZERO ops, which is the trap.
    if wm != 0x18:
        print('self-test: draw write mask expected 18, got %02X' % wm)
        return 1
    if draw_write_mask(draws) != 0x18:
        print('self-test: draw_write_mask disagrees: %02X'
              % draw_write_mask(draws))
        return 1
    # Free of the draws alone: everything but the read FF -> nothing. The
    # read mask is FF here on purpose: it is the case the design calls the
    # end of the stencil route, and the tool must report it as such.
    if ((~(rm | wm)) & 0xFF) != 0x00:
        print('self-test: free bits expected 00 under a FF read mask')
        return 1

    # The same arithmetic with a REALISTIC read mask: a game that reads only
    # bit 2 leaves six bits free (bits 3 and 4 are written by the lighting
    # draw and so are not free either).
    narrow = 0x04
    if ((~(narrow | 0x18)) & 0xFF) != 0xE3:
        print('self-test: narrow free-bit arithmetic wrong')
        return 1

    # A stencil-disabled draw must contribute nothing in either direction.
    off = [e for e in draws if e.sten.enable is False]
    if len(off) != 1 or off[0].sten.read_bits() or off[0].sten.write_bits():
        print('self-test: a stencil-disabled draw contributed bits')
        return 1

    # The back-face form must parse, and a front-only form must alias back
    # to front rather than inventing a second face.
    s = parse_sten({'st': '14', 'so': 'r0F/w0F/f3/1,1,3+f8/1,2,3'})
    if not s.full or s.back is s.front or s.back.func != 8 or \
            s.back.zfail != 2:
        print('self-test: two-faced so= parsed wrong: %r' % (s.text(),))
        return 1
    s = parse_sten({'st': '14', 'so': 'rFF/wFF/f8/1,1,2'})
    if s.back is not s.front:
        print('self-test: one-faced so= should alias back to front')
        return 1

    # Question 2, both verdicts.
    mv_dl1, mv_seen1 = mv_reads(c1)
    if pair_res not in mv_dl1 or mv_dl1[pair_res].get(0) != 8:
        print('self-test: census 1 mv dispatch not found on the pair')
        return 1
    # The dispatch's OUTPUT slots must not make a target look like the scene
    # depth: @40 is in u= and nothing else, and must not be marked.
    if c1.res_of('@40') in mv_dl1:
        print('self-test: a dispatch OUTPUT was marked as a depth it reads')
        return 1
    # The pair's last draw is #6 at q=7; the dispatch is at q=8. Strictly
    # between them there is nothing, so census 1 is the CLEAN verdict.
    after1 = [e for e in c1.events
              if e.frame == 0 and e.q > 7 and e.q < 8
              and touches(c1, e, pair_res)]
    if after1:
        print('self-test: census 1 should have a clean tail, got %r'
              % [(e.q, e.tag) for e in after1])
        return 1
    mv_dl2, _ = mv_reads(c2)
    pair2 = '00000000BEEF0000'
    after2 = [e for e in c2.events
              if e.frame == 0 and e.q > 1 and e.q < mv_dl2[pair2][0]
              and touches(c2, e, pair2)]
    if len(after2) != 1 or after2[0].tag != 'DCL' or after2[0].flags != 3:
        print('self-test: census 2 should show ONE stencil clear after the '
              'last draw, got %r' % [(e.q, e.tag) for e in after2])
        return 1

    # And the report must run over both without raising.
    import io
    buf = io.StringIO()
    for c in cs:
        report(c, buf.write, True)
    out = buf.getvalue()
    for want in ('the scene depth: the mv dispatch reads it',
                 'bits FREE',
                 'no so= column anywhere in this census',
                 'The free set above is an UPPER BOUND',
                 'clears DEPTH|STENCIL'):
        if want not in out:
            print('self-test: report is missing %r' % want)
            return 1

    print('self-test: ok')
    return 0


def main(argv):
    global MV_HASH
    args = [a for a in argv[1:]]
    if '--self-test' in args:
        return self_test()
    listing = '--listing' in args
    args = [a for a in args if a != '--listing']
    for i, a in enumerate(args):
        if a == '--mv' and i + 1 < len(args):
            MV_HASH = args[i + 1].upper()
            args = args[:i] + args[i + 2:]
            break
    if not args:
        print(__doc__)
        return 1
    total = 0
    for path in args:
        censuses = parse(path)
        sys.stdout.write('%s: %d census(es)\n' % (path, len(censuses)))
        for c in censuses:
            report(c, sys.stdout.write, listing)
        total += len(censuses)
    return 0 if total else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
