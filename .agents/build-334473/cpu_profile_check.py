#!/usr/bin/env python3
"""Check tools/cpu_profile/Stacks.cs and SelfTests.cs for the build 334473 retarget.

No dotnet on this host, so the C# logic the checks rely on is re-implemented here:

  A. RvaTable.Build() from Stacks.cs, RvaClass.Contains (rva >= Entry && rva < Bound),
     and every Check() in SelfTests.cs that calls Matches() or touches RvaTable.*.
     Each equality prints the expected (right side) and computed (left side) value.
  B. Stack fixtures: each ClassIds entry must classify the game frame it names, and
     each GameTop must equal that fixture's game frames.
  C. Expected strings (Report.Site, Signature, gameCallSite, blockingSite, site, and
     TryParseRva) recomputed from the fixtures.
  D. Token check: every 0x literal and every address-named identifier (name_hex) in
     both files is compared with the same token in the pre-branch file (d67db44d, all
     build 332841). The expected value is the mapper's new address (IDENTICAL,
     RELOC-ONLY, XREF-*), or the offset from the containing table window for a
     boundary the mapper reports NOT-FOUND, or unchanged when the mapper has no code
     unit for the value. Non-token text must be identical to the pre-branch text.
  E. Each pre-branch table window against its new window under the same offset rule.
  F. Stale scan: no token equals an old address that now maps to a different address.

Usage:
  python3 .agents/build-334473/cpu_profile_check.py            working tree
  python3 .agents/build-334473/cpu_profile_check.py --rev HEAD  a committed revision
  python3 .agents/build-334473/cpu_profile_check.py --rev d67db44d  control: all 332841
Exit status 0 only if every check matches.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PRE_REV = "d67db44d"
OLD_EXE, NEW_EXE = "analysis/EliteDangerous64.exe", "analysis/EliteDangerous64-334473.exe"
CACHE = "analysis/build_map_332841_to_334473.cache.json"
STACKS = "tools/cpu_profile/Stacks.cs"
SELF = "tools/cpu_profile/SelfTests.cs"
USABLE = {"IDENTICAL", "RELOC-ONLY", "XREF-EXACT", "XREF-BRACKET"}
RVA_LIMIT = 0x10000000          # game RVAs are far below 256 MB; module bases are above
GAME_BASE = 0x140000000         # ImageBase of both executables (checked with the PE header)

problems = []
counts = {"A": 0, "B": 0, "C": 0, "D": 0, "D_changed": 0, "E": 0, "F": 0}


def git_show(rev, path):
    return subprocess.run(["git", "-C", str(REPO), "show", f"{rev}:{path}"],
                          check=True, capture_output=True, text=True, encoding="utf-8").stdout


def num(s):
    s = s.strip()
    return int(s, 16) if s[:2].lower() == "0x" else int(s)


# ---------------------------------------------------------------- Stacks.cs: table
SEED_RE = re.compile(r'\("([^"]+)",\s*(0x[0-9A-Fa-f]+|\d+),\s*(0x[0-9A-Fa-f]+|\d+),\s*RvaKind\.(\w+),\s*(true|false)\)')
CONST_RE = re.compile(r'const\s+u?int\s+(\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*;')


class Cls:
    def __init__(self, name, entry, bound, kind, pipeline, size):
        self.Name, self.Entry, self.Bound, self.Kind, self.Pipeline, self.Size = name, entry, bound, kind, pipeline, size

    def Contains(self, rva):                       # RvaClass.Contains
        return rva >= self.Entry and rva < self.Bound


class Table:
    """Re-implements RvaTable.Seeds / Build() / BoundRule() from Stacks.cs."""

    def __init__(self, text):
        i = text.index("Seeds =")
        block = text[i:text.index("];", i)]
        self.seeds = [(m.group(1), num(m.group(2)), num(m.group(3)), m.group(4), m.group(5) == "true")
                      for m in SEED_RE.finditer(block)]
        self.consts = {m.group(1): num(m.group(2)) for m in CONST_RE.finditer(text)}
        c = self.consts
        entries = sorted(set([s[1] for s in self.seeds] + [c["SchedulerStart"]]))
        classes = []
        for name, entry, size, kind, pipe in self.seeds:
            nxt = next((e for e in entries if e > entry), 0xFFFFFFFF)   # FirstOrDefault(.., uint.MaxValue)
            if kind == "CallSite":
                bound = entry + c["CallSiteWindow"]
            elif kind == "Thunk":
                bound = entry + c["ThunkWindow"]
            elif size > 0:
                bound = entry + size
            else:
                bound = min(nxt, entry + c["BodyWindow"])
            classes.append(Cls(name, entry, bound, kind, pipe, size))
        classes.append(Cls("scheduler_range", c["SchedulerStart"], c["SchedulerEnd"], "Scheduler", False, 0))
        self.classes = sorted(classes, key=lambda x: x.Entry)

    def cls(self, name):                           # ClassId(name) then Classes[...]
        hits = [x for x in self.classes if x.Name == name]
        if not hits:
            raise KeyError(f"no class named {name}")
        return hits[0]

    def size_of(self, name):
        return next((s[2] for s in self.seeds if s[0] == name), 0)

    def bound_rule(self, c):
        if c.Kind == "CallSite":
            return "entry+0x10 (return address after the call)"
        if c.Kind == "Thunk":
            return "entry+0x10 (thunk)"
        if c.Kind == "Scheduler":
            return "explicit range"
        if c.Kind == "Census":
            return "entry+0x1000 (assumed, no decompile)"
        if self.size_of(c.Name) > 0:
            return f"decompile size ({self.size_of(c.Name)} bytes)"
        return "entry+0x2000" if c.Bound == c.Entry + self.consts["BodyWindow"] else "next known entry"


# ---------------------------------------------------------------- SelfTests.cs: assertions
def statements(text, word):
    """(line, argument text) for each call word(...), honouring strings and nesting."""
    out = []
    for m in re.finditer(r'\b%s\(' % word, text):
        i, depth, in_str = m.end(), 1, False
        start = i
        while depth:
            ch = text[i]
            if in_str:
                if ch == "\\":
                    i += 1
                elif ch == '"':
                    in_str = False
            elif ch == '"':
                in_str = True
            elif ch in "([{":
                depth += 1
            elif ch in ")]}":
                depth -= 1
            i += 1
        out.append((text.count("\n", 0, m.start()) + 1, text[start:i - 1]))
    return out


def split_args(s):
    parts, depth, cur, in_str, i = [], 0, "", False, 0
    while i < len(s):
        ch = s[i]
        if in_str:
            cur += ch
            if ch == "\\":
                cur += s[i + 1]
                i += 1
            elif ch == '"':
                in_str = False
        elif ch == '"':
            in_str = True
            cur += ch
        elif ch in "([{":
            depth += 1
            cur += ch
        elif ch in ")]}":
            depth -= 1
            cur += ch
        elif ch == "," and depth == 0:
            parts.append(cur.strip())
            cur = ""
        else:
            cur += ch
        i += 1
    if cur.strip():
        parts.append(cur.strip())
    return parts


def translate(cond):
    s = re.sub(r'RvaTable\.Classes\[ClassId\("([^"]+)"\)\]', r'C("\1")', cond)
    s = s.replace("RvaTable.Classes.Count(item => item.Pipeline)", "NPIPE()")
    s = s.replace("RvaTable.Classes.Length", "NCLS()")
    s = re.sub(r"RvaTable\.BoundRule\(", "BR(", s)
    s = re.sub(r'(?<![A-Za-z_])Matches\("([^"]+)",\s*', r'M("\1", ', s)
    s = s.replace("&&", " and ").replace("||", " or ")
    s = re.sub(r"!(?!=)", " not ", s)
    s = re.sub(r"\btrue\b", "True", s)
    s = re.sub(r"\bfalse\b", "False", s)
    return s


def check_assertions(T, text, lines_out):
    env = {"C": T.cls, "M": lambda n, r: T.cls(n).Contains(r), "BR": T.bound_rule,
           "NPIPE": lambda: sum(1 for x in T.classes if x.Pipeline), "NCLS": lambda: len(T.classes),
           "__builtins__": {}}
    for line, args in statements(text, "Check"):
        parts = split_args(args)
        if not parts:
            continue
        cond = " ".join(parts[0].split())
        msg = parts[-1] if len(parts) > 1 else ""
        if "item.Bound > item.Entry" in cond:
            ok = all(x.Bound > x.Entry for x in T.classes)
            counts["A"] += 1
            lines_out.append(f"  A  L{line:<5} PASS expected=True computed={ok}  every class has a positive window")
            if not ok:
                problems.append(f"A L{line}: a class has a non-positive window")
            continue
        if not re.search(r'(?<![A-Za-z_])Matches\(|RvaTable\.', cond):
            continue
        py = translate(cond)
        try:
            if py.count(" == ") == 1 and not re.search(r" and | or | not ", py):
                lhs, rhs = py.split(" == ")
                computed, expected = eval(lhs, env), eval(rhs, env)
            else:
                expected, computed = True, eval(py, env)
            ok = computed == expected
        except Exception as exc:  # a C# exception would fail the test run
            ok, expected, computed = False, "value", f"error: {exc}"
        counts["A"] += 1
        tag = "PASS" if ok else "FAIL"
        show = lambda x: hex(x) if isinstance(x, int) and not isinstance(x, bool) and x > 255 else repr(x)
        lines_out.append(f"  A  L{line:<5} {tag} expected={show(expected)} computed={show(computed)}  "
                         f"{' '.join(cond.split())[:70]}")
        if not ok:
            problems.append(f"A L{line}: expected {expected!r}, computed {computed!r} ({msg})")


# ---------------------------------------------------------------- SelfTests.cs: fixtures
BLOCK_RE = re.compile(r'var (\w+) = stacks\.AddForTest\(new StackInfo\s*\{(.*?)\}\);', re.S)
MOD_NAMES = {"exe": "GAME", "edvrD3d11": "EDVR", "systemD3d11": "SYS", "nvidia": "NV"}
DISPLAY = {"NV": "nvwgf2umx.dll", "UNRESOLVED": "unresolved", "SYS": "d3d11.dll [system32]",
           "EDVR": "d3d11.dll [edvr]"}


def top_level_items(s):
    return [x for x in split_args(s) if x]


def parse_blocks(text):
    blocks = {}
    for m in BLOCK_RE.finditer(text):
        body = m.group(2)
        fld = lambda f: re.search(r'%s = \[(.*?)\]' % f, body, re.S)
        mods = [MOD_NAMES.get(x, "UNRESOLVED" if x == "-1" else x) for x in top_level_items(fld("ModuleIds").group(1))]
        frames = []
        for mod, addr in zip(mods, top_level_items(fld("Addresses").group(1))):
            g = re.fullmatch(r'GameBase \+ (0x[0-9A-Fa-f]+)', addr)
            frames.append((mod, "game" if g else "abs", num(g.group(1)) if g else num(addr)))
        cids = [re.search(r'"([^"]+)"', x).group(1) for x in top_level_items(fld("ClassIds").group(1))] \
            if fld("ClassIds") else []
        gt = re.search(r'GameTops = new GameTop\((.*?)\)', body)
        tops = [num(x) for x in gt.group(1).split(",")] if gt else None
        blocks[m.group(1)] = {"frames": frames, "classes": cids, "tops": tops}
    return blocks


def site_of(tops):                                  # Report.Site(GameTop)
    n = tops[3]
    if n == 0:
        return "no_game_frame"
    return " < ".join(f"0x{v:x}" for v in tops[:min(n, 3)])


def check_fixtures(T, text, blocks, lines_out):
    for name, b in blocks.items():
        game = [(i, v) for i, (mod, kind, v) in enumerate(b["frames"]) if mod == "GAME" and kind == "game"]
        if b["classes"]:
            if len(b["classes"]) == len(b["frames"]):
                pairs = [(i, v, b["classes"][i]) for i, v in game]
            else:
                pairs = [(game[0][0], game[0][1], b["classes"][0])] if game else []
            for i, v, cname in pairs:
                ok = T.cls(cname).Contains(v)
                counts["B"] += 1
                lines_out.append(f"  B  {name}[{i}] 0x{v:x} in {cname:<24} "
                                 f"{'PASS' if ok else 'FAIL'} (window 0x{T.cls(cname).Entry:x}..0x{T.cls(cname).Bound:x})")
                if not ok:
                    problems.append(f"B {name}[{i}]: 0x{v:x} is not in {cname}")
        if b["tops"] is not None:
            n = b["tops"][3]
            want = [v for _, v in game]
            ok = n == len(want) and b["tops"][:min(n, 3)] == want[:min(n, 3)]
            counts["B"] += 1
            lines_out.append(f"  B  {name} GameTop count={n} tops={[hex(x) for x in b['tops'][:min(n,3)]]} "
                             f"{'PASS' if ok else 'FAIL'} vs game frames {[hex(x) for x in want]}")
            if not ok:
                problems.append(f"B {name}: GameTop {b['tops']} does not match its game frames {want}")


def expect_string(T, text, blocks, lines_out):
    """C: literal strings in SelfTests.cs recomputed from the fixtures they describe."""
    def emit(line, label, expected, computed):
        ok = expected == computed
        counts["C"] += 1
        lines_out.append(f"  C  L{line:<5} {'PASS' if ok else 'FAIL'} expected={expected!r} computed={computed!r}  {label}")
        if not ok:
            problems.append(f"C L{line} {label}: expected {expected!r}, computed {computed!r}")

    def lineno(pos):
        return text.count("\n", 0, pos) + 1

    def sig(block):
        parts = []
        for mod, kind, v in blocks[block]["frames"]:
            parts.append(f"0x{v:x}" if (mod == "GAME" and kind == "game") else DISPLAY.get(mod, mod))
        return " < ".join(parts)

    def gamesite(block):
        t = blocks[block]["tops"]
        return site_of(t)

    for m in re.finditer(r'Report\.Site\(new GameTop\(([^()]*)\)\)\s*==\s*"([^"]*)"', text):
        args = [num(x) for x in m.group(1).split(",")]
        emit(lineno(m.start()), "Report.Site(new GameTop(..))", m.group(2), site_of(args))
    for m in re.finditer(r'Report\.Site\(first\.(BlockingTops|WakerReadyTops)\)\s*==\s*"([^"]*)"', text):
        block = "scheduler" if m.group(1) == "BlockingTops" else "wakerReady"
        emit(lineno(m.start()), f"Report.Site(first.{m.group(1)})", m.group(2), gamesite(block))
    for m in re.finditer(r'Signature\(stacks\[(\w+)\]\)\s*==\s*"([^"]*)"', text, re.S):
        emit(lineno(m.start()), f"Signature(stacks[{m.group(1)}])", m.group(2), sig(m.group(1)))
    for m in re.finditer(r'gameCallSite"\]!\s*==\s*"([^"]*)"', text):
        first_game = [v for mod, kind, v in blocks["submit"]["frames"] if mod == "GAME"][0]
        emit(lineno(m.start()), "chains[0].gameCallSite (submit game frame)", m.group(1), f"0x{first_game:x}")
    for m in re.finditer(r'\["site"\]!\s*==\s*"([^"]*)"', text):
        emit(lineno(m.start()), "cross site (scheduler blocking tops)", m.group(1), gamesite("scheduler"))
    for m in re.finditer(r'blockingSite\\":\\"(0x[0-9A-Fa-f]+)', text):
        emit(lineno(m.start()), "blockingSite JSON prefix (scheduler first top)", m.group(1),
             gamesite("scheduler").split(" < ")[0])
    for m in re.finditer(r'TryParseRva\("(0x[0-9A-Fa-f]+)", out var parsed\) && parsed == (0x[0-9A-Fa-f]+)', text):
        emit(lineno(m.start()), "TryParseRva text vs parsed value", hex(num(m.group(2))), hex(num(m.group(1))))


# ---------------------------------------------------------------- tokens and mapping
TOKEN_RE = re.compile(r'\b0[xX][0-9A-Fa-f]+\b|\b[A-Za-z][A-Za-z0-9_]*?_[0-9a-fA-F]{6,7}\b')


def tokens_of(text):
    vals = set()
    for t in TOKEN_RE.findall(text):
        if t[:2].lower() == "0x":
            vals.add(int(t, 16))
        else:
            vals.add(int(t.rsplit("_", 1)[1], 16))
    return vals


def run_mapper(values):
    with tempfile.TemporaryDirectory(prefix="cpuprof_check_") as d:
        rv, js = os.path.join(d, "rvas.txt"), os.path.join(d, "map.json")
        with open(rv, "w", encoding="utf-8") as f:
            f.write("\n".join(hex(v) for v in sorted(values)) + "\n")
        subprocess.run([sys.executable, "tools/build_diff.py", "map", "--old", OLD_EXE, "--new", NEW_EXE,
                        "--cache", CACHE, "--rvas-file", rv, "--json", js],
                       cwd=REPO, check=True, capture_output=True)
        out = {}
        for e in json.load(open(js, encoding="utf-8")):
            new = int(e["new_rva"], 16) if e.get("new_rva") else None
            out[int(e["rva"], 16)] = (e["status"], new, e.get("detail", ""))
        return out


def fmt_hex(new, original):
    digits = format(new, "x")
    return ("0x" + (digits.lower() if re.search(r"[a-f]", original[2:]) else digits.upper()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rev", help="check a git revision instead of the working tree")
    args = ap.parse_args()

    if args.rev:
        stacks_txt, self_txt = git_show(args.rev, STACKS), git_show(args.rev, SELF)
    else:
        stacks_txt = (REPO / STACKS).read_text(encoding="utf-8")
        self_txt = (REPO / SELF).read_text(encoding="utf-8")
    pre_stacks, pre_self = git_show(PRE_REV, STACKS), git_show(PRE_REV, SELF)
    cur_stacks_lines, cur_self_lines = stacks_txt.split("\n"), self_txt.split("\n")
    pre_stacks_lines, pre_self_lines = pre_stacks.split("\n"), pre_self.split("\n")

    pre_T, T = Table(pre_stacks), Table(stacks_txt)

    # mapper for every token value in both pre-branch and checked texts, plus pre table entries
    values = {v for v in tokens_of(pre_stacks + pre_self + stacks_txt + self_txt) if v <= RVA_LIMIT}
    values |= {c.Entry for c in pre_T.classes}
    mapping = run_mapper(values)

    def new_of(v):
        st = mapping.get(v)
        if st and st[0] in USABLE:
            return st[1]
        return None

    def offset_new(v):
        """NOT-FOUND boundary inside an old window: the same offset from the mapped entry."""
        cands = [c for c in pre_T.classes if c.Entry <= v <= c.Bound and new_of(c.Entry) is not None]
        if not cands:
            return None
        c = max(cands, key=lambda x: x.Entry)
        return new_of(c.Entry) + (v - c.Entry)

    def expected_value(v):
        """(new value or None when the token should stay, rule text)."""
        if v > RVA_LIMIT:
            return None, "not an RVA (module base, EDVR module offset, or size)"
        st = mapping.get(v)
        if st is None:
            return None, "not mapped"
        if st[0] in USABLE:
            return st[1], st[0].lower()
        if st[0] == "NOT-FOUND":
            off = offset_new(v)
            if off is not None:
                return off, "offset inside an old window (mapper NOT-FOUND)"
            return None, "mapper NOT-FOUND, outside every table window (synthetic or header offset)"
        problems.append(f"D: 0x{v:x} has mapper status {st[0]} and needs review")
        return None, st[0]

    # ---- A, B, C
    lines_out = []
    print("== A. assertions in SelfTests.cs (Matches / RvaTable.*), expected vs computed")
    check_assertions(T, self_txt, lines_out)
    print("\n".join(lines_out))
    lines_out = []
    blocks = parse_blocks(self_txt)
    print("== B. stack fixtures: ClassIds classify their game frame, GameTop matches the frames")
    check_fixtures(T, self_txt, blocks, lines_out)
    print("\n".join(lines_out))
    lines_out = []
    print("== C. expected strings recomputed from the fixtures")
    expect_string(T, self_txt, blocks, lines_out)
    print("\n".join(lines_out))

    # ---- D: token-by-token against the pre-branch file
    print("\n== D. every token against the pre-branch file (d67db44d), with the mapper's expected value")
    unchanged = {}
    changed = {}
    for label, pre_lines, cur_lines in (("Stacks.cs", pre_stacks_lines, cur_stacks_lines),
                                        ("SelfTests.cs", pre_self_lines, cur_self_lines)):
        if len(pre_lines) != len(cur_lines):
            problems.append(f"D {label}: line count changed")
            continue
        for ln, (pl, cl) in enumerate(zip(pre_lines, cur_lines), 1):
            if TOKEN_RE.sub("@", pl) != TOKEN_RE.sub("@", cl):
                problems.append(f"D {label} L{ln}: text other than an address token changed")
            ptoks, ctoks = TOKEN_RE.findall(pl), TOKEN_RE.findall(cl)
            if len(ptoks) != len(ctoks):
                problems.append(f"D {label} L{ln}: token count {len(ptoks)} -> {len(ctoks)}")
                continue
            for p, c in zip(ptoks, ctoks):
                counts["D"] += 1
                if p[:2].lower() == "0x":
                    v = int(p, 16)
                    new, rule = expected_value(v)
                    want = p if new is None else fmt_hex(new, p)
                else:
                    prefix, suffix = p.rsplit("_", 1)
                    v = int(suffix, 16)
                    new, rule = expected_value(v)
                    want = p if new is None else f"{prefix}_{format(new, 'x')}"
                if c != want:
                    problems.append(f"D {label} L{ln}: {p} -> {c}, mapper expects {want}")
                if c != p:
                    counts["D_changed"] += 1
                    changed.setdefault((p, c, rule), []).append(f"{label[:-3]}:{ln}")
                else:
                    unchanged.setdefault((p, rule), 0)
                    unchanged[(p, rule)] += 1
    print(f"  tokens checked: {counts['D']}, changed to the mapper's value: {counts['D_changed']}")
    for (p, c, rule), where in sorted(changed.items(), key=lambda kv: kv[0][0].lower()):
        print(f"    {p:>24} -> {c:<24} {rule:<52} {len(where)}x  {', '.join(where[:4])}")
    print("  unchanged tokens, with the reason they stay:")
    for (p, rule), n in sorted(unchanged.items(), key=lambda kv: kv[0][0].lower()):
        print(f"    {p:>24}  {rule:<60} x{n}")

    # ---- E: windows
    print("\n== E. table windows: pre-branch window -> current window (offset rule)")
    names_now = {c.Entry: c for c in T.classes}
    for c in pre_T.classes:
        new_entry = new_of(c.Entry)
        if new_entry is None:
            problems.append(f"E {c.Name}: entry 0x{c.Entry:x} has no mapper value")
            print(f"  E  {c.Name}: no mapping")
            continue
        exp_bound = new_entry + (c.Bound - c.Entry)
        cur = names_now.get(new_entry)
        ok = cur is not None and cur.Bound == exp_bound and cur.Kind == c.Kind and cur.Pipeline == c.Pipeline \
            and cur.Size == c.Size
        counts["E"] += 1
        cur_txt = f"0x{cur.Entry:x}..0x{cur.Bound:x} {cur.Name}" if cur else "missing"
        print(f"  E  {c.Name:<26} old 0x{c.Entry:x}..0x{c.Bound:x} -> expected 0x{new_entry:x}..0x{exp_bound:x}"
              f"  current {cur_txt:<46} {'PASS' if ok else 'FAIL'}")
        if not ok:
            problems.append(f"E {c.Name}: expected 0x{new_entry:x}..0x{exp_bound:x}, found {cur_txt}")

    # ---- F: stale scan
    print("\n== F. stale scan: current tokens equal to an old address that now maps elsewhere")
    stale = set()
    for v in tokens_of(pre_stacks + pre_self):
        new, _ = expected_value(v)
        if new is not None and new != v:
            stale.add(v)
    cur_tokens = tokens_of(stacks_txt + self_txt)
    hits = sorted(v for v in cur_tokens if v in stale)
    counts["F"] = len(stale)
    print(f"  stale old addresses: {len(stale)}; current tokens equal to one: {len(hits)} "
          f"{[hex(h) for h in hits]}")
    if hits:
        problems.append(f"F: stale values still present: {[hex(h) for h in hits]}")

    print("\n== summary")
    print(f"  A assertions: {counts['A']}  B fixture checks: {counts['B']}  C expected strings: {counts['C']}"
          f"  D tokens: {counts['D']} ({counts['D_changed']} changed)  E windows: {counts['E']}")
    if problems:
        print(f"  MISMATCHES: {len(problems)}")
        for p in problems:
            print(f"    - {p}")
        return 1
    print("  ALL MATCH")
    return 0


if __name__ == "__main__":
    sys.exit(main())
