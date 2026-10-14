#!/usr/bin/env python3
"""Wall-clock phase timer for build.bat.

    python tools/build_phases.py --start FILE
    python tools/build_phases.py --mark FILE NAME
    python tools/build_phases.py --report FILE

--start writes FILE (JSON) with t0 = now.  --mark appends {name, t}.
--report prints each phase (time since the previous mark), its share of
the total, and the total.  A missing or corrupt FILE is a one-line note
and exit 0: a broken clock must never fail the build.  --dry-run with
any of the above writes nothing at all.
"""
import argparse
import json
import sys
import tempfile
import time
from pathlib import Path


def _write(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data), encoding="utf-8")


def _load(path):
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
        if not isinstance(data, dict) or "t0" not in data:
            return None
        data.setdefault("marks", [])
        return data
    except (OSError, ValueError):
        return None


def start(path, dry_run=False, now=None):
    t = time.time() if now is None else now
    if dry_run:
        print("[edvr] build_phases: dry-run, would start %s" % path)
        return 0
    try:
        _write(path, {"t0": t, "marks": []})
    except OSError as e:
        print("[edvr] build_phases: cannot write %s (%s)" % (path, e))
    return 0


def mark(path, name, dry_run=False, now=None):
    t = time.time() if now is None else now
    data = _load(path)
    if data is None:
        print("[edvr] build_phases: no clock at %s, mark '%s' skipped" % (path, name))
        return 0
    if dry_run:
        print("[edvr] build_phases: dry-run, would mark '%s'" % name)
        return 0
    data["marks"].append({"name": name, "t": t})
    try:
        _write(path, data)
    except OSError as e:
        print("[edvr] build_phases: cannot write %s (%s)" % (path, e))
    return 0


def phases(data):
    prev = data["t0"]
    rows = []
    for m in data["marks"]:
        rows.append((m["name"], m["t"] - prev))
        prev = m["t"]
    total = prev - data["t0"]
    return rows, total


def report(path, out=print):
    data = _load(path)
    if data is None:
        out("[edvr] build_phases: no usable clock at %s, no report" % path)
        return 0
    rows, total = phases(data)
    out("[edvr] build phases:")
    width = max([len(n) for n, _ in rows] + [5])
    for name, d in rows:
        share = (100.0 * d / total) if total > 0 else 0.0
        out("[edvr]   %-*s %8.1f s %5.1f%%" % (width, name, d, share))
    out("[edvr]   %-*s %8.1f s" % (width, "total", total))
    return 0


def self_test():
    import io
    import contextlib
    with tempfile.TemporaryDirectory() as td:
        f = Path(td) / "sub" / "p.json"
        assert start(f, now=100.0) == 0
        assert mark(f, "a", now=102.5) == 0
        assert mark(f, "b", now=110.0) == 0
        data = _load(f)
        rows, total = phases(data)
        assert [n for n, _ in rows] == ["a", "b"], rows
        assert abs(rows[0][1] - 2.5) < 1e-9 and abs(rows[1][1] - 7.5) < 1e-9
        assert abs(total - 10.0) < 1e-9
        lines = []
        report(f, out=lines.append)
        text = "\n".join(lines)
        assert all(l.startswith("[edvr] ") for l in lines), text
        assert "2.5 s" in text and " 25.0%" in text and " 75.0%" in text, text
        assert "total" in text and "10.0 s" in text, text
        # Missing and corrupt files never fail.
        missing = Path(td) / "nope.json"
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            assert mark(missing, "x") == 0
            assert report(missing) == 0
        assert not missing.exists()
        bad = Path(td) / "bad.json"
        bad.write_text("{not json", encoding="utf-8")
        with contextlib.redirect_stdout(buf):
            assert report(bad) == 0
            assert mark(bad, "x") == 0
        assert bad.read_text(encoding="utf-8") == "{not json"
        # Dry run writes nothing: patch every write path to raise.
        global _write
        real = _write

        def boom(*a, **k):
            raise AssertionError("dry-run wrote")
        _write = boom
        try:
            new = Path(td) / "newdir" / "d.json"
            with contextlib.redirect_stdout(buf):
                start(new, dry_run=True)
                mark(f, "c", dry_run=True)
                report(f)
            assert not new.exists() and not new.parent.exists()
            assert len(_load(f)["marks"]) == 2
        finally:
            _write = real
    print("build_phases self-test passed")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--start", metavar="FILE")
    ap.add_argument("--mark", nargs=2, metavar=("FILE", "NAME"))
    ap.add_argument("--report", metavar="FILE")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if a.start:
        return start(a.start, a.dry_run)
    if a.mark:
        return mark(a.mark[0], a.mark[1], a.dry_run)
    if a.report:
        return report(a.report)
    ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
