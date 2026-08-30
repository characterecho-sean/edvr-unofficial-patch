#!/usr/bin/env python3
"""Diff two draw censuses and name the draws only one of them contains.

The census (hotkey.dump_draws, written by src/d3d11/draw_census.cpp) logs a
few whole frames of every draw that reaches the eye textures. Press the key
once with a visual effect absent and once with it present, and the draws the
effect adds are the ones in every frame of the second census and no frame of
the first. That is the whole method: it found nothing clever, it just refuses
to let a hundred unrelated HUD draws hide two overlay quads.

Written for frontier issue 69074 -- the RemLok helmet edge lines, drawn into
the WRONG eyes -- where the fix needs those draws identified before anything
can act on them.

What identifies a draw across censuses (its SIGNATURE): the draw kind, the
per-draw vertex or index count, what the render target and depth view resolve
to (size and format, not pointer), and what pixel-shader slots 0-3 resolve
to. Pointer identities are deliberately excluded: the game recreates objects
freely, and the constant buffer a draw uses is stable WITHIN a census but
means nothing between two. INSTANCE COUNTS are excluded too, and that was
learned from the first field capture: Elite's HUD draws are instanced, and
flipping life support off re-counted half of them (5 warning icons become
15), flooding ADDED and REMOVED with the same draws wearing new instance
counts. A signature names the draw; how many instances it ran is reported as
data, and a steady signature whose instance count changed directionally lands
in its own CHANGED section -- an effect drawn by an existing draw family
gaining instances shows up there, not nowhere. Two eye textures share a size
and format, so a per-eye overlay shows up as one signature with two draws per
frame -- which is exactly the shape worth reporting.

Usage:
  python tools/diff_draw_census.py <gfx-log> [<gfx-log2>] [--a N --b N]
  python tools/diff_draw_census.py --self-test

With one log, the last two censuses in it are compared (first = baseline).
With two logs, the last census of each is used. --a / --b pick censuses by
their position in the printed list instead. Exit code: 0 with a diff printed
(or a clean "no differences"), 1 on malformed input or self-test failure.
"""

import argparse
import re
import sys

LINE_RE = re.compile(r'^\[\d{2}:\d{2}:\d{2}\.\d{3}\] (DC .*)$')
# Every census regex tolerates ADDITIVE trailing fields, and that is a scar,
# not generosity: the DLL grew off=/copies= on its frame and end lines and
# offscreen= on its begin line (2026-08-24, the FSS hunt) while these
# anchors still demanded the old text, so a current log parsed as ZERO
# censuses -- and the self-test stayed green because it synthesised its own
# old-format lines instead of the emitter's. The self-test now carries one
# census in each format; a field that changes MEANING still has to fail
# here, but a field that is merely NEW must not cost a session again.
BEGIN_RE = re.compile(r'^DC begin census=(\d+) frames=(\d+) frame=(\d+)'
                      r'(?: \S+=\S+)*$')
# The IA/VS tail is optional: the DLL leaves it off when its probe faulted or
# its budget is spent, and censuses captured before the tail existed have none
# at all. Both must keep parsing -- a tool that rejects an old log cannot read
# the sessions already paid for. q= is the shared event ordinal (draws,
# copies, dispatches, one counter); positional, per-capture, and so no part
# of a signature.
DRAW_RE = re.compile(r'^DC (\d+) #(\d+) ([A-Z]) n=(\d+) i=(\d+) '
                     r'r=(\S+) d=(\S+) c=(\S+) s=(\S+),(\S+),(\S+),(\S+)'
                     r'(?: vs=(\S+)(?: vh=([0-9A-Fa-f]+))? vb=(\S+) '
                     r'sd=(\d+) of=(\d+) tp=(\d+))?'
                     r'(?: x=\S+(?:,\S+){3})?(?: ph=[0-9A-Fa-f]+)?'
                     # The viewport/scissor tail (2026-08-30), optional for
                     # the same reason every tail before it is: logs already
                     # captured do not carry it and must keep parsing. It is
                     # deliberately NOT part of a draw's signature -- see
                     # signature() -- because a diff across two censuses
                     # compares what was drawn, and where it landed is read
                     # off the line directly when that is the question.
                     r'(?: vp=\S+(?: z=\S+)? sc=\S+)?'
                     r'(?: q=\d+)?$')
FRAME_RE = re.compile(r'^DC frame (\d+) draws=(\d+)(?: \S+=\d+)*$')
# res= is the underlying resource's identity -- what connects an SRV @id to
# an RTV @id over the same texture WITHIN one census. Parsed past here
# because it is per-session noise BETWEEN censuses, exactly like the pointer
# ordinals the signature already excludes.
ID_TEX_RE = re.compile(r'^DC id @(\d+) tex (\d+)x(\d+) fmt=(\d+)'
                       r'(?: res=\S+)?$')
ID_BUF_RE = re.compile(r'^DC id @(\d+) buf (\d+)(?: res=\S+)?$')
ID_UNK_RE = re.compile(r'^DC id @(\d+) \?$')
END_RE = re.compile(r'^DC end census=(\d+) draws=(\d+)(?: \S+=\d+)*? '
                    r'lines=(\d+) interned=(\d+) overflow=(\d+) '
                    r'truncated=(\d+)$')


class Census(object):
    def __init__(self, number, frames, at_frame, source):
        self.number = number          # the in-log census counter
        self.frames = frames          # frames the capture was asked for
        self.at_frame = at_frame      # vscreen frame number at begin
        self.source = source          # file name, for the report
        self.draws = []               # (frame, idx, kind, n, i, r, d, c, slots)
        self.ids = {}                 # intern ordinal -> resolved string
        self.frame_draws = {}         # frame ordinal -> draws the DLL counted
        self.truncated = 0
        self.overflow = 0
        self.complete = False


def parse_logs(paths):
    """Every complete census in the given logs, in encounter order."""
    censuses = []
    for path in paths:
        with open(path, encoding='utf-8', errors='replace') as f:
            lines = [m.group(1) for m in map(LINE_RE.match, f) if m]
        censuses.extend(parse_dc_lines(lines, path))
    return censuses


def parse_dc_lines(lines, source):
    censuses = []
    cur = None
    for line in lines:
        m = BEGIN_RE.match(line)
        if m:
            # A begin while one is open means the previous census lost its end
            # line (log cap, crash); it is dropped rather than half-trusted.
            cur = Census(int(m.group(1)), int(m.group(2)), int(m.group(3)),
                         source)
            continue
        if cur is None:
            continue
        m = DRAW_RE.match(line)
        if m:
            # The tail's groups are all-or-nothing apart from vh, which
            # newer DLLs add; sd/of/tp arrive as numbers because they are
            # the half of it that means the same thing in two different
            # censuses. The shader HASH is the other cross-census half --
            # a content identity, unlike the vs pointer beside it -- and
            # rides at the end of the tuple so the older indices hold.
            ia = None
            if m.group(13) is not None:
                vh = m.group(14)
                if vh is not None and set(vh) == {'0'}:
                    vh = None   # unregistered shader: no identity to key on
                ia = (m.group(13), m.group(15), int(m.group(16)),
                      int(m.group(17)), int(m.group(18)), vh)
            cur.draws.append((int(m.group(1)), int(m.group(2)), m.group(3),
                              int(m.group(4)), int(m.group(5)), m.group(6),
                              m.group(7), m.group(8),
                              (m.group(9), m.group(10), m.group(11),
                               m.group(12)), ia))
            continue
        m = FRAME_RE.match(line)
        if m:
            cur.frame_draws[int(m.group(1))] = int(m.group(2))
            continue
        m = ID_TEX_RE.match(line)
        if m:
            # The same compact spelling the DLL emits INLINE when its intern
            # table overflows, so a slot interned in one census and inline in
            # the other still compares equal.
            cur.ids[int(m.group(1))] = 'tex%sx%sf%s' % (
                m.group(2), m.group(3), m.group(4))
            continue
        m = ID_BUF_RE.match(line)
        if m:
            cur.ids[int(m.group(1))] = 'buf%s' % m.group(2)
            continue
        m = ID_UNK_RE.match(line)
        if m:
            cur.ids[int(m.group(1))] = '?'
            continue
        m = END_RE.match(line)
        if m:
            cur.overflow = int(m.group(5))
            cur.truncated = int(m.group(6))
            cur.complete = True
            censuses.append(cur)
            cur = None
    return censuses


def resolve(census, token):
    """An id token from a draw line, as its cross-census meaning."""
    if token in ('-', '?'):
        return token
    if token.startswith('@'):
        return census.ids.get(int(token[1:]), '?')
    # Inline-resolved by the DLL when its intern table was full: already the
    # cross-census meaning ("tex512x64f28", "buf256").
    return token


def signature(census, draw):
    _frame, _idx, kind, n, _i, r, d, _c, slots, ia = draw
    # The constant buffer is left out because a pointer ordinal means nothing
    # across two censuses -- the same buffer interns as @2 in one and @9 in the
    # other, and a signature built on that never matches. The IA tail splits on
    # the same line: vs and vb are pointers and stay out for that reason, while
    # the stride and the topology are numbers that mean the same thing in both,
    # and a draw that changed either really is a different draw.
    stride = ia[2] if ia else None
    topology = ia[4] if ia else None
    vh = ia[5] if ia and len(ia) > 5 else None
    return (kind, n, resolve(census, r), resolve(census, d),
            tuple(resolve(census, s) for s in slots), stride, topology, vh)


def by_signature(census):
    """signature -> {frame ordinal -> [draw, ...]}"""
    out = {}
    for draw in census.draws:
        out.setdefault(signature(census, draw), {}).setdefault(
            draw[0], []).append(draw)
    return out


def frames_seen(census):
    """Frame ordinals that actually recorded anything."""
    seen = set(census.frame_draws)
    seen.update(d[0] for d in census.draws)
    return seen or {0}


def steady(sigs, frames):
    """Signatures present in EVERY frame -- a steady effect, not churn."""
    return {s for s, per in sigs.items() if set(per) >= frames}


def describe(sig):
    kind_names = {'D': 'Draw', 'I': 'DrawIndexed', 'N': 'DrawInstanced',
                  'X': 'DrawIndexedInstanced'}
    topo_names = {0: 'undefined', 1: 'points', 2: 'lines', 3: 'linestrip',
                  4: 'trilist', 5: 'tristrip'}
    kind, n, r, d, slots, stride, topology, vh = sig
    parts = ['%s n=%d' % (kind_names.get(kind, kind), n)]
    parts.append('target=%s' % r)
    parts.append('depth=%s' % ('none' if d == '-' else d))
    shown = [s for s in slots if s != '-']
    parts.append('samples=%s' % (','.join(shown) if shown else 'nothing'))
    # Only when the census carried an IA tail. A census that did not is not a
    # census whose draws had no vertex buffer, and must not read as one.
    if topology is not None:
        parts.append('topology=%s' % topo_names.get(topology, topology))
        parts.append('stride=%s' % ('no vertex buffer' if stride == 0 else stride))
    if vh is not None:
        # The vertex shader's content hash: the one key two draws running
        # different code cannot share, and the blob's file name under
        # edvr_logs\shaders when glare_shader_dump was on.
        parts.append('vshader=%s' % vh)
    return '  '.join(parts)


def skip_spec(sig):
    """What to put in advanced.census_skip to suppress this draw."""
    return '%s:%d' % (sig[0], sig[1])


def instance_totals(per, census):
    """Per-frame sum of instance counts, in frame order."""
    return [sum(d[4] for d in per.get(f, []))
            for f in sorted(frames_seen(census))]


def report_side(title, sigs_map, census, chosen):
    print()
    print(title)
    if not chosen:
        print('  (none)')
        return
    for sig in sorted(chosen, key=lambda s: sum(
            len(v) for v in sigs_map[s].values())):
        per = sigs_map[sig]
        counts = [len(per.get(f, [])) for f in sorted(frames_seen(census))]
        totals = instance_totals(per, census)
        idxs = [d[1] for v in per.values() for d in v]
        rtvs = {}
        for v in per.values():
            for d in v:
                rtvs[d[5]] = rtvs.get(d[5], 0) + 1
        print('  %s' % describe(sig))
        print('    draws per frame: %s   instances per frame: %s   '
              'eye-draw index range: %d-%d' % (
                  ','.join(map(str, counts)), ','.join(map(str, totals)),
                  min(idxs), max(idxs)))
        print('    render targets hit: %s   census_skip spec: %s' % (
            ', '.join('%s x%d' % (k, v) for k, v in sorted(rtvs.items())),
            skip_spec(sig)))


def report_changed(a_sigs, b_sigs, a, b):
    """Signatures steady in BOTH censuses whose instance volume moved in one
    direction -- where an effect drawn by an existing draw family shows up."""
    changed = []
    for sig in steady(a_sigs, frames_seen(a)) & steady(b_sigs, frames_seen(b)):
        at = instance_totals(a_sigs[sig], a)
        bt = instance_totals(b_sigs[sig], b)
        if min(bt) > max(at) or max(bt) < min(at):
            changed.append((max(min(bt) - max(at), min(at) - max(bt)), sig,
                            at, bt))
    print()
    print('CHANGED -- same draw in both, instance volume moved one way:')
    if not changed:
        print('  (none)')
        return set()
    for delta, sig, at, bt in sorted(changed, reverse=True)[:20]:
        print('  %s' % describe(sig))
        print('    instances per frame: %s -> %s (spec %s)' % (
            ','.join(map(str, at)), ','.join(map(str, bt)), skip_spec(sig)))
    if len(changed) > 20:
        print('  (%d more not shown)' % (len(changed) - 20))
    return {c[1] for c in changed}


def diff(a, b):
    a_sigs, b_sigs = by_signature(a), by_signature(b)
    added = steady(b_sigs, frames_seen(b)) - set(a_sigs)
    removed = steady(a_sigs, frames_seen(a)) - set(b_sigs)

    for label, c in (('baseline', a), ('effect', b)):
        note = []
        if c.truncated:
            note.append('%d draws past the line cap' % c.truncated)
        if c.overflow:
            note.append('%d bindings resolved inline past the intern table'
                        % c.overflow)
        print('census %d (%s, %s): %d frames, %d draws recorded%s' % (
            c.number, label, c.source, len(frames_seen(c)), len(c.draws),
            ' -- NOTE: ' + ', '.join(note) if note else ''))

    report_side('ADDED -- in every frame with the effect, never without it:',
                b_sigs, b, added)
    report_side('REMOVED -- in every frame without the effect, never with it:',
                a_sigs, a, removed)
    changed = report_changed(a_sigs, b_sigs, a, b)
    if not added and not removed and not changed:
        print()
        print('No steady difference. Either the effect was not visible during '
              'the second census, or its draws land somewhere this census '
              'cannot see (a deferred context, or a target that is not an eye '
              'texture).')
    return added, removed, changed


def self_test():
    def dc(lines):
        return ['[12:00:00.000] %s' % l for l in lines]

    eye = 'tex1832x1920f87'
    # Baseline: an instanced scene draw (i=5, will grow), a HUD atlas draw,
    # and one draw that will vanish.
    #
    # The scene draw carries an IA/VS tail and the other two do not, on
    # purpose: a census recorded before the tail existed, or one whose IA
    # probe spent its budget partway, holds exactly this mixture, and both
    # halves have to survive the same parse.
    a = ['DC begin census=1 frames=3 frame=1000']
    for f in range(3):
        a += ['DC %d #%d I n=5000 i=5 r=@0 d=@1 c=@2 s=@3,-,-,- '
              'vs=@7 vb=@8 sd=32 of=0 tp=4' % (f, 1),
              'DC %d #%d I n=6 i=1 r=@0 d=- c=@2 s=@4,-,-,-' % (f, 2),
              'DC %d #%d D n=3 i=1 r=@0 d=- c=@2 s=-,-,-,-' % (f, 3),
              'DC frame %d draws=3' % f]
    a += ['DC id @0 tex 1832x1920 fmt=87', 'DC id @1 tex 1832x1920 fmt=45',
          'DC id @2 buf 256', 'DC id @3 tex 4096x4096 fmt=98',
          'DC id @4 tex 2048x2048 fmt=28', 'DC id @7 ?', 'DC id @8 buf 96',
          'DC end census=1 draws=9 lines=9 interned=7 overflow=0 truncated=0']

    # Effect present: the scene draw re-counted (i=5 -> i=24: CHANGED, the
    # first field capture's noise shape, and must NOT read as added), the HUD
    # draw unchanged with a different cb ordinal (pointer identity must not
    # defeat a match), the vanished draw gone, a steady two-per-frame overlay
    # added -- one eye's line through the intern table and the other eye's
    # sampled slot resolved INLINE, as a full table writes it, which must land
    # in the same signature -- plus one-frame churn that must not be reported.
    #
    # The two overlay draws carry DIFFERENT vs ordinals for the same effect,
    # which must not split them either: a shader pointer is no more comparable
    # across two censuses than a constant buffer pointer is. Both have no
    # vertex buffer and a strip topology -- the shape of a pass that
    # synthesises its corners in the shader, which is the case the curved
    # screen work has to be able to recognise.
    #
    # This census is written in the CURRENT emitter's format -- offscreen= on
    # the begin line, q= on draw lines, off=/copies=/disp= on the frame and
    # end lines, res= on id lines -- while census a above stays in the
    # pre-2026-08-24 format. The pairing is the point: the DLL once grew its
    # lines while the anchored regexes here demanded the old text, a current
    # log parsed as zero censuses, and this self-test stayed green because
    # every line in it was the OLD format. Both vintages must parse, from one
    # test, forever.
    b = ['DC begin census=2 frames=3 frame=2000 offscreen=yes']
    for f in range(3):
        b += ['DC %d #%d I n=5000 i=24 r=@1 d=@2 c=@9 s=@3,-,-,- '
              'vs=@7 vb=@8 sd=32 of=0 tp=4 x=@4,-,-,- '
              'ph=00AA11BB22CC33DD q=0' % (f, 1),
              'DC %d #%d I n=6 i=1 r=@1 d=- c=@9 s=@4,-,-,- q=1' % (f, 2),
              'DC %d #%d D n=4 i=1 r=@1 d=- c=@9 s=@5,-,-,- '
              'vs=@7 vb=- sd=0 of=0 tp=5 q=2' % (f, 610),
              'DC %d #%d D n=4 i=1 r=@6 d=- c=@9 s=tex512x64f28,-,-,- '
              'vs=@10 vb=- sd=0 of=0 tp=5 q=3' % (f, 611)]
        if f == 1:
            b += ['DC %d #99 I n=12 i=1 r=@1 d=- c=@9 s=@4,-,-,- q=4' % f]
        b += ['DC frame %d draws=%d off=0 copies=2 disp=1' % (
            f, 5 if f == 1 else 4)]
    b += ['DC id @1 tex 1832x1920 fmt=87 res=000001B2C3D40000',
          'DC id @2 tex 1832x1920 fmt=45 res=000001B2C3D48000',
          'DC id @3 tex 4096x4096 fmt=98 res=000001B2C3D50000',
          'DC id @4 tex 2048x2048 fmt=28 res=000001B2C3D58000',
          'DC id @5 tex 512x64 fmt=28 res=000001B2C3D60000',
          'DC id @6 tex 1832x1920 fmt=87 res=000001B2C3D68000',
          'DC id @9 buf 256 res=000001B2C3D70000', 'DC id @7 ?',
          'DC id @8 buf 96 res=000001B2C3D78000', 'DC id @10 ?',
          'DC end census=2 draws=13 off=0 copies=6 disp=3 lines=13 '
          'interned=10 overflow=1 truncated=0']

    # Through LINE_RE, exactly as a file would be read: the timestamp-prefix
    # regex is part of what is being tested, and stripping the prefix by hand
    # here once hid a LINE_RE that matched nothing.
    prefixed = dc(a + b) + ['[12:00:00.000] vScreen: unrelated line',
                            'no prefix at all']
    stripped = [m.group(1) for m in map(LINE_RE.match, prefixed) if m]
    censuses = parse_dc_lines(stripped, 'self-test')
    if len(censuses) != 2:
        print('self-test: expected 2 censuses, parsed %d' % len(censuses))
        return 1
    added, removed, changed = diff(censuses[0], censuses[1])
    # The trailing triple is (stride, topology, vshader hash): filled in where
    # the line had a tail, None where it did not. None is "not measured" and
    # must never collapse into 0, which is "measured, and nothing was bound";
    # the hash is None for old logs and for shaders created before the hooks.
    want_added = {('D', 4, eye, '-', ('tex512x64f28', '-', '-', '-'), 0, 5,
                   None)}
    want_removed = {('D', 3, eye, '-', ('-', '-', '-', '-'), None, None, None)}
    want_changed = {('I', 5000, eye, 'tex1832x1920f45',
                     ('tex4096x4096f98', '-', '-', '-'), 32, 4, None)}
    if added != want_added:
        print('self-test: ADDED mismatch: %r' % added)
        return 1
    if removed != want_removed:
        print('self-test: REMOVED mismatch: %r' % removed)
        return 1
    if changed != want_changed:
        print('self-test: CHANGED mismatch: %r' % changed)
        return 1
    # The added overlay must be TWO draws a frame -- the interned form and the
    # inline form merged -- or the normalisation is not actually normalising.
    per = by_signature(censuses[1])[next(iter(want_added))]
    if sorted(len(v) for v in per.values()) != [2, 2, 2]:
        print('self-test: inline and interned tokens did not merge: %r' %
              {f: len(v) for f, v in per.items()})
        return 1
    print()
    print('self-test: ok')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('logs', nargs='*', help='edvr gfx log file(s)')
    ap.add_argument('--a', type=int, help='baseline census, 1-based position '
                    'in the collected list')
    ap.add_argument('--b', type=int, help='effect census, same numbering')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if not args.logs:
        ap.error('a log file is required (or --self-test)')

    censuses = parse_logs(args.logs)
    if len(censuses) < 2:
        print('Found %d complete census(es); two are needed. Bind '
              'hotkey.dump_draws, press it once with the effect absent and '
              'once with it present.' % len(censuses))
        return 1

    print('censuses found:')
    for i, c in enumerate(censuses, 1):
        print('  %d: census=%d at frame %d, %d draws (%s)' % (
            i, c.number, c.at_frame, len(c.draws), c.source))
    print()

    if len(args.logs) == 2 and args.a is None and args.b is None:
        per_file = {}
        for i, c in enumerate(censuses, 1):
            per_file[c.source] = i
        ia, ib = per_file[args.logs[0]], per_file[args.logs[1]]
    else:
        ia = args.a if args.a is not None else len(censuses) - 1
        ib = args.b if args.b is not None else len(censuses)
    if not (1 <= ia <= len(censuses) and 1 <= ib <= len(censuses) and ia != ib):
        print('census selection out of range: --a %s --b %s of %d' % (
            ia, ib, len(censuses)))
        return 1

    diff(censuses[ia - 1], censuses[ib - 1])
    return 0


if __name__ == '__main__':
    sys.exit(main())
