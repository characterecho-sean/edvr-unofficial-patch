#!/usr/bin/env python3
"""One-off retarget of build-bound addresses from build 332841 to build 334473.

Agent material for the 2026-10-20 retarget. Not a repo tool, not in the build gate.

Scope: tracked text files under src/ and tools/, excluding src/installer/ and
tools/install_edvr.py, tools/build_diff.py, tools/build_diff_targets.json.

  retarget.py --dry-run   print the summary and write the full log; change no source file
  retarget.py --apply     rewrite the in-scope source files (refuses a second run)

Every replacement and every item left for manual review is written to
.agents/build-334473/retarget_log.txt.
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys
import time

REPO = '/home/deck/projects/edvr-unofficial-patch'
AGENT = os.path.join(REPO, '.agents', 'build-334473')
MAP = os.path.join(AGENT, 'map_all.json')
INV = os.path.join(AGENT, 'inventory.json')
LOG = os.path.join(AGENT, 'retarget_log.txt')
MARK = os.path.join(AGENT, 'retarget_applied.json')

VA = 0x140000000
OLD_SHA = 'e6be8bbe04e6a7ae226d4318945af7f367de13dc5a007a261964d9ba8144e988'
NEW_SHA = '6891f70a021fa9f3a0e00458c284f48d1226e68ae7fc02ad48081aed47857e93'
EXCLUDED_FILES = {'tools/install_edvr.py', 'tools/build_diff.py', 'tools/build_diff_targets.json'}
EXCLUDED_PREFIX = ('src/installer/',)

# Identity values: hex (any 0x spelling) and decimal. Decimal spellings are the
# exact decimal values of the PE fields, checked by the coordinator's python:
# 0x6A989634 = 1788384820, 0x6409000 = 104894464 (the brief's 1788450356 was wrong).
SPECIAL_HEX = {0x6A989634: 0x6ACD513D, 0x6409000: 0x640C000}
SPECIAL_DEC = {1788384820: 1791840573, 104894464: 104906752, 332841: 334473}
# ".text end" constants are NOT in these tables: kRangeHi holds the page-rounded
# end (0x4DB3000) and is retargeted by hand, with its fixture, after this run.

BINARY_EXT = re.compile(r'\.(dxbc|bin|dll|exe|png|jpg|jpeg|zip|pdb|obj|lib|cso|ico|bmp|dds|ttf|otf|cur|xnb|pyc|so|o|a)$', re.I)


def load_map():
    entries = json.load(open(MAP))
    rva = {}
    nf = set()
    for e in entries:
        old = int(e['rva'], 16)
        if e['new_rva'] is None:
            nf.add(old)
        else:
            rva[old] = int(e['new_rva'], 16)
    return rva, nf


RVA, NOT_FOUND = load_map()
assert len(RVA) == 211 and len(NOT_FOUND) == 4, (len(RVA), len(NOT_FOUND))
assert all(v != k for k, v in RVA.items())


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


def map_value(v):
    """(new value, category, old RVA) for an integer that spells an old-build address or identity value."""
    if v in RVA:
        return RVA[v], 'rva', v
    if v in SPECIAL_HEX:
        return SPECIAL_HEX[v], 'identity', v
    if v >= VA and (v - VA) in RVA:
        return VA + RVA[v - VA], 'va', v - VA
    return None, None, None


class FileScan:
    def __init__(self, path):
        self.path = path
        self.records = []      # (line, category, old_text, new_text, old_rva_or_identity_int, ctx)
        self.review = []       # (line, reason, text, ctx)
        self.notfound = []     # (line, text)
        self.hashskip = []     # (line, text)
        self.arrayline = []    # (line,)  lines that changed and look like byte arrays
        self.lineno = 0
        self.line = ''

    def rec(self, cat, old, new, value):
        self.records.append((self.lineno, cat, old, new, value, self.line.strip()[:150]))

    def rev(self, reason, text):
        self.review.append((self.lineno, reason, text, self.line.strip()[:170]))


HEX0X = re.compile(r'(?<![0-9A-Za-z_])0[xX]([0-9A-Fa-f]+)([uUlL]*)(?![0-9A-Za-z_])')
DEC = re.compile(r'(?:(?<![0-9A-Za-z_])|(?<=[Bb]uild))(1788384820|104894464|332841)([uUlL]*)(?![0-9A-Za-z_])')
DEC_TOKEN = re.compile(r'(1788384820|104894464|332841)([uUlL]*)')
SHA_ABBR = re.compile(r'(?<![0-9A-Za-z])(?i:e6be8bbe\.\.\.4e988)(?![0-9A-Za-z])')
BARE = re.compile(r'(?<![0-9A-Za-z])([0-9A-Fa-f]+)(?![0-9A-Za-z_])')
BYTE2 = re.compile(r'\b0[xX][0-9A-Fa-f]{1,2}\b')


def make_subs(scan):
    def cb_sha_abbr(m):
        new = style(m.group(0), '6891f70a...7857e93')
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
        new_v, cat, old_rva = map_value(v)
        if new_v is None:
            if v in NOT_FOUND or (v >= VA and v - VA in NOT_FOUND):
                scan.notfound.append((scan.lineno, whole))
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
        if t.lower() == 'e6be8bbe':
            new = style(t, '6891f70a')
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
        if pre == '_':
            # Identifier prefix (FUN_, primary_, state-reader-FUN_ ...). A "_" before a token
            # cannot make it a decimal number, so an address-named identifier follows its address.
            # Provenance file names (decomp_42B4420.txt, follow_28431D0.txt) name analysis files
            # that still carry the old address, so they stay.
            tail = m.string[m.end():m.end() + 6]
            head = m.string[max(0, m.start() - 9):m.start()]
            if re.match(r'\.(txt|log|json|csv|c|h|cpp|py|md|bin|dxbc|cs|bat)\b', tail) or \
                    re.search(r'(decomp|follow|dump)_$', head):
                if v in RVA or (v >= VA and v - VA in RVA):
                    scan.rev('provenance file name (analysis file named by the old address): kept', t)
                return t
            if v >= VA and (v - VA) in RVA:
                new = hexdigits(VA + RVA[v - VA], t)
                scan.rec('va-identifier', t, new, v - VA)
                return new
            if v in RVA:
                new = hexdigits(RVA[v], t)
                scan.rec('ident-rva', t, new, v)
                return new
            return t
        if v in RVA:
            if letters:
                new = hexdigits(RVA[v], t)
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
        if v >= VA and (v - VA) in RVA:
            if letters or pre == '-':
                new = hexdigits(VA + RVA[v - VA], t)
                scan.rec('bare-va' if letters else 'bare-va-string', t, new, v - VA)
                return new
            scan.rev('all-digit bare VA (may be decimal): not replaced', t)
            return t
        if v in NOT_FOUND or (v >= VA and v - VA in NOT_FOUND):
            scan.notfound.append((scan.lineno, t))
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


def check_coverage(results):
    """Every inventory code/data RVA must have a replacement at its file:line."""
    inv = json.load(open(INV))
    have = collections.defaultdict(set)
    for f, sc in results.items():
        for rec in sc.records:
            ln, value = rec[0], rec[4]
            if value is not None:
                have[(f, ln)].add(value)
    missing = []
    checked = 0
    for e in inv:
        if e['kind'] not in ('code_rva', 'data_rva'):
            continue
        checked += 1
        old = e['old']
        # inventory 'old' fields can carry prose ("0x5E9DC28 (int .data, ...)"); take the hex values only
        for val in (int(x, 16) for x in re.findall(r'0[xX]([0-9A-Fa-f]+)', old)):
            if val not in RVA:
                continue   # not a mapped old-build address (or NOT-FOUND): nothing to check
            # The inventory line is often the declaration; the value sits a few lines below
            # (kJobRvas[] at 123, values at 124), so a replacement within +-4 lines counts.
            # Tables (kPartFrame[], kCalls[]) span many lines, so the test is per file: the value
            # must be replaced somewhere in the file the inventory names.
            ln0 = (e.get('lines') or [e['line']])[0]
            if not any(val in vals for (f2, _ln), vals in have.items() if f2 == e['file']):
                missing.append((e['file'], ln0, old[:60], e.get('symbol', '')))
    return checked, missing


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
    files = in_scope_files()
    for f in files:
        raw = open(os.path.join(REPO, f), 'rb').read()
        if b'\0' in raw:
            continue
        text = raw.decode('utf-8', 'surrogateescape')
        scan = FileScan(f)
        new_text = scan_text(scan, text)
        results[f] = scan
        if new_text != text:
            changed[f] = new_text

    # Log
    cats = collections.Counter()
    per_file = []
    lines = []
    lines.append('retarget log: %s  mode=%s  files scanned=%d' % (
        time.strftime('%Y-%m-%d %H:%M:%S'), 'apply' if args.apply else 'dry-run', len(results)))
    for f in sorted(results):
        sc = results[f]
        if not (sc.records or sc.review or sc.notfound or sc.hashskip or sc.arrayline):
            continue
        n = len(sc.records)
        for c in sc.records:
            cats[c[1]] += 1
        per_file.append((f, n))
        lines.append('== %s: %d replacement(s), %d review, %d not-found kept, %d hash-like skipped, %d array-like changed lines' % (
            f, n, len(sc.review), len(sc.notfound), len(sc.hashskip), len(sc.arrayline)))
        for (ln, cat, old, new, value, ctx) in sc.records:
            lines.append('  L%d [%s] %s -> %s | %s' % (ln, cat, old, new, ctx))
        for (ln, reason, text, ctx) in sc.review:
            lines.append('  REVIEW L%d [%s] %s | %s' % (ln, reason, text, ctx))
        for (ln, text) in sc.notfound:
            lines.append('  NOT-FOUND-KEPT L%d %s' % (ln, text))
        for (ln, text) in sc.hashskip:
            lines.append('  HASH-SKIP L%d %s' % (ln, text))
        for ln in sc.arrayline:
            lines.append('  ARRAY-LIKE-CHANGED L%d (check: a byte array must not be rewritten)' % ln)

    checked, missing = check_coverage(results)
    lines.append('')
    lines.append('== inventory coverage: %d code_rva/data_rva entries checked, %d missing' % (checked, len(missing)))
    for m in missing:
        lines.append('  MISSING %s:%s %s %s' % m)

    with open(LOG, 'w', encoding='utf-8') as fh:
        fh.write('\n'.join(lines) + '\n')

    total = sum(cats.values())
    print('replacements: %d in %d files' % (total, sum(1 for _, n in per_file if n)))
    for c, n in sorted(cats.items(), key=lambda kv: -kv[1]):
        print('  %-20s %d' % (c, n))
    review_total = sum(len(s.review) for s in results.values())
    notfound_total = sum(len(s.notfound) for s in results.values())
    hash_total = sum(len(s.hashskip) for s in results.values())
    array_total = sum(len(s.arrayline) for s in results.values())
    print('review (not replaced): %d; not-found kept: %d; hash-like skipped: %d; array-like changed lines: %d' % (
        review_total, notfound_total, hash_total, array_total))
    print('inventory coverage: %d code/data entries checked, %d missing' % (checked, len(missing)))
    for m in missing:
        print('  MISSING %s:%s %s %s' % m)
    print('log: %s' % LOG)

    if args.apply:
        for f, new_text in changed.items():
            with open(os.path.join(REPO, f), 'wb') as fh:
                fh.write(new_text.encode('utf-8', 'surrogateescape'))
        with open(MARK, 'w') as fh:
            json.dump({'applied': time.strftime('%Y-%m-%dT%H:%M:%S'), 'files_changed': len(changed),
                       'replacements': total, 'per_category': dict(cats)}, fh, indent=1)
        print('applied: %d files rewritten' % len(changed))
    else:
        print('dry-run: no source file changed (%d files would change)' % len(changed))
    return 0


if __name__ == '__main__':
    sys.exit(main())
