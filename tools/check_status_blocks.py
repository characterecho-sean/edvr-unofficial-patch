#!/usr/bin/env python3
"""Check that every docs\\*.md `## Status` block is at most 60 lines.

AGENTS.md: an investigation doc starts with a `## Status` block of at most
60 lines, so a session reads the block and goes into the journal only where
it points. The block runs from the `## Status` heading (counted) to the next
`## ` heading, or to the end of the file. A fenced code block is skipped when
looking for the next heading, so a `## ` inside a fence does not end it.

    python tools\\check_status_blocks.py            # check docs\\*.md
    python tools\\check_status_blocks.py --list     # print every block length
    python tools\\check_status_blocks.py --self-test

Exit 0 when every block is within the limit, 1 when any is over, 2 on usage
or read errors. Read-only: this tool writes nothing.
"""
import os
import sys

LIMIT = 60


def status_block_length(text):
    """Lines from the `## Status` heading to the next `## ` heading,
    heading included; None when the doc has no Status block."""
    lines = text.splitlines()
    start = None
    fence = None
    for i, line in enumerate(lines):
        stripped = line.lstrip()
        if stripped.startswith("```") or stripped.startswith("~~~"):
            marker = stripped[:3]
            if fence is None:
                fence = marker
            elif marker == fence:
                fence = None
            continue
        if fence is not None:
            continue
        if start is None:
            if line.rstrip() == "## Status":
                start = i
        elif line.startswith("## "):
            return i - start
    if start is None:
        return None
    return len(lines) - start


def read_text(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8")


def measure(docs_dir):
    """[(name, length)] for every doc that has a Status block, name-sorted."""
    out = []
    for name in sorted(os.listdir(docs_dir)):
        if not name.lower().endswith(".md"):
            continue
        n = status_block_length(read_text(os.path.join(docs_dir, name)))
        if n is not None:
            out.append((name, n))
    return out


def self_test():
    def check(cond, what):
        if not cond:
            print("self-test FAILED: " + what)
            sys.exit(1)

    doc = "# T\n\n## Status\n\na\nb\n\n## Journal\n\nx\n"
    check(status_block_length(doc) == 5, "heading counted through the blank line")
    check(status_block_length("# T\n\n## Journal\nx\n") is None, "no Status block")
    check(status_block_length("## Status\na\nb") == 3, "block runs to end of file")
    fenced = "## Status\n```\n## not a heading\n```\nz\n## Next\n"
    check(status_block_length(fenced) == 5, "a fenced ## does not end the block")
    check(status_block_length("### Status\nx\n") is None, "### is not the block")
    check(status_block_length("## Status\r\na\r\n## J\r\n") == 2, "CRLF")
    check(status_block_length("## Status\n" + "l\n" * 59 + "## J\n") == 60, "60 is legal")
    check(status_block_length("## Status\n" + "l\n" * 60 + "## J\n") == 61, "61 is over")
    print("check_status_blocks self-test OK")


def main(argv):
    if "--self-test" in argv:
        self_test()
        return 0
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    docs_dir = os.path.join(root, "docs")
    if not os.path.isdir(docs_dir):
        print("no docs directory at " + docs_dir)
        return 2
    rows = measure(docs_dir)
    if "--list" in argv:
        for name, n in sorted(rows, key=lambda r: -r[1]):
            print("%4d  %s%s" % (n, name, "  OVER" if n > LIMIT else ""))
    over = [(name, n) for name, n in rows if n > LIMIT]
    for name, n in sorted(over, key=lambda r: -r[1]):
        print("docs\\%s: Status block is %d lines, limit %d" % (name, n, LIMIT))
    if over:
        print("%d of %d Status blocks over the %d-line limit" % (len(over), len(rows), LIMIT))
        return 1
    print("all %d Status blocks within %d lines" % (len(rows), LIMIT))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
