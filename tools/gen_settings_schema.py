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

So the schema is generated from those two sources. A key read in several places
takes its type from the TYPED read -- getBool, getInt, getFloat -- and a
getString read of the same key beside one is the raw text for a log line or a
status echo, never a second opinion; two typed reads that disagree fail the
build (readers() says why). The only thing added by hand is the part neither
source can know: what to call the setting in a list,
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

THE IN-HEADSET MENU (docs/settings-menu.md) is the second consumer, and it
reads the same annotations. A `menu` token on a [fix] setting's ui: line puts
that row on the menu's Fixes page; `menu performance` puts it on the
Performance page. A [fix] row with no token stays desktop-only. The menu's
developer tier shows every [advanced] and [experimental] key the code reads,
with no annotation at all: a getBool read is a switch, a bounded number is a
number, and a getString read is read-only unless a `# dev: choices a, b, c`
line above it names its values (the mirror of ui:, allowed only OUTSIDE
[fix]). `# dev: hidden` keeps a key off the menu entirely.

The menu's restart flagging is derived here too, from the same prose the
window reads: a menu row on the Fixes or Performance page that does not say
when it takes effect is a build error (as for the window); a developer-tier
key that does not say is a WARNING, listed, and the row wears a "?" badge
until the sentence is written.

Usage:
  python tools/gen_settings_schema.py --root <repo> --out <gen dir>
  python tools/gen_settings_schema.py --root <repo> --check    (no output written)
  python tools/gen_settings_schema.py --self-test              (fixtures in %TEMP%)
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
# The sections whose keys may carry a `ui:` line at all. The installer's
# window still shows only EXPOSED_SECTIONS; what a ui: line buys a developer
# key is its LABEL and its CHOICES in the in-headset menu, so that demoting a
# setting from [fix] to [experimental] costs it its tier and its page but not
# the words somebody already wrote for it.
UI_SECTIONS = ('fix', 'advanced', 'experimental', 'menu')
# The sections whose keys may be ordinary rows of the in-headset menu -- the
# fixes, and the menu's own switches, which are the only [menu] keys anyone
# would reach for while wearing the headset. Both are Fix tier: neither is a
# developer instrument, and neither is hidden behind menu.developer.
MENU_ROW_SECTIONS = ('fix', 'menu')

READ_RE = re.compile(
    r'get(Bool|Int|Float|String)([A-Za-z]*)\s*\(\s*"([^"]+)"\s*,\s*([^;]*?)\)', re.S)
KEY_RE = re.compile(r'^([A-Za-z0-9_.-]+)\s*=\s*(.*)$')
UI_RE = re.compile(r'^ui\s*:\s*(.*)$', re.I)
DEV_RE = re.compile(r'^dev\s*:\s*(.*)$', re.I)
# The sections the menu's developer tier lists in full.
DEV_SECTIONS = ('advanced', 'experimental')
MENU_PAGES = ('fixes', 'performance')
# A heading in edvr.ini: a rule, the title, a rule. The heading a person
# reads in the file is the heading the window shows, so there is no second
# list of group names to keep in step with this one.
RULE_RE = re.compile(r'^-{10,}$')


def source_files(src):
    # Sorted, so that "the first read" below means the same thing on every
    # filesystem, and in the fixtures --self-test lays out.
    for base, dirs, names in os.walk(src):
        dirs.sort()
        for n in sorted(names):
            if n.endswith(('.cpp', '.h')):
                yield os.path.join(base, n)


# The accessors that say what a value IS. getString makes no such claim --
# every value in the file is a string -- so a getString read of a key that is
# also read with one of these is a passthrough: the file's own text wanted for
# a status line, or kept raw for a "not a value this build can use" report.
TYPED_READS = ('Bool', 'Int', 'Float')


def readers(src):
    """dotted key -> (kind, default, lo, hi, precision) as the code asks for it,
    plus the keys the code asks for two different ways.

    A key is read in more than one place more often than not, and not always
    with the same accessor. Only a typed read (getBool, getInt, getFloat and
    their InRange forms) says what the value is; a getString read beside one
    is the raw text for a log line or a status echo. So the typed read wins,
    whichever file is walked first -- and the walk order is what used to
    decide it. The menu's status page reads fix.render_sharpness with
    getString to echo the file's own text, from a file that sorts before the
    pass reading it with getFloat, and that made the window's Sharpening row
    a text box. A percentage row that is a text box takes "20" as 20 and
    shows it as 2000%, which is how it reached the field (issue 35).

    Among typed reads of one kind, the one that declares a range wins: an
    InRange read clamps the value to its bounds, so those are the bounds a
    box may show. A plain getInt in the first file walked used to hide the
    getIntInRange in the next -- fix.vscreen_res_width, read plain by the
    panel patch and bounded 640..8192 by the intro upscaler -- and the window
    showed bounds only because the annotation repeated them by hand.

    Two typed reads that disagree -- on the kind (getInt in one file, getFloat
    in another) or on the declared range -- are a real conflict, returned for
    the caller to fail the build on rather than pick one: the window would be
    typing into a box the code reads another way.
    """
    reads = {}   # key -> [(base, label, default, lo, hi, relpath)] in walk order
    for path in source_files(src):
        with open(path, encoding='utf-8', errors='replace') as f:
            text = f.read()
        rel = os.path.relpath(path, src)
        for m in READ_RE.finditer(text):
            base, suffix, key, args = m.group(1), m.group(2), m.group(3), m.group(4)
            parts = [a.strip() for a in split_args(args)]
            lo = hi = None
            if 'InRange' in suffix and len(parts) >= 3:
                lo, hi = parts[1], parts[2]
            default = parts[0] if parts else ''
            label = 'get%s%s' % (base, suffix)
            if lo is not None:
                label += ' %s..%s' % (lo, hi)
            reads.setdefault(key, []).append((base, label, default, lo, hi, rel))

    found = {}
    conflicts = []   # [(key, [(label, relpath), ...])]
    for key, entries in reads.items():
        typed = [e for e in entries if e[0] in TYPED_READS]
        bounded = [e for e in typed if e[3] is not None]
        if len(set(e[0] for e in typed)) > 1 or len(set((e[3], e[4]) for e in bounded)) > 1:
            conflicts.append((key, [(e[1], e[5]) for e in entries]))
            continue
        # The first bounded read, else the first typed one, else the first
        # read there is.
        base, _label, default, lo, hi, _rel = (bounded or typed or entries)[0]
        kind = {'Bool': 'toggle', 'Int': 'number', 'Float': 'number',
                'String': 'text'}[base]
        precision = 0 if base in ('Bool', 'Int') else 2
        found[key] = (kind, default, lo, hi, precision)
    return found, conflicts


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
        # The menu's half (docs/settings-menu.md): which page a [fix] row
        # sits on (None = desktop-only), and the dev: annotation's answers
        # for the developer tier.
        self.menuPage = None
        self.devAnnotated = False
        self.devChoices = []
        self.devHidden = False


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
    devAnnotation = None
    movedFrom = []      # (old dotted, old default) lines since the last key

    for index, raw in enumerate(lines):
        line = raw.strip()
        if not line:
            prose = []
            annotation = None
            devAnnotation = None
            movedFrom = []
            continue
        if line.startswith('['):
            close = line.find(']')
            if close > 0:
                section = line[1:close]
            group = ''
            prose = []
            annotation = None
            devAnnotation = None
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
            dev = DEV_RE.match(body)
            if ui:
                annotation = ui.group(1).strip()
            elif dev:
                devAnnotation = dev.group(1).strip()
            elif body.startswith('moved-from:'):
                # Migration metadata, not prose -- captured so the window can
                # read an un-migrated file the way the runtime does.
                spec = body[len('moved-from:'):].strip()
                dm = re.match(r'^(\S+)\s*\(default\s+([^)]*)\)$', spec)
                if dm:
                    movedFrom.append((dm.group(1), dm.group(2).strip()))
                else:
                    movedFrom.append((spec, ''))
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
        s.group = group
        s.movedFrom = list(movedFrom)
        movedFrom = []
        s.line = index + 1
        if annotation is not None:
            s.annotated = True
            apply_annotation(s, annotation)
        if devAnnotation is not None:
            s.devAnnotated = True
            apply_dev_annotation(s, devAnnotation)
        settings.append(s)
        prose = []
        annotation = None
        devAnnotation = None
    return settings


def apply_dev_annotation(setting, text):
    """`dev: choices a, b, c` or `dev: hidden`, for the menu's developer tier."""
    parts = [p.strip() for p in text.split('|')]
    for part in parts:
        lower = part.lower()
        if lower.startswith('hidden'):
            setting.devHidden = True
        elif lower.startswith('choices'):
            rest = part.split(None, 1)[1] if ' ' in part else ''
            setting.devChoices = [c.strip() for c in rest.split(',') if c.strip()]


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
        elif lower == 'menu' or lower.startswith('menu '):
            # The in-headset menu's page: bare `menu` is the Fixes page,
            # `menu performance` the Performance page.
            page = part.split(None, 1)[1].strip().lower() if ' ' in part else 'fixes'
            setting.menuPage = page


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


def run(root, out, check):
    """Generate into `out` (or only check, when `check` is set or `out` is
    None). Returns the process exit code."""
    ini_path = os.path.join(root, 'edvr.ini')
    code, conflicts = readers(os.path.join(root, 'src'))
    settings = parse_ini(ini_path)

    # ---- the enforcement --------------------------------------------------
    if conflicts:
        print('gen_settings_schema: ERROR: %d setting(s) are read two different ways.'
              % len(conflicts))
        print()
        print('The accessor the code reads a key with is what shapes its row: a switch,')
        print('a number box, a text box, and the bounds an InRange read clamps to. Two')
        print('typed reads that disagree -- on the kind, or on the range -- leave no shape')
        print('to choose. Read it one way; or with getString where only the raw text is')
        print('wanted, which makes no claim about the type and never overrides one.')
        print()
        for key, entries in conflicts:
            print('  %s' % key)
            for label, rel in entries:
                print('      %s  %s' % (label, rel))
        return 1

    # A percentage is a number. The window's display multiplies a percent row
    # by 100 whatever its kind, while its parse hands a text row's typing to
    # the file verbatim -- so `percent` on a row that is not a number shows
    # 0.1 as 10% and writes a typed 20 as 20, to be shown as 2000%. That is the
    # shape the render_sharpness row had while a getString read typed it.
    fractions = []
    for s in settings:
        dotted = '%s.%s' % (s.section, s.key)
        if not s.percent or dotted not in code:
            continue
        if code[dotted][0] != 'number' or s.choices:
            fractions.append((s, code[dotted][0] if not s.choices else 'choice'))
    if fractions:
        print('gen_settings_schema: ERROR: %d setting(s) say `percent` but are not numbers.'
              % len(fractions))
        print()
        print('A percentage is shown as a number and typed as one; a row of any other')
        print('kind cannot round-trip it. Read the key with getFloat, or drop the token.')
        print()
        for s, kind in fractions:
            print('  edvr.ini:%d  %s.%s  (%s)' % (s.line, s.section, s.key, kind))
        return 1

    stray = [s for s in settings if s.annotated and s.section not in UI_SECTIONS]
    if stray:
        print('gen_settings_schema: ERROR: a ui: line belongs to a [%s] setting.'
              % ']/['.join(UI_SECTIONS))
        print()
        print('The window shows only [%s]; a ui: line elsewhere buys a label and'
              % ']/['.join(EXPOSED_SECTIONS))
        print('choices for the in-headset menu\'s developer tier and nothing more. The')
        print('remaining sections are plumbing rather than settings. Remove the ui: line:')
        print()
        for s in stray:
            print('  edvr.ini:%d  %s.%s' % (s.line, s.section, s.key))
        return 1

    bothAnnotated = [s for s in settings if s.annotated and s.devAnnotated]
    if bothAnnotated:
        print('gen_settings_schema: ERROR: a setting carries both a ui: and a dev: line; '
              'the ui: line already gives the menu its label and choices.')
        for s in bothAnnotated:
            print('  edvr.ini:%d  %s.%s' % (s.line, s.section, s.key))
        return 1

    strayDev = [s for s in settings if s.devAnnotated and s.section in EXPOSED_SECTIONS]
    if strayDev:
        print('gen_settings_schema: ERROR: a dev: line belongs outside [%s]; inside it the '
              'ui: line carries the choices.' % ']/['.join(EXPOSED_SECTIONS))
        for s in strayDev:
            print('  edvr.ini:%d  %s.%s' % (s.line, s.section, s.key))
        return 1

    badPage = [s for s in settings if s.menuPage and s.menuPage not in MENU_PAGES]
    if badPage:
        print('gen_settings_schema: ERROR: the menu token names a page this build has no '
              'page for. Pages: %s.' % ', '.join(MENU_PAGES))
        for s in badPage:
            print('  edvr.ini:%d  %s.%s  (menu %s)' % (s.line, s.section, s.key, s.menuPage))
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

    # ---- the menu's rows (docs/settings-menu.md) --------------------------
    #
    # A [fix] row with a menu token, plus every [advanced] / [experimental] key
    # the code reads (dev: hidden excepted). Each carries what the window's
    # row carries and three things more: which page, which tier, and when it
    # applies as a tri-state -- unknown is allowed for the developer tier and
    # shown as a badge, never silently read as live.
    menuRows = []
    menuFix = [s for s in settings
               if s.menuPage and not s.hidden and s.section in MENU_ROW_SECTIONS
               and '%s.%s' % (s.section, s.key) in code]
    for s in menuFix:
        menuRows.append((s, s.menuPage, 'Fix'))
    strayToken = [s for s in settings
                  if s.menuPage and s.section not in MENU_ROW_SECTIONS]
    if strayToken:
        print('gen_settings_schema: ERROR: a `| menu` token belongs to a [%s] setting; '
              'elsewhere the section decides the page.' % ']/['.join(MENU_ROW_SECTIONS))
        for s in strayToken:
            print('  edvr.ini:%d  %s.%s' % (s.line, s.section, s.key))
        return 1
    devSilent = []
    for s in settings:
        if s.section not in DEV_SECTIONS or s.devHidden:
            continue
        dotted = '%s.%s' % (s.section, s.key)
        if dotted not in code:
            continue
        if when_it_applies(s) is None:
            devSilent.append(s)
        menuRows.append((s, s.section, 'Advanced' if s.section == 'advanced' else 'Experimental'))
    if devSilent:
        print('gen_settings_schema: WARNING: %d developer-tier setting(s) do not say when they '
              'take effect; the menu shows them with a "?" badge. End the comment block with '
              '"Live." or the sentence that says a restart is needed:' % len(devSilent))
        for s in devSilent:
            print('  edvr.ini:%d  %s.%s' % (s.line, s.section, s.key))

    if check or not out:
        restarts = len([s for s in exposed if when_it_applies(s) == 'restart'])
        print('gen_settings_schema: %d exposed, %d of them needing a game restart; '
              '%d live in [%s]; menu: %d fix rows on %s, %d developer rows'
              % (len(exposed), restarts,
                 len([s for s in settings if s.live and s.section in EXPOSED_SECTIONS]),
                 ']/['.join(EXPOSED_SECTIONS), len(menuFix),
                 '/'.join(sorted(set(s.menuPage for s in menuFix))) or 'no page',
                 len(menuRows) - len(menuFix)))
        return 0

    menuOut = []
    for s, page, tier in menuRows:
        dotted = '%s.%s' % (s.section, s.key)
        kind, default, lo, hi, precision = code[dotted]
        if lo is None and s.range_lo is not None:
            lo, hi = s.range_lo, s.range_hi
        # A developer key with a ui: line keeps its label and its choices; one
        # without falls back to its own name and its dev: line.
        choices = s.choices if s.choices else s.devChoices
        if choices:
            kind = 'choice'
        applies = when_it_applies(s)
        menuOut.append(
            '    {%s, %s, %s, %s,\n     %s,\n     MenuKind::%s, %s, %s, %s, %d, %s, %s, %d,\n'
            '     MenuTier::%s, %s, %s},' % (
                c_string(s.section), c_string(s.key),
                c_string(s.label if s.annotated else s.key),
                c_string(summarise(s.description)),
                c_string(s.description),
                {'toggle': 'Toggle', 'number': 'Number', 'text': 'Text',
                 'choice': 'Choice'}[kind],
                c_string(s.value), c_string(lo or ''), c_string(hi or ''), precision,
                c_string('|'.join(choices)),
                'true' if s.percent else 'false',
                {None: 0, 'live': 1, 'restart': 2}[applies],
                tier, c_string(page), c_string(s.group)))
    os.makedirs(out, exist_ok=True)
    menu_path = os.path.join(out, 'menu_schema.inc')
    with open(menu_path, 'w', encoding='utf-8', newline='\r\n') as f:
        f.write('// Generated by tools/gen_settings_schema.py from edvr.ini and src/.\n')
        f.write('// Do not edit, and do not commit: the sources are the ini and the code.\n')
        f.write('// The in-headset menu\'s rows (src/d3d11/menu_schema.h).\n')
        f.write('static const MenuRowDef kMenuRows[] = {\n')
        f.write('\n'.join(menuOut))
        f.write('\n};\n')
    print('gen_settings_schema: wrote %s (%d rows)' % (menu_path, len(menuOut)))

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

    os.makedirs(out, exist_ok=True)
    out_path = os.path.join(out, 'settings_schema.inc')
    with open(out_path, 'w', encoding='utf-8', newline='\r\n') as f:
        f.write('// Generated by tools/gen_settings_schema.py from edvr.ini and src/.\n')
        f.write('// Do not edit, and do not commit: the sources are the ini and the code.\n')
        f.write('static const SettingDef kSettings[] = {\n')
        f.write('\n'.join(rows))
        f.write('\n};\n')
        f.write('\n')
        f.write('// {old dotted name, new dotted name, the old key\'s shipped\n')
        f.write('// default (a user line still carrying it is stale, not a choice)}\n')
        f.write('static const char* const kMovedSettings[][3] = {\n')
        movedRows = []
        for s in exposed:
            for old, oldDefault in getattr(s, 'movedFrom', []):
                movedRows.append('    {%s, %s, %s},' % (
                    c_string(old), c_string('%s.%s' % (s.section, s.key)),
                    c_string(oldDefault)))
        if not movedRows:
            movedRows.append('    {"", "", ""},  // none; empty arrays are not C++')
        f.write('\n'.join(movedRows))
        f.write('\n};\n')
    print('gen_settings_schema: wrote %s (%d settings)' % (out_path, len(rows)))
    return 0


# ---------------------------------------------------------------------------
# --self-test
# ---------------------------------------------------------------------------

def self_test():
    """The reader rule and the two gates it feeds, against fixtures laid out in
    the temp folder: the shape of issue 35 in both walk orders, and the two
    things the rule must refuse. build.bat runs this before the schema is
    generated, so a rule that drifts fails there and not in the window."""
    import contextlib
    import io
    import shutil
    import tempfile

    base = tempfile.mkdtemp(prefix='edvr-gen-schema-')
    failures = []

    def case(name, ini, sources, expect):
        """Lay out one root, run the generator over it, and hand back what it
        wrote (on success) or what it said (on the expected failure)."""
        root = os.path.join(base, name)
        src = os.path.join(root, 'src')
        os.makedirs(src)
        with open(os.path.join(root, 'edvr.ini'), 'w', encoding='utf-8') as f:
            f.write(ini)
        for filename, text in sources.items():
            with open(os.path.join(src, filename), 'w', encoding='utf-8') as f:
                f.write(text)
        out = os.path.join(root, 'gen')
        said = io.StringIO()
        with contextlib.redirect_stdout(said):
            code = run(root, out, False)
        if code != expect:
            failures.append('%s: exit %d, expected %d\n%s' % (name, code, expect, said.getvalue()))
            return ''
        if code != 0:
            return said.getvalue()
        wrote = ''
        for leaf in ('settings_schema.inc', 'menu_schema.inc'):
            with open(os.path.join(out, leaf), encoding='utf-8') as f:
                wrote += f.read()
        return wrote

    def expect_in(name, text, needle):
        if needle not in text:
            failures.append('%s: expected %r in the output' % (name, needle))

    def expect_not_in(name, text, needle):
        if needle in text:
            failures.append('%s: did not expect %r in the output' % (name, needle))

    # Issue 35's shape: a status page echoing the file's text with getString,
    # in a file that sorts before the pass reading the value with getFloat.
    # The row is a number and a percentage either way round.
    sharpen_ini = ('[fix]\n'
                   '# Sharpen every outgoing frame, 0 (off) to 1 (strongest). Live.\n'
                   '# ui: Sharpening | range 0..1 | percent | menu performance\n'
                   'render_sharpness = 0.0\n')
    echo = 'const std::string v = Config::get().getString("fix.render_sharpness", "0.0");\n'
    use = 'float v = cfg.getFloat("fix.render_sharpness", 0.0f);\n'
    for name, sources in (('echo-first', {'a_menu.cpp': echo, 'b_pass.cpp': use}),
                          ('use-first', {'a_pass.cpp': use, 'b_menu.cpp': echo})):
        wrote = case(name, sharpen_ini, sources, 0)
        expect_in(name, wrote, 'SettingKind::Number, "0.0", "0.0",\n     "0.0", "1.0", 2, "", true, false, true,')
        expect_not_in(name, wrote, 'SettingKind::Text')
        expect_in(name, wrote, 'MenuKind::Number')
        expect_not_in(name, wrote, 'MenuKind::Text')

    # A key the code only ever reads as text is still a text row: the rule
    # promotes a string read beside a typed one, it does not retype the rest.
    name = 'text-stays-text'
    wrote = case(name, '[fix]\n# A name. Live.\n# ui: Name\nname = abc\n',
                 {'a.cpp': 'const std::string n = cfg.getString("fix.name", "");\n'}, 0)
    expect_in(name, wrote, 'SettingKind::Text')

    # Two typed reads that disagree are a conflict, named with both files.
    name = 'typed-conflict'
    said = case(name, '[fix]\n# A thing. Live.\n# ui: Thing\nthing = 1\n',
                {'a.cpp': 'int t = cfg.getInt("fix.thing", 1);\n',
                 'b.cpp': 'float t = cfg.getFloat("fix.thing", 1.0f);\n'}, 1)
    expect_in(name, said, 'read two different ways')
    expect_in(name, said, 'fix.thing')
    expect_in(name, said, 'getInt  a.cpp')
    expect_in(name, said, 'getFloat  b.cpp')

    # vscreen_res_width's shape: the panel patch reads it plain, the intro
    # upscaler bounded. The declared range reaches the row whichever file is
    # walked first, and a whole-number range is written as it was declared.
    width_ini = ('[fix]\n'
                 '# The on-foot screen width. Needs a game restart.\n'
                 '# ui: On-foot screen resolution: width\n'
                 'vscreen_res_width = 1920\n')
    plain = 'const uint32_t w = static_cast<uint32_t>(cfg.getInt("fix.vscreen_res_width", 0));\n'
    clamped = 'g_targetW = cfg.getIntInRange("fix.vscreen_res_width", 1920, 640, 8192);\n'
    for name, sources in (('plain-first', {'a_patch.cpp': plain, 'b_intro.cpp': clamped}),
                          ('clamped-first', {'a_intro.cpp': clamped, 'b_patch.cpp': plain})):
        wrote = case(name, width_ini, sources, 0)
        expect_in(name, wrote, 'SettingKind::Number, "1920", "1920",\n     "640", "8192", 0,')

    # Two declared ranges that disagree are a conflict too, and the message
    # shows each read's bounds so the reader can see which is which.
    name = 'range-conflict'
    said = case(name, '[fix]\n# A thing. Live.\n# ui: Thing\nthing = 1\n',
                {'a.cpp': 'int t = cfg.getIntInRange("fix.thing", 1, 0, 10);\n',
                 'b.cpp': 'int t = cfg.getIntInRange("fix.thing", 1, 0, 20);\n'}, 1)
    expect_in(name, said, 'read two different ways')
    expect_in(name, said, 'getIntInRange 0..10  a.cpp')
    expect_in(name, said, 'getIntInRange 0..20  b.cpp')

    # `percent` on a row that is not a number cannot round-trip a typed value.
    name = 'percent-on-text'
    said = case(name, '[fix]\n# A word. Live.\n# ui: Word | percent\nword = 0.5\n',
                {'a.cpp': 'const std::string w = cfg.getString("fix.word", "");\n'}, 1)
    expect_in(name, said, '`percent` but are not numbers')
    expect_in(name, said, 'fix.word  (text)')

    # fix.openxr_resolution's shape: a live [fix] key whose shipped value is
    # empty, read as text, with a restart ui: line and no range or percent.
    # It generates a Text menu row with empty bounds, and the summary is the
    # block's first sentence whole -- the grammar and the example are what
    # the settings window shows above a text box (no other [fix] key ships
    # empty with a ui: line, so nothing else exercises this).
    name = 'empty-text-restart'
    first = ('Per-headset OpenXR render width in pixels per eye, as runtime/system:width '
             'entries such as oculus/meta-quest-3:3283.')
    wrote = case(name, ('[fix]\n'
                        '# Per-headset OpenXR render width in pixels per eye, as runtime/system:width\n'
                        '# entries such as oculus/meta-quest-3:3283. Set it from the F8 menu with the\n'
                        '# headset on -- copy the key from the log. Entries are separated by\n'
                        '# commas, up to 8. Swapchains are fixed when VR starts, so a change\n'
                        '# applies after a game restart.\n'
                        '# ui: OpenXR resolution | restart | menu performance\n'
                        'openxr_resolution =\n'),
                 {'a.cpp': 'const std::string v = cfg.getString("fix.openxr_resolution", "");\n'}, 0)
    expect_in(name, wrote, 'MenuKind::Text, "", "", "", 2, "", false, 2,')
    expect_in(name, wrote, 'MenuTier::Fix, "performance"')
    expect_in(name, wrote, '"' + first + '",')
    expect_in(name, wrote, 'SettingKind::Text, "", "",')
    expect_not_in(name, wrote, 'MenuKind::Number')

    shutil.rmtree(base, ignore_errors=True)
    if failures:
        print('gen_settings_schema: self-test FAILED')
        for f in failures:
            print('  ' + f.replace('\n', '\n    '))
        return 1
    print('gen_settings_schema: self-test OK')
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root')
    ap.add_argument('--out')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--self-test', action='store_true',
                    help='check this script against fixtures and exit')
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.root:
        ap.error('--root is required')
    return run(args.root, args.out, args.check)


if __name__ == '__main__':
    sys.exit(main())
