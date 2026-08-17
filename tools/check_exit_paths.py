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


if __name__ == "__main__":
    sys.exit(main())
