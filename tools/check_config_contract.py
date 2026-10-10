#!/usr/bin/env python3
"""Check that the code, edvr.ini and the log messages agree about setting names.

Three things have to line up, and nothing enforced it:

  1. the keys the code reads      -- cfg.getBool("fix.black_void", ...)
  2. the keys edvr.ini defines    -- [fix] / black_void = 1
  3. the keys the log names       -- "Pin it with exposure_shader under [advanced]"

Every one of these has been wrong in a shipped build:

  * transition_flash_units, _speed_factor and _max_consecutive were documented
    under [advanced] and read from [fix] for the whole of 0.5.x. The section is
    part of the key, so all three did nothing. Invisible, because the ini stated
    the defaults -- the values agreed until somebody edited one.
  * The exposure fix told users to pin a shader with fix.b1_exposure_cs, a name
    from the predecessor repo that nothing here reads.
  * panel_distance and vscreen_distance_scale drifted apart between the two
    repos, and an ini written for one silently did nothing in the other.

None of these fail a build, produce a warning, or look any different from a fix
that does not work. That is what this script is for.

Usage:  python tools/check_config_contract.py [--quiet] [--emit PATH [--dry-run]]
        python tools/check_config_contract.py --self-test
Exit:   0 all three agree, 1 they do not.

--emit writes a generated header (known keys + the moved-from map parsed
from edvr.ini's own annotations) for the runtime's config audit: the DLL
falls back to a moved key's old location when the new one is absent --
hand-copied DLLs meet old-layout inis all the time -- and names any key
it does not read. Emit never fails the build on contract problems (the
late check does); it fails only if the files cannot be read at all.
--dry-run reports what --emit would write and writes nothing.

--self-test runs the checks over fixtures laid out in the temp folder: every
kind of disagreement must be reported, and the shapes that look like one but
are not (a sentence that starts `word =`, a call the formatter wrapped, a
file name in a message) must not be. build.bat runs it before it trusts the
tool with the real tree.
"""

import os
import re
import sys
import json

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'src')
INI = os.path.join(ROOT, 'edvr.ini')

# cfg.getBool("fix.black_void", true) and friends, plus the early reader.
# getIntInRange and friends count as reads. A validated accessor is the
# preferred one, so a checker that only knew the bare gets would push new
# code back towards the unvalidated form to stay visible.
READ_RE = re.compile(r'get(?:Bool|Int|Float|String)[A-Za-z]*\s*\(\s*"([^"]+)"')
EARLY_RE = re.compile(r'readConfigStringEarly\s*\([^,]+,[^,]+,\s*"([^"]+)"', re.S)

# A section we know about followed by a dotted name, anywhere in a string. Used
# to catch key names mentioned in log text that nothing actually reads.
SECTIONS = ('fix', 'advanced', 'hotkey', 'log', 'd3d11',
            'experimental', 'luminance', 'menu')
# A dotted name INSIDE a string literal. The literals are pulled out first and
# searched separately, because scanning the raw line let a match start at a
# CLOSING quote and run on into the C++ after it: a line reading
# `remove(path, "removes EDVR's d3d11.dll", s.d3d11.sha256)` was reported as
# naming a setting called d3d11.sha256 in a message.
STRING_RE = re.compile(r'"(?:[^"\\]|\\.)*"')
MENTION_RE = re.compile(r'\b((?:%s)\.[a-z0-9_]+)' % '|'.join(SECTIONS))


def source_files():
    for base, _dirs, names in os.walk(SRC):
        for n in names:
            if n.endswith(('.cpp', '.h')):
                yield os.path.join(base, n)


def keys_read():
    """Keys the code actually asks Config for, as {key: [file:line, ...]}.

    Matched against the whole file, not line by line. A call the formatter
    wrapped across two lines is the same call, and a contract check whose answer
    depends on where an argument list breaks is worse than none -- the first time
    that happened it reported a live, correctly-read key as dead.
    """
    found = {}
    for path in source_files():
        rel = os.path.relpath(path, ROOT).replace('\\', '/')
        with open(path, encoding='utf-8', errors='replace') as f:
            text = f.read()
        for rx in (READ_RE, EARLY_RE):
            for m in rx.finditer(text):
                line = text.count('\n', 0, m.start()) + 1
                found.setdefault(m.group(1), []).append('%s:%d' % (rel, line))
    return found


def keys_mentioned():
    """Key-shaped names appearing inside string literals, {key: [file:line]}."""
    found = {}
    for path in source_files():
        rel = os.path.relpath(path, ROOT).replace('\\', '/')
        with open(path, encoding='utf-8', errors='replace') as f:
            for i, line in enumerate(f, 1):
                bare = line.strip()
                # Comments and includes are not instructions to a user. Only
                # text the program can actually print counts.
                if bare.startswith(('//', '*', '/*', '#include')):
                    continue
                # Skip the read itself; we only want prose mentions.
                stripped = READ_RE.sub('', EARLY_RE.sub('', line))
                mentioned = []
                for literal in STRING_RE.findall(stripped):
                    mentioned.extend(MENTION_RE.findall(literal))
                for key in mentioned:
                    # "d3d11.dll", "log.h", "settings-menu.md" -- a filename,
                    # not a setting.
                    if key.rsplit('.', 1)[-1] in ('dll', 'h', 'cpp', 'ini', 'txt', 'exe', 'md'):
                        continue
                    found.setdefault(key, []).append('%s:%d' % (rel, i))
    return found


def keys_documented(duplicate_sections=None):
    """Keys edvr.ini defines, as {key: line number}. Commented-out keys count as
    documented -- a template line is how a user learns the name.

    Each section header must appear exactly once: the shipped ini once grew
    three [fix] blocks and two [advanced] blocks by accretion, and a reader
    scanning for a key stopped at the first block and missed the rest. Repeats
    are collected into duplicate_sections (a list of (name, line)) when given.
    """
    found = {}
    section = ''
    seen_sections = {}
    if not os.path.exists(INI):
        return found
    with open(INI, encoding='utf-8', errors='replace') as f:
        for i, raw in enumerate(f, 1):
            line = raw.strip().lstrip('﻿')
            m = re.match(r'^\[([^\]]+)\]', line)
            if m:
                section = m.group(1).strip()
                if section in seen_sections and duplicate_sections is not None:
                    duplicate_sections.append((section, i, seen_sections[section]))
                seen_sections.setdefault(section, i)
                continue
            # "# key = value" counts, but "# some prose = here" must not, so a
            # commented line only counts when it looks exactly like a setting.
            body = line[1:].strip() if line[:1] in ('#', ';') else line
            m = re.match(r'^([A-Za-z0-9_]+)\s*=', body)
            if not m:
                continue
            if line[:1] in ('#', ';') and ' ' in body.split('=')[0].strip():
                continue
            key = m.group(1)
            found.setdefault('%s.%s' % (section, key) if section else key, i)
    return found


def keys_moved():
    """The ini's own migration map: '# moved-from: old.dotted' annotations,
    each naming where the NEXT key line used to live. {old_lower: new_dotted}.
    Mirrors the installer's movedKeys() (src/installer/iniedit.cpp) -- one
    source of truth, three consumers (this header, the installer merge, and
    a human reading the file)."""
    out = {}
    pending = []   # annotations STACK: several old names, one merged key
    section = ''
    if not os.path.exists(INI):
        return out
    with open(INI, encoding='utf-8', errors='replace') as f:
        for raw in f:
            line = raw.strip().lstrip('﻿')
            m = re.match(r'^\[([^\]]+)\]', line)
            if m:
                section = m.group(1).strip()
                pending = []
                continue
            if not line:
                pending = []
                continue
            if line[:1] in ('#', ';'):
                body = line.lstrip('#;').strip()
                if body.lower().startswith('moved-from:'):
                    spec = body[len('moved-from:'):].strip()
                    dm = re.match(r'^(\S+)\s*\(default\s+([^)]*)\)$', spec)
                    if dm:
                        pending.append((dm.group(1), dm.group(2).strip()))
                    else:
                        pending.append((spec, ''))
                    continue
                km = re.match(r'^([A-Za-z0-9_]+)\s*=', body)
                if km and ' ' not in body.split('=')[0].strip() and pending:
                    for p, d in pending:
                        out[p.lower()] = ('%s.%s' % (section, km.group(1)), d)
                    pending = []
                continue
            km = re.match(r'^([A-Za-z0-9_]+)\s*=', line)
            if km:
                for p, d in pending:
                    out[p.lower()] = ('%s.%s' % (section, km.group(1)), d)
            pending = []
    return out


def config_ownership_problems(keys, manifest_path=None):
    """Require one explicit manifest owner for every documented setting."""
    path = manifest_path or os.path.join(ROOT, 'src', 'plugins', 'plugin_manifest.json')
    try:
        with open(path, encoding='utf-8') as f:
            manifest = json.load(f)
    except (OSError, ValueError) as exc:
        return ['PLUGIN MANIFEST UNAVAILABLE: %s (%s)' % (path, exc)]
    if not isinstance(manifest, dict):
        return ['PLUGIN MANIFEST INVALID: expected a JSON object']

    groups = [('core', manifest.get('coreOwnedConfigKeys'))]
    plugins = manifest.get('plugins')
    if not isinstance(plugins, list):
        return ['PLUGIN MANIFEST INVALID: plugins must be an array']
    for plugin in plugins:
        if not isinstance(plugin, dict) or not isinstance(plugin.get('id'), str):
            return ['PLUGIN MANIFEST INVALID: every plugin must have a string id']
        groups.append((plugin['id'], plugin.get('ownedConfigKeys')))
    owners = {}
    problems = []
    for owner, owned in groups:
        if not isinstance(owned, list) or any(not isinstance(key, str) or not key for key in owned):
            problems.append('PLUGIN CONFIG OWNERSHIP INVALID: %s must list ownedConfigKeys' % owner)
            continue
        for key in owned:
            owners.setdefault(key.lower(), []).append((owner, key))
    canonical_keys = {key.lower(): key for key in keys}
    for folded, entries in sorted(owners.items()):
        if len(entries) > 1:
            problems.append('DUPLICATE CONFIG OWNER: %s (%s)' %
                            (entries[0][1], ', '.join(owner for owner, _key in entries)))
        if folded not in canonical_keys:
            problems.append('UNKNOWN CONFIG OWNER KEY: %s (owned by %s)' %
                            (entries[0][1], ', '.join(owner for owner, _key in entries)))
        elif any(key != canonical_keys[folded] for _owner, key in entries):
            problems.append('CONFIG OWNER KEY SPELLING DOES NOT MATCH: %s (expected %s)' %
                            (entries[0][1], canonical_keys[folded]))
    for key in sorted(keys):
        if key.lower() not in owners:
            problems.append('CONFIG KEY HAS NO OWNER: %s' % key)
    return problems


def emit_header(path, read, doc, moved):
    known = sorted(set(k.lower() for k in read) | set(k.lower() for k in doc))
    lines = [
        '// Generated by tools/check_config_contract.py -- do not edit.',
        '// Known settings (everything this build reads or documents) and the',
        '// moved-from map parsed from edvr.ini\'s own annotations. Consumed by',
        '// src/common/config.cpp for the runtime config audit.',
        '#pragma once',
        '',
        'namespace edvr { namespace contractgen {',
        '',
        'inline constexpr const char* kKnownKeys[] = {',
    ]
    lines += ['    "%s",' % k for k in known]
    lines += [
        '};',
        '',
        "// {old dotted name (lowercase), new dotted name, the old key's shipped",
        '// default -- a user line still carrying it is stale, not a choice}',
        'inline constexpr const char* kMovedKeys[][3] = {',
    ]
    for old in sorted(moved):
        lines.append('    {"%s", "%s", "%s"},' %
                     (old, moved[old][0].lower(), moved[old][1]))
    if not moved:
        lines.append('    {"", "", ""},  // none; a zero-length array is not C++')
    lines += ['};', '', '}}  // namespace edvr::contractgen', '']
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines))


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if '--self-test' in argv:
        return self_test()
    quiet = '--quiet' in argv
    dry_run = '--dry-run' in argv
    emit = None
    if '--emit' in argv:
        emit = argv[argv.index('--emit') + 1]
    read = keys_read()
    dup_sections = []
    doc = keys_documented(dup_sections)
    moved = keys_moved()
    if emit:
        if not dry_run:
            emit_header(emit, read, doc, moved)
        if not quiet:
            print('[edvr] config contract header: %d known keys, %d moves -> %s%s'
                  % (len(set(read) | set(doc)), len(moved), emit,
                     ' (dry run: not written)' if dry_run else ''))
        return 0
    mentioned = keys_mentioned()

    problems = []

    # The moved-from annotations are load-bearing three times over (installer
    # merge, runtime fallback, human reader), so they get their own checks: an
    # old name that is still live is ambiguous, and a move pointing at itself
    # is a typo.
    live = set(k.lower() for k in read) | set(k.lower() for k in doc)
    for old, (new, _oldDefault) in sorted(moved.items()):
        if old in live:
            problems.append(
                'MOVED-FROM NAMES A LIVE KEY: "%s" (annotated as moved to %s)\n'
                '    The old name is still read or documented, so the fallback\n'
                '    and the merge cannot tell which one the user meant.'
                % (old, new))
        if old == new.lower():
            problems.append('MOVED-FROM POINTS AT ITSELF: %s' % old)
        if new.lower() not in live:
            problems.append(
                'MOVED-FROM TARGET IS NOT A KNOWN KEY: %s -> %s' % (old, new))

    for name, line, first in dup_sections:
        problems.append(
            'SECTION [%s] APPEARS TWICE in edvr.ini (line %d; first at line %d)\n'
            '    One section per name: a reader scanning for a key stops at the\n'
            '    first block and misses everything in the second.'
            % (name, line, first))

    for key in sorted(read):
        if key not in doc:
            problems.append(
                'READ BUT NOT IN edvr.ini: %s\n'
                '    read at %s\n'
                '    Nobody can set it, because nothing tells them it exists.'
                % (key, ', '.join(read[key][:3])))

    for key in sorted(doc):
        if key not in read:
            section = key.split('.')[0] if '.' in key else ''
            bare = key.split('.')[-1]
            elsewhere = [k for k in read if k.split('.')[-1] == bare]
            hint = ''
            if elsewhere:
                hint = ('\n    The code reads %s. The section is part of the key, so '
                        'this line does nothing.' % ', '.join(sorted(elsewhere)))
            problems.append(
                'IN edvr.ini BUT NEVER READ: %s (line %d)%s' % (key, doc[key], hint))

    for key in sorted(mentioned):
        if key not in read:
            problems.append(
                'NAMED IN A MESSAGE BUT NEVER READ: %s\n'
                '    mentioned at %s\n'
                '    A user following that instruction is silently ignored.'
                % (key, ', '.join(mentioned[key][:3])))

    problems.extend(config_ownership_problems(set(doc) | set(read)))

    if problems:
        print('[edvr] config contract FAILED\n')
        for p in problems:
            print('  ' + p.replace('\n', '\n  ') + '\n')
        print('[edvr] %d problem(s). Every one of these is invisible at runtime:' % len(problems))
        print('       an unknown key is ignored and a missing one falls back to its')
        print('       default, which looks exactly like a fix that does not work.')
        return 1

    if not quiet:
        print('[edvr] config contract ok: %d keys read, %d documented, all agree'
              % (len(read), len(doc)))
    return 0


def self_test():
    """The three-way agreement, against fixtures laid out in the temp folder:
    each kind of disagreement must be reported by name, the shapes that look
    like one and are not must pass, and --emit must write what the runtime's
    audit reads (and write nothing under --dry-run). build.bat runs this
    before it trusts the tool with the real tree."""
    import contextlib
    import io
    import shutil
    import tempfile

    global ROOT, SRC, INI
    saved = (ROOT, SRC, INI)
    base = tempfile.mkdtemp(prefix='edvr-contract-')
    failures = []

    def lay(name, ini, sources):
        root = os.path.join(base, name)
        os.makedirs(os.path.join(root, 'src', 'plugins'))
        with open(os.path.join(root, 'edvr.ini'), 'w', encoding='utf-8') as f:
            f.write(ini)
        section = ''
        fixture_keys = []
        for raw in ini.splitlines():
            line = raw.strip().lstrip('\ufeff')
            m = re.match(r'^\[([^\]]+)\]', line)
            if m:
                section = m.group(1).strip()
                continue
            body = line[1:].strip() if line[:1] in ('#', ';') else line
            m = re.match(r'^([A-Za-z0-9_]+)\s*=', body)
            if m and not (line[:1] in ('#', ';') and ' ' in body.split('=')[0].strip()):
                fixture_keys.append('%s.%s' % (section, m.group(1)) if section else m.group(1))
        with open(os.path.join(root, 'src', 'plugins', 'plugin_manifest.json'), 'w', encoding='utf-8') as f:
            json.dump({'coreOwnedConfigKeys': fixture_keys, 'plugins': []}, f)
        for leaf, text in sources.items():
            with open(os.path.join(root, 'src', leaf), 'w', encoding='utf-8') as f:
                f.write(text)
        return root

    def point_at(root):
        global ROOT, SRC, INI
        ROOT, SRC, INI = root, os.path.join(root, 'src'), os.path.join(root, 'edvr.ini')

    def run(root, *argv):
        point_at(root)
        said = io.StringIO()
        with contextlib.redirect_stdout(said):
            code = main(list(argv))
        return code, said.getvalue()

    def expect(name, condition, what):
        if not condition:
            failures.append('%s: %s' % (name, what))

    def expect_in(name, text, needle):
        expect(name, needle in text, 'expected %r in\n%s' % (needle, text))

    try:
        # Everything agrees, including the look-alikes: a sentence that starts
        # `word =` is not a documented key, a call wrapped after its paren is
        # still one read, and a filename or a comment is not a message.
        ini = ('[fix]\n'
               'black_void = 1\n'
               '# some prose = here, which is a sentence and not a setting\n'
               '#early_key = 0\n'
               '\n'
               '[advanced]\n'
               '#max_mb = 4\n')
        src = {'a.cpp': ('bool a = cfg.getBool("fix.black_void", true);\n'
                         'int b = cfg.getIntInRange(\n'
                         '    "advanced.max_mb", 4, 1, 64);\n'
                         'auto e = readConfigStringEarly(path, section, "fix.early_key");\n'),
               'b.h': ('// fix.only_in_a_comment is not a message\n'
                       'log("pin it with advanced.max_mb; see d3d11.dll and settings-menu.md");\n')}
        name = 'agree'
        root = lay(name, ini, src)
        code, said = run(root)
        expect(name, code == 0, 'exit %d\n%s' % (code, said))
        expect_in(name, said, '3 keys read, 3 documented, all agree')
        expect(name, keys_read() == {'fix.black_void': ['src/a.cpp:1'],
                                     'advanced.max_mb': ['src/a.cpp:2'],
                                     'fix.early_key': ['src/a.cpp:4']},
               'keys_read() -> %r' % (keys_read(),))
        expect(name, keys_documented() == {'fix.black_void': 2, 'fix.early_key': 4,
                                           'advanced.max_mb': 7},
               'keys_documented() -> %r' % (keys_documented(),))
        code, said = run(root, '--quiet')
        expect(name, code == 0 and said == '', '--quiet said %r' % said)

        name = 'plugin-owner-missing'
        own = lay(name, '[fix]\nblack_void = 1\n',
                  {'a.cpp': 'cfg.getBool("fix.black_void", true);\n'})
        owner_path = os.path.join(own, 'src', 'plugins', 'plugin_manifest.json')
        with open(owner_path, encoding='utf-8') as f:
            owner_manifest = json.load(f)
        owner_manifest['coreOwnedConfigKeys'] = []
        with open(owner_path, 'w', encoding='utf-8') as f:
            json.dump(owner_manifest, f)
        code, said = run(own)
        expect(name, code == 1, 'exit %d' % code)
        expect_in(name, said, 'CONFIG KEY HAS NO OWNER: fix.black_void')

        name = 'plugin-owner-duplicate-and-unknown'
        own = lay(name, '[fix]\nblack_void = 1\n',
                  {'a.cpp': 'cfg.getBool("fix.black_void", true);\n'})
        owner_path = os.path.join(own, 'src', 'plugins', 'plugin_manifest.json')
        with open(owner_path, encoding='utf-8') as f:
            owner_manifest = json.load(f)
        owner_manifest['plugins'] = [{'id': 'fixture', 'ownedConfigKeys':
                                      ['fix.black_void', 'fix.unknown']}]
        with open(owner_path, 'w', encoding='utf-8') as f:
            json.dump(owner_manifest, f)
        code, said = run(own)
        expect(name, code == 1, 'exit %d' % code)
        expect_in(name, said, 'DUPLICATE CONFIG OWNER: fix.black_void')
        expect_in(name, said, 'UNKNOWN CONFIG OWNER KEY: fix.unknown')

        name = 'plugin-owner-case-mismatch'
        own = lay(name, '[fix]\nblack_void = 1\n',
                  {'a.cpp': 'cfg.getBool("fix.black_void", true);\n'})
        owner_path = os.path.join(own, 'src', 'plugins', 'plugin_manifest.json')
        with open(owner_path, encoding='utf-8') as f:
            owner_manifest = json.load(f)
        owner_manifest['coreOwnedConfigKeys'] = ['FIX.black_void']
        with open(owner_path, 'w', encoding='utf-8') as f:
            json.dump(owner_manifest, f)
        code, said = run(own)
        expect(name, code == 1, 'exit %d' % code)
        expect_in(name, said, 'CONFIG OWNER KEY SPELLING DOES NOT MATCH: FIX.black_void (expected fix.black_void)')

        # A byte-order mark on the ini's first line is not part of its header, for the
        # documented keys and for the moved-from map alike.
        name = 'bom'
        root = lay(name, '\ufeff[fix]\n# moved-from: fix.old_bom\nblack_void = 1\n',
                   {'a.cpp': 'bool a = cfg.getBool("fix.black_void", true);\n'})
        code, said = run(root)
        expect(name, code == 0, 'exit %d\n%s' % (code, said))
        expect(name, keys_moved() == {'fix.old_bom': ('fix.black_void', '')}, 'keys_moved() -> %r' % (keys_moved(),))

        # Read, but nobody can learn the name from the file.
        name = 'read-not-documented'
        rnd = lay(name, '[fix]\nblack_void = 1\n',
                  {'a.cpp': 'cfg.getBool("fix.black_void", true);\n'
                            'cfg.getFloat("fix.extra", 1.0f);\n'})
        code, said = run(rnd)
        expect(name, code == 1, 'exit %d' % code)
        expect_in(name, said, 'READ BUT NOT IN edvr.ini: fix.extra')
        expect_in(name, said, 'src/a.cpp:2')
        expect_in(name, said, '2 problem(s)')

        # The 0.5.x shape: documented under [fix], read from [advanced]. Both
        # halves are reported, and the second says where the code looks.
        name = 'wrong-section'
        code, said = run(lay(name, '[fix]\nthing = 1\n',
                             {'a.cpp': 'cfg.getInt("advanced.thing", 1);\n'}))
        expect(name, code == 1, 'exit %d' % code)
        expect_in(name, said, 'READ BUT NOT IN edvr.ini: advanced.thing')
        expect_in(name, said, 'IN edvr.ini BUT NEVER READ: fix.thing (line 2)')
        expect_in(name, said, 'The code reads advanced.thing. The section is part of the key')

        # A message that tells a user to set something nothing reads. A commented-out
        # line that has the same message in it is not a message anybody can print.
        name = 'named-in-a-message'
        code, said = run(lay(name, '[fix]\nreal = 1\n',
                             {'a.cpp': 'cfg.getBool("fix.real", true);\n'
                                       '// log("set fix.comment_ghost");\n'
                                       ' * log("set fix.block_comment_ghost");\n'
                                       'log("set fix.ghost_key to fix it");\n'}))
        expect(name, code == 1, 'exit %d' % code)
        expect_in(name, said, 'NAMED IN A MESSAGE BUT NEVER READ: fix.ghost_key')
        expect_in(name, said, 'src/a.cpp:4')
        expect(name, 'comment_ghost' not in said, 'a comment was reported as a message')

        # One header per section.
        name = 'duplicate-section'
        code, said = run(lay(name, '[fix]\na = 1\n[fix]\nb = 1\n',
                             {'a.cpp': 'cfg.getInt("fix.a", 1); cfg.getInt("fix.b", 1);\n'}))
        expect(name, code == 1, 'exit %d' % code)
        expect_in(name, said, 'SECTION [fix] APPEARS TWICE in edvr.ini (line 3; first at line 1)')

        # The moved-from map: the annotations stack onto the next key line, an
        # old name that is still a key is ambiguous, and one that points at
        # itself is a typo.
        moved_ini = ('[fix]\n'
                     '# moved-from: fix.old_a\n'
                     '# moved-from: advanced.old_b (default 2)\n'
                     'new_key = 1\n')
        moved_src = {'a.cpp': 'cfg.getInt("fix.new_key", 1);\n'}
        name = 'moved-from'
        root = lay(name, moved_ini, moved_src)
        code, said = run(root)
        expect(name, code == 0, 'exit %d\n%s' % (code, said))
        point_at(root)
        expect(name, keys_moved() == {'fix.old_a': ('fix.new_key', ''),
                                      'advanced.old_b': ('fix.new_key', '2')},
               'keys_moved() -> %r' % (keys_moved(),))
        # The annotation belongs to the NEXT key line, commented out or not, and to
        # no later one.
        name = 'moved-from-commented-key'
        root = lay(name, ('[fix]\n'
                          '# moved-from: fix.old_c\n'
                          '#new_c = 1\n'
                          '#other = 2\n'),
                   {'a.cpp': 'cfg.getInt("fix.new_c", 1); cfg.getInt("fix.other", 2);\n'})
        point_at(root)
        expect(name, keys_moved() == {'fix.old_c': ('fix.new_c', '')}, 'keys_moved() -> %r' % (keys_moved(),))
        # A blank line drops an annotation that has no key after it.
        name = 'moved-from-orphan'
        root = lay(name, ('[fix]\n'
                          '# moved-from: fix.old_d\n'
                          '\n'
                          'new_d = 1\n'),
                   {'a.cpp': 'cfg.getInt("fix.new_d", 1);\n'})
        point_at(root)
        expect(name, keys_moved() == {}, 'keys_moved() -> %r' % (keys_moved(),))
        name = 'moved-from-live-and-self'
        code, said = run(lay(name, ('[fix]\n'
                                    '# moved-from: fix.live_old\n'
                                    'new_key = 1\n'
                                    'live_old = 1\n'
                                    '# moved-from: fix.selfy\n'
                                    'selfy = 1\n'),
                             {'a.cpp': 'cfg.getInt("fix.new_key", 1);\n'
                                       'cfg.getInt("fix.live_old", 1);\n'
                                       'cfg.getInt("fix.selfy", 1);\n'}))
        expect(name, code == 1, 'exit %d' % code)
        expect_in(name, said, 'MOVED-FROM NAMES A LIVE KEY: "fix.live_old"')
        expect_in(name, said, 'MOVED-FROM POINTS AT ITSELF: fix.selfy')

        # --emit writes the audit header, never fails on contract problems, and
        # under --dry-run writes nothing at all.
        name = 'emit'
        out_dir = os.path.join(base, 'emit-out')
        os.makedirs(out_dir)
        header = os.path.join(out_dir, 'config_contract_gen.h')
        code, said = run(lay(name, moved_ini, moved_src), '--emit', header)
        expect(name, code == 0, 'exit %d\n%s' % (code, said))
        expect_in(name, said, '1 known keys, 2 moves')
        with open(header, encoding='utf-8') as f:
            text = f.read()
        expect_in(name, text, 'inline constexpr const char* kKnownKeys[] = {\n    "fix.new_key",\n};')
        expect_in(name, text, '    {"advanced.old_b", "fix.new_key", "2"},\n'
                              '    {"fix.old_a", "fix.new_key", ""},\n')
        name = 'emit-with-problems'
        os.remove(header)
        code, said = run(rnd, '--emit', header, '--quiet')
        expect(name, code == 0 and os.path.isfile(header) and said == '',
               'emit must not fail the build over contract problems: exit %d, %r' % (code, said))
        name = 'emit-dry-run'
        os.remove(header)
        code, said = run(rnd, '--emit', header, '--dry-run')
        expect(name, code == 0, 'exit %d\n%s' % (code, said))
        expect_in(name, said, '(dry run: not written)')
        expect(name, os.listdir(out_dir) == [], '--dry-run wrote %r' % os.listdir(out_dir))
        fresh = os.path.join(base, 'never-made', 'gen.h')
        code, said = run(rnd, '--emit', fresh, '--dry-run', '--quiet')
        expect(name, code == 0 and not os.path.exists(os.path.dirname(fresh)),
               '--dry-run created a directory')
    finally:
        ROOT, SRC, INI = saved
        shutil.rmtree(base, ignore_errors=True)

    if failures:
        print('check_config_contract: self-test FAILED')
        for f in failures:
            print('  ' + f.replace('\n', '\n    '))
        return 1
    print('check_config_contract: self-test OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
