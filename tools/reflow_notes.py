#!/usr/bin/env python3
"""Reflow release notes and docs to a fixed column, without wrecking them.

    python tools/reflow_notes.py docs/per-object-motion.md
    python tools/reflow_notes.py --check docs/*.md
    python tools/reflow_notes.py --in-place --dry-run notes.md
    python tools/reflow_notes.py --in-place --width 72 notes.md

Written for the pass over the published release notes, which had been
hand-wrapped at whatever column the window happened to be, and which read
badly on GitHub as a result. It is the sanctioned replacement for doing
that in an editor or with an ad-hoc regex.

What it will NOT touch, because reflowing any of them changes meaning:

  * fenced code (``` and ~~~), including the long lines inside it;
  * indented code blocks;
  * tables -- any line with a pipe in it;
  * headings, horizontal rules, and link reference definitions;
  * HTML blocks;
  * a line already inside something it decided to leave alone.

Lists and block quotes ARE reflowed, keeping their marker and hanging
indent, because that is most of a release note.

Encoding is UTF-8 in and UTF-8 out, always, decoded and encoded here
rather than left to the console codepage -- PowerShell 5.1 reads a
BOM-less UTF-8 file as the ANSI codepage, which has turned an em-dash into
mojibake on a published comment before. A file that arrives with a BOM
loses it, and the run says so. Line endings are preserved as found.

Writing is opt-in: by default the reflowed text goes to stdout. --in-place
rewrites the file; --dry-run with it prints a unified diff and writes
nothing. --check writes nothing ever and exits 1 if any file would change,
which is the form to put in a build.

Exit 0 when nothing failed, 1 when a file would change under --check or
could not be read.
"""

import argparse
import difflib
import os
import re
import sys
import tempfile
import textwrap

FENCE_RE = re.compile(r"^\s{0,3}(```+|~~~+)")
HEADING_RE = re.compile(r"^\s{0,3}#{1,6}\s")
RULE_RE = re.compile(r"^\s{0,3}([-*_])(\s*\1){2,}\s*$")
LINKDEF_RE = re.compile(r"^\s{0,3}\[[^\]]+\]:\s")
HTML_RE = re.compile(r"^\s{0,3}<")
# `- `, `* `, `+ `, `1. `, `1) ` -- the marker plus the indent that
# continuation lines have to line up under.
LIST_RE = re.compile(r"^(?P<indent>\s*)(?P<marker>[-*+]|\d+[.)])(?P<gap>\s+)")
QUOTE_RE = re.compile(r"^(?P<prefix>\s{0,3}(?:>\s?)+)(?P<rest>.*)$")


def _is_quote(line):
    """QUOTE_RE needs at least one `>`, so it returns None on ordinary
    prose. Asking for .group() on that is an AttributeError in the middle
    of a paragraph, which is the whole of this helper's reason to exist."""
    m = QUOTE_RE.match(line)
    return bool(m and m.group("prefix").strip())


def decode(data):
    """(text, had_bom). utf-8-sig strips a BOM if one is there."""
    if data.startswith(b"\xef\xbb\xbf"):
        return data[3:].decode("utf-8", errors="replace"), True
    return data.decode("utf-8", errors="replace"), False


def dominant_newline(text):
    crlf = text.count("\r\n")
    lf = text.count("\n") - crlf
    return "\r\n" if crlf >= lf else "\n"


def _wrap(text, width, initial, subsequent):
    if not text.strip():
        return []
    return textwrap.wrap(
        text, width=width, initial_indent=initial,
        subsequent_indent=subsequent,
        break_long_words=False,   # a URL is not a place to break
        break_on_hyphens=False,   # nor is a hyphenated shader name
    ) or [initial.rstrip()]


def _verbatim(line):
    """Lines that are left exactly as they are."""
    if not line.strip():
        return True
    if HEADING_RE.match(line) or RULE_RE.match(line):
        return True
    if LINKDEF_RE.match(line) or HTML_RE.match(line):
        return True
    if "|" in line:                    # a table row, or near enough
        return True
    if line.startswith("    ") and not LIST_RE.match(line):
        return True                    # indented code
    return False


def reflow(text, width=79):
    """Reflow prose in `text`. Newline-agnostic: works on a list of lines."""
    lines = text.replace("\r\n", "\n").split("\n")
    out = []
    i = 0
    in_fence = None

    while i < len(lines):
        line = lines[i]

        # Fenced code: copy through, closing fence included, untouched.
        m = FENCE_RE.match(line)
        if in_fence is None and m:
            in_fence = m.group(1)[0] * 3
            out.append(line)
            i += 1
            while i < len(lines):
                out.append(lines[i])
                closed = FENCE_RE.match(lines[i]) and \
                    lines[i].strip().startswith(in_fence)
                i += 1
                if closed:
                    break
            in_fence = None
            continue

        if _verbatim(line):
            out.append(line)
            i += 1
            continue

        # A block quote keeps its `> ` on every line it produces.
        if _is_quote(line):
            q = QUOTE_RE.match(line)
            prefix = q.group("prefix")
            body = [q.group("rest").strip()]
            i += 1
            while i < len(lines):
                nq = QUOTE_RE.match(lines[i])
                if not nq or not nq.group("prefix").strip() or \
                        not nq.group("rest").strip():
                    break
                body.append(nq.group("rest").strip())
                i += 1
            out.extend(_wrap(" ".join(body), width, prefix, prefix))
            continue

        # A list item: the marker on the first line, its width as the
        # hanging indent on the rest.
        lm = LIST_RE.match(line)
        if lm:
            initial = lm.group("indent") + lm.group("marker") + lm.group("gap")
            subsequent = " " * len(initial)
            body = [line[len(initial):].strip()]
            i += 1
            # Continuation lines are indented at least as far as the text.
            while i < len(lines):
                nxt = lines[i]
                if not nxt.strip() or LIST_RE.match(nxt) or _verbatim(nxt) \
                        or FENCE_RE.match(nxt):
                    break
                if len(nxt) - len(nxt.lstrip()) < len(lm.group("indent")) + 1:
                    break
                body.append(nxt.strip())
                i += 1
            out.extend(_wrap(" ".join(body), width, initial, subsequent))
            continue

        # An ordinary paragraph.
        indent = line[:len(line) - len(line.lstrip())]
        body = [line.strip()]
        i += 1
        while i < len(lines):
            nxt = lines[i]
            if not nxt.strip() or _verbatim(nxt) or FENCE_RE.match(nxt) \
                    or LIST_RE.match(nxt) or _is_quote(nxt):
                break
            body.append(nxt.strip())
            i += 1
        out.extend(_wrap(" ".join(body), width, indent, indent))

    return "\n".join(out)


def process(path, width, in_place, dry_run, check, out=sys.stdout):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        print("[edvr] cannot read %s: %s" % (path, e), file=out)
        return 1, False

    text, had_bom = decode(data)
    nl = dominant_newline(text)
    new = reflow(text, width)
    # reflow() works in \n; put the file's own endings back.
    old_norm = text.replace("\r\n", "\n")
    changed = (new != old_norm) or had_bom

    if check:
        if changed:
            print("[edvr] would change: %s%s"
                  % (path, " (has a UTF-8 BOM)" if had_bom else ""), file=out)
        return 0, changed

    if not in_place:
        out.write(new.replace("\n", nl))
        if not new.endswith("\n"):
            out.write(nl)
        return 0, changed

    if dry_run:
        if changed:
            print("[edvr] %s would change:" % path, file=out)
            for d in difflib.unified_diff(
                    old_norm.splitlines(), new.splitlines(),
                    fromfile=path + " (now)", tofile=path + " (reflowed)",
                    lineterm=""):
                print(d, file=out)
        else:
            print("[edvr] %s is already reflowed." % path, file=out)
        print("[edvr] dry run: wrote nothing.", file=out)
        return 0, changed

    if not changed:
        print("[edvr] %s unchanged." % path, file=out)
        return 0, False

    body = new.replace("\n", nl)
    if not body.endswith(nl):
        body += nl
    with open(path, "wb") as f:
        f.write(body.encode("utf-8"))       # no BOM, on purpose
    print("[edvr] reflowed %s%s"
          % (path, " (dropped a UTF-8 BOM)" if had_bom else ""), file=out)
    return 0, True


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Reflow markdown prose to a fixed column.")
    ap.add_argument("files", nargs="*", help="markdown files")
    ap.add_argument("--width", type=int, default=79,
                    help="column to wrap at (default 79)")
    ap.add_argument("--in-place", action="store_true",
                    help="rewrite the files instead of printing")
    ap.add_argument("--dry-run", action="store_true",
                    help="with --in-place, show a diff and write nothing")
    ap.add_argument("--check", action="store_true",
                    help="write nothing; exit 1 if any file would change")
    ap.add_argument("--self-test", action="store_true",
                    help="check this script against itself and exit")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.files:
        ap.error("name at least one file")
    if args.dry_run and not args.in_place:
        # Without --in-place nothing is written anyway; saying --dry-run
        # would suggest a write was averted that was never going to happen.
        ap.error("--dry-run only means something with --in-place")

    rc, any_changed = 0, False
    for path in args.files:
        r, changed = process(path, args.width, args.in_place, args.dry_run,
                             args.check)
        rc = rc or r
        any_changed = any_changed or changed
    if args.check and any_changed:
        return 1
    return rc


def self_test():
    ok = True

    def check(label, got, want):
        if got != want:
            print("%s:\n--- got ---\n%s\n--- want ---\n%s" % (label, got, want))
            return False
        return True

    # reflow() maps lines to lines, so a file ending in a newline still
    # ends in one afterwards. Every expectation below carries it.

    # A fence, with a long line in it that must survive untouched.
    long_code = "    " + "x" * 120
    src = "```\n%s\n```\n" % long_code
    ok &= check("fenced code", reflow(src, 40), src)

    # A table row is left alone however long it is.
    table = "| a | b |\n|---|---|\n| %s | y |\n" % ("z" * 90)
    ok &= check("table", reflow(table, 40), table)

    # Headings and rules survive.
    ok &= check("heading", reflow("## A heading that is quite long here\n", 20),
                "## A heading that is quite long here\n")

    # Prose wraps.
    got = reflow("one two three four five six seven eight nine ten\n", 20)
    ok &= check("paragraph", got, "one two three four\nfive six seven eight\n"
                                  "nine ten\n")

    # A list keeps its marker and hangs its continuation under the text.
    got = reflow("- alpha beta gamma delta epsilon zeta eta\n", 20)
    ok &= check("list", got,
                "- alpha beta gamma\n  delta epsilon zeta\n  eta\n")

    # A block quote keeps its prefix on every line.
    got = reflow("> alpha beta gamma delta epsilon\n", 20)
    ok &= check("quote", got, "> alpha beta gamma\n> delta epsilon\n")

    # A URL is never broken, even past the column.
    got = reflow("see https://issues.frontierstore.net/issue-detail/69074 ok\n",
                 20)
    if "issue-detail/69074" not in got or "issue-\ndetail" in got:
        print("a URL was broken:\n%s" % got)
        ok = False

    # Reflowing twice changes nothing the second time. This is what makes
    # --check meaningful: a file it passes must not change on a rerun.
    once = reflow("alpha beta gamma delta epsilon zeta eta theta\n", 20)
    twice = reflow(once, 20)
    if twice != once:
        print("reflow is not idempotent:\n%r\n%r" % (once, twice))
        ok = False

    tmp = tempfile.mkdtemp(prefix="edvr_reflow_test_")
    try:
        import io
        import shutil
        path = os.path.join(tmp, "notes.md")
        original = ("one two three four five six seven eight nine ten\n"
                    ).encode("utf-8")
        with open(path, "wb") as f:
            f.write(original)

        # --check writes nothing and exits 1 when a file would change.
        buf = io.StringIO()
        rc, changed = process(path, 20, False, False, True, out=buf)
        if open(path, "rb").read() != original:
            print("--check modified the file")
            ok = False
        if not changed:
            print("--check did not notice a file that would change")
            ok = False
        if main(["--check", "--width", "20", path]) != 1:
            print("--check on a changing file did not exit 1")
            ok = False

        # --in-place --dry-run writes nothing either.
        buf = io.StringIO()
        process(path, 20, True, True, False, out=buf)
        if open(path, "rb").read() != original:
            print("--in-place --dry-run modified the file")
            ok = False
        if "wrote nothing" not in buf.getvalue():
            print("--dry-run did not say it wrote nothing")
            ok = False

        # --in-place does write, as UTF-8 with no BOM.
        process(path, 20, True, False, False, out=io.StringIO())
        written = open(path, "rb").read()
        if written == original:
            print("--in-place did not rewrite the file")
            ok = False
        if written.startswith(b"\xef\xbb\xbf"):
            print("--in-place wrote a BOM")
            ok = False
        if main(["--check", "--width", "20", path]) != 0:
            print("a reflowed file still reports as changing")
            ok = False

        # A file that arrives with a BOM loses it and is reported.
        bom = os.path.join(tmp, "bom.md")
        with open(bom, "wb") as f:
            f.write(b"\xef\xbb\xbfshort line\n")
        buf = io.StringIO()
        process(bom, 40, True, False, False, out=buf)
        if open(bom, "rb").read().startswith(b"\xef\xbb\xbf"):
            print("the BOM survived")
            ok = False
        if "BOM" not in buf.getvalue():
            print("dropping the BOM was not reported")
            ok = False

        # An em-dash round-trips as itself, which is the mojibake case.
        em = os.path.join(tmp, "em.md")
        with open(em, "wb") as f:
            f.write("a \u2014 dash\n".encode("utf-8"))
        process(em, 40, True, False, False, out=io.StringIO())
        if open(em, "rb").read().decode("utf-8") != "a \u2014 dash\n":
            print("the em-dash did not survive the round trip")
            ok = False
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)

    print("self-test: %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
