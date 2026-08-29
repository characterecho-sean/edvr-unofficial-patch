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

Only [fix] is exposed. [advanced] and [experimental] are safety valves and
developer instruments -- the log names one when it wants you to change it, and
that is the only way anybody should arrive at them -- and the remaining sections
are plumbing named after the halves of EDVR that read them. A ui: line outside
[fix] is an error, so the rule cannot drift by accident.

Commented-out expert settings inside [fix] are not required to have one.
Annotate one and it appears; leave it and it stays where it is, which is the
right default for a developer instrument.

Usage:
  python tools/gen_settings_schema.py --root <repo> --out <gen dir>
  python tools/gen_settings_schema.py --root <repo> --check    (no output written)
"""

import argparse
import os
import re
import sys

# The window shows [fix] and nothing else.
#
# [advanced] and [experimental] are safety valves and developer instruments --
# the log names one when it wants you to change it, and that is the only way
# anybody should arrive at them. Offering them in a list invites changing things
# nobody asked you to change, and turns a support thread into a guessing game.
# The remaining sections ([hotkey], [log], [openvr], [d3d11]) are plumbing named
# after the halves of EDVR that read them, not fixes somebody came here to turn
# on. Explorer Cam's own switches live in [fix] and are exposed; the metre
# offsets it is tuned with are not, because tuning them means wearing the
# headset and watching, which is what the ini's hot reload is for.
EXPOSED_SECTIONS = ('fix',)

READ_RE = re.compile(
    r'get(Bool|Int|Float|String)([A-Za-z]*)\s*\(\s*"([^"]+)"\s*,\s*([^;]*?)\)', re.S)
KEY_RE = re.compile(r'^([A-Za-z0-9_.-]+)\s*=\s*(.*)$')
UI_RE = re.compile(r'^ui\s*:\s*(.*)$', re.I)
# A heading in edvr.ini: a rule, the title, a rule. The heading a person
# reads in the file is the heading the window shows, so there is no second
# list of group names to keep in step with this one.
RULE_RE = re.compile(r'^-{10,}$')


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
        self.applies = None   # 'live' or 'restart'
        self.group = ''
        self.percent = False
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
    group = ''
    expectTitle = None
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
            group = ''
            prose = []
            annotation = None
            continue

        commented = line[0] in '#;'
        body = line.lstrip('#;').strip() if commented else line
        m = KEY_RE.match(body)
        is_key = bool(m) and (not commented or ' ' not in m.group(1))

        if commented and not is_key:
            # A heading is rule / title / rule. None means "not in one", True
            # means "opening rule seen, the next comment is the title", False
            # means "title taken, waiting for the closing rule". Toggling a
            # single flag instead re-armed on the closing rule and ate the
            # first line of the next setting's explanation as a heading.
            if RULE_RE.match(body):
                expectTitle = True if expectTitle is None else None
                continue
            if expectTitle:
                group = body
                expectTitle = False   # the closing rule is still to come
                continue
            ui = UI_RE.match(body)
            if ui:
                annotation = ui.group(1).strip()
            elif not body.startswith('moved-from:'):
                # Migration metadata for the contract tooling, not prose:
                # without this the settings window's tooltips end in a list
                # of retired key names.
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
        s.group = group
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
        elif lower == 'percent':
            # The file holds a fraction because that is what the shader wants;
            # the window shows a percentage because that is what the number
            # means. panel_curvature = 0.3 is thirty percent of a full circle,
            # and reading it as "0.3 of something" is a puzzle nobody should
            # have to solve in a settings list.
            setting.percent = True
        elif lower == 'restart':
            setting.applies = 'restart'
        elif lower == 'live':
            setting.applies = 'live'


def when_it_applies(setting):
    """live or restart, read out of the prose the setting already carries.

    edvr.ini has said "Live." or "Needs a game restart" at the end of a comment
    block since long before there was a window to show it in, so that is where
    this comes from rather than from a fourth thing to keep in step. The
    annotation can say `| live` or `| restart` where the prose does not, and
    wins where both do.
    """
    if setting.applies:
        return setting.applies
    text = setting.description.lower()
    if 'restart' in text:
        return 'restart'
    if re.search(r'\blive\b', text):
        return 'live'
    return None


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


def looks_like_a_switch(lo, hi):
    """Bounds a reader would take for two states: whole numbers, one apart."""
    if not lo or not hi:
        return False
    try:
        low, high = float(lo), float(hi)
    except ValueError:
        return False
    return low == int(low) and high == int(high) and (high - low) <= 1.0


def decimal(text):
    """A bound written so it cannot be mistaken for a switch.

    Bounds are read out of prose and out of accessor calls, where "0..1" is a
    perfectly ordinary way to write the range of a fraction. In a window, next
    to a box you type into, "0 to 1" reads as two states -- so a setting the
    game treats as continuous shows continuous bounds.
    """
    if not text:
        return text
    return text if '.' in text else text + '.0'


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
    stray = [s for s in settings if s.annotated and s.section not in EXPOSED_SECTIONS]
    if stray:
        print('gen_settings_schema: ERROR: only [%s] settings appear in the window.'
              % ']/['.join(EXPOSED_SECTIONS))
        print()
        print('[advanced] and [experimental] are safety valves and developer instruments:')
        print('a window that offers them invites people to change things the log is')
        print('supposed to send them to, and turns a support thread into a guessing game.')
        print('The other sections are plumbing rather than fixes. Remove the ui: line:')
        print()
        for s in stray:
            print('  edvr.ini:%d  %s.%s' % (s.line, s.section, s.key))
        return 1

    missing = [s for s in settings
               if s.live and s.section in EXPOSED_SECTIONS and not s.annotated]
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
               if s.annotated and not s.hidden and s.section in EXPOSED_SECTIONS
               and '%s.%s' % (s.section, s.key) not in code]
    if unknown:
        for s in unknown:
            print('gen_settings_schema: ERROR: %s.%s is in the settings window but nothing '
                  'in src/ reads it.' % (s.section, s.key))
        return 1

    exposed = [s for s in settings
               if s.annotated and not s.hidden and s.section in EXPOSED_SECTIONS]

    # A whole-number setting bounded 0 to 1 has two states, and a text box is
    # the wrong way to offer two states. Either it should be read with getBool
    # -- which makes it a switch here automatically -- or its values mean
    # something a switch cannot say, and it wants `choices 1=..., 0=...`.
    disguised = []
    for s in exposed:
        dotted = '%s.%s' % (s.section, s.key)
        if dotted not in code or s.choices:
            continue
        kind, _default, lo, hi, precision = code[dotted]
        if kind == 'number' and precision == 0 and (lo, hi) == ('0', '1'):
            disguised.append(s)
    if disguised:
        print('gen_settings_schema: ERROR: %d setting(s) are a switch wearing a text box.'
              % len(disguised))
        print()
        print('A whole number bounded 0 to 1 has two states. Read it with getBool so the')
        print('window shows a switch, or say what the numbers mean with')
        print('`choices 1=on, 0=off` on the ui: line.')
        print()
        for s in disguised:
            print('  edvr.ini:%d  %s.%s' % (s.line, s.section, s.key))
        return 1

    silent = [s for s in exposed if when_it_applies(s) is None]
    if silent:
        print('gen_settings_schema: ERROR: %d setting(s) do not say when they take effect.'
              % len(silent))
        print()
        print('Somebody who changes a setting and sees nothing happen has no way to tell')
        print('a fix that needs a game restart from one that is simply not working. End')
        print('the comment block with "Live." or with the sentence that says a restart is')
        print('needed -- or put `| live` or `| restart` on the ui: line.')
        print()
        for s in silent:
            print('  edvr.ini:%d  %s.%s' % (s.line, s.section, s.key))
        return 1

    if args.check or not args.out:
        restarts = len([s for s in exposed if when_it_applies(s) == 'restart'])
        print('gen_settings_schema: %d exposed, %d of them needing a game restart; '
              '%d live in [%s]'
              % (len(exposed), restarts,
                 len([s for s in settings if s.live and s.section in EXPOSED_SECTIONS]),
                 ']/['.join(EXPOSED_SECTIONS)))
        return 0

    rows = []
    for s in exposed:
        dotted = '%s.%s' % (s.section, s.key)
        kind, default, lo, hi, precision = code[dotted]
        # A range the code declares wins: it is the one that is enforced.
        if lo is None and s.range_lo is not None:
            lo, hi = s.range_lo, s.range_hi

        # "0 to 1" beside a text box reads as on and off, and the settings
        # bounded that way are continuous -- 0.3 is the value that matters on a
        # curve. Their bounds say so.
        #
        # Only for that case, though: a line angle bounded 0 to 60 is not going
        # to be mistaken for a switch, and "0.0 to 60.0" would be decimals
        # nobody needs on a whole number of degrees.
        if precision > 0 and looks_like_a_switch(lo, hi):
            lo = decimal(lo)
            hi = decimal(hi)
        if s.choices:
            kind = 'choice'
        # The ini's own value is the shipped default, and it is the one the
        # user sees; the code's default only matters when the key is absent.
        shipped = s.value
        recommended = s.recommended if s.recommended is not None else shipped
        rows.append(
            '    {%s, %s, %s, %s, %s,\n     SettingKind::%s, %s, %s,\n'
            '     %s, %s, %d, %s, %s, %s, %s, %s},' % (
                c_string(s.section), c_string(s.key), c_string(s.label),
                c_string(summarise(s.description)), c_string(s.description),
                {'toggle': 'Toggle', 'number': 'Number', 'text': 'Text',
                 'choice': 'Choice'}[kind],
                c_string(shipped), c_string(recommended),
                c_string(lo or ''), c_string(hi or ''), precision,
                c_string('|'.join(s.choices)),
                'true' if s.live else 'false',
                'true' if when_it_applies(s) == 'restart' else 'false',
                'true' if s.percent else 'false',
                c_string(s.group)))

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
