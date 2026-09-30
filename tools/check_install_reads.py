#!/usr/bin/env python3
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

`--self-test` runs the check over C++ fixtures in the temp folder: the reader
called on both paths, only on reload (the shipped bug), never on reload, never
at all, the early-return guard form, a declaration that is not a call, and a
file that moved. A scanner that drifts from the shape it reads fails in the
build, not in the ten minutes after a flight reproduced the effect.
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
#
# Until 2026-09-16 every reader here named src/openvr/compositor_hook.cpp; that
# file went with the legacy OpenVR proxy. The d3d11 half's readers with the
# same shape -- called from the reload poll in vscreen.cpp AND from its install
# path -- take their place.
READERS = [
    # The head-offset gate (fix.head_offset_gate): the very reader whose
    # reload-only call site shipped the do-nothing release described above,
    # now on the d3d11 side.
    ('headOffsetGateConfigure', os.path.join('src', 'd3d11', 'vscreen.cpp')),
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
        # Definitions and declarations are not calls. A call may pass the
        # config object (headOffsetGateConfigure(cfg)); a declaration's
        # parameter list has a type in it, which a name-only argument does not.
        calls = [m.start() for m in re.finditer(
            r'(?<![\w:])%s\s*\(\s*(?:[A-Za-z_]\w*)?\s*\)\s*;' % reader, text)]
        if not calls:
            print('  FAIL  %s() is never called in %s' % (reader, rel))
            bad += 1
            continue
        spans = reload_spans(text)
        outside = [c for c in calls
                   if not any(a <= c <= b for a, b in spans)]
        line_of = lambda p: text.count('\n', 0, p) + 1
        # The docstring says "at least twice, and at least one outside" -- and
        # only the second half was enforced, so deleting the RELOAD-path call
        # (the whole live-tuning loop) passed silently. A checker that enforces
        # less than it claims is the same defect it exists to catch.
        inside = [c for c in calls if any(a <= c <= b for a, b in spans)]
        if not inside:
            print('  FAIL  %s() is never called from the config-reload path in %s'
                  % (reader, rel))
            print('        Its settings would then be startup-only, and the ini')
            print('        advertises them as live. Call it from both.')
            bad += 1
            continue
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


def self_test():
    """The check over fixtures laid out in the temp folder. Each one is a
    shape the real file's call sites could take; the check must pass exactly
    the ones with a call on the install path AND a call on the reload path."""
    import contextlib
    import io
    import shutil
    import tempfile

    global ROOT, READERS
    saved = (ROOT, READERS)
    base = tempfile.mkdtemp(prefix='edvr-install-reads-')
    failures = []

    def run(name, text):
        """The check's verdict on one file (None = the file is absent)."""
        global ROOT, READERS
        root = os.path.join(base, name)
        os.makedirs(os.path.join(root, 'src'))
        if text is not None:
            with open(os.path.join(root, 'src', 'hook.cpp'), 'w', encoding='utf-8') as f:
                f.write(text)
        ROOT, READERS = root, [('readThing', os.path.join('src', 'hook.cpp'))]
        said = io.StringIO()
        with contextlib.redirect_stdout(said):
            code = main()
        return code, said.getvalue()

    def expect(name, code, said, want_code, *needles):
        if code != want_code:
            failures.append('%s: exit %d, expected %d\n%s' % (name, code, want_code, said))
            return
        for needle in needles:
            if needle not in said:
                failures.append('%s: expected %r in\n%s' % (name, needle, said))

    definition = 'void readThing(const Config& cfg) { g_thing = cfg.getInt("fix.thing", 1); }\n'

    try:
        # Called where the hook is installed, and again from the reload block:
        # the only shape that is both live-tunable and applied at startup.
        code, said = run('both', definition +
                         'void install() { readThing(cfg); }\n'
                         'void poll() {\n'
                         '    if (cfg.reloadIfChanged()) {\n'
                         '        readThing(cfg);\n'
                         '    }\n'
                         '}\n')
        expect('both', code, said, 0, 'readThing()', '2 call(s), 1 on the install path (line 2)',
               'INSTALL READ CHECK PASSED')

        # The shipped bug: the only call is inside the reload block, which
        # returns early unless the ini's write time moved.
        code, said = run('reload-only', definition +
                         'void poll() {\n'
                         '    if (cfg.reloadIfChanged()) {\n'
                         '        readThing(cfg);\n'
                         '    }\n'
                         '}\n')
        expect('reload-only', code, said, 1, 'is called only from the config-reload path',
               '(line(s) 4, all inside reloadIfChanged)', 'INSTALL READ CHECK FAILED (1)')

        # The other half the docstring claims: deleting the reload call is
        # startup-only, and the ini advertises the settings as live.
        code, said = run('install-only', definition + 'void install() { readThing(cfg); }\n')
        expect('install-only', code, said, 1, 'is never called from the config-reload path')

        # A definition and a declaration are not calls.
        code, said = run('never-called', definition + 'void readThing(Config& cfg);\n')
        expect('never-called', code, said, 1, 'readThing() is never called in ')

        # The early-return form guards the rest of its function: that call is
        # reload-only, and the install path's own call is what saves it.
        guarded = (definition +
                   'void poll() {\n'
                   '    if (!cfg.reloadIfChanged()) return;\n'
                   '    readThing(cfg);\n'
                   '}\n')
        code, said = run('guarded-only', guarded)
        expect('guarded-only', code, said, 1, 'is called only from the config-reload path')
        code, said = run('guarded-and-install', guarded + 'void install() { readThing(cfg); }\n')
        expect('guarded-and-install', code, said, 0, '2 call(s), 1 on the install path (line 6)')

        # A call with no argument, and one in a nested block on the install
        # path, are calls; a longer name that merely ends in the reader's is not.
        code, said = run('other-callers', definition +
                         'void install() { if (x) { readThing(); } }\n'
                         'void poll() { if (cfg.reloadIfChanged()) { prereadThing(cfg); } }\n')
        expect('other-callers', code, said, 1, 'is never called from the config-reload path')

        # The file moved: say so, do not pass.
        code, said = run('moved', None)
        expect('moved', code, said, 1, 'src' + os.sep + 'hook.cpp does not exist')
    finally:
        ROOT, READERS = saved
        shutil.rmtree(base, ignore_errors=True)

    if failures:
        print('check_install_reads: self-test FAILED')
        for f in failures:
            print('  ' + f.replace('\n', '\n    '))
        return 1
    print('check_install_reads: self-test OK')
    return 0


if __name__ == '__main__':
    sys.exit(self_test() if '--self-test' in sys.argv[1:] else main())
