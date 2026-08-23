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

Usage:  python tools/check_config_contract.py [--quiet]
Exit:   0 all three agree, 1 they do not.
"""

import os
import re
import sys

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
SECTIONS = ('fix', 'advanced', 'hotkey', 'log', 'openvr', 'd3d11',
            'experimental', 'luminance')
MENTION_RE = re.compile(r'"[^"]*\b((?:%s)\.[a-z0-9_]+)' % '|'.join(SECTIONS))


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
                for key in MENTION_RE.findall(stripped):
                    # "d3d11.dll", "log.h" -- a filename, not a setting.
                    if key.rsplit('.', 1)[-1] in ('dll', 'h', 'cpp', 'ini', 'txt', 'exe'):
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


def main():
    quiet = '--quiet' in sys.argv
    read = keys_read()
    dup_sections = []
    doc = keys_documented(dup_sections)
    mentioned = keys_mentioned()

    problems = []

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


if __name__ == '__main__':
    sys.exit(main())
