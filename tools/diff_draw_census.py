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

What identifies a draw across censuses (its SIGNATURE): the draw kind, vertex
and instance counts, what the render target and depth view resolve to (size
and format, not pointer), and what pixel-shader slots 0-3 resolve to. Pointer
identities are deliberately excluded: the game recreates objects freely, and
the constant buffer a draw uses is stable WITHIN a census but means nothing
between two. Two eye textures share a size and format, so a per-eye overlay
shows up as one signature with two draws per frame -- which is exactly the
shape worth reporting.

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
BEGIN_RE = re.compile(r'^DC begin census=(\d+) frames=(\d+) frame=(\d+)$')
DRAW_RE = re.compile(r'^DC (\d+) #(\d+) ([A-Z]) n=(\d+) i=(\d+) '
                     r'r=(\S+) d=(\S+) c=(\S+) s=(\S+),(\S+),(\S+),(\S+)$')
FRAME_RE = re.compile(r'^DC frame (\d+) draws=(\d+)$')
ID_TEX_RE = re.compile(r'^DC id @(\d+) tex (\d+)x(\d+) fmt=(\d+)$')
ID_BUF_RE = re.compile(r'^DC id @(\d+) buf (\d+)$')
ID_UNK_RE = re.compile(r'^DC id @(\d+) \?$')
END_RE = re.compile(r'^DC end census=(\d+) draws=(\d+) lines=(\d+) '
                    r'interned=(\d+) overflow=(\d+) truncated=(\d+)$')


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
            cur.draws.append((int(m.group(1)), int(m.group(2)), m.group(3),
                              int(m.group(4)), int(m.group(5)), m.group(6),
                              m.group(7), m.group(8),
                              (m.group(9), m.group(10), m.group(11),
                               m.group(12))))
            continue
        m = FRAME_RE.match(line)
        if m:
            cur.frame_draws[int(m.group(1))] = int(m.group(2))
            continue
        m = ID_TEX_RE.match(line)
        if m:
            cur.ids[int(m.group(1))] = 'tex%sx%s fmt=%s' % (
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
    return '?'


def signature(census, draw):
    _frame, _idx, kind, n, i, r, d, _c, slots = draw
    return (kind, n, i, resolve(census, r), resolve(census, d),
            tuple(resolve(census, s) for s in slots))


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
    kind, n, i, r, d, slots = sig
    parts = ['%s n=%d i=%d' % (kind_names.get(kind, kind), n, i)]
    parts.append('target=%s' % r)
    parts.append('depth=%s' % ('none' if d == '-' else d))
    shown = [s for s in slots if s != '-']
    parts.append('samples=%s' % (','.join(shown) if shown else 'nothing'))
    return '  '.join(parts)


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
        idxs = [d[1] for v in per.values() for d in v]
        rtvs = {}
        for v in per.values():
            for d in v:
                rtvs[d[5]] = rtvs.get(d[5], 0) + 1
        print('  %s' % describe(sig))
        print('    draws per frame: %s   eye-draw index range: %d-%d' % (
            ','.join(map(str, counts)), min(idxs), max(idxs)))
        print('    render targets hit: %s' % ', '.join(
            '%s x%d' % (k, v) for k, v in sorted(rtvs.items())))


def diff(a, b):
    a_sigs, b_sigs = by_signature(a), by_signature(b)
    added = steady(b_sigs, frames_seen(b)) - set(a_sigs)
    removed = steady(a_sigs, frames_seen(a)) - set(b_sigs)

    for label, c in (('baseline', a), ('effect', b)):
        note = []
        if c.truncated:
            note.append('%d draws past the line cap' % c.truncated)
        if c.overflow:
            note.append('%d bindings past the intern table' % c.overflow)
        print('census %d (%s, %s): %d frames, %d draws recorded%s' % (
            c.number, label, c.source, len(frames_seen(c)), len(c.draws),
            ' -- INCOMPLETE: ' + ', '.join(note) if note else ''))

    report_side('ADDED -- in every frame with the effect, never without it:',
                b_sigs, b, added)
    report_side('REMOVED -- in every frame without the effect, never with it:',
                a_sigs, a, removed)
    if not added and not removed:
        print()
        print('No steady difference. Either the effect was not visible during '
              'the second census, or its draws land somewhere this census '
              'cannot see (a deferred context, or a target that is not an eye '
              'texture).')
    return added, removed


def self_test():
    def dc(lines):
        return ['[12:00:00.000] %s' % l for l in lines]

    eye = 'tex1832x1920 fmt=87'
    # Baseline: scene draws, a HUD atlas draw, and one draw that will vanish.
    a = ['DC begin census=1 frames=3 frame=1000']
    for f in range(3):
        a += ['DC %d #%d I n=5000 i=1 r=@0 d=@1 c=@2 s=@3,-,-,-' % (f, 1),
              'DC %d #%d I n=6 i=1 r=@0 d=- c=@2 s=@4,-,-,-' % (f, 2),
              'DC %d #%d D n=3 i=1 r=@0 d=- c=@2 s=-,-,-,-' % (f, 3),
              'DC frame %d draws=3' % f]
    a += ['DC id @0 tex 1832x1920 fmt=87', 'DC id @1 tex 1832x1920 fmt=45',
          'DC id @2 buf 256', 'DC id @3 tex 4096x4096 fmt=98',
          'DC id @4 tex 2048x2048 fmt=28',
          'DC end census=1 draws=9 lines=9 interned=5 overflow=0 truncated=0']

    # Effect present: same scene and HUD (different cb ordinal on purpose --
    # pointer identity must not defeat the match), the vanished draw gone, a
    # steady two-per-frame overlay added, plus one-frame churn that must NOT
    # be reported.
    b = ['DC begin census=2 frames=3 frame=2000']
    for f in range(3):
        b += ['DC %d #%d I n=5000 i=1 r=@1 d=@2 c=@9 s=@3,-,-,-' % (f, 1),
              'DC %d #%d I n=6 i=1 r=@1 d=- c=@9 s=@4,-,-,-' % (f, 2),
              'DC %d #%d D n=4 i=1 r=@1 d=- c=@9 s=@5,-,-,-' % (f, 610),
              'DC %d #%d D n=4 i=1 r=@6 d=- c=@9 s=@5,-,-,-' % (f, 611)]
        if f == 1:
            b += ['DC %d #99 I n=12 i=1 r=@1 d=- c=@9 s=@4,-,-,-' % f]
        b += ['DC frame %d draws=%d' % (f, 5 if f == 1 else 4)]
    b += ['DC id @1 tex 1832x1920 fmt=87', 'DC id @2 tex 1832x1920 fmt=45',
          'DC id @3 tex 4096x4096 fmt=98', 'DC id @4 tex 2048x2048 fmt=28',
          'DC id @5 tex 512x64 fmt=28', 'DC id @6 tex 1832x1920 fmt=87',
          'DC id @9 buf 256',
          'DC end census=2 draws=13 lines=13 interned=7 overflow=0 truncated=0']

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
    added, removed = diff(censuses[0], censuses[1])
    want_added = {('D', 4, 1, eye, '-', ('tex512x64 fmt=28', '-', '-', '-'))}
    want_removed = {('D', 3, 1, eye, '-', ('-', '-', '-', '-'))}
    if added != want_added:
        print('self-test: ADDED mismatch: %r' % added)
        return 1
    if removed != want_removed:
        print('self-test: REMOVED mismatch: %r' % removed)
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
