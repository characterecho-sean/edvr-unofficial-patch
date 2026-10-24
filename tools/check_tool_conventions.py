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
    steam_library.py; other tools resolve installs through it.

    python tools\\check_tool_conventions.py
    python tools\\check_tool_conventions.py --self-test

Exit 0 when every rule holds, 1 when any is broken (each named), 2 on usage
or read errors. Read-only: this tool writes nothing.
"""
import os
import re
import sys
import tempfile

STEAM_KEY = "Software\\Valve\\Steam"
STEAM_OWNER = "steam_library.py"
MAIN_RE = re.compile(r"""^if\s+__name__\s*==\s*["']__main__["']\s*:""", re.M)
DECLARES_RE = re.compile(r"""add_argument\(\s*["']--self-test|["']--self-test["']\s*(?:in|==|not\s+in)\s"""
                         r"""|(?:==|\bin)\s*[\(\[]?\s*["']--self-test""")


def gated_names(build_bat_text):
    """Tool file names build.bat runs with --self-test, in either form."""
    names = set()
    for m in re.finditer(r"""tools[\\/]([A-Za-z0-9_]+\.py)"?\s+--self-test\b""", build_bat_text):
        names.add(m.group(1).lower())
    return names


def violations(tools_dir, build_bat_path):
    with open(build_bat_path, "r", encoding="utf-8", errors="replace") as f:
        gated = gated_names(f.read())
    found = []
    for name in sorted(os.listdir(tools_dir)):
        if not name.endswith(".py"):
            continue
        with open(os.path.join(tools_dir, name), "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        if STEAM_KEY in text and name != STEAM_OWNER:
            found.append("tools\\%s: names the Steam registry key; resolve installs through %s "
                         "(install_edvr.resolve_target)" % (name, STEAM_OWNER))
        if not MAIN_RE.search(text):
            continue
        if not DECLARES_RE.search(text):
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
            "steam_library.py": 'KEY = r"Software\\Valve\\Steam"\np.add_argument("--self-test")' + main,
            "own_steam.py": 'KEY = r"Software\\Valve\\Steam"\n',
        }
        for name, text in files.items():
            with open(os.path.join(tools, name), "w", encoding="utf-8") as f:
                f.write(text)
        bat = os.path.join(td, "build.bat")
        with open(bat, "w", encoding="utf-8") as f:
            f.write('python tools\\run_jobs.py ^\n    --gate "tools\\gated_gate.py --self-test" ^\n'
                    '    --gate "tools\\steam_library.py --self-test"\n'
                    'python "tools\\gated_plain.py" --self-test || exit /b 1\n')
        got = "\n".join(violations(tools, bat))
        check("gated_gate.py" not in got, "a run_jobs --gate counts as gated")
        check("gated_plain.py" not in got, "a plain python line counts as gated")
        check("ungated.py: --self-test is not run" in got, "an ungated tool is reported")
        check("no_selftest.py: a tool without --self-test" in got, "a tool without --self-test is reported")
        check("tools\\library.py" not in got, "a module without __main__ is not a tool")
        check("own_steam.py: names the Steam registry key" in got, "a second Steam search is reported")
        check("tools\\steam_library.py" not in got, "steam_library.py may name the Steam key")
        check(len(got.splitlines()) == 3, "exactly the three planted violations are reported")
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
