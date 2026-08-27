#!/usr/bin/env python3
"""Generate the installer's settings schema, and enforce that it stays complete.

WHY THIS EXISTS

A settings window needs, for every setting: a type, a range, a default, the
choices where there are any, a short label and an explanation. All of that
already exists in this repository, in two places -- the accessor call in the
code says the type, the range and the default; the comment block above the key
in edvr.ini says what it does. Writing it down a THIRD time in C++ would create
another list to forget to update, which is exactly the failure
tools/check_config_contract.py exists to catch between the other two.

So the schema is generated from those two sources, and the only thing added by
hand is the part neither of them can know: what to call the setting in a list,
and which value is RECOMMENDED (often the shipped default, sometimes not -- a
0.3 curve with a 0.7 distance is a tested pairing, and neither number is the
default). That lives in edvr.ini too, on one annotation line above the key:

    # ui: On-foot screen curve | recommended 0.3
    panel_curvature = 0.0

    # ui: Sun glare | choices vivid, realistic, stock
    sun_glare = vivid

    # ui: hidden -- the installer manages this one
    real_dll =

THE ENFORCEMENT

Every setting that is LIVE in edvr.ini -- uncommented, which is what promoting a
fix to shipped-on looks like -- must have one of those lines. A fix that gets
promoted and does not appear in the settings window is invisible to everybody
who does not read ini files, and nothing else in the build would notice: the
game reads it, the log names it, and the window that is supposed to expose it
simply does not. That is a build failure here.

Commented-out expert settings are not required to have one. Annotate them and
they appear in the window under "expert"; leave them and they stay where they
are, which is the right default for a developer instrument.

Usage:
  python tools/gen_settings_schema.py --root <repo> --out <gen dir>
  python tools/gen_settings_schema.py --root <repo> --check    (no output written)
"""

import argparse
import os
import re
import sys

READ_RE = re.compile(
    r'get(Bool|Int|Float|String)([A-Za-z]*)\s*\(\s*"([^"]+)"\s*,\s*([^;]*?)\)', re.S)
KEY_RE = re.compile(r'^([A-Za-z0-9_.-]+)\s*=\s*(.*)$')
UI_RE = re.compile(r'^ui\s*:\s*(.*)$', re.I)


def source_files(src):
    for base, _dirs, names in os.walk(src):
        for n in names:
            if n.endswith(('.cpp', '.h')):
                yield os.path.join(base, n)


def readers(src):
    """dotted key -> (kind, default, lo, hi) as the code asks for it."""
    found = {}
    for path in source_files(src):
        with open(path, encoding='utf-8', errors='replace') as f:
            text = f.read()
        for m in READ_RE.finditer(text):
            base, suffix, key, args = m.group(1), m.group(2), m.group(3), m.group(4)
            parts = [a.strip() for a in split_args(args)]
            lo = hi = None
            if 'InRange' in suffix and len(parts) >= 3:
                lo, hi = parts[1], parts[2]
            default = parts[0] if parts else ''
            kind = {'Bool': 'toggle', 'Int': 'number', 'Float': 'number',
                    'String': 'text'}[base]
            precision = 0 if base in ('Bool', 'Int') else 2
            # First reader wins; a key read in two places reads the same way.
            found.setdefault(key, (kind, default, lo, hi, precision))
    return found


def split_args(text):
    """Split a C++ argument list on top-level commas."""
    out, depth, current = [], 0, ''
    for ch in text:
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(current)
            current = ''
        else:
            current += ch
    if current.strip():
        out.append(current)
    return out


class Setting(object):
    def __init__(self):
        self.section = ''
        self.key = ''
        self.value = ''
        self.live = False
        self.description = ''
        self.label = ''
        self.recommended = None
        self.choices = []
        self.restart = False
        self.range_lo = None
        self.range_hi = None
        self.hidden = False
        self.annotated = False
        self.line = 0


def parse_ini(path):
    """Every setting in edvr.ini, live or commented, with its prose and its
    ui: annotation."""
    with open(path, encoding='utf-8', errors='replace') as f:
        lines = f.read().splitlines()

    settings = []
    section = ''
    prose = []          # comment lines since the last key or blank run
    annotation = None

    for index, raw in enumerate(lines):
        line = raw.strip()
        if not line:
            prose = []
            annotation = None
            continue
        if line.startswith('['):
            close = line.find(']')
            if close > 0:
                section = line[1:close]
            prose = []
            annotation = None
            continue

        commented = line[0] in '#;'
        body = line.lstrip('#;').strip() if commented else line
        m = KEY_RE.match(body)
        is_key = bool(m) and (not commented or ' ' not in m.group(1))

        if commented and not is_key:
            ui = UI_RE.match(body)
            if ui:
                annotation = ui.group(1).strip()
            else:
                prose.append(body)
            continue

        if not m:
            continue

        s = Setting()
        s.section = section
        s.key = m.group(1)
        s.value = strip_inline_comment(m.group(2)).strip()
        s.live = not commented
        s.description = ' '.join(prose).strip()
        if not s.description and settings and settings[-1].section == section:
            # edvr.ini documents some settings as a group -- vscreen_res_width
            # and _height, the three head-offset axes -- with one comment block
            # above the first. The others are not undocumented; they share it.
            s.description = settings[-1].description
        s.line = index + 1
        if annotation is not None:
            s.annotated = True
            apply_annotation(s, annotation)
        settings.append(s)
        prose = []
        annotation = None
    return settings


def strip_inline_comment(value):
    for i in range(1, len(value)):
        if value[i] in ';#' and value[i - 1] in ' \t':
            return value[:i]
    return value


def apply_annotation(setting, text):
    parts = [p.strip() for p in text.split('|')]
    head = parts[0]
    if head.lower().startswith('hidden'):
        setting.hidden = True
        setting.label = setting.key
    else:
        setting.label = head
    for part in parts[1:]:
        lower = part.lower()
        if lower.startswith('recommended'):
            setting.recommended = part.split(None, 1)[1].strip() if ' ' in part else ''
        elif lower.startswith('choices'):
            # "choices vivid, realistic, stock" or, where the value is a number
            # that means something, "choices 1=on, 2=both eyes, 0=off": the
            # window shows the label and writes the value.
            rest = part.split(None, 1)[1] if ' ' in part else ''
            setting.choices = [c.strip() for c in rest.split(',') if c.strip()]
        elif lower.startswith('range'):
            # "range 1..600" -- for the settings whose bounds are documented in
            # prose rather than declared by a getIntInRange call. Shown beside
            # the value, because a number box with no bounds is a guess.
            rest = part.split(None, 1)[1] if ' ' in part else ''
            bounds = rest.replace(' to ', '..').split('..')
            if len(bounds) == 2:
                setting.range_lo = bounds[0].strip()
                setting.range_hi = bounds[1].strip()
        elif lower == 'restart':
            setting.restart = True


def summarise(description):
    """The first sentence, for a row in a list.

    The prose in edvr.ini is written to be read in the file: three to eight
    lines, with the measurements and the caveats. A settings row has space for
    about one sentence, and a sentence cut off mid-clause reads worse than a
    short one -- so the summary ends where the author ended a sentence, not
    where the rectangle ran out.
    """
    text = description.strip()
    if not text:
        return ''
    # A dash clause counts as the end too: several of these settings open with
    # a plain sentence and then qualify it at length after " -- ", and the
    # qualification is exactly the part a list has no room for.
    cuts = []
    for end in ('. ', '! ', '? '):
        cut = text.find(end)
        if cut != -1:
            cuts.append((cut, cut + 1))
    dash = text.find(' -- ')
    if dash != -1:
        cuts.append((dash, dash + 1))
    cuts = [c for c in cuts if c[0] < 200]
    if cuts:
        first = min(cuts)
        summary = text[:first[1]].rstrip()
        # A cut inside a bracket leaves it hanging open, which reads as a typo.
        if summary.count('(') > summary.count(')'):
            summary = summary[:summary.rfind('(')].rstrip().rstrip(',')
        return summary if summary.endswith(('.', '!', '?')) else summary + '.'
    if len(text) <= 200:
        return text
    return text[:197].rsplit(' ', 1)[0] + '...'


def c_string(text):
    out = text.replace('\\', '\\\\').replace('"', '\\"')
    return '"%s"' % out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    ap.add_argument('--out')
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()

    ini_path = os.path.join(args.root, 'edvr.ini')
    code = readers(os.path.join(args.root, 'src'))
    settings = parse_ini(ini_path)

    # ---- the enforcement --------------------------------------------------
    missing = [s for s in settings if s.live and not s.annotated]
    if missing:
        print('gen_settings_schema: ERROR: %d live setting(s) are not in the settings window.'
              % len(missing))
        print()
        print('A setting that is uncommented in edvr.ini is one this build ships ON, and it')
        print('has to be reachable by somebody who does not edit ini files. Add one line')
        print('directly above the key:')
        print()
        print('    # ui: Short label | recommended <value>')
        print('    # ui: Short label | choices a, b, c')
        print('    # ui: hidden -- <why this one is not for the window>')
        print()
        for s in missing:
            print('  edvr.ini:%d  %s.%s' % (s.line, s.section, s.key))
        return 1

    unknown = [s for s in settings
               if s.annotated and not s.hidden
               and '%s.%s' % (s.section, s.key) not in code]
    if unknown:
        for s in unknown:
            print('gen_settings_schema: ERROR: %s.%s is in the settings window but nothing '
                  'in src/ reads it.' % (s.section, s.key))
        return 1

    exposed = [s for s in settings if s.annotated and not s.hidden]

    if args.check or not args.out:
        print('gen_settings_schema: %d settings exposed, %d live, %d hidden by request'
              % (len(exposed), len([s for s in settings if s.live]),
                 len([s for s in settings if s.hidden])))
        return 0

    rows = []
    for s in exposed:
        dotted = '%s.%s' % (s.section, s.key)
        kind, default, lo, hi, precision = code[dotted]
        # A range the code declares wins: it is the one that is enforced.
        if lo is None and s.range_lo is not None:
            lo, hi = s.range_lo, s.range_hi
        if s.choices:
            kind = 'choice'
        # The ini's own value is the shipped default, and it is the one the
        # user sees; the code's default only matters when the key is absent.
        shipped = s.value
        recommended = s.recommended if s.recommended is not None else shipped
        rows.append(
            '    {%s, %s, %s, %s, %s,\n     SettingKind::%s, %s, %s,\n     %s, %s, %d, %s, %s},' % (
                c_string(s.section), c_string(s.key), c_string(s.label),
                c_string(summarise(s.description)), c_string(s.description),
                {'toggle': 'Toggle', 'number': 'Number', 'text': 'Text',
                 'choice': 'Choice'}[kind],
                c_string(shipped), c_string(recommended),
                c_string(lo or ''), c_string(hi or ''), precision,
                c_string('|'.join(s.choices)),
                'true' if s.live else 'false'))

    os.makedirs(args.out, exist_ok=True)
    out_path = os.path.join(args.out, 'settings_schema.inc')
    with open(out_path, 'w', encoding='utf-8', newline='\r\n') as f:
        f.write('// Generated by tools/gen_settings_schema.py from edvr.ini and src/.\n')
        f.write('// Do not edit, and do not commit: the sources are the ini and the code.\n')
        f.write('static const SettingDef kSettings[] = {\n')
        f.write('\n'.join(rows))
        f.write('\n};\n')
    print('gen_settings_schema: wrote %s (%d settings)' % (out_path, len(rows)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
