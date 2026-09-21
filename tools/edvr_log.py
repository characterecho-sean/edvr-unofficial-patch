#!/usr/bin/env python3
"""Find the log a flight just wrote, and say whether it is the build you made.

    python tools/edvr_log.py --target steam --version
    python tools/edvr_log.py --target steam --expect-build HEAD
    python tools/edvr_log.py --target steam --grep "Stats\\[4[0-9]\\]"
    python tools/edvr_log.py --target steam --tail 80
    python tools/edvr_log.py --target frontier --tally vh
    python tools/edvr_log.py --target frontier --tally vh --frame 1
    python tools/edvr_log.py --list

This is the sanctioned replacement for `Get-Content <some path> -Tail 200 |
Select-String ...`, retyped once a flight for a month.

The first line of any flight-log analysis is not "what do the counters
say", it is "is this log from the build I just installed". EDVR writes
that into every log it opens:

    version 0.14.1-93-gf78eba4 (build 68C0A1F2) -- this DLL was linked ...

--expect-build compares that against a git ref (HEAD by default) or a
literal string, and exits 2 when it does not match. A session has been
spent reading counters off a log that a stale DLL wrote; the exit code is
there so a script can refuse to go on.

Log names are edvr_<tag>_YYYYMMDD_HHMMSS.log, where the tag is `gfx` for
the d3d11 half and `vr` for the openvr half; a second process opening the
same tag in the same second gets millisecond and pid fields appended,
edvr_<tag>_YYYYMMDD_HHMMSS_mmm_pid.log. Native OpenXR logs always carry those
fields: edvr_openxr_YYYYMMDD_HHMMSS_mmm_pid.log. New native logs always land
in edvr_logs\\ beside the executable, with local-time names and UTC body
timestamps. Older native logs beside the DLL have UTC names, and no fields.
Legacy logs honor log.dir in edvr.ini, and this reads
that key rather than assuming the default -- a redirected log directory
is exactly when you would rather not be told there are no logs.

Read-only by construction: it opens files for reading and nothing else,
which is why it has no --dry-run.

--tally vh aggregates a draw-census log instead of dumping lines: it
counts eye-texture DC lines per vh= shader-content hash, splits each
hash's count by its r= render-target token (the per-eye view), and
averages the n=/i= draw arguments, with the DC frame summary lines as
the totals row. The census caps its log output at 16384 lines
(draw_census.cpp), so a long census keeps per-draw detail only for the
first frames; --tally says which frames survive only as summaries
rather than printing an empty table.

Exit 0 when a log was read, 1 when none was found, 2 when --expect-build
did not match.
"""

import argparse
import os
import re
import sys
import tempfile

GAME_EXE = "EliteDangerous64.exe"
LOG_RE = re.compile(r"^edvr_(?P<tag>[a-z0-9]+)_(?P<stamp>\d{8}_\d{6})"
                    r"(?:_(?P<ms>\d{3})_(?P<pid>\d+))?\.log$",
                    re.IGNORECASE)
# `version <string> (build <hex>)`, with the linked-at tail optional --
# log.cpp prints a shorter form when the timestamp will not convert.
#
# Every line Log::note() writes is prefixed `[HH:MM:SS.mmm] `, so the
# optional group is not decoration: anchored without it this matched the
# synthetic logs in the self-test and NOTHING in a real one. It still
# anchors at the start of the line rather than searching, so a sentence
# with the word "version" in it cannot be mistaken for the version note.
VERSION_RE = re.compile(r"^(?:\[[\d:.]+\]\s*)?version\s+(?P<ver>\S+)"
                        r"(?:\s+\(build\s+(?P<stamp>[0-9A-Fa-f]+)\))?")
NATIVE_VERSION_RE = re.compile(
    r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} UTC "
    r"pid=\d+ tid=\d+ module_init,version=(?P<ver>[^,\s]+),durable_log=1$")

# Draw-census lines, written by src/d3d11/draw_census.cpp. Every note
# carries the [HH:MM:SS.mmm] prefix like anything else Log::note() writes.
# The per-draw head is fixed: "DC <frame> #<n> <type> n=.. i=.. r=.."; the
# variable tail (vs=/vh=/ia=/...) comes after. DC begin/end/frame/id and
# the DCC/DCL/DCS/DCX/DCS lines must not match: the frame-and-# anchor
# excludes them, so a grep for "] DC " counts spurious lines this doesn't.
CENSUS_DRAW_RE = re.compile(
    r"^(?:\[[\d:.]+\]\s*)?(?P<kind>DC|DCO) (?P<frame>\d+) #(?P<idx>\d+) "
    r"(?P<type>\S+) n=(?P<n>\d+) i=(?P<i>\d+) r=(?P<r>\S+)")
# vh= lives in the IA tail, which readDrawState can skip under budget
# pressure -- a draw line without it is real and lands in the "(none)"
# bucket. Anchored on whitespace: an unanchored search also matches the
# "pr=" token two fields later.
VH_RE = re.compile(r"(?:^|\s)vh=([0-9A-Fa-f]+)")
# "DC frame <n> draws=.. off=.. copies=.. disp=.. clears=.. unseen=.."
CENSUS_FRAME_RE = re.compile(
    r"^(?:\[[\d:.]+\]\s*)?DC frame (?P<frame>\d+) draws=(?P<draws>\d+) "
    r"off=(?P<off>\d+) copies=(?P<copies>\d+) disp=(?P<disp>\d+) "
    r"clears=(?P<clears>\d+) unseen=(?P<unseen>\d+)")
# "DC end census=.. draws=.. ... lines=<cap> ... truncated=<dropped>"
CENSUS_END_RE = re.compile(r"^(?:\[[\d:.]+\]\s*)?DC end\b")
CENSUS_LINES_RE = re.compile(r"\blines=(\d+)")
CENSUS_TRUNC_RE = re.compile(r"\btruncated=(\d+)")


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_text(path):
    """Logs are ASCII in practice, but a shader name or a path can carry
    anything. Decode explicitly as UTF-8 -- never leave it to the console
    codepage, which is what turned an em-dash into mojibake on a public
    issue comment once."""
    with open(path, "rb") as f:
        return f.read().decode("utf-8", errors="replace")


def config_log_dir(game_dir):
    """log.dir out of the target's edvr.ini, if it sets one.

    A section-insensitive scan: the key is read as `log.dir` by the DLL,
    and the ini carries it under a [log] section as `dir`. Both spellings
    appear in the wild, so accept either rather than quietly finding
    nothing.
    """
    ini = os.path.join(game_dir, "edvr.ini")
    if not os.path.isfile(ini):
        return None
    section = ""
    try:
        text = read_text(ini)
    except OSError:
        return None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith((";", "#")):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().lower()
            continue
        if "=" not in line:
            continue
        key, _, val = line.partition("=")
        key = key.strip().lower()
        val = val.strip().strip('"')
        full = key if "." in key else (section + "." + key if section else key)
        if full == "log.dir" and val:
            return val
    return None


def log_dir_for(game_dir):
    configured = config_log_dir(game_dir)
    if configured:
        return configured if os.path.isabs(configured) else \
            os.path.join(game_dir, configured)
    return os.path.join(game_dir, "edvr_logs")


def log_dirs_for(game_dir, tag):
    """Native OpenXR always follows the executable; legacy halves honor log.dir."""
    native = os.path.join(game_dir, "edvr_logs")
    legacy = log_dir_for(game_dir)
    if tag == "openxr":
        return [native]
    if tag == "all" and os.path.normcase(os.path.abspath(native)) != os.path.normcase(os.path.abspath(legacy)):
        return [legacy, native]
    return [legacy]


def find_logs(directory, tag=None):
    """Newest first. The name carries the timestamp, so it sorts without
    stat()ing anything -- and a file copied off another rig keeps the time
    it was written rather than the time it was copied."""
    if not os.path.isdir(directory):
        return []
    out = []
    for name in os.listdir(directory):
        m = LOG_RE.match(name)
        if not m:
            continue
        suffixed = m.group("ms") is not None
        if m.group("tag").lower() == "openxr" and not suffixed:
            continue   # an older native log with a UTC name, not this build's
        if tag and m.group("tag").lower() != tag.lower():
            continue
        # A legacy log opened in the same second as another carries the suffix
        # too, and sorts after the one that took the plain name.
        stamp = m.group("stamp") + ("_" + m.group("ms") if suffixed else "")
        out.append((stamp, m.group("tag"),
                    os.path.join(directory, name)))
    out.sort(reverse=True)
    return out


def version_line(text):
    """(line, version, link stamp) for the log's own version note."""
    for line in text.splitlines():
        m = VERSION_RE.search(line)
        if m:
            return line.strip(), m.group("ver"), m.group("stamp")
        m = NATIVE_VERSION_RE.match(line)
        if m:
            return line.strip(), m.group("ver"), None
    return None, None, None


def describe_cmd(ref, root):
    """The `git describe` to run for --expect-build.

    `--dirty` and a commit-ish "cannot be used together" -- git calls that
    fatal. Asking for both made every `--expect-build HEAD` fail silently
    and fall back to comparing the log against the literal string "HEAD",
    which can never match: an instrument that reports a mismatch on every
    run looks exactly like one that works. So --dirty is asked for only
    where it means something, which is HEAD.
    """
    cmd = ["git", "-C", root, "describe", "--tags", "--always"]
    if ref in ("HEAD", "", None):
        cmd.append("--dirty")
    else:
        cmd.append(ref)
    return cmd


def expected_version(ref, root):
    """A git ref becomes `git describe`; anything else is taken literally,
    so a version copied out of a release note works too."""
    import subprocess
    try:
        out = subprocess.run(describe_cmd(ref, root), capture_output=True,
                             text=True, timeout=10)
        if out.returncode == 0 and out.stdout.strip():
            return out.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return ref


def version_matches(actual, expect):
    """A build's version is `git describe` output; the log may carry a
    -dirty or -N-g<hash> suffix the expectation does not, and the commit
    hash is the part that decides. Compare on the trailing g<hash> when
    both have one, and fall back to substring either way."""
    if not actual or not expect:
        return False
    if actual == expect:
        return True
    a = re.search(r"g([0-9a-f]{7,})", actual)
    e = re.search(r"g([0-9a-f]{7,})", expect)
    if a and e:
        short = min(len(a.group(1)), len(e.group(1)))
        return a.group(1)[:short] == e.group(1)[:short]
    # `git describe` on a tagged commit prints the bare tag, with no hash.
    return expect in actual or actual in expect


def parse_census(text):
    """Split a log into census draw records, frame summaries and the
    truncation stats from its DC end line.

    Returns (draws, summaries, cap, dropped): draws are dicts with kind
    ("DC" eye-texture / "DCO" offscreen), frame, r (render-target token),
    vh (content hash or None) and the n=/i= arguments; summaries map a
    frame ordinal to the DC frame line's counters; cap and dropped come
    from the DC end line (None when the log has none).
    """
    draws = []
    summaries = {}
    cap = None
    dropped = None
    for raw in text.splitlines():
        m = CENSUS_DRAW_RE.match(raw)
        if m:
            vh = VH_RE.search(raw)
            draws.append({
                "kind": m.group("kind"),
                "frame": int(m.group("frame")),
                "r": m.group("r"),
                "vh": vh.group(1).upper() if vh else None,
                "n": int(m.group("n")),
                "i": int(m.group("i")),
            })
            continue
        m = CENSUS_FRAME_RE.match(raw)
        if m:
            summaries[int(m.group("frame"))] = m.groupdict()
            continue
        if CENSUS_END_RE.match(raw):
            lines_m = CENSUS_LINES_RE.search(raw)
            trunc_m = CENSUS_TRUNC_RE.search(raw)
            if lines_m:
                cap = int(lines_m.group(1))
            if trunc_m:
                dropped = int(trunc_m.group(1))
    return draws, summaries, cap, dropped


def tally_vh(draws, frame=None):
    """Group eye-texture DC lines by vh= hash for one tally table.

    Returns (rows, eye_total, off_total): rows are dicts sorted by count
    descending -- vh, count, sub (r= token -> count, so the per-eye
    split is visible), avg_n and avg_i -- eye_total is the number of DC
    lines the percentages divide by, off_total the DCO lines kept out of
    the table. frame restricts to one frame ordinal; DCO lines never
    enter the rows, only the off_total.
    """
    scope = [d for d in draws if frame is None or d["frame"] == frame]
    eye = [d for d in scope if d["kind"] == "DC"]
    off_total = sum(1 for d in scope if d["kind"] == "DCO")
    by_vh = {}
    for d in eye:
        row = by_vh.setdefault(d["vh"], {"count": 0, "sub": {}, "n": 0, "i": 0})
        row["count"] += 1
        row["sub"][d["r"]] = row["sub"].get(d["r"], 0) + 1
        row["n"] += d["n"]
        row["i"] += d["i"]
    rows = []
    for vh, row in by_vh.items():
        rows.append({
            "vh": vh,
            "count": row["count"],
            "sub": row["sub"],
            "avg_n": row["n"] / float(row["count"]),
            "avg_i": row["i"] / float(row["count"]),
        })
    rows.sort(key=lambda r: (-r["count"], r["vh"] or ""))
    return rows, len(eye), off_total


def print_vh_tally(text, frame):
    """The --tally vh report: per-hash table, then the summary totals.
    Returns the process exit code."""
    draws, summaries, cap, dropped = parse_census(text)
    if not draws and not summaries:
        print("[edvr] no draw-census lines (DC/DCO/DC frame) in this log")
        return 0

    detail_frames = sorted({d["frame"] for d in draws})
    summary_frames = sorted(summaries)
    if dropped:
        only_summary = [f for f in summary_frames if f not in detail_frames]
        where = (", ".join(str(f) for f in detail_frames) or "none")
        print("[edvr] census truncated: %d line(s) dropped past the %s-line "
              "cap; per-draw detail survives for frame(s) %s%s."
              % (dropped, cap or "?", where,
                 (", %s summary-only" % ", ".join(str(f) for f in only_summary))
                 if only_summary else ""))

    if frame is not None:
        scope_summary = summaries.get(frame)
        if frame not in detail_frames:
            if scope_summary is not None:
                s = scope_summary
                print("[edvr] frame %d has no per-draw lines -- only its "
                      "summary survived the census line cap:" % frame)
                print("[edvr] frame %d summary: draws=%s off=%s copies=%s "
                      "disp=%s clears=%s unseen=%s"
                      % (frame, s["draws"], s["off"], s["copies"],
                         s["disp"], s["clears"], s["unseen"]))
                return 0
            print("[edvr] frame %d appears in no census line or summary"
                  % frame)
            return 0

    rows, eye_total, off_total = tally_vh(draws, frame)
    label = "frame %d" % frame if frame is not None else "all frames"
    print("[edvr] tally vh, %s: %d eye-texture DC lines, %d offscreen DCO "
          "lines" % (label, eye_total, off_total))
    if not rows:
        print("[edvr] no eye-texture DC lines in scope")
        return 0
    sub_width = max([len("per r=")] + [
        len("  ".join("%s:%d" % (r, c) for r, c in
                      sorted(row["sub"].items(),
                             key=lambda kv: (-kv[1], kv[0]))))
        for row in rows])
    print("%-4s  %-16s  %5s  %6s  %-*s  %9s  %9s"
          % ("rank", "vh", "count", "% eye", sub_width, "per r=",
             "avg n", "avg i"))
    for rank, row in enumerate(rows, 1):
        sub = "  ".join("%s:%d" % (r, c) for r, c in
                        sorted(row["sub"].items(),
                               key=lambda kv: (-kv[1], kv[0])))
        pct = 100.0 * row["count"] / eye_total if eye_total else 0.0
        print("%-4d  %-16s  %5d  %5.1f%%  %-*s  %9.1f  %9.2f"
              % (rank, row["vh"] or "(no vh=)", row["count"], pct,
                 sub_width, sub, row["avg_n"], row["avg_i"]))
    for f in summary_frames:
        if frame is not None and f != frame:
            continue
        s = summaries[f]
        print("[edvr] frame %d summary: draws=%s off=%s copies=%s disp=%s "
              "clears=%s unseen=%s"
              % (f, s["draws"], s["off"], s["copies"], s["disp"],
                 s["clears"], s["unseen"]))
    return 0


def _products_under(root):
    found = []
    products = os.path.join(root, "Products")
    if not os.path.isdir(products):
        return found
    for name in sorted(os.listdir(products)):
        leaf = os.path.join(products, name)
        if os.path.isfile(os.path.join(leaf, GAME_EXE)):
            found.append(os.path.normpath(leaf))
    return found


def resolve_target(spec):
    if spec not in ("steam", "frontier"):
        p = os.path.abspath(spec)
        if os.path.isdir(p):
            return p
        raise SystemExit("[edvr] no such directory: %s" % p)
    # Deliberately shares no code with install_edvr.py's fuller search:
    # this one only has to find a log, and a wrong guess here costs a
    # message rather than an overwritten DLL.
    if spec == "frontier":
        local = os.environ.get("LOCALAPPDATA", "")
        found = _products_under(os.path.join(local, "Frontier_Developments")) \
            if local else []
    else:
        found = []
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                                r"Software\Valve\Steam") as k:
                steam = winreg.QueryValueEx(k, "SteamPath")[0]
        except (ImportError, OSError):
            steam = r"C:\Steam"
        found = _products_under(os.path.join(steam, "steamapps", "common",
                                             "Elite Dangerous"))
    if not found:
        raise SystemExit("[edvr] no %s install found; pass a path to "
                         "--target." % spec)
    if len(found) > 1:
        raise SystemExit("[edvr] %d installs found; name one:\n       %s"
                         % (len(found), "\n       ".join(found)))
    return found[0]


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Locate and read EDVR flight logs.")
    ap.add_argument("--target", default="steam",
                    help="steam, frontier, or a path to the game directory")
    ap.add_argument("--dir", default=None,
                    help="read this log directory directly, ignoring --target")
    ap.add_argument("--file", default=None, help="read exactly this log file")
    ap.add_argument("--tag", default="gfx",
                    help="gfx (d3d11), vr (legacy OpenVR proxy, retired 2026-09-16), "
                         "openxr (native OpenXR), or all")
    ap.add_argument("--nth", type=int, default=0,
                    help="0 is the newest log, 1 the one before it")
    ap.add_argument("--list", action="store_true",
                    help="list the logs found and stop")
    ap.add_argument("--version", action="store_true",
                    help="print the log's version line and stop")
    ap.add_argument("--expect-build", default=None,
                    help="a git ref (HEAD) or literal version; exit 2 if the "
                         "log was not written by that build")
    ap.add_argument("--grep", default=None,
                    help="print only lines matching this regular expression")
    ap.add_argument("--tail", type=int, default=None,
                    help="print only the last N lines (after --grep)")
    ap.add_argument("--tally", choices=["vh"], default=None,
                    help="aggregate instead of dumping: vh counts eye-texture "
                         "DC lines per vh= hash, split by r= render-target "
                         "token, with the DC frame summaries as totals")
    ap.add_argument("--frame", type=int, default=None,
                    help="with --tally, restrict to this census frame ordinal; "
                         "frames past the census line cap have no per-draw "
                         "lines and are reported as summaries")
    ap.add_argument("--root", default=None,
                    help="repository to resolve --expect-build against")
    ap.add_argument("--self-test", action="store_true",
                    help="check this script against itself and exit")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    if args.file:
        path = os.path.abspath(args.file)
        if not os.path.isfile(path):
            print("[edvr] no such log: %s" % path)
            return 1
        logs = [("", "", path)]
    else:
        directories = [args.dir] if args.dir else log_dirs_for(resolve_target(args.target), args.tag.lower())
        logs = []
        for directory in directories:
            logs.extend(find_logs(directory, None if args.tag == "all" else args.tag))
        logs.sort(reverse=True)
        if not logs:
            print("[edvr] no edvr_%s_*.log in %s"
                  % (args.tag, ", ".join(directories)))
            return 1
        if args.list:
            print("[edvr] %d log(s) in %s:" % (len(logs), ", ".join(directories)))
            for stamp, tag, p in logs:
                print("       %-4s %s  %s"
                      % (tag, stamp, os.path.basename(p)))
            return 0
        if args.nth >= len(logs):
            print("[edvr] only %d log(s); --nth %d is past the end"
                  % (len(logs), args.nth))
            return 1
        logs = [logs[args.nth]]

    path = logs[0][2]
    text = read_text(path)
    print("[edvr] %s  (%d lines, %.1f KB)"
          % (path, text.count("\n") + 1, len(text) / 1024.0))

    line, ver, stamp = version_line(text)
    if line:
        print("[edvr] %s" % line)
    else:
        print("[edvr] WARNING: this log has no version line -- it may be "
              "truncated, or not an EDVR log.")

    if args.expect_build:
        want = expected_version(args.expect_build,
                                os.path.abspath(args.root) if args.root
                                else repo_root())
        if version_matches(ver, want):
            print("[edvr] build matches: %s" % want)
        else:
            print("[edvr] BUILD MISMATCH\n"
                  "       log says   %s\n"
                  "       expected   %s\n"
                  "       This flight is not evidence about that build. "
                  "Reinstall and fly again."
                  % (ver or "(nothing)", want))
            return 2

    if args.version:
        return 0

    if args.tally:
        return print_vh_tally(text, args.frame)

    lines = text.splitlines()
    if args.grep:
        try:
            rx = re.compile(args.grep)
        except re.error as e:
            print("[edvr] bad --grep pattern: %s" % e)
            return 1
        lines = [l for l in lines if rx.search(l)]
        print("[edvr] %d line(s) match %s" % (len(lines), args.grep))
    if args.tail is not None:
        lines = lines[-args.tail:]
    if args.grep or args.tail is not None:
        for l in lines:
            print(l)
    return 0


def self_test():
    ok = True

    for name, want in (("edvr_gfx_20260910_042313.log", "gfx"),
                       ("edvr_vr_20260910_042313.log", "vr"),
                       ("edvr_openxr_20260914_042313_007_1234.log", "openxr"),
                       ("notalog.txt", None)):
        m = LOG_RE.match(name)
        got = m.group("tag") if m else None
        if got != want:
            print("LOG_RE on %s -> %r, want %r" % (name, got, want))
            ok = False

    # The exact line log.cpp writes, long form and short.
    long_form = ("[00:00:00.001] version 0.14.1-93-gf78eba4 (build 68C0A1F2) "
                 "-- this DLL was linked 2026-09-09 20:34:39 UTC")
    _, ver, stamp = version_line("first line\n" + long_form + "\nmore\n")
    if ver != "0.14.1-93-gf78eba4" or stamp != "68C0A1F2":
        print("version_line long form -> %r %r" % (ver, stamp))
        ok = False
    _, ver2, stamp2 = version_line("[00:00:00.001] version unversioned test "
                                   "build\n")
    if ver2 != "unversioned":
        print("version_line short form -> %r" % ver2)
        ok = False
    # And a sentence that merely contains the word is not the version note.
    _, ver3, _ = version_line("[00:00:01.500] the version of the shader is 3\n")
    if ver3 is not None:
        print("version_line matched prose: %r" % ver3)
        ok = False

    native = ("2026-09-13 14:01:42.659 UTC pid=1234 tid=5678 "
              "module_init,version=v0.16.2-77-gab80a6c-dirty,durable_log=1")
    native2 = native.replace("14:01:42.659", "14:01:43.001")
    _, nver, nstamp = version_line("noise version=v0.0.0\n" + native + "\n" + native2 + "\n")
    if nver != "v0.16.2-77-gab80a6c-dirty" or nstamp is not None:
        print("native version_line -> %r %r" % (nver, nstamp)); ok = False
    for label in ("v0.16.2", "ab80a6c", "ab80a6c-dirty"):
        if version_line(native.replace("v0.16.2-77-gab80a6c-dirty", label))[1] != label:
            print("native version label rejected: %r" % label); ok = False
    for bad in ("native module_init,version=v0.16.2-77-gab80a6c,durable_log=1",
                native.replace("durable_log=1", "durable_log=0"),
                native.replace("UTC", "LOCAL"),
                "[14:01:42.659] module_init,version=v0.16.2-77-gab80a6c,durable_log=1"):
        if version_line(bad)[1] is not None:
            print("native false positive: %r" % bad); ok = False

    # Matching has to tolerate the suffixes `git describe` adds, and must
    # still refuse a genuinely different commit -- the whole point.
    # HEAD asks about the working tree; a named ref must not, because
    # git refuses the combination outright.
    if "--dirty" not in describe_cmd("HEAD", "."):
        print("describe_cmd(HEAD) does not ask about a dirty tree")
        ok = False
    named = describe_cmd("v0.14.0", ".")
    if "--dirty" in named or named[-1] != "v0.14.0":
        print("describe_cmd(v0.14.0) -> %r" % named)
        ok = False

    for a, e, want in (
            ("0.14.1-93-gf78eba4", "0.14.1-93-gf78eba4", True),
            ("0.14.1-93-gf78eba4", "0.14.1-93-gf78eba4-dirty", True),
            ("0.14.1-93-gf78eba4", "0.14.1-40-gc468661", False),
            ("v0.14.0", "v0.14.0", True),
            (None, "v0.14.0", False)):
        if version_matches(a, e) != want:
            print("version_matches(%r, %r) != %s" % (a, e, want))
            ok = False

    tmp = tempfile.mkdtemp(prefix="edvr_log_test_")
    try:
        game = os.path.join(tmp, "game")
        logs = os.path.join(game, "edvr_logs")
        os.makedirs(logs)
        for stamp_s in ("20260910_040000", "20260910_050000"):
            with open(os.path.join(logs, "edvr_gfx_%s.log" % stamp_s),
                      "wb") as f:
                # With the timestamp prefix Log::note() really writes: a
                # fixture without it is what hid a regex that matched
                # nothing in the field.
                f.write(("[00:00:00.001] version 0.14.1-93-gf78eba4 "
                         "(build 68C0A1F2) -- this DLL was linked "
                         "2026-09-09 20:34:39 UTC\n"
                         "[00:00:12.400] Stats[40] ships 3\n"
                         "[00:00:12.400] Stats[41] ships 0\n"
                         "[00:00:12.401] something else\n").encode("utf-8"))
        with open(os.path.join(logs, "edvr_vr_20260910_060000.log"),
                  "wb") as f:
            f.write(b"[00:00:00.001] version 0.14.1-93-gf78eba4 "
                    b"(build 68C0A1F2)\n")
        with open(os.path.join(logs, "edvr_openxr_20260910_060001_123_77.log"),
                  "wb") as f:
            f.write(b"2026-09-13 14:01:42.659 UTC pid=1234 tid=5678 "
                    b"module_init,version=v0.16.2-77-gab80a6c,durable_log=1\n")

        # The default log directory, and the ini's override, both found.
        if log_dir_for(game) != logs:
            print("log_dir_for did not default to edvr_logs")
            ok = False
        other = os.path.join(tmp, "elsewhere")
        os.makedirs(other)
        with open(os.path.join(game, "edvr.ini"), "wb") as f:
            f.write(("[log]\ndir = %s\n" % other).encode("utf-8"))
        if log_dir_for(game) != other:
            print("log_dir_for ignored log.dir in the ini")
            ok = False
        if log_dirs_for(game, "openxr") != [logs] or log_dirs_for(game, "all") != [other, logs]:
            print("native and redirected legacy discovery did not remain independent")
            ok = False
        os.remove(os.path.join(game, "edvr.ini"))

        # Native discovery remains beside the executable even if legacy logs
        # are redirected by log.dir, and --all combines both directories.
        if len(log_dirs_for(game, "openxr")) != 1 or \
                log_dirs_for(game, "openxr")[0] != logs:
            print("native discovery did not use executable edvr_logs")
            ok = False
        if main(["--dir", logs, "--tag", "openxr", "--version"]) != 0:
            print("native --version discovery failed")
            ok = False

        # Newest first, and the tag filter separates the two halves.
        found = find_logs(logs, "gfx")
        if len(found) != 2 or not found[0][2].endswith("050000.log"):
            print("find_logs did not return the newest gfx log first: %r"
                  % [os.path.basename(p) for _, _, p in found])
            ok = False
        if len(find_logs(logs, "vr")) != 1:
            print("find_logs tag filter is wrong")
            ok = False
        if len(find_logs(logs)) != 4:
            print("find_logs with no tag did not return all four")
            ok = False
        if len(find_logs(logs, "openxr")) != 1:
            print("find_logs native OpenXR tag filter is wrong")
            ok = False
        # A legacy log opened in the same second as another: log.cpp gives it
        # the suffix, and it is a log like any other, later than the plain
        # name it lost the second to.
        same_second = os.path.join(logs, "edvr_gfx_20260910_040000_001_1.log")
        with open(same_second, "wb") as f:
            f.write(b"a second process in the same second\n")
        found = [os.path.basename(p) for _, _, p in find_logs(logs, "gfx")]
        if found != ["edvr_gfx_20260910_050000.log", "edvr_gfx_20260910_040000_001_1.log",
                     "edvr_gfx_20260910_040000.log"]:
            print("a suffixed legacy log is not found, or sorts wrong: %r" % found)
            ok = False
        os.remove(same_second)
        with open(os.path.join(logs, "edvr_openxr_20260910_060002.log"), "wb") as f:
            f.write(b"an old native log with a UTC name\n")
        if len(find_logs(logs, "openxr")) != 1:
            print("an unsuffixed native name was accepted")
            ok = False
        os.remove(os.path.join(logs, "edvr_openxr_20260910_060002.log"))
        # Distinct native Init attempts within one second sort by milliseconds.
        newer_native = os.path.join(logs, "edvr_openxr_20260910_060001_999_2.log")
        with open(newer_native, "wb") as f:
            f.write(b"native later in the same second\n")
        if find_logs(logs, "openxr")[0][2] != newer_native:
            print("native millisecond ordering is wrong")
            ok = False
        os.remove(newer_native)

        newest = os.path.join(logs, "edvr_gfx_20260910_050000.log")
        if main(["--file", newest, "--version"]) != 0:
            print("--version on a good log did not exit 0")
            ok = False
        # The exit code a caller keys off: 2, distinct from 1.
        rc = main(["--file", newest, "--expect-build",
                   "0.14.1-40-gc468661", "--version"])
        if rc != 2:
            print("a build mismatch exited %d, want 2" % rc)
            ok = False
        rc = main(["--file", newest, "--expect-build",
                   "0.14.1-93-gf78eba4", "--version"])
        if rc != 0:
            print("a matching build exited %d, want 0" % rc)
            ok = False
        if main(["--dir", logs, "--tag", "gfx", "--grep", r"Stats\[4[01]\]"]) \
                != 0:
            print("--grep exited nonzero on a readable log")
            ok = False
        if main(["--file", os.path.join(logs, "nope.log")]) != 1:
            print("a missing log did not exit 1")
            ok = False

        # --tally vh: the fixture feeds the parser the way a located log
        # does, through main() on a directory the tool discovers on its
        # own. Frame 2 carries only a DC frame summary -- the shape a
        # truncated census leaves behind.
        import contextlib
        import io

        census_dir = os.path.join(tmp, "census_logs")
        os.makedirs(census_dir)
        census_lines = [
            "[00:00:00.001] version 0.14.1-93-gf78eba4 (build 68C0A1F2)\n",
            "[00:00:01.000] DC begin census=1 frames=3 frame=100 offscreen=yes\n",
            "[00:00:01.001] DC 0 #0 X n=10 i=1 r=@10 d=@50 c=- s=-,-,-,- "
            "vs=@1 vh=AAAAAAAAAAAAAAAA vb=@2 sd=8 of=0 tp=4 ia=0,0,0 "
            "ib=@3 x=-,-,-,- q=0\n",
            "[00:00:01.002] DC 0 #1 X n=20 i=1 r=@10 d=@50 c=- s=-,-,-,- "
            "vs=@1 vh=aaaaaaaaaaaaaaaa vb=@2 ia=0,0,0 ib=@3 q=1\n",
            "[00:00:01.003] DC 0 #2 X n=60 i=1 r=@11 d=@50 c=- s=-,-,-,- "
            "vs=@1 vh=AAAAAAAAAAAAAAAA vb=@2 ia=0,0,0 ib=@3 q=2\n",
            "[00:00:01.004] DC 0 #3 X n=30 i=2 r=@11 d=@50 c=- s=-,-,-,- "
            "vs=@1 vh=BBBBBBBBBBBBBBBB vb=@2 ia=0,0,0 ib=@3 q=3\n",
            "[00:00:01.005] DC 0 #4 X n=40 i=4 r=@12 d=@50 c=- s=-,-,-,- "
            "vs=@1 vh=BBBBBBBBBBBBBBBB vb=@2 ia=0,0,0 ib=@3 q=4\n",
            # A draw whose IA tail was skipped under budget pressure has
            # no vh=: it tallies, in its own bucket.
            "[00:00:01.006] DC 0 #5 X n=50 i=1 r=- d=@50 c=- s=-,-,-,- q=5\n",
            "[00:00:01.007] DCO 0 #0 X n=5 i=1 r=- d=@51 c=- s=-,-,-,- "
            "vs=@4 vh=CCCCCCCCCCCCCCCC vb=@5 ia=0,0,0 ib=@6 q=6\n",
            "[00:00:01.008] DCO 0 #1 X n=6 i=1 r=@20 d=@51 c=- s=-,-,-,- "
            "vs=@4 vh=CCCCCCCCCCCCCCCC vb=@5 ia=0,0,0 ib=@6 q=7\n",
            "[00:00:01.009] DC frame 0 draws=8 off=2 copies=7 disp=9 "
            "clears=3 unseen=0\n",
            "[00:00:01.010] DC 1 #0 X n=8 i=1 r=@10 d=@50 c=- s=-,-,-,- "
            "vs=@1 vh=DDDDDDDDDDDDDDDD vb=@2 ia=0,0,0 ib=@3 q=8\n",
            "[00:00:01.011] DC 1 #1 X n=2 i=1 r=@30 d=@50 c=- s=-,-,-,- "
            "vs=@1 vh=DDDDDDDDDDDDDDDD vb=@2 ia=0,0,0 ib=@3 q=9\n",
            "[00:00:01.012] DC frame 1 draws=2 off=0 copies=0 disp=0 "
            "clears=0 unseen=0\n",
            "[00:00:01.013] DC frame 2 draws=9 off=1 copies=0 disp=0 "
            "clears=1 unseen=0\n",
            "[00:00:01.014] DC end census=1 draws=19 off=3 copies=7 disp=9 "
            "clears=4 unseen=0 lines=16384 interned=2048 overflow=0 "
            "truncated=12\n",
        ]
        census_log = os.path.join(census_dir, "edvr_gfx_20260910_070000.log")
        with open(census_log, "wb") as f:
            f.write("".join(census_lines).encode("utf-8"))

        # Parser-level: counting order, percent base, per-eye split by
        # r=, DCO separation, lowercase hash folding, the no-vh bucket.
        draws, summaries, cap, dropped = parse_census("".join(census_lines))
        if len(draws) != 10 or set(summaries) != {0, 1, 2} \
                or cap != 16384 or dropped != 12:
            print("parse_census -> %d draws, frames %r, cap %r, dropped %r"
                  % (len(draws), sorted(summaries), cap, dropped))
            ok = False
        rows, eye_total, off_total = tally_vh(draws)
        if eye_total != 8 or off_total != 2:
            print("tally_vh totals -> eye %d off %d, want 8/2"
                  % (eye_total, off_total))
            ok = False
        if [r["vh"] for r in rows] != ["AAAAAAAAAAAAAAAA", "BBBBBBBBBBBBBBBB",
                                       "DDDDDDDDDDDDDDDD", None]:
            print("tally_vh order/vh -> %r" % [r["vh"] for r in rows])
            ok = False
        if rows[0]["count"] != 3 or rows[0]["sub"] != {"@10": 2, "@11": 1} \
                or rows[0]["avg_n"] != 30.0 or rows[0]["avg_i"] != 1.0:
            print("tally_vh row A -> %r" % rows[0])
            ok = False
        if rows[1]["count"] != 2 or rows[1]["sub"] != {"@11": 1, "@12": 1} \
                or rows[1]["avg_n"] != 35.0 or rows[1]["avg_i"] != 3.0:
            print("tally_vh row B -> %r" % rows[1])
            ok = False
        if any(r["vh"] == "CCCCCCCCCCCCCCCC" for r in rows):
            print("an offscreen DCO hash leaked into the eye table")
            ok = False

        # Frame restriction keeps only that frame's lines.
        rows1, eye1, _ = tally_vh(draws, frame=1)
        if eye1 != 2 or len(rows1) != 1 or rows1[0]["vh"] != "DDDDDDDDDDDDDDDD" \
                or rows1[0]["sub"] != {"@10": 1, "@30": 1}:
            print("tally_vh frame=1 -> %r eye %d" % (rows1, eye1))
            ok = False
        # A frame with a summary but no lines -- the truncated-census
        # shape -- tallies empty instead of guessing.
        rows2, eye2, _ = tally_vh(draws, frame=2)
        if rows2 or eye2 != 0:
            print("tally_vh frame=2 -> %r eye %d" % (rows2, eye2))
            ok = False

        def run_census_tally(argv):
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = main(argv)
            return rc, buf.getvalue()

        rc, out = run_census_tally(["--dir", census_dir, "--tally", "vh",
                                    "--frame", "0"])
        if rc != 0 or "6 eye-texture DC lines, 2 offscreen DCO lines" not in out:
            print("--tally vh --frame 0 rc=%d header missing:\n%s" % (rc, out))
            ok = False
        for want in ("50.0%", "AAAAAAAAAAAAAAAA", "BBBBBBBBBBBBBBBB",
                     "@10:2", "frame 0 summary: draws=8 off=2 copies=7 "
                     "disp=9 clears=3 unseen=0"):
            if want not in out:
                print("--tally vh --frame 0 output lacks %r:\n%s" % (want, out))
                ok = False
        rc, out = run_census_tally(["--dir", census_dir, "--tally", "vh",
                                    "--frame", "2"])
        if rc != 0 or "only its summary survived" not in out \
                or "frame 2 summary: draws=9" not in out:
            print("--tally vh --frame 2 did not report the summary-only "
                  "frame (rc=%d):\n%s" % (rc, out))
            ok = False
        rc, out = run_census_tally(["--dir", census_dir, "--tally", "vh"])
        if rc != 0 or "truncated: 12 line(s)" not in out \
                or "2 summary-only" not in out:
            print("--tally vh did not report truncation (rc=%d):\n%s"
                  % (rc, out))
            ok = False
        rc, out = run_census_tally(["--dir", census_dir, "--tally", "vh",
                                    "--frame", "9"])
        if rc != 0 or "appears in no census line or summary" not in out:
            print("--tally vh --frame 9 rc=%d:\n%s" % (rc, out))
            ok = False
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)

    print("self-test: %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
