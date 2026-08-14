#!/usr/bin/env python3
# GENERATED from tools/check_install_reads.py in the private edvr repo -- do not edit here.
# Edit there, then: python tools/sync_common.py --write   [body-sha256 6d8a4649205fa661]
"""Config readers must be called on the INSTALL path, not only on reload.

WHY THIS EXISTS

A shipped release of the head offset did nothing at all. `readHeadOffset()`'s
only call site in the public repo was inside the once-a-second reload poll,
behind `Config::reloadIfChanged()` -- which returns early unless the ini's write
time has moved. So the offsets kept their zero initialisers for the entire
session: the feature was configured, the gate armed, the log printed "head
offset ON", and the viewpoint never moved. The bug produced no error, no wrong
pixel and no log line.

That is the third time this class has cost a flight. The first two were on the
d3d11 side and produced tools/check_config_paths.py, which compares the KEYS
read by the two config functions in context_hook.cpp. This one slipped past
because it is a whole reader, in a different file, in the other repo -- and
because compositor_hook.cpp is FORKED, so no sync check could see the missing
line either.

WHAT IT CHECKS

For each reader below, in each file: it must be called at least twice, and at
least one of those calls must be OUTSIDE the reload block. "Outside" is judged
by brace depth relative to the nearest enclosing `reloadIfChanged` -- a call
that only ever appears inside one is reload-only by definition.

It runs in BOTH repos' builds, over their own copies, because the failure was
that the two copies differed.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# reader -> file it must be installed from, relative to the repo root.
#
# A reader is listed here once it is the sole source of a setting that changes
# what the player sees. The cost of being wrong is a feature that silently does
# nothing, which is the hardest failure to attribute from a log.
READERS = [
    ('headOffsetConfigure', os.path.join('src', 'openvr', 'compositor_hook.cpp')),
]


def reload_spans(text):
    """(start, end) character ranges of every `if (...reloadIfChanged...) { }`."""
    spans = []
    for m in re.finditer(r'reloadIfChanged\s*\(\s*\)', text):
        # Walk back to the statement's `if`, then forward over its block. An
        # early-return form -- `if (!cfg.reloadIfChanged()) return;` -- guards
        # everything after it in that function, so the whole rest of the
        # function counts as inside.
        line_start = text.rfind('\n', 0, m.start()) + 1
        line_end = text.find('\n', m.end())
        line = text[line_start:line_end]
        if 'return' in line:
            # Guarded-return form: everything to the end of the enclosing
            # function is reload-only.
            depth, k = 0, line_start
            while k > 0 and depth <= 0:
                if text[k] == '}':
                    depth -= 1
                elif text[k] == '{':
                    depth += 1
                    if depth == 1:
                        break
                k -= 1
            start = k
            d, j = 0, k
            while j < len(text):
                if text[j] == '{':
                    d += 1
                elif text[j] == '}':
                    d -= 1
                    if d == 0:
                        break
                j += 1
            spans.append((line_start, j))
            continue
        brace = text.find('{', m.end())
        if brace < 0:
            continue
        d, j = 0, brace
        while j < len(text):
            if text[j] == '{':
                d += 1
            elif text[j] == '}':
                d -= 1
                if d == 0:
                    break
            j += 1
        spans.append((brace, j))
    return spans


def main():
    bad = 0
    for reader, rel in READERS:
        path = os.path.join(ROOT, rel)
        if not os.path.isfile(path):
            print('  FAIL  %s does not exist -- has it moved?' % rel)
            bad += 1
            continue
        text = open(path, encoding='utf-8', errors='replace').read()
        # Definitions and declarations are not calls.
        calls = [m.start() for m in re.finditer(r'(?<![\w:])%s\s*\(\s*\)\s*;' % reader,
                                                text)]
        if not calls:
            print('  FAIL  %s() is never called in %s' % (reader, rel))
            bad += 1
            continue
        spans = reload_spans(text)
        outside = [c for c in calls
                   if not any(a <= c <= b for a, b in spans)]
        line_of = lambda p: text.count('\n', 0, p) + 1
        if not outside:
            print('  FAIL  %s() is called only from the config-reload path in %s'
                  % (reader, rel))
            print('        (line(s) %s, all inside reloadIfChanged)'
                  % ', '.join(str(line_of(c)) for c in calls))
            print('        reloadIfChanged() returns early unless the ini\'s write')
            print('        time moved, so every value this reader owns keeps its')
            print('        C++ initialiser for the whole session. The feature is')
            print('        configured, logs as running, and does nothing.')
            print('        Call it once where the hook is installed as well.')
            bad += 1
            continue
        print('  %-24s %d call(s), %d on the install path (line %s)'
              % (reader + '()', len(calls), len(outside),
                 ', '.join(str(line_of(c)) for c in outside)))

    if bad:
        print('INSTALL READ CHECK FAILED (%d)' % bad)
        return 1
    print('  ok    every config reader runs at install, not only on reload')
    print('INSTALL READ CHECK PASSED')
    return 0


if __name__ == '__main__':
    sys.exit(main())
