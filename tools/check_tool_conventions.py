#!/usr/bin/env python3
"""Check the mechanical rules AGENTS.md sets for tools\\*.py.

AGENTS.md: every tool in tools\\ carries --self-test and build.bat gates on
it, and steam_library.py is the one Steam search. A new tool that skipped
either rule reached review twice (PR 121); this makes both fail the build.

A tool is a tools\\*.py with an `if __name__ == "__main__":` block; a module
without one (rig_mutants_lib.py) is a library and is not checked.

  * every tool declares --self-test;
  * build.bat runs it with --self-test, as a run_jobs gate
    (`--gate "tools\\x.py --self-test"`) or a plain line
    (`python "tools\\x.py" --self-test`);
  * the Steam registry key (Software\\Valve\\Steam) appears in no tool but
    steam_library.py and this checker; other tools resolve installs through
    steam_library.py.

    python tools\\check_tool_conventions.py
    python tools\\check_tool_conventions.py --self-test

Exit 0 when every rule holds, 1 when any is broken (each named), 2 on usage
or read errors. Read-only: this tool writes nothing.
"""
import ast
import os
import re
import sys
import tempfile

STEAM_KEY = "Software\\Valve\\Steam"
# steam_library.py owns the key; this file names it because it checks for it.
STEAM_OWNERS = ("steam_library.py", "check_tool_conventions.py")
STEAM_OWNER = STEAM_OWNERS[0]
# A REM or :: that starts a command (line start, or after & && || | or "(") comments out the rest of the line.
REM_RE = re.compile(r"(?:^|[&|(])\s*@?\s*(?:rem(?=\s|$)|::)", re.I)


def gated_names(build_bat_text):
    """Tool file names build.bat runs with --self-test, in either form.

    A REM or :: line, or the part of a line after an inline `& rem` / `( rem`,
    is a comment, not a command, and is skipped."""
    names = set()
    for line in build_bat_text.splitlines():
        c = REM_RE.search(line)
        for m in re.finditer(r"""tools[\\/]([A-Za-z0-9_]+\.py)"?\s+--self-test\b""", line):
            if c and m.start() >= c.start():
                continue
            names.add(m.group(1).lower())
    return names


def _is_str(node, value=None):
    return isinstance(node, ast.Constant) and isinstance(node.value, str) and (value is None or node.value == value)


def _docstring_nodes(tree):
    """The string-constant nodes that are docstrings or bare string statements."""
    return {id(n.value) for n in ast.walk(tree) if isinstance(n, ast.Expr) and _is_str(n.value)}


def _has_main_guard(tree):
    for node in tree.body:
        if isinstance(node, ast.If) and isinstance(node.test, ast.Compare) and len(node.test.ops) == 1 \
                and isinstance(node.test.ops[0], ast.Eq):
            sides = [node.test.left, node.test.comparators[0]]
            if any(isinstance(s, ast.Name) and s.id == "__name__" for s in sides) \
                    and any(_is_str(s, "__main__") for s in sides):
                return True
    return False


def _declares_self_test(tree):
    """add_argument("--self-test"...) or a comparison against "--self-test", as code."""
    for node in ast.walk(tree):
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) \
                and node.func.attr == "add_argument" and node.args and _is_str(node.args[0], "--self-test"):
            return True
        if isinstance(node, ast.Compare):
            for side in [node.left] + node.comparators:
                members = side.elts if isinstance(side, (ast.Tuple, ast.List, ast.Set)) else [side]
                if any(_is_str(m, "--self-test") for m in members):
                    return True
    return False


def _fold(node):
    """The string a node builds at compile time, or None. Handles str constants,
    `+` of foldable strings, constant-only f-strings and "sep".join(<tuple/list>);
    nothing else is evaluated."""
    if _is_str(node):
        return node.value
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        a, b = _fold(node.left), _fold(node.right)
        return a + b if a is not None and b is not None else None
    if isinstance(node, ast.JoinedStr):
        parts = [_fold(v) for v in node.values]
        return None if None in parts else "".join(parts)
    if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and node.func.attr == "join" \
            and _is_str(node.func.value) and len(node.args) == 1 and not node.keywords \
            and isinstance(node.args[0], (ast.Tuple, ast.List)):
        parts = [_fold(e) for e in node.args[0].elts]
        return None if None in parts else node.func.value.value.join(parts)
    return None


def _names_steam_key(tree):
    """Registry key names are case-insensitive, so compare folded case."""
    docs = _docstring_nodes(tree)
    key = STEAM_KEY.lower()
    for n in ast.walk(tree):
        if id(n) in docs:
            continue
        v = _fold(n)
        if v is not None and key in v.lower():
            return True
    return False


def violations(tools_dir, build_bat_path):
    with open(build_bat_path, "r", encoding="utf-8", errors="replace") as f:
        gated = gated_names(f.read())
    found = []
    for name in sorted(os.listdir(tools_dir)):
        if not name.endswith(".py"):
            continue
        with open(os.path.join(tools_dir, name), "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        try:
            tree = ast.parse(text, name)
        except (SyntaxError, ValueError) as exc:
            found.append("tools\\%s: does not parse (%s)" % (name, exc))
            continue
        if name not in STEAM_OWNERS and _names_steam_key(tree):
            found.append("tools\\%s: names the Steam registry key; resolve installs through %s "
                         "(install_edvr.resolve_target)" % (name, STEAM_OWNER))
        if not _has_main_guard(tree):
            continue
        if not _declares_self_test(tree):
            found.append("tools\\%s: a tool without --self-test" % name)
        elif name.lower() not in gated:
            found.append("tools\\%s: --self-test is not run by build.bat; add "
                         "--gate \"tools\\%s --self-test\" to its gate list" % (name, name))
    return found


def self_test():
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    main = '\nif __name__ == "__main__":\n    main()\n'
    with tempfile.TemporaryDirectory() as td:
        tools = os.path.join(td, "tools")
        os.makedirs(tools)
        files = {
            "gated_gate.py": 'p.add_argument("--self-test")' + main,
            "gated_plain.py": 'if "--self-test" in sys.argv: pass' + main,
            "ungated.py": 'p.add_argument("--self-test", action="store_true")' + main,
            "no_selftest.py": 'p.add_argument("--dry-run")' + main,
            "library.py": 'RIG_ARGS = ["--self-test"]\n',
            "steam_library.py": 'KEY = r"%s"\np.add_argument("--self-test")' % STEAM_KEY + main,
            "own_steam.py": 'KEY = r"%s"\n' % STEAM_KEY,
            # The same key spelled with ordinary escapes: source text differs, value is equal.
            "escaped_steam.py": 'KEY = "Software\\\\Valve\\\\Steam"\n',
            # A docstring naming the key is prose, not a lookup.
            "doc_steam.py": '"""Do not use Software\\\\Valve\\\\Steam here."""\n',
            # A comment is not a declaration.
            "comment_selftest.py": '# p.add_argument("--self-test")\np.add_argument("--dry-run")' + main,
            # A main-guard example inside a docstring does not make a library a tool.
            "doc_main_library.py": '"""Example:\n\nif __name__ == "__main__":\n    main()\n"""\nX = 1\n',
            # Its only gate is commented out.
            "rem_gated.py": 'p.add_argument("--self-test")' + main,
            "colon_gated.py": 'p.add_argument("--self-test")' + main,
            # Gate text after an inline rem is commented out.
            "amp_rem.py": 'p.add_argument("--self-test")' + main,
            "paren_rem.py": 'p.add_argument("--self-test")' + main,
            # Not commented out: a tool named like rem, and a gate before a trailing rem.
            "remote_x.py": 'p.add_argument("--self-test")' + main,
            "before_rem.py": 'p.add_argument("--self-test")' + main,
            # The Steam key in other case, and built at compile time.
            "lower_steam.py": 'KEY = r"%s"\n' % STEAM_KEY.lower(),
            "plus_steam.py": 'KEY = "Software\\\\" + ("Valve" + "\\\\Steam")\n',
            "join_tuple_steam.py": 'KEY = "\\\\".join(("Software", "Valve", "Steam"))\n',
            "join_list_steam.py": 'KEY = "\\\\".join(["Software", "Valve", "Steam"])\n',
            # Built strings that are not the key.
            "other_join.py": 'KEY = "\\\\".join(("Software", "Valve", "Other")) + "Steam"\n',
        }
        for name, text in files.items():
            with open(os.path.join(tools, name), "w", encoding="utf-8") as f:
                f.write(text)
        bat = os.path.join(td, "build.bat")
        with open(bat, "w", encoding="utf-8") as f:
            f.write('python tools\\run_jobs.py ^\n    --gate "tools\\gated_gate.py --self-test" ^\n'
                    '    --gate "tools\\steam_library.py --self-test"\n'
                    'python "tools\\gated_plain.py" --self-test || exit /b 1\n'
                    'REM python tools\\rem_gated.py --self-test\n'
                    '  :: python tools\\colon_gated.py --self-test\n'
                    'echo x & rem python tools\\amp_rem.py --self-test\n'
                    'if 1==1 ( rem python tools\\paren_rem.py --self-test )\n'
                    'echo x & python "tools\\remote_x.py" --self-test\n'
                    'python "tools\\before_rem.py" --self-test & rem done\n')
        got = "\n".join(violations(tools, bat))
        check("escaped_steam.py: names the Steam registry key" in got, "an escaped Steam key literal is reported")
        check("doc_steam.py" not in got, "a docstring naming the Steam key is not reported")
        check("comment_selftest.py: a tool without --self-test" in got, "a commented declaration does not count")
        check("doc_main_library.py" not in got, "a main guard inside a docstring is not a tool")
        check("rem_gated.py: --self-test is not run" in got, "a REM line is not a gate")
        check("colon_gated.py: --self-test is not run" in got, "a :: line is not a gate")
        check("gated_gate.py" not in got, "a run_jobs --gate counts as gated")
        check("gated_plain.py" not in got, "a plain python line counts as gated")
        check("ungated.py: --self-test is not run" in got, "an ungated tool is reported")
        check("no_selftest.py: a tool without --self-test" in got, "a tool without --self-test is reported")
        check("tools\\library.py" not in got, "a module without __main__ is not a tool")
        check("own_steam.py: names the Steam registry key" in got, "a second Steam search is reported")
        check("tools\\steam_library.py" not in got, "steam_library.py may name the Steam key")
        check("amp_rem.py: --self-test is not run" in got, "text after & rem is not a gate")
        check("paren_rem.py: --self-test is not run" in got, "text after ( rem is not a gate")
        check("remote_x.py" not in got, "a tool named remote_x.py is gated, not a rem comment")
        check("before_rem.py" not in got, "a gate before a trailing rem still counts")
        check("lower_steam.py: names the Steam registry key" in got, "a lowercase Steam key is reported")
        check("plus_steam.py: names the Steam registry key" in got, "a + built Steam key is reported")
        check("join_tuple_steam.py: names the Steam registry key" in got, "a .join over a tuple is reported")
        check("join_list_steam.py: names the Steam registry key" in got, "a .join over a list is reported")
        check("other_join.py" not in got, "a built string that is not the key is not reported")
        check(len(got.splitlines()) == 13, "exactly the thirteen planted violations are reported")
    for f in failures:
        print("check_tool_conventions self-test FAIL: %s" % f)
    print("check_tool_conventions self-test %s" % ("FAILED" if failures else "OK"))
    return 1 if failures else 0


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    if argv:
        print(__doc__)
        return 2
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    try:
        found = violations(os.path.join(repo, "tools"), os.path.join(repo, "build.bat"))
    except OSError as exc:
        print("check_tool_conventions: %s" % exc)
        return 2
    for line in found:
        print(line)
    print("tool conventions: %s" % ("%d violation(s)" % len(found) if found else "all tools gated, one Steam search"))
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
