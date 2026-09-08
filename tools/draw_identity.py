#!/usr/bin/env python3
"""Does a stable per-draw identity exist? Read off a draw census.

docs/per-object-motion.md, Phase 0 question 4. The design's step-2 memo wants
to recognise a scene draw again next frame -- and a few seconds later -- by
its shape, so that one IAGetVertexBuffers per NEW draw buys a hash lookup
for every draw after it. The two 2026-09-07 flights said the shape the
census could see then (kind, count, vertex shader, vertex buffer, stride,
offset, topology, the VS constant object) does not distinguish one draw
from the next: hundreds of draws a frame share it, and the draw ORDER
agrees only 47-62% positionally across three seconds. So the memo as
written cannot work, and the question became whether ANY per-draw identity
is stable. The 2026-09-08 DLL prints the two things that could make one --
ia=start,base,startInstance (the draw call's own arguments) and ib= (the
bound index buffer) -- and this tool measures what they buy.

For every census it reports, per frame and for two keys side by side:

  shape   the key the 2026-09-07 analysis used: (DC/DCO, kind, n, i, vh,
          vb, sd, of, tp, c)
  args    the same plus ia= and ib=

  distinct   how many different keys the frame's draws fall into
  unique     the share of draws whose key no other draw in the frame has --
             the share that an identity could name at all

and between consecutive frames of the census:

  recur      the share of frame f's draws whose key is present in frame f+1
             (multiset-aware, so three draws sharing a key need three next
             frame to count as three)
  positional the share of frame f's draws whose key sits at the SAME ordinal
             in frame f+1 -- the draw-order stability the flights measured
  usable     the share of frame f's draws whose key is unique in f AND
             unique in f+1: the memo's clean hit rate, a draw it could look
             up and be sure of

Both populations are reported: every recorded draw, and the scene pair's
(the two busiest depth targets, which is where the design's tags go). With
two or more censuses in hand it also compares the first frame of each
against the next census's, with every token resolved to its cross-session
meaning (a pointer means nothing between sessions; a size does), and says
how far apart the two were taken.

A census without ia= (any log before 2026-09-08, or one whose IA probe spent
its budget) reports the shape key alone and says so; its args column is not
a measurement.

Usage:
    python tools/draw_identity.py <gfx log> [<gfx log> ...] [--scene-only]
    python tools/draw_identity.py --self-test
"""

import re
import sys

LINE_RE = re.compile(r'^\[(\d{2}):(\d{2}):(\d{2})\.(\d{3})\] (DC\S* .*)$')
HEAD_RE = re.compile(r'^(DC|DCO) (\d+) #(\d+) ([A-Z]) n=(\d+) i=(\d+) '
                     r'r=(\S+) d=(\S+) c=(\S+) s=(\S+),(\S+),(\S+),(\S+)(.*)$')
ID_TEX_RE = re.compile(r'^DC id @(\d+) tex (\d+)x(\d+) fmt=(\d+)'
                       r'(?: res=(\S+))?(?: vf=(\d+))?$')
ID_BUF_RE = re.compile(r'^DC id @(\d+) buf (\d+)(?: res=(\S+))?'
                       r'(?: stride=(\d+))?$')
BEGIN_RE = re.compile(r'^DC begin census=(\d+) frames=(\d+) frame=(\d+)')
END_RE = re.compile(r'^DC end census=(\d+)(.*)$')
KV_RE = re.compile(r'(\w+)=(\S+)')


class Draw(object):
    __slots__ = ('tag', 'frame', 'idx', 'kind', 'n', 'i', 'r', 'd', 'c',
                 'vh', 'vb', 'sd', 'of', 'tp', 'ia', 'ib', 'q')

    def __init__(self):
        self.vh = self.vb = self.sd = self.of = self.tp = None
        self.ia = self.ib = None
        self.q = -1


class Census(object):
    def __init__(self, no, frames, first_frame, source, seconds):
        self.no = no
        self.frames = frames
        self.first_frame = first_frame
        self.source = source
        self.seconds = seconds       # the begin line's time of day
        self.draws = []
        self.ids = {}                # '@N' -> dict(kind, w, h, fmt, res, stride)
        self.complete = False

    def frame_draws(self):
        """frame ordinal -> [Draw, ...] in recorded order."""
        out = {}
        for d in self.draws:
            out.setdefault(d.frame, []).append(d)
        return out

    def has_args(self):
        return any(d.ia is not None for d in self.draws)

    def res_of(self, tok):
        """The token's underlying resource identity within this census: the
        res= of its id line when there is one, else the token itself (a '-',
        an inline-resolved token, or a capped census with no table)."""
        e = self.ids.get(tok)
        if e and e['res']:
            return e['res']
        return tok

    def cross(self, tok):
        """The token's cross-session meaning: a size and format, never a
        pointer. Inline tokens already are; '-' stays '-'; an unresolvable
        '@N' (a capped census) reads as '?'."""
        if tok is None or tok == '-' or not tok.startswith('@'):
            return tok
        base, plus, off = tok.partition('+')
        e = self.ids.get(base)
        if not e:
            return '?'
        if e['kind'] == 'tex':
            s = 'tex%dx%df%d' % (e['w'], e['h'], e['fmt'])
        else:
            s = 'buf%d' % e['w'] + ('s%d' % e['stride'] if e['stride'] else '')
        return s + (plus + off if plus else '')


def parse_lines(lines, source):
    """lines: (seconds, text) pairs, text without the timestamp."""
    censuses = []
    cur = None
    for seconds, line in lines:
        b = BEGIN_RE.match(line)
        if b:
            cur = Census(int(b.group(1)), int(b.group(2)), int(b.group(3)),
                         source, seconds)
            censuses.append(cur)
            continue
        if cur is None:
            continue
        e = END_RE.match(line)
        if e:
            cur.complete = True
            cur = None
            continue
        h = HEAD_RE.match(line)
        if h:
            d = Draw()
            d.tag = h.group(1)
            d.frame = int(h.group(2))
            d.idx = int(h.group(3))
            d.kind = h.group(4)
            d.n = int(h.group(5))
            d.i = int(h.group(6))
            d.r, d.d, d.c = h.group(7), h.group(8), h.group(9)
            kv = dict(KV_RE.findall(h.group(14)))
            d.vh = (kv.get('vh') or '').upper() or None
            if d.vh is not None and set(d.vh) == {'0'}:
                d.vh = None   # a shader created before the hooks: no identity
            d.vb = kv.get('vb')
            d.sd = kv.get('sd')
            d.of = kv.get('of')
            d.tp = kv.get('tp')
            d.ia = kv.get('ia')
            d.ib = kv.get('ib')
            d.q = int(kv['q']) if 'q' in kv else -1
            cur.draws.append(d)
            continue
        t = ID_TEX_RE.match(line)
        if t:
            cur.ids['@' + t.group(1)] = dict(
                kind='tex', w=int(t.group(2)), h=int(t.group(3)),
                fmt=int(t.group(4)), res=t.group(5), stride=0)
            continue
        u = ID_BUF_RE.match(line)
        if u:
            cur.ids['@' + u.group(1)] = dict(
                kind='buf', w=int(u.group(2)), h=0, fmt=0, res=u.group(3),
                stride=int(u.group(4) or 0))
    return [c for c in censuses if c.complete]


def parse(path):
    out = []
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for raw in f:
            m = LINE_RE.match(raw.rstrip('\r\n'))
            if not m:
                continue
            seconds = (int(m.group(1)) * 3600 + int(m.group(2)) * 60 +
                       int(m.group(3)) + int(m.group(4)) / 1000.0)
            out.append((seconds, m.group(5)))
    return parse_lines(out, path)


# --- the keys -----------------------------------------------------------------

def key_shape(census, d, cross=False):
    f = census.cross if cross else (lambda t: t)
    return (d.tag, d.kind, d.n, d.i, d.vh, f(d.vb), d.sd, d.of, d.tp, f(d.c))


def key_args(census, d, cross=False):
    f = census.cross if cross else (lambda t: t)
    return key_shape(census, d, cross) + (d.ia, f(d.ib))


def ia_parts(d):
    """(start, base, startInstance) as strings, None where the line had none."""
    parts = (d.ia or '').split(',')
    while len(parts) < 3:
        parts.append(None)
    return parts[0], parts[1], parts[2]


# The args key taken apart, one field at a time. The 2026-09-08 flight
# answered the question this way: the geometry (index buffer, start index,
# base vertex) recurs at 95-98% frame to frame but names only 6-9% of the
# scene's draws -- the same mesh drawn many times -- while the start
# instance names 77-81% uniquely and recurs at 98% in a quiet scene and at
# 19-29% in flight, because it is the draw's address in an instance stream
# that is packed afresh every frame and shifts for every later draw when
# anything before it changes count. A key that lumps the fields together
# reports the worst of them; these say which field did what.
FIELD_KEYS = (
    ('shape', lambda c, d, x: key_shape(c, d, x)),
    ('+ib', lambda c, d, x: key_shape(c, d, x) + ((c.cross(d.ib) if x else d.ib),)),
    ('+start+base', lambda c, d, x: key_shape(c, d, x) + ia_parts(d)[:2]),
    ('+startInstance', lambda c, d, x: key_shape(c, d, x) + (ia_parts(d)[2],)),
    ('geometry', lambda c, d, x: key_shape(c, d, x) + ((c.cross(d.ib) if x else d.ib),) + ia_parts(d)[:2]),
    ('args (all)', lambda c, d, x: key_args(c, d, x)),
)


def scene_targets(census):
    """The res identities of the two busiest depth targets: the scene pair.
    Returns a set of res values (or raw tokens when the table is missing)."""
    counts = {}
    for d in census.draws:
        if d.d == '-':
            continue
        counts[census.res_of(d.d)] = counts.get(census.res_of(d.d), 0) + 1
    ranked = sorted(counts.items(), key=lambda kv: -kv[1])
    return set(r for r, _ in ranked[:2]), ranked[:2]


# --- the measurements ---------------------------------------------------------

def frame_stats(keys):
    """(count, distinct, unique) for one frame's list of keys."""
    counts = {}
    for k in keys:
        counts[k] = counts.get(k, 0) + 1
    unique = sum(1 for k in keys if counts[k] == 1)
    return len(keys), len(counts), unique


def pair_stats(keys_a, keys_b):
    """(recur, positional, usable) as counts over keys_a's draws."""
    ca, cb = {}, {}
    for k in keys_a:
        ca[k] = ca.get(k, 0) + 1
    for k in keys_b:
        cb[k] = cb.get(k, 0) + 1
    recur = sum(min(n, cb.get(k, 0)) for k, n in ca.items())
    positional = sum(1 for p in range(min(len(keys_a), len(keys_b)))
                     if keys_a[p] == keys_b[p])
    usable = sum(1 for k in keys_a if ca[k] == 1 and cb.get(k, 0) == 1)
    return recur, positional, usable


def pct(num, den):
    return '%3.0f%%' % (100.0 * num / den) if den else '  - '


def report(census, out, scene_only=False):
    per_frame = census.frame_draws()
    frames = sorted(per_frame)
    dc = sum(1 for d in census.draws if d.tag == 'DC')
    dco = len(census.draws) - dc
    with_args = sum(1 for d in census.draws if d.ia is not None)
    ibs = set(d.ib for d in census.draws if d.ib is not None)
    out.write('\ncensus %d (%s): %d frames, %d draws (DC %d + DCO %d)' % (
        census.no, census.source, len(frames), len(census.draws), dc, dco))
    if with_args:
        out.write('; ia= on %d of %d lines, %d distinct index buffers\n' % (
            with_args, len(census.draws), len(ibs)))
    else:
        out.write('\n  NO ia= COLUMN: this census predates the 2026-09-08 DLL '
                  '(or its IA probe spent its budget). The args column below '
                  'is the shape key again, not a measurement.\n')
    scene, ranked = scene_targets(census)
    if ranked:
        names = []
        for res, n in ranked:
            tok = next((d.d for d in census.draws if census.res_of(d.d) == res), res)
            names.append('%s (%s) x%d' % (tok, census.cross(tok), n))
        out.write('  scene pair: %s\n' % ', '.join(names))

    pops = [('scene', lambda d: census.res_of(d.d) in scene)]
    if not scene_only:
        pops.insert(0, ('all', lambda d: True))
    out.write('  %-22s %-26s   %s\n' % ('', 'shape key', 'args key (+ia/ib)'))
    for name, take in pops:
        for f in frames:
            draws = [d for d in per_frame[f] if take(d)]
            ks = [key_shape(census, d) for d in draws]
            ka = [key_args(census, d) for d in draws]
            n, ds, us = frame_stats(ks)
            _, da, ua = frame_stats(ka)
            out.write('  frame %d %-6s %5d draws  distinct %5d unique %s   '
                      'distinct %5d unique %s\n' % (
                          f, name, n, ds, pct(us, n), da, pct(ua, n)))
        for a, b in zip(frames, frames[1:]):
            da_ = [d for d in per_frame[a] if take(d)]
            db_ = [d for d in per_frame[b] if take(d)]
            ks_a = [key_shape(census, d) for d in da_]
            ks_b = [key_shape(census, d) for d in db_]
            ka_a = [key_args(census, d) for d in da_]
            ka_b = [key_args(census, d) for d in db_]
            r, p, u = pair_stats(ks_a, ks_b)
            r2, p2, u2 = pair_stats(ka_a, ka_b)
            n = len(ks_a)
            out.write('  frame %d->%d %-4s recur %s positional %s usable %s   '
                      'recur %s positional %s usable %s\n' % (
                          a, b, name, pct(r, n), pct(p, n), pct(u, n),
                          pct(r2, n), pct(p2, n), pct(u2, n)))
    # The fields one at a time, on the scene pair's first frame pair: which
    # of them names a draw, and which of them survives a frame.
    if len(frames) >= 2 and census.has_args():
        da_ = [d for d in per_frame[frames[0]] if census.res_of(d.d) in scene]
        db_ = [d for d in per_frame[frames[1]] if census.res_of(d.d) in scene]
        if da_:
            out.write('  the scene pair, frame %d->%d, by field:\n' % (frames[0], frames[1]))
            for name, fn in FIELD_KEYS:
                ka = [fn(census, d, False) for d in da_]
                kb = [fn(census, d, False) for d in db_]
                n, _, uq = frame_stats(ka)
                r, p, u = pair_stats(ka, kb)
                out.write('    %-15s unique %s  recur %s  positional %s  usable %s\n' % (
                    name, pct(uq, n), pct(r, n), pct(p, n), pct(u, n)))


def report_across(a, b, out):
    """The first frame of census a against the first frame of census b, every
    token in its cross-session meaning -- for every recorded draw and for the
    scene pair, since the pair is where the design's tags go and the whole
    population's order is dominated by the offscreen draws around it."""
    fa = a.frame_draws()
    fb = b.frame_draws()
    if not fa or not fb:
        return
    gap = b.seconds - a.seconds
    out.write('\ncensus %d frame %d -> census %d frame %d (%.1f s apart, tokens '
              'resolved):\n' % (a.no, min(fa), b.no, min(fb), gap))
    scene_a, _ = scene_targets(a)
    scene_b, _ = scene_targets(b)
    pops = [('all', lambda c, s, d: True),
            ('scene', lambda c, s, d: c.res_of(d.d) in s)]
    for name, take in pops:
        da_ = [d for d in fa[min(fa)] if take(a, scene_a, d)]
        db_ = [d for d in fb[min(fb)] if take(b, scene_b, d)]
        ks_a = [key_shape(a, d, True) for d in da_]
        ks_b = [key_shape(b, d, True) for d in db_]
        ka_a = [key_args(a, d, True) for d in da_]
        ka_b = [key_args(b, d, True) for d in db_]
        r, p, u = pair_stats(ks_a, ks_b)
        r2, p2, u2 = pair_stats(ka_a, ka_b)
        n = len(ks_a)
        out.write('  %-6s %5d draws  shape recur %s positional %s usable %s   '
                  'args recur %s positional %s usable %s\n' % (
                      name, n, pct(r, n), pct(p, n), pct(u, n), pct(r2, n),
                      pct(p2, n), pct(u2, n)))
    if not (a.has_args() and b.has_args()):
        out.write('  (one or both censuses carry no ia=: the args figures are '
                  'the shape figures again)\n')


# --- the self-test ------------------------------------------------------------

def self_test():
    def dc(lines):
        return [(43200.0 + i, l) for i, l in enumerate(lines)]

    # Census 1, the 2026-09-08 format: three scene draws that share every
    # shape field and differ only in their start index (A, B, C), and two UI
    # quads that are identical in every column (D, E). Frame 1 swaps B and C,
    # keeps D and E, and adds F.
    scene = ('X n=900 i=1 r=@1 d=@10 c=@9 s=@3,-,-,- vs=@30 vh=AAAA000000000001 '
             'vb=@31 sd=32 of=0 tp=4 ia=%s,0,0 ib=@32')
    quad = ('I n=6 i=1 r=@1 d=- c=@9 s=@4,-,-,- vs=@33 vh=BBBB000000000002 '
            'vb=@34 sd=16 of=0 tp=4 ia=0,0,0 ib=@35')
    a = ['DC begin census=1 frames=2 frame=100 offscreen=yes',
         'DC 0 #0 ' + scene % 0 + ' q=0',
         'DC 0 #1 ' + scene % 2700 + ' q=1',
         'DC 0 #2 ' + scene % 5400 + ' q=2',
         'DC 0 #3 ' + quad + ' q=3',
         'DC 0 #4 ' + quad + ' q=4',
         'DC frame 0 draws=5 off=0 copies=0 disp=0 clears=0 unseen=0',
         'DC 1 #0 ' + scene % 0 + ' q=0',
         'DC 1 #1 ' + scene % 5400 + ' q=1',
         'DC 1 #2 ' + scene % 2700 + ' q=2',
         'DC 1 #3 ' + quad + ' q=3',
         'DC 1 #4 ' + quad + ' q=4',
         'DC 1 #5 X n=12 i=1 r=@1 d=@10 c=@9 s=@3,-,-,- vs=@36 '
         'vh=CCCC000000000003 vb=@37 sd=32 of=0 tp=4 ia=0,0,0 ib=@32 q=5',
         'DC frame 1 draws=6 off=0 copies=0 disp=0 clears=0 unseen=0',
         'DC id @1 tex 1832x1920 fmt=87 res=00000000AAAA0000',
         'DC id @3 tex 4096x4096 fmt=98 res=00000000AAAA2000',
         'DC id @4 tex 2048x2048 fmt=28 res=00000000AAAA2800',
         'DC id @9 buf 256 res=00000000AAAA3000',
         'DC id @10 tex 1832x1920 fmt=19 res=00000000DEAD0000 vf=20',
         'DC id @31 buf 98304 res=00000000AAAA4000',
         'DC id @32 buf 196608 res=00000000AAAA5000',
         'DC id @34 buf 640 res=00000000AAAA6000',
         'DC id @35 buf 96 res=00000000AAAA7000',
         'DC id @37 buf 98304 res=00000000AAAA8000',
         'DC end census=1 draws=11 off=0 copies=0 disp=0 unseen=0 lines=11 '
         'interned=10 overflow=0 truncated=0']
    # Census 2, the OLD format: no ia=/ib=, the same three scene draws in the
    # same order. Against census 1 the shape key recurs entirely and the args
    # key, which it lacks, must read the same -- and be flagged.
    old = ('X n=900 i=1 r=@1 d=@10 c=@9 s=@3,-,-,- vs=@30 vh=AAAA000000000001 '
           'vb=@31 sd=32 of=0 tp=4')
    b = ['DC begin census=2 frames=1 frame=400',
         'DC 0 #0 ' + old + ' q=0',
         'DC 0 #1 ' + old + ' q=1',
         'DC 0 #2 ' + old + ' q=2',
         'DC frame 0 draws=3',
         'DC id @1 tex 1832x1920 fmt=87 res=00000000BBBB0000',
         'DC id @3 tex 4096x4096 fmt=98 res=00000000BBBB2000',
         'DC id @9 buf 256 res=00000000BBBB3000',
         'DC id @10 tex 1832x1920 fmt=19 res=00000000BEEF0000 vf=20',
         'DC id @31 buf 98304 res=00000000BBBB4000',
         'DC end census=2 draws=3 lines=3 interned=5 overflow=0 truncated=0']
    prefixed = ['[12:00:%02d.000] %s' % (i % 60, l) for i, l in enumerate(a + b)]
    prefixed += ['[12:00:00.000] vScreen: unrelated line', 'no prefix at all']
    parsed = []
    for raw in prefixed:
        m = LINE_RE.match(raw)
        if m:
            parsed.append((int(m.group(3)) + int(m.group(4)) / 1000.0, m.group(5)))
    cs = parse_lines(parsed, 'self-test')
    if len(cs) != 2:
        print('self-test: expected 2 censuses, parsed %d' % len(cs))
        return 1
    c1, c2 = cs
    if not c1.has_args() or c2.has_args():
        print('self-test: ia= presence read wrong (%s, %s)' % (c1.has_args(), c2.has_args()))
        return 1
    if c1.draws[0].ia != '0,0,0' or c1.draws[1].ia != '2700,0,0' or c1.draws[0].ib != '@32':
        print('self-test: ia=/ib= parsed wrong: %r %r %r' % (
            c1.draws[0].ia, c1.draws[1].ia, c1.draws[0].ib))
        return 1
    per = c1.frame_draws()
    # Frame 0 by the shape key: two keys (A=B=C, D=E), nothing unique. By the
    # args key: four keys, three unique -- the start index told A, B and C
    # apart and could not tell D from E.
    f0 = per[0]
    if frame_stats([key_shape(c1, d) for d in f0]) != (5, 2, 0):
        print('self-test: frame 0 shape stats: %r' % (frame_stats([key_shape(c1, d) for d in f0]),))
        return 1
    if frame_stats([key_args(c1, d) for d in f0]) != (5, 4, 3):
        print('self-test: frame 0 args stats: %r' % (frame_stats([key_args(c1, d) for d in f0]),))
        return 1
    # Frame 0 -> 1. Shape: everything recurs, everything sits at the same
    # ordinal (B and C are the same key), nothing usable. Args: everything
    # recurs, B and C swapped so 3 of 5 positional, A B C usable.
    ks0 = [key_shape(c1, d) for d in per[0]]
    ks1 = [key_shape(c1, d) for d in per[1]]
    ka0 = [key_args(c1, d) for d in per[0]]
    ka1 = [key_args(c1, d) for d in per[1]]
    if pair_stats(ks0, ks1) != (5, 5, 0):
        print('self-test: shape pair stats: %r' % (pair_stats(ks0, ks1),))
        return 1
    if pair_stats(ka0, ka1) != (5, 3, 3):
        print('self-test: args pair stats: %r' % (pair_stats(ka0, ka1),))
        return 1
    # The scene pair is the one depth target with draws; the quads are off it.
    scene, ranked = scene_targets(c1)
    if scene != {'00000000DEAD0000'} or ranked[0][1] != 7:
        print('self-test: scene targets: %r %r' % (scene, ranked))
        return 1
    # Across the two censuses, tokens resolved: the three scene draws of
    # census 2 match census 1's frame 0 by shape (3 of 5 recur; A B C at
    # ordinals 0..2 positional), and the quads have nowhere to go.
    ks_a = [key_shape(c1, d, True) for d in per[0]]
    ks_b = [key_shape(c2, d, True) for d in c2.frame_draws()[0]]
    if pair_stats(ks_a, ks_b) != (3, 3, 0):
        print('self-test: cross-census shape stats: %r' % (pair_stats(ks_a, ks_b),))
        return 1
    if c1.cross('@32') != 'buf196608' or c1.cross('@32+64') != 'buf196608+64' or \
            c1.cross('@10') != 'tex1832x1920f19' or c1.cross('-') != '-' or c1.cross('@99') != '?':
        print('self-test: cross-session tokens: %r' % (
            [c1.cross(t) for t in ('@32', '@32+64', '@10', '-', '@99')],))
        return 1
    # The report itself must run over both vintages without tripping.
    import io
    sink = io.StringIO()
    for c in cs:
        report(c, sink)
    report_across(c1, c2, sink)
    text = sink.getvalue()
    if 'NO ia= COLUMN' not in text or 'usable  60%' not in text:
        print('self-test: the report did not say what it should:\n%s' % text)
        return 1
    # The field split: in the fixture the start index is what tells A, B
    # and C apart (60% unique) and every start instance is 0, so that field
    # adds nothing to the shape (0%). A split that read the fields in the
    # wrong order would swap those two figures.
    f0 = per[0]
    by = dict((name, fn) for name, fn in FIELD_KEYS)
    if frame_stats([by['+start+base'](c1, d, False) for d in f0])[2] != 3 or \
            frame_stats([by['+startInstance'](c1, d, False) for d in f0])[2] != 0:
        print('self-test: the field split reads the wrong fields')
        return 1
    if 'by field:' not in text or '+startInstance  unique   0%' not in text:
        print('self-test: the field split is missing from the report:\n%s' % text)
        return 1
    print('self-test: ok')
    return 0


def main(argv):
    if '--self-test' in argv[1:]:
        return self_test()
    scene_only = '--scene-only' in argv[1:]
    paths = [a for a in argv[1:] if not a.startswith('--')]
    if not paths:
        print(__doc__)
        return 1
    censuses = []
    for path in paths:
        got = parse(path)
        sys.stdout.write('%s: %d complete census(es)\n' % (path, len(got)))
        censuses += got
    for c in censuses:
        report(c, sys.stdout, scene_only)
    for a, b in zip(censuses, censuses[1:]):
        report_across(a, b, sys.stdout)
    return 0 if censuses else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
