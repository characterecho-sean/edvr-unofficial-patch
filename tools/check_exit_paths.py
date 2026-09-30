#!/usr/bin/env python3
"""Cleanup that matters must run on PROCESS EXIT, not only on FreeLibrary.

WHY THIS EXISTS

A game closing is process termination. DllMain gets DLL_PROCESS_DETACH with
`reserved != NULL`, and the FreeLibrary branch -- the one that calls shutdown()
-- never runs. This codebase has now paid for that fact three times:

  1. The running totals were logged from shutdown(), so no session ever printed
     them. Fixed by moving them onto a timer.
  2. The guard's fault totals, the same way, fixed the same way.
  3. The crash sentinel, 2026-08-17. shutdownDeviceHooks() confirmed it on a
     clean unload, with a comment stating exactly why that was necessary --
     "a session that ends cleanly inside the first six seconds would arm the
     next launch's refusal" -- and that call sat on the FreeLibrary path. So a
     five-second session at 07:27 armed the sentinel, the 09:53 launch reported
     SENTINEL TRIPPED, every d3d11 fix switched itself off, and the headset
     showed a grey void. The reasoning was right, was written down, and was
     wired to the branch that does not execute.

The third one is the reason this file exists rather than a fourth comment. A
fix on an unreachable path is worse than a missing fix: it reads as handled.

WHAT IT CHECKS

For each entry below: the named function must be called from inside the
PROCESS-TERMINATION branch of that file's DllMain -- the `reserved != nullptr`
side -- and not merely from the else branch or from shutdown().

The parse is deliberately dumb: find `case DLL_PROCESS_DETACH`, find the
`if (reserved != nullptr) {` inside it, and take the text up to the matching
`} else`. Anything cleverer would need a C++ parser, and anything vaguer would
stop catching the thing it is here for.

`--self-test` runs the check over DllMain fixtures in the temp folder -- the
call on the exit branch, the call only on the FreeLibrary branch (the 2026-08-17
bug), nested braces, a restructured DllMain -- so a parse that drifts fails in
the build and not in the field.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (file, function that must run on process exit, why it matters)
REQUIRED = [
    ("src/d3d11/d3d11_proxy.cpp", "deviceHookNoteCleanExit",
     "the crash sentinel stays armed, and the NEXT launch disables every "
     "d3d11 fix over a clean quit that just happened to be short"),
]


def termination_branch(text):
    """The body of the `reserved != nullptr` branch of DLL_PROCESS_DETACH."""
    at = text.find("DLL_PROCESS_DETACH")
    if at < 0:
        return None
    m = re.compile(r"if\s*\(\s*reserved\s*!=\s*(?:nullptr|NULL)\s*\)\s*\{").search(text, at)
    if not m:
        return None
    depth = 1
    i = m.end()
    while i < len(text) and depth:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    return text[m.end():i]


def main():
    bad = 0
    for rel, func, why in REQUIRED:
        path = os.path.join(ROOT, rel.replace("/", os.sep))
        if not os.path.exists(path):
            print("  FAIL  %s is missing, so %s cannot be checked" % (rel, func))
            bad += 1
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        branch = termination_branch(text)
        if branch is None:
            print("  FAIL  %s: could not find the reserved != nullptr branch of "
                  "DLL_PROCESS_DETACH. If DllMain was restructured, update this "
                  "check rather than deleting it." % rel)
            bad += 1
            continue
        if (func + "(") not in branch:
            print("  FAIL  %s: %s() is not called on the process-exit path.\n"
                  "        A game closing is process termination -- the "
                  "FreeLibrary branch never runs -- so %s."
                  % (rel, func, why))
            bad += 1
            continue
        print("  ok    %s() runs on process exit (%s)" % (func, rel))
    if bad:
        print("\nEXIT PATH CHECK FAILED (%d)" % bad)
        return 1
    print("EXIT PATH CHECK PASSED")
    return 0


def self_test():
    """The check over fixtures laid out in the temp folder. Each DllMain
    below is one shape the real file could take; the check must pass exactly
    the ones that run the function on the process-exit branch."""
    import contextlib
    import io
    import shutil
    import tempfile

    global ROOT, REQUIRED
    saved = (ROOT, REQUIRED)
    base = tempfile.mkdtemp(prefix="edvr-exit-paths-")
    failures = []

    def dllmain(exit_branch, else_branch, prologue="", keyword="nullptr"):
        return (
            prologue +
            "BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {\n"
            "    switch (reason) {\n"
            "    case DLL_PROCESS_ATTACH:\n"
            "        init();\n"
            "        break;\n"
            "    case DLL_PROCESS_DETACH:\n"
            "        if (reserved != %s) {\n"
            "%s"
            "        } else {\n"
            "%s"
            "        }\n"
            "        break;\n"
            "    }\n"
            "    return TRUE;\n"
            "}\n" % (keyword, exit_branch, else_branch))

    def run(name, text, func="noteCleanExit"):
        """The check's verdict on one file (None = the file is absent)."""
        global ROOT, REQUIRED
        root = os.path.join(base, name)
        os.makedirs(os.path.join(root, "src"))
        if text is not None:
            with open(os.path.join(root, "src", "proxy.cpp"), "w", encoding="utf-8") as f:
                f.write(text)
        ROOT, REQUIRED = root, [("src/proxy.cpp", func, "the sentinel stays armed")]
        said = io.StringIO()
        with contextlib.redirect_stdout(said):
            code = main()
        return code, said.getvalue()

    def expect(name, code, said, want_code, needle):
        if code != want_code:
            failures.append("%s: exit %d, expected %d\n%s" % (name, code, want_code, said))
        elif needle not in said:
            failures.append("%s: expected %r in\n%s" % (name, needle, said))

    try:
        # The call on the process-exit branch passes; both spellings of the
        # null pointer, and with other statements and nested braces before it.
        code, said = run("on-exit", dllmain("            noteCleanExit();\n",
                                            "            shutdown();\n"))
        expect("on-exit", code, said, 0, "noteCleanExit() runs on process exit")
        code, said = run("on-exit-NULL", dllmain("            noteCleanExit();\n",
                                                 "            shutdown();\n", keyword="NULL"))
        expect("on-exit-NULL", code, said, 0, "EXIT PATH CHECK PASSED")
        code, said = run("on-exit-after-nested-braces", dllmain(
            "            if (armed) {\n"
            "                disarm();\n"
            "            }\n"
            "            noteCleanExit();\n",
            "            shutdown();\n"))
        expect("on-exit-after-nested-braces", code, said, 0, "EXIT PATH CHECK PASSED")

        # The 2026-08-17 bug: the call is on the FreeLibrary branch, which a
        # closing game never runs. The exit branch does other work, so a check
        # that merely found the name anywhere in DllMain would pass it.
        code, said = run("on-free-library-only", dllmain("            flushLogs();\n",
                                                          "            noteCleanExit();\n"))
        expect("on-free-library-only", code, said, 1, "noteCleanExit() is not called on the process-exit path")
        expect("on-free-library-only", code, said, 1, "EXIT PATH CHECK FAILED (1)")
        # ... or only inside shutdown(), somewhere else in the file.
        code, said = run("in-shutdown-only", dllmain(
            "            flushLogs();\n", "            shutdown();\n",
            prologue="void shutdown() { noteCleanExit(); }\n"))
        expect("in-shutdown-only", code, said, 1, "is not called on the process-exit path")
        # The closing brace of a nested block must not end the branch early,
        # nor its else swallow the FreeLibrary branch into the exit branch.
        code, said = run("nested-then-else", dllmain(
            "            if (armed) {\n"
            "                disarm();\n"
            "            }\n",
            "            noteCleanExit();\n"))
        expect("nested-then-else", code, said, 1, "is not called on the process-exit path")
        # A call whose name merely starts with the function's is not the call.
        code, said = run("prefix-name", dllmain("            noteCleanExitLater();\n",
                                                "            shutdown();\n"))
        expect("prefix-name", code, said, 1, "is not called on the process-exit path")

        # A restructured DllMain must fail loudly, not pass for want of a branch.
        code, said = run("no-detach", "BOOL WINAPI DllMain() { return TRUE; }\n")
        expect("no-detach", code, said, 1, "could not find the reserved != nullptr branch")
        code, said = run("no-reserved-branch",
                         "BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {\n"
                         "    if (reason == DLL_PROCESS_DETACH) { noteCleanExit(); }\n"
                         "    return TRUE;\n}\n")
        expect("no-reserved-branch", code, said, 1, "could not find the reserved != nullptr branch")
        # ... and a moved file the same.
        code, said = run("missing-file", None)
        expect("missing-file", code, said, 1, "src/proxy.cpp is missing")
    finally:
        ROOT, REQUIRED = saved
        shutil.rmtree(base, ignore_errors=True)

    if failures:
        print("check_exit_paths: self-test FAILED")
        for f in failures:
            print("  " + f.replace("\n", "\n    "))
        return 1
    print("check_exit_paths: self-test OK")
    return 0


if __name__ == "__main__":
    sys.exit(self_test() if "--self-test" in sys.argv[1:] else main())
