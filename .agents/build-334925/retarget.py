#!/usr/bin/env python3
"""One-off retarget of build-bound addresses from build 334473 to build 334925.

Agent material for the 2026-10-23 retarget. Not a repo tool, not in the build gate.
Ported from .agents/build-334473/retarget.py; run with the repo's Python.

Scope: tracked text files under src/ and tools/, excluding src/installer/ and
tools/install_edvr.py, tools/build_diff.py, tools/build_diff_targets.json.

  retarget.py --dry-run   print the summary and write the full log; change no source file
  retarget.py --apply     rewrite the in-scope source files (refuses a second run)

map_all.json carries main's build-bound addresses (334473 -> 334925). A token
that is only a code citation in a comment or an address in a file the inventory
never listed (mem_tick.cpp, the CPU profiler tables, rigs) is mapped from
map_extra.json, and only inside EXTRA_FILES. Every replacement and every item
left for manual review is written to .agents/build-334925/retarget_log.txt.
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
AGENT = HERE
MAP = os.path.join(AGENT, 'map_all.json')
EXTRA_MAP = os.path.join(AGENT, 'map_extra.json')
LOG = os.path.join(AGENT, 'retarget_log.txt')
MARK = os.path.join(AGENT, 'retarget_applied.json')

VA = 0x140000000
OLD_SHA = '6891f70a021fa9f3a0e00458c284f48d1226e68ae7fc02ad48081aed47857e93'
NEW_SHA = 'ed5a8f8cd10f9a80d3bda5badd7ba1d4fcfc15996c8c7e018852cb170e264b63'
OLD_PREFIX, NEW_PREFIX = '6891f70a', 'ed5a8f8c'
OLD_ABBR, NEW_ABBR = '6891f70a...7857e93', 'ed5a8f8c...e264b63'
EXCLUDED_FILES = {'tools/install_edvr.py', 'tools/build_diff.py', 'tools/build_diff_targets.json'}
EXCLUDED_PREFIX = ('src/installer/',)

# Identity values: hex (any 0x spelling) and decimal. SizeOfImage is unchanged.
# 0x6ACD513D = 1791840573 -> 0x6ADA8048 = 1792704584.
SPECIAL_HEX = {0x6ACD513D: 0x6ADA8048}
SPECIAL_DEC = {1791840573: 1792704584, 334473: 334925}

# Files where a code address the inventory does not list is still an address:
# comment citations, rig layouts, and the later additions (mem_tick, the CPU
# profiler's class tables). Tokens there map from map_extra.json.
EXTRA_FILES = {
    'src/d3d11/cull_gate_probe.h', 'src/d3d11/engine_velocity_emit.h', 'src/d3d11/explorer_cam.cpp',
    'src/d3d11/explorer_cam_core.h', 'src/d3d11/kinematic_eval_hook.cpp', 'src/d3d11/kinematic_eval_hook.h',
    'src/d3d11/kinematic_eval_probe.cpp', 'src/d3d11/light_grid_cache_core.h', 'src/d3d11/mem_tick.cpp',
    'src/d3d11/optimized_station_core.h', 'src/d3d11/skin_ledger.h', 'src/d3d11/terrain_cull_cover_core.h',
    'src/d3d11/ui_sizing_math.h', 'src/d3d11/vertex_resync_core.h', 'src/d3d11/vr_ssaa_hold.cpp',
    'src/d3d11/vr_window_trampolines.h',
    'tools/cpu_profile/SelfTests.cs', 'tools/cpu_profile/Stacks.cs',
    'tools/light_grid_cache_test/light_grid_cache_test.cpp', 'tools/mem_watch_test/mem_watch_test.cpp',
    'tools/object_classification_test/object_classification_test.cpp',
    'tools/openxr_system_test/abi_caller.cpp', 'tools/terrain_cover_test/terrain_cover_test.cpp',
    'tools/ui_quality_test/ui_quality_test.cpp', 'tools/vertex_resync_test/vertex_resync_test.cpp',
    'tools/vr_camera_census_test/vr_camera_census_test.cpp',
}
EXTRA_MIN = 0x100000   # below this a token is more likely a struct offset than a code address
# Values that sit in the code range numerically but are not build addresses in the
# EXTRA_FILES: the FNV prime, 24-bit masks, a pointer and synthetic image offsets in
# the rigs, rig-only fixture values, a displacement, and page and range bases.
EXTRA_DENY = {0x01000193, 0xFFFFFF, 0x7F0000, 0x1000000, 0x3000000, 0x3000100, 0x1A00000, 0x1A00008,
              0x2000000, 0x4301234, 0x4305678, 0x86DF55, 0x4441000, 0x4458000, 0x4400000}

BINARY_EXT = re.compile(r'\.(dxbc|bin|dll|exe|png|jpg|jpeg|zip|pdb|obj|lib|cso|ico|bmp|dds|ttf|otf|cur|xnb|pyc|so|o|a)$', re.I)


def load_map(path):
    entries = json.load(open(path))
    rva = {}
    for e in entries:
        if e['new_rva'] is None:
            continue
        old, new = int(e['rva'], 16), int(e['new_rva'], 16)
        if old != new:
            rva[old] = new
    return rva


RVA = load_map(MAP)
RVA_EXTRA = load_map(EXTRA_MAP) if os.path.exists(EXTRA_MAP) else {}


def style(orig, new):
    """Write `new` in the letter case of `orig` (lower only if orig has lower letters and no upper)."""
    has_lo = any(c.islower() for c in orig if c.isalpha())
    has_up = any(c.isupper() for c in orig if c.isalpha())
    return new.lower() if (has_lo and not has_up) else new.upper()


def hexdigits(value, orig):
    """Hex digits of `value`, zero-padded to the width of `orig`, in orig's case style."""
    s = '%X' % value
    s = s.zfill(len(orig))
    return style(orig, s)


class FileScan:
    def __init__(self, path):
        self.path = path
        self.extra = path in EXTRA_FILES
        self.records = []      # (line, category, old_text, new_text, old_rva_or_identity_int, ctx)
        self.review = []       # (line, reason, text, ctx)
        self.hashskip = []     # (line, text)
        self.arrayline = []    # (line,)  lines that changed and look like byte arrays
        self.lineno = 0
        self.line = ''

    def rec(self, cat, old, new, value):
        self.records.append((self.lineno, cat, old, new, value, self.line.strip()[:150]))

    def rev(self, reason, text):
        self.review.append((self.lineno, reason, text, self.line.strip()[:170]))

    def lookup(self, rva):
        """New RVA for an old RVA, or None. Extras only inside EXTRA_FILES and above EXTRA_MIN."""
        if rva in RVA:
            return RVA[rva]
        if self.extra and rva >= EXTRA_MIN and rva not in EXTRA_DENY and rva in RVA_EXTRA:
            return RVA_EXTRA[rva]
        return None

    def map_value(self, v):
        """(new value, category, old RVA) for an integer that spells an old-build address or identity value."""
        n = self.lookup(v)
        if n is not None:
            return n, 'rva', v
        if v in SPECIAL_HEX:
            return SPECIAL_HEX[v], 'identity', v
        if v >= VA:
            n = self.lookup(v - VA)
            if n is not None:
                return VA + n, 'va', v - VA
        return None, None, None


HEX0X = re.compile(r'(?<![0-9A-Za-z_])0[xX]([0-9A-Fa-f]+)([uUlL]*)(?![0-9A-Za-z_])')
DEC = re.compile(r'(?:(?<![0-9A-Za-z_])|(?<=[Bb]uild))(1791840573|334473)([uUlL]*)(?![0-9A-Za-z_])')
DEC_TOKEN = re.compile(r'(1791840573|334473)([uUlL]*)')
SHA_ABBR = re.compile(r'(?<![0-9A-Za-z])(?i:' + re.escape(OLD_ABBR) + r')(?![0-9A-Za-z])')
BARE = re.compile(r'(?<![0-9A-Za-z])([0-9A-Fa-f]+)(?![0-9A-Za-z_])')
BYTE2 = re.compile(r'\b0[xX][0-9A-Fa-f]{1,2}\b')


def make_subs(scan):
    def cb_sha_abbr(m):
        new = style(m.group(0), NEW_ABBR)
        scan.rec('sha-abbrev', m.group(0), new, None)
        return new

    def cb_hex0x(m):
        digits, suffix = m.group(1), m.group(2)
        whole = m.group(0)
        if len(digits) > 9:
            if int(digits, 16) in RVA:
                scan.hashskip.append((scan.lineno, whole))
            return whole
        v = int(digits, 16)
        new_v, cat, old_rva = scan.map_value(v)
        if new_v is None:
            return whole
        new_digits = hexdigits(new_v, digits)
        new = '0' + whole[1] + new_digits + suffix
        scan.rec('0x-' + cat, whole, new, old_rva)
        return new

    def cb_dec(m):
        digits, suffix = m.group(1), m.group(2)
        new_digits = str(SPECIAL_DEC[int(digits)])
        new = new_digits + suffix
        scan.rec('decimal-identity', m.group(0), new, int(digits))
        return new

    def cb_bare(m):
        t = m.group(0)
        s = m.string
        pre = s[m.start() - 1] if m.start() > 0 else ''
        if t.lower() == OLD_PREFIX:
            new = style(t, NEW_PREFIX)
            scan.rec('sha-prefix', t, new, None)
            return new
        if len(t) == 64:
            if t.lower() == OLD_SHA:
                new = style(t, NEW_SHA)
                scan.rec('sha-full', t, new, None)
                return new
            return t
        if len(t) > 9:
            return t
        v = int(t, 16)
        letters = any(c in 'ABCDEFabcdef' for c in t)
        n_rva = scan.lookup(v)
        n_va = scan.lookup(v - VA) if v >= VA else None
        if pre == '_':
            # Identifier prefix (FUN_, primary_, state-reader-FUN_ ...). A "_" before a token
            # cannot make it a decimal number, so an address-named identifier follows its address.
            # Provenance file names (decomp_42B4420.txt, follow_28431D0.txt) name analysis files
            # that still carry the old address, so they stay.
            tail = m.string[m.end():m.end() + 6]
            head = m.string[max(0, m.start() - 9):m.start()]
            if re.match(r'\.(txt|log|json|csv|c|h|cpp|py|md|bin|dxbc|cs|bat)\b', tail) or \
                    re.search(r'(decomp|follow|dump)_$', head):
                if n_rva is not None or n_va is not None:
                    scan.rev('provenance file name (analysis file named by the old address): kept', t)
                return t
            if n_va is not None:
                new = hexdigits(VA + n_va, t)
                scan.rec('va-identifier', t, new, v - VA)
                return new
            if n_rva is not None:
                new = hexdigits(n_rva, t)
                scan.rec('ident-rva', t, new, v)
                return new
            return t
        if n_rva is not None:
            if letters:
                new = hexdigits(n_rva, t)
                scan.rec('bare-rva', t, new, v)
                return new
            scan.rev('all-digit bare RVA (may be decimal): not replaced', t)
            return t
        if v in SPECIAL_HEX:
            if letters:
                new = hexdigits(SPECIAL_HEX[v], t)
                scan.rec('bare-identity', t, new, v)
                return new
            scan.rev('all-digit bare PE identity value (may be decimal): not replaced', t)
            return t
        if n_va is not None:
            if letters or pre == '-':
                new = hexdigits(VA + n_va, t)
                scan.rec('bare-va' if letters else 'bare-va-string', t, new, v - VA)
                return new
            scan.rev('all-digit bare VA (may be decimal): not replaced', t)
            return t
        return t

    return cb_sha_abbr, cb_hex0x, cb_dec, cb_bare


COMBINED = re.compile('|'.join([
    '(?P<a>' + SHA_ABBR.pattern + ')',
    '(?P<x>' + HEX0X.pattern + ')',
    '(?P<d>' + DEC.pattern + ')',
    '(?P<b>' + BARE.pattern + ')',
]))


def scan_text(scan, text):
    cb_sha_abbr, cb_hex0x, cb_dec, cb_bare = make_subs(scan)

    def dispatch(m):
        # Group numbers differ inside COMBINED, so each token is re-matched alone
        # with its own pattern, which gives the digit and suffix groups back.
        tok = m.group(0)
        if m.group('a') is not None:
            return cb_sha_abbr(m)
        if m.group('x') is not None:
            return cb_hex0x(HEX0X.fullmatch(tok))
        if m.group('d') is not None:
            return cb_dec(DEC_TOKEN.fullmatch(tok))
        return cb_bare(m)

    out_lines = []
    for ln, line in enumerate(text.split('\n'), 1):
        scan.lineno = ln
        scan.line = line
        before = len(scan.records)
        # One pass over each line, so no replacement is ever re-scanned.
        new_line = COMBINED.sub(dispatch, line)
        if len(scan.records) > before and len(BYTE2.findall(line)) >= 4:
            scan.arrayline.append(ln)
        out_lines.append(new_line)
    return '\n'.join(out_lines)


def in_scope_files():
    tracked = subprocess.run(['git', 'ls-files', '-co', '--exclude-standard', 'src', 'tools'],
                             cwd=REPO, capture_output=True, text=True, check=True).stdout.split()
    files = []
    for f in sorted(set(tracked)):
        if f.startswith(EXCLUDED_PREFIX) or f in EXCLUDED_FILES:
            continue
        if BINARY_EXT.search(f):
            continue
        files.append(f)
    return files


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--dry-run', action='store_true')
    g.add_argument('--apply', action='store_true')
    args = ap.parse_args()

    if args.apply and os.path.exists(MARK):
        print('refusing: %s exists (already applied)' % MARK)
        return 2

    results = {}
    changed = {}
    for f in in_scope_files():
        raw = open(os.path.join(REPO, f), 'rb').read()
        if b'\0' in raw:
            continue
        text = raw.decode('utf-8', 'surrogateescape')
        scan = FileScan(f)
        new_text = scan_text(scan, text)
        results[f] = scan
        if new_text != text:
            changed[f] = new_text

    cats = collections.Counter()
    lines = ['retarget log: %s  mode=%s  files scanned=%d  map=%d extra=%d' % (
        time.strftime('%Y-%m-%d %H:%M:%S'), 'apply' if args.apply else 'dry-run', len(results),
        len(RVA), len(RVA_EXTRA))]
    for f in sorted(results):
        sc = results[f]
        if not (sc.records or sc.review or sc.hashskip or sc.arrayline):
            continue
        for c in sc.records:
            cats[c[1]] += 1
        lines.append('== %s: %d replacement(s), %d review, %d hash-like skipped, %d array-like changed lines' % (
            f, len(sc.records), len(sc.review), len(sc.hashskip), len(sc.arrayline)))
        for (ln, cat, old, new, value, ctx) in sc.records:
            lines.append('  L%d [%s] %s -> %s | %s' % (ln, cat, old, new, ctx))
        for (ln, reason, text, ctx) in sc.review:
            lines.append('  REVIEW L%d [%s] %s | %s' % (ln, reason, text, ctx))
        for (ln, text) in sc.hashskip:
            lines.append('  HASH-SKIP L%d %s' % (ln, text))
        for ln in sc.arrayline:
            lines.append('  ARRAY-LIKE-CHANGED L%d (check: a byte array must not be rewritten)' % ln)

    with open(LOG, 'w', encoding='utf-8') as fh:
        fh.write('\n'.join(lines) + '\n')

    total = sum(cats.values())
    print('replacements: %d in %d files' % (total, len(changed)))
    for c, n in sorted(cats.items(), key=lambda kv: -kv[1]):
        print('  %-20s %d' % (c, n))
    print('review (not replaced): %d; hash-like skipped: %d; array-like changed lines: %d' % (
        sum(len(s.review) for s in results.values()),
        sum(len(s.hashskip) for s in results.values()),
        sum(len(s.arrayline) for s in results.values())))
    print('log: %s' % LOG)

    if args.apply:
        for f, new_text in changed.items():
            with open(os.path.join(REPO, f), 'wb') as fh:
                fh.write(new_text.encode('utf-8', 'surrogateescape'))
        with open(MARK, 'w') as fh:
            json.dump({'applied': time.strftime('%Y-%m-%dT%H:%M:%S'), 'files_changed': len(changed),
                       'replacements': total, 'per_category': dict(cats)}, fh, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
