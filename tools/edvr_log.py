#!/usr/bin/env python3
"""Find the log a flight just wrote, and say whether it is the build you made.

    python tools/edvr_log.py --target steam --version
    python tools/edvr_log.py --target steam --expect-build HEAD
    python tools/edvr_log.py --target steam --grep "Stats\\[4[0-9]\\]"
    python tools/edvr_log.py --target steam --tail 80
    python tools/edvr_log.py --target frontier --tally vh
    python tools/edvr_log.py --target frontier --tally vh --frame 1
    python tools/edvr_log.py --target frontier --tally periodic --expect-build HEAD
    python tools/edvr_log.py --target frontier --tally periodic --window-ms 250
    python tools/edvr_log.py --target frontier --tally periodic --infer-runs
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

--tally periodic answers one question about one flight: which periodic work
coincides with long frames. It reads the graphics log and the runtime log
that opened nearest it (--runtime-file names one), takes every `periodic
work:` line (src/common/periodic_work.h: a summary per 30 s window and the
SLOW runs, each with the local time the run finished) and every long frame
(the graphics log's LONG FRAME lines, the runtime's native_long_cycle
lines), puts all of it on one local clock, and counts per operation the long
frames that come within --window-ms of one of its events. A long frame ends
at its line's time and lasts its ms, so an event inside it counts. The two
logs use different clocks; the report says which. The runtime log's UTC
offset is read off its own file name (local) against its first line (UTC),
not from the machine running this, so a supporter's logs convert too. Only
what the timing wrote can be matched -- a summary names one run per 30 s and
SLOW lines are limited per operation -- so a long frame with no event beside
it does not show the operation idle; the report prints the number of matches
chance alone would give, and says plainly when a log has no `periodic work:`
lines at all or the runtime log is missing. --infer-runs adds the runs nobody
logged for an operation on a timer whose cadence two logged times confirm (the
journal re-glob: every 4 s, slow every time, a logged time for one run in
three), marked est. A flight may cross midnight; a DST change inside one is
not handled.

Exit 0 when a log was read, 1 when none was found, 2 when --expect-build
did not match (--tally periodic checks the runtime log against it too).
"""

import argparse
import bisect
import calendar
import datetime
import os
import re
import statistics
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


# --tally periodic: the phase-0 timing of periodic work, laid against long
# frames. What each source writes, and on which clock (read out of the source
# 2026-09-29):
#
#   graphics log  Log::note (src/common/log.cpp) prefixes "[HH:MM:SS.mmm] " from
#                 GetLocalTime: LOCAL time of day, no date. The file name is local
#                 as well and carries the date.
#   runtime log   nativeTracePrintf -> NativeTrace::write (src/openxr/
#                 native_trace.h) prefixes "YYYY-MM-DD HH:MM:SS.mmm UTC pid=<pid>
#                 tid=<tid> " from GetSystemTime: UTC. Its FILE NAME is local
#                 (GetLocalTime, with millisecond and pid fields), which gives the
#                 UTC offset without asking this machine's time zone.
#   periodic work "periodic work: <op> n=<runs> total=<ms> max=<ms> at HH:MM:SS.mmm
#                 slow=<runs>[ <label>=<v>]" (a 30 s summary) and "periodic work:
#                 <op> SLOW ms=<ms> at HH:MM:SS.mmm[ <label>=<v>]" (one slow run),
#                 src/common/periodic_work.h. `at` is GetLocalTime in BOTH logs
#                 (frame_cycle_report goes to the runtime log with the same local
#                 clock, native_runtime_host.h recordFrameCycleReport), read when
#                 the run FINISHED, so the run covers [at - ms, at].
#   LONG FRAME    "monitor: LONG FRAME -- <ms> ms between Presents ..." in the
#                 graphics log, src/d3d11/perf_monitor.cpp: written by the
#                 post-Present block that measured the frame, so the line's time is
#                 the frame's END and the frame covers [time - ms, time]. The DLL
#                 rate-limits these lines: a sample, not a count.
#   long cycle    "native_long_cycle,sequence=..,cycle_ms=..,period_ms=.." in the
#                 runtime log, native_runtime_host.h noteLongCycle: written as the
#                 next WaitGetPoses returns, so again the END. Its session-close
#                 line "native_long_cycle_summary,count=..,logged=.." has the true
#                 count. The LONG FRAME line's "runtime sequence N" is the same
#                 counter as the cycle's sequence= (to within a frame), which is
#                 how the two clocks are checked against each other.
#
# Everything is put on one clock: seconds since 2000-01-01 00:00 LOCAL, as a
# float, so a subtraction is a duration whichever log a time came from.

GFX_STAMP_RE = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d{3})\] ")
RT_STAMP_RE = re.compile(
    r"^(\d{4})-(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d)\.(\d{3}) UTC pid=\d+ tid=\d+ ")
PERIODIC_SLOW_RE = re.compile(
    r"periodic work: (?P<op>\w+) SLOW ms=(?P<ms>[0-9.]+) "
    r"at (?P<at>\d\d:\d\d:\d\d\.\d{3})(?: (?P<label>\w+)=(?P<ctx>\d+))?(?:\s|$)")
PERIODIC_WINDOW_RE = re.compile(
    r"periodic work: (?P<op>\w+) n=(?P<n>\d+) total=(?P<total>[0-9.]+) "
    r"max=(?P<max>[0-9.]+) at (?P<at>\d\d:\d\d:\d\d\.\d{3}) slow=(?P<slow>\d+)"
    r"(?: (?P<label>\w+)=(?P<ctx>\d+))?(?:\s|$)")
# Anchored on the message start: the flip-timeline dump also says "monitor:
# LONG FRAME" but continues "-- this line is about FRAME", which is no frame.
LONG_FRAME_RE = re.compile(
    r"monitor: LONG FRAME -- (?P<ms>[0-9.]+) ms between Presents")
LONG_FRAME_SEQ_RE = re.compile(r"\bruntime sequence (?P<seq>\d+)")
LONG_FRAME_CAP_RE = re.compile(
    r"monitor: (?P<logged>\d+) dropped or long frames were logged this session "
    r"\(of (?P<cap>\d+) at most\)")
LONG_CYCLE_RE = re.compile(
    r"native_long_cycle,sequence=(?P<seq>\d+),cycle_ms=(?P<ms>[0-9.]+),"
    r"period_ms=(?P<period>[0-9.]+),")
LONG_CYCLE_SUMMARY_RE = re.compile(
    r"native_long_cycle_summary,count=(?P<count>\d+),logged=(?P<logged>\d+)")

DAY = 86400.0
EPOCH = datetime.datetime(2000, 1, 1)
PAIR_TOLERANCE_S = 900.0    # a runtime log opens within seconds of its graphics log
# The operations the source times, in the order they are listed. A new one
# still tallies when it appears; this only lets "not seen" name what is absent.
KNOWN_OPS = {"gfx": ("journal_status", "journal_tail", "journal_reglob",
                     "xinput_probe", "luma_round", "ui_layer_totals"),
             "rt": ("frame_cycle_report",)}
SRC_LABEL = {"gfx": "LONG FRAME", "rt": "native_long_cycle"}
MATCHED_ROWS = 30   # rows of section 3; a busy flight has more matches than a reader needs


def log_name_time(name):
    """(local datetime the file name says the log opened, tag, has the
    millisecond and pid fields), or None when the name is not an EDVR log's."""
    m = LOG_RE.match(os.path.basename(name))
    if not m:
        return None
    try:
        opened = datetime.datetime.strptime(m.group("stamp"), "%Y%m%d_%H%M%S")
    except ValueError:
        return None
    if m.group("ms") is not None:
        opened += datetime.timedelta(milliseconds=int(m.group("ms")))
    return opened, m.group("tag").lower(), m.group("ms") is not None


def _utc_of(m):
    """The datetime an RT_STAMP_RE match names (naive, UTC), or None."""
    try:
        return datetime.datetime(int(m.group(1)), int(m.group(2)),
                                 int(m.group(3)), int(m.group(4)),
                                 int(m.group(5)), int(m.group(6)),
                                 int(m.group(7)) * 1000)
    except ValueError:
        return None


def runtime_to_local(rt_path, rt_text):
    """(to_local, how, first line's UTC datetime): what to add to the runtime
    log's UTC prefix to get local time, and where that came from.

    The file name is local time (GetLocalTime) and the first line follows the
    file's creation by milliseconds, so first line minus name is the UTC offset
    to within a second; offsets are whole quarter hours, so it rounds. A name
    without the millisecond and pid fields (an older log) or a first line that
    disagrees with it falls back to this machine's zone, and `how` says so."""
    first_utc = None
    for raw in rt_text.splitlines():
        m = RT_STAMP_RE.match(raw)
        if m:
            first_utc = _utc_of(m)
            if first_utc is not None:
                break
    if first_utc is None:
        return (datetime.timedelta(0),
                "no timestamped line to read it from; treated as UTC", None)
    named = log_name_time(rt_path) if rt_path else None
    if named and named[1] == "openxr" and named[2]:
        raw_s = (first_utc - named[0]).total_seconds()
        quarter = round(raw_s / 900.0) * 900.0
        if abs(raw_s - quarter) <= 2.0:
            return (datetime.timedelta(seconds=-quarter),
                    "from the log's file name (local) against its first line (UTC)",
                    first_utc)
    try:
        stamp = calendar.timegm(first_utc.timetuple())
        local = datetime.datetime.fromtimestamp(stamp)
        return (local - first_utc.replace(microsecond=0),
                "from this machine's time zone; the file name could not give it",
                first_utc)
    except (OverflowError, OSError, ValueError):
        return (datetime.timedelta(0),
                "no time zone available; treated as UTC", first_utc)


def _at_time(at_text, t_line):
    """`at HH:MM:SS.mmm` is a time of day. Give it the day that puts it nearest
    the line that carries it: a run finished at most one 30 s window before the
    line that reports it, so the nearest day is the right one, midnight
    included."""
    tod = (int(at_text[0:2]) * 3600 + int(at_text[3:5]) * 60 +
           int(at_text[6:8]) + int(at_text[9:12]) / 1000.0)
    return tod + DAY * round((t_line - tod) / DAY)


def _op_stats(scan, op):
    return scan["ops"].setdefault(op, {
        "windows": 0, "runs": 0, "total_ms": 0.0, "slow_runs": 0,
        "slow_lines": 0, "max_ms": -1.0, "max_at": None, "max_ctx": ""})


def _unparsed(scan, msg):
    """A line that opens like one of ours but matches none of the formats: the
    C++ side changed its wording. Counted and shown, never dropped quietly --
    a parser that drifted from what it reads would look like a quiet flight."""
    scan["unparsed"]["count"] += 1
    if len(scan["unparsed"]["samples"]) < 2:
        scan["unparsed"]["samples"].append(msg[:120])


def _note_line(scan, msg, t):
    """File one line's message, stamped t, into the scan if it is one of ours."""
    if msg.startswith("periodic work: "):
        m = PERIODIC_SLOW_RE.match(msg)
        if m:
            at = _at_time(m.group("at"), t)
            ms = float(m.group("ms"))
            ctx = ("%s=%s" % (m.group("label"), m.group("ctx"))
                   if m.group("label") else "")
            st = _op_stats(scan, m.group("op"))
            st["slow_lines"] += 1
            if ms > st["max_ms"]:
                st["max_ms"], st["max_at"], st["max_ctx"] = ms, at, ctx
            scan["events"].append({"op": m.group("op"), "kind": "slow",
                                   "at": at, "ms": ms, "ctx": ctx,
                                   "src": scan["kind"]})
            return
        m = PERIODIC_WINDOW_RE.match(msg)
        if m:
            at = _at_time(m.group("at"), t)
            top = float(m.group("max"))
            ctx = ("%s=%s" % (m.group("label"), m.group("ctx"))
                   if m.group("label") else "")
            st = _op_stats(scan, m.group("op"))
            st["windows"] += 1
            st["runs"] += int(m.group("n"))
            st["total_ms"] += float(m.group("total"))
            st["slow_runs"] += int(m.group("slow"))
            if top > st["max_ms"]:
                st["max_ms"], st["max_at"], st["max_ctx"] = top, at, ctx
            scan["events"].append({"op": m.group("op"), "kind": "max",
                                   "at": at, "ms": top, "ctx": ctx,
                                   "src": scan["kind"]})
            # The summary is written by the run that closes the window, so its
            # line time is that run's end (infer_runs reads the cadence off it).
            scan["windows"].append({"op": m.group("op"), "close": t,
                                    "n": int(m.group("n")),
                                    "total": float(m.group("total"))})
            return
        _unparsed(scan, msg)
        return
    if msg.startswith("monitor: LONG FRAME -- "):
        m = LONG_FRAME_RE.match(msg)
        if m:
            seq = LONG_FRAME_SEQ_RE.search(msg)
            scan["frames"].append({"src": "gfx", "t": t,
                                   "ms": float(m.group("ms")),
                                   "seq": int(seq.group("seq")) if seq else 0})
        else:
            _unparsed(scan, msg)
        return
    if msg.startswith("native_long_cycle,"):
        m = LONG_CYCLE_RE.match(msg)
        if m:
            scan["frames"].append({"src": "rt", "t": t,
                                   "ms": float(m.group("ms")),
                                   "seq": int(m.group("seq"))})
        else:
            _unparsed(scan, msg)
        return
    if msg.startswith("native_long_cycle_summary,"):
        m = LONG_CYCLE_SUMMARY_RE.match(msg)
        if m:
            scan["cycle_summary"] = (int(m.group("count")),
                                     int(m.group("logged")))
        return
    if msg.startswith("monitor: ") and "dropped or long frames were logged" in msg:
        m = LONG_FRAME_CAP_RE.match(msg)
        if m:
            scan["frame_cap"] = (int(m.group("logged")), int(m.group("cap")))


def scan_flight_log(text, kind, base_days=0, start_tod=None,
                    to_local=datetime.timedelta(0)):
    """One pass over a log: its `periodic work:` lines and its long frames on
    the common clock, plus the first and last stamped line.

    kind "gfx": a local time-of-day prefix. base_days is the flight's first day
    (days since 2000-01-01) and start_tod the time of day the file name says the
    log opened; the prefix has no date, so the day rolls over whenever the clock
    goes back by more than half a day (a line stamped just before a midnight
    already crossed, which threads can write out of order, keeps the old day).
    kind "rt": a full UTC prefix, turned to local by to_local."""
    scan = {"kind": kind, "stamped": 0, "first": None, "last": None,
            "ops": {}, "events": [], "frames": [], "windows": [],
            "cycle_summary": None, "frame_cap": None,
            "unparsed": {"count": 0, "samples": []}}
    day = 0
    prev = start_tod
    for raw in text.splitlines():
        if kind == "gfx":
            m = GFX_STAMP_RE.match(raw)
            if not m:
                continue
            tod = (int(m.group(1)) * 3600 + int(m.group(2)) * 60 +
                   int(m.group(3)) + int(m.group(4)) / 1000.0)
            line_day = day
            if prev is None:
                prev = tod
            elif tod < prev - DAY / 2:
                day += 1
                line_day = day
                prev = tod
            elif tod > prev + DAY / 2:
                line_day = day - 1
            else:
                prev = tod
            t = (base_days + line_day) * DAY + tod
        else:
            m = RT_STAMP_RE.match(raw)
            if not m:
                continue
            utc = _utc_of(m)
            if utc is None:
                continue
            t = (utc + to_local - EPOCH).total_seconds()
        scan["stamped"] += 1
        if scan["first"] is None or t < scan["first"]:
            scan["first"] = t
        if scan["last"] is None or t > scan["last"]:
            scan["last"] = t
        _note_line(scan, raw[m.end():], t)
    return scan


def scan_flight(gfx_path, gfx_text, rt_path=None, rt_text=None):
    """Both logs scanned onto the one local clock. Returns (graphics scan,
    runtime scan or None, clock), clock naming how the runtime's UTC became
    local. The graphics log's date is the one its file name carries; without a
    usable name it is the runtime log's first local day."""
    to_local, how, first_utc = datetime.timedelta(0), None, None
    if rt_text is not None:
        to_local, how, first_utc = runtime_to_local(rt_path, rt_text)
    named = log_name_time(gfx_path) if gfx_path else None
    first_day = start_tod = None
    if named:
        first_day = named[0].date()
        start_tod = (named[0].hour * 3600 + named[0].minute * 60 +
                     named[0].second + named[0].microsecond / 1e6)
    elif first_utc is not None:
        first_day = (first_utc + to_local).date()
    base_days = (first_day - EPOCH.date()).days if first_day else 0
    gscan = scan_flight_log(gfx_text, "gfx", base_days, start_tod)
    rscan = None
    if rt_text is not None:
        rscan = scan_flight_log(rt_text, "rt", to_local=to_local)
    clock = {"to_local": to_local if rt_text is not None else None,
             "how": how}
    return gscan, rscan, clock


def infer_runs(scan, window_s):
    """Runs nobody logged, put back where a fixed cadence says they ran.

    Only a window's slowest run and at most one SLOW line per 10 s carry a
    time, so an operation that runs every 4 s and is slow every time (the
    journal re-glob) has a logged time for a third of its runs. A summary is
    written by the run that closes its window and the window before it closed
    on the run just ahead of this one's first, so an operation on a timer that
    ran n times between two closes ran every (close - previous close) / n.
    That grid is believed only when at least two logged times inside the window
    (its SLOW lines, its slowest run) sit on it to within tol, a frame or so,
    and only when its spacing is at least four windows and a quarter second: a
    window around a run every 100 ms covers everything and proves nothing.

    Returns (events of kind "est", one per grid run no logged time stands for,
    each with its window's mean ms; notes {op: windows accepted, runs inferred,
    median period in s})."""
    by_op = {}
    for w in scan["windows"]:
        by_op.setdefault(w["op"], []).append(w)
    est, notes = [], {}
    for op, wins in by_op.items():
        wins.sort(key=lambda w: w["close"])
        known = sorted(e["at"] for e in scan["events"] if e["op"] == op)
        periods, added = [], 0
        for prev, w in zip(wins, wins[1:]):
            n = w["n"]
            if n < 2:
                continue
            period = (w["close"] - prev["close"]) / n
            if period < max(4.0 * window_s, 0.25):
                continue
            tol = min(0.05, period / 4.0)
            grid = [prev["close"] + i * period for i in range(1, n + 1)]
            # Distinct instants: a slowest run that also has a SLOW line is one
            # run, named twice, and confirms nothing more than once.
            inside = sorted({round(k, 3) for k in known
                             if prev["close"] < k <= w["close"] + tol})
            if len(inside) < 2 or any(min(abs(k - g) for g in grid) > tol
                                      for k in inside):
                continue
            periods.append(period)
            for g in grid:
                if all(abs(k - g) > tol for k in known):
                    est.append({"op": op, "kind": "est", "at": g,
                                "ms": w["total"] / n, "ctx": "",
                                "src": scan["kind"]})
                    added += 1
        if periods:
            notes[op] = {"windows": len(periods), "runs": added,
                         "period": statistics.median(periods)}
    return est, notes


def pair_runtime_log(gfx_path, native_dirs):
    """The runtime log that goes with a graphics log: the edvr_openxr_*.log
    that opened nearest it, within PAIR_TOLERANCE_S (both are opened by the one
    game process, seconds apart). Returns (path, None), or (None, why)."""
    named = log_name_time(gfx_path)
    if not named:
        return None, ("the graphics log's name carries no timestamp to pair "
                      "by; name the runtime log with --runtime-file")
    best = None
    for directory in native_dirs:
        for _, _, path in find_logs(directory, "openxr"):
            other = log_name_time(path)
            if not other:
                continue
            gap = abs((other[0] - named[0]).total_seconds())
            if best is None or gap < best[0]:
                best = (gap, path)
    if best is None:
        return None, "no edvr_openxr_*.log in %s" % ", ".join(native_dirs)
    if best[0] > PAIR_TOLERANCE_S:
        return None, ("the nearest edvr_openxr_*.log (%s) opened %.0f min from "
                      "the graphics log, more than %.0f min"
                      % (os.path.basename(best[1]), best[0] / 60.0,
                         PAIR_TOLERANCE_S / 60.0))
    return best[1], None


def nearest_event(frame, events):
    """The event closest to a long frame [t - ms, t]: gap 0 when the event's
    end lies inside it, else the distance to the nearer edge. offset_ms is the
    event's end minus the frame's end (negative: it finished first). None when
    there are no events."""
    start = frame["t"] - frame["ms"] / 1000.0
    end = frame["t"]
    best = None
    for ev in events:
        gap = max(0.0, start - ev["at"], ev["at"] - end)
        key = (gap, abs(ev["at"] - end))
        if best is None or key < best[0]:
            best = (key, ev)
    if best is None:
        return None
    return {"event": best[1], "gap_s": best[0][0],
            "offset_ms": (best[1]["at"] - end) * 1000.0}


def analyse_periodic(events, frames, window_s, span_s):
    """Lay the long frames against the periodic events.

    A long frame [t - ms, t] coincides with an operation when one of its
    events' end times lies within window_s of that interval. A window summary
    and a SLOW line that name the same run (same end time) are one event.
    chance is the matches independence would give: each event lands in a given
    frame's padded interval with probability (ms + 2 window) / span, so the
    expected matches are the sum over frames of min(1, events * that).

    Returns events (op -> sorted distinct events), hits and chance (both keyed
    (op, source)), counts and touched (per source: long frames, and those with
    any match), matches (each long frame with a match and, per operation that
    matched, its largest event there; the largest event first) and top (per
    source: the ten longest, each with its nearest event and whether it is
    within the window)."""
    distinct = {}
    for ev in sorted(events, key=lambda e: (e["at"], e["kind"] != "slow")):
        distinct.setdefault((ev["op"], round(ev["at"], 3)), ev)
    by_op = {}
    for (op, _), ev in distinct.items():
        by_op.setdefault(op, []).append(ev)
    for lst in by_op.values():
        lst.sort(key=lambda e: e["at"])
    times = {op: [e["at"] for e in lst] for op, lst in by_op.items()}
    hits, chance = {}, {}
    counts = {"gfx": 0, "rt": 0}
    touched = {"gfx": 0, "rt": 0}
    matches = []
    for f in frames:
        lo = f["t"] - f["ms"] / 1000.0 - window_s
        hi = f["t"] + window_s
        counts[f["src"]] += 1
        found = []
        for op, ts in times.items():
            i = bisect.bisect_left(ts, lo)
            biggest = None
            while i < len(ts) and ts[i] <= hi:
                if biggest is None or by_op[op][i]["ms"] > biggest["ms"]:
                    biggest = by_op[op][i]
                i += 1
            if biggest is not None:
                hits[(op, f["src"])] = hits.get((op, f["src"]), 0) + 1
                found.append(biggest)
            if span_s > 0:
                chance[(op, f["src"])] = chance.get((op, f["src"]), 0.0) + \
                    min(1.0, len(ts) * (hi - lo) / span_s)
        if found:
            touched[f["src"]] += 1
            matches.append({"frame": f, "events": found})
    matches.sort(key=lambda m: (-max(e["ms"] for e in m["events"]),
                                m["frame"]["t"]))
    flat = [e for lst in by_op.values() for e in lst]
    top = {"gfx": [], "rt": []}
    for src in top:
        longest = sorted((f for f in frames if f["src"] == src),
                         key=lambda f: (-f["ms"], f["t"]))[:10]
        for f in longest:
            near = nearest_event(f, flat)
            top[src].append({"frame": f, "near": near,
                             "hit": bool(near and near["gap_s"] <= window_s)})
    return {"events": by_op, "hits": hits, "chance": chance,
            "counts": counts, "touched": touched, "matches": matches,
            "top": top}


def clock_check(frames):
    """Pair each LONG FRAME line with the native_long_cycle line of the same
    runtime sequence and return (pairs, median of graphics time minus runtime
    time in ms, None without pairs). The two describe one hitch a few ms apart;
    a wrong UTC offset would show as minutes or hours."""
    cycles = [f for f in frames if f["src"] == "rt" and f["seq"]]
    deltas = []
    for f in frames:
        if f["src"] != "gfx" or not f["seq"]:
            continue
        near = [c for c in cycles if abs(c["seq"] - f["seq"]) <= 2]
        if near:
            c = min(near, key=lambda c: abs(c["t"] - f["t"]))
            deltas.append((f["t"] - c["t"]) * 1000.0)
    return len(deltas), (statistics.median(deltas) if deltas else None)


def _kind_label(ev, long=False):
    """What sort of time an event carries: a SLOW line, a window's slowest run,
    or a run placed by cadence (infer_runs)."""
    if ev["kind"] == "slow":
        return "SLOW"
    if ev["kind"] == "est":
        return "inferred run" if long else "est"
    return "window max" if long else "max"


def fmt_clock(t, day0):
    """HH:MM:SS.mmm of a common-clock time, with the day if it is not the
    flight's first (day0 = that day's index)."""
    ms = int(round(t * 1000.0))
    day, in_day = divmod(ms, 86400000)
    h, rest = divmod(in_day, 3600000)
    m, rest = divmod(rest, 60000)
    s, milli = divmod(rest, 1000)
    text = "%02d:%02d:%02d.%03d" % (h, m, s, milli)
    if day != day0:
        text += " %+dd" % (day - day0)
    return text


def _fmt_zone(delta):
    minutes = int(round(delta.total_seconds() / 60.0))
    return "UTC%s%02d:%02d" % ("+" if minutes >= 0 else "-",
                               abs(minutes) // 60, abs(minutes) % 60)


def print_periodic_report(gfx_path, gfx_text, gfx_ver, want, args, native_dirs):
    """The --tally periodic report. Returns the process exit code."""
    window_s = args.window_ms / 1000.0

    # The runtime log that goes with this graphics log, and whether it is the
    # right build: its lines are as much evidence as the graphics log's.
    rt_path = rt_why = None
    if args.runtime_file:
        rt_path = os.path.abspath(args.runtime_file)
        if not os.path.isfile(rt_path):
            print("[edvr] no such runtime log: %s" % rt_path)
            return 1
    else:
        rt_path, rt_why = pair_runtime_log(gfx_path, native_dirs)
    rt_text = None
    if rt_path:
        rt_text = read_text(rt_path)
        print("[edvr] runtime log: %s  (%d lines, %.1f KB)"
              % (rt_path, rt_text.count("\n") + 1, len(rt_text) / 1024.0))
        rt_line, rt_ver, _ = version_line(rt_text)
        if rt_line:
            print("[edvr] %s" % rt_line)
        else:
            print("[edvr] WARNING: the runtime log has no version line -- it "
                  "may be truncated, or not an EDVR log.")
        if want is not None:
            if version_matches(rt_ver, want):
                print("[edvr] runtime build matches: %s" % want)
            else:
                print("[edvr] BUILD MISMATCH (runtime log)\n"
                      "       log says   %s\n"
                      "       expected   %s\n"
                      "       This flight is not evidence about that build. "
                      "Reinstall and fly again."
                      % (rt_ver or "(nothing)", want))
                return 2
        elif gfx_ver and rt_ver and not version_matches(gfx_ver, rt_ver):
            print("[edvr] WARNING: the runtime log is from build %s and the "
                  "graphics log from %s; they are not one flight of one build."
                  % (rt_ver, gfx_ver))
    else:
        print("[edvr] runtime log: NONE FOUND -- %s.\n"
              "       native_long_cycle and frame_cycle_report lines are "
              "unavailable; only the graphics log is read." % rt_why)

    gscan, rscan, clock = scan_flight(gfx_path, gfx_text, rt_path, rt_text)
    if gscan["stamped"] == 0:
        print("[edvr] the graphics log has no [HH:MM:SS.mmm] lines; "
              "--tally periodic reads edvr_gfx_*.log (a runtime log goes to "
              "--runtime-file).")
        return 1
    if rscan is not None and rscan["stamped"] == 0:
        print("[edvr] WARNING: the runtime log has no 'YYYY-MM-DD HH:MM:SS.mmm "
              "UTC pid= tid=' lines; it is not used.")
        rscan = None

    day0 = int(gscan["first"] // DAY)

    def clk(t):
        return fmt_clock(t, day0)

    def span_text(sc):
        return "%s .. %s (%.1f s)" % (clk(sc["first"]), clk(sc["last"]),
                                      sc["last"] - sc["first"])

    print("[edvr] periodic work vs long frames; window +/-%g ms" % args.window_ms)
    print("[edvr] clocks (every time below is LOCAL):")
    print("       graphics log  local time of day, [HH:MM:SS.mmm] prefix; no "
          "date, so the date is the file name's and midnight rolls it over")
    if rscan is not None:
        print("       runtime log   UTC prefix, converted at %s (%s)"
              % (_fmt_zone(clock["to_local"]), clock["how"]))
    print("       `at` in a `periodic work:` line: local time of day, in both "
          "logs")
    print("[edvr] graphics log spans %s" % span_text(gscan))
    if rscan is not None:
        print("[edvr] runtime log spans  %s" % span_text(rscan))
        if rscan["last"] < gscan["first"] or rscan["first"] > gscan["last"]:
            print("[edvr] WARNING: the two logs' time spans do not overlap: "
                  "the clock conversion is wrong, or these are not one flight.")

    scans = [gscan] + ([rscan] if rscan is not None else [])
    for sc, where in ((gscan, "graphics"), (rscan, "runtime")):
        if sc is not None and sc["unparsed"]["count"]:
            print("[edvr] WARNING: %d line(s) in the %s log open like a "
                  "`periodic work:`, LONG FRAME or native_long_cycle line but "
                  "match none of the formats this reads; the C++ wording has "
                  "probably changed, and what follows leaves them out. First: %s"
                  % (sc["unparsed"]["count"], where,
                     " | ".join(sc["unparsed"]["samples"])))

    # 1. What the timing saw, per operation.
    print("[edvr] 1. periodic work, per operation")
    length = gscan["last"] - gscan["first"]
    if not gscan["ops"]:
        if length >= 60.0:
            print("[edvr] NO `periodic work:` lines in the graphics log (it "
                  "spans %.0f s). The timing was never wired into this build, "
                  "or the build is wrong; check the version line above and "
                  "rerun with --expect-build HEAD." % length)
        else:
            print("[edvr] no `periodic work:` lines in the graphics log, which "
                  "spans only %.0f s; the first 30 s summary may not be "
                  "written yet." % length)
    if rscan is not None and not rscan["ops"]:
        print("[edvr] no `periodic work:` lines in the runtime log (its build "
              "predates the timing, or it spans under 30 s).")
    order = {op: i for i, op in enumerate(KNOWN_OPS["gfx"] + KNOWN_OPS["rt"])}
    rows = sorted(((op, sc["kind"], st) for sc in scans
                   for op, st in sc["ops"].items()),
                  key=lambda r: (order.get(r[0], len(order)), r[0]))
    if rows:
        print("%-19s %-3s %7s %8s %10s %8s %8s %9s %10s  %-12s %s"
              % ("operation", "log", "windows", "runs", "total ms", "mean ms",
                 "max ms", "slow runs", "SLOW lines", "max at", "at the max"))
        for op, src, st in rows:
            mean = ("%.3f" % (st["total_ms"] / st["runs"])) if st["runs"] else "-"
            print("%-19s %-3s %7d %8d %10.3f %8s %8.3f %9d %10d  %-12s %s"
                  % (op, src, st["windows"], st["runs"], st["total_ms"], mean,
                     st["max_ms"], st["slow_runs"], st["slow_lines"],
                     clk(st["max_at"]), st["max_ctx"]))
    for sc, where in ((gscan, "graphics"), (rscan, "runtime")):
        if sc is None or not sc["ops"]:
            continue
        absent = [op for op in KNOWN_OPS[sc["kind"]] if op not in sc["ops"]]
        if absent:
            print("[edvr] not seen in the %s log: %s (it never ran this flight, "
                  "or this build does not time it; absence does not show it was "
                  "fast)" % (where, ", ".join(absent)))

    # 2. The long frames, and which operation each coincides with.
    frames = list(gscan["frames"]) + (list(rscan["frames"]) if rscan else [])
    print("[edvr] 2. long frames")
    cap = gscan["frame_cap"]
    if cap and cap[0] >= cap[1]:
        note = (" (the DLL's session cap of %d lines was reached, so later long "
                "frames are not in the log)" % cap[1])
    else:
        note = (" (the DLL rate-limits these lines: a sample of the long "
                "frames, not a count)")
    print("       LONG FRAME lines, graphics log: %d%s"
          % (len(gscan["frames"]), note))
    if rscan is not None:
        if rscan["cycle_summary"]:
            note = (" (the runtime counted %d over twice its predicted period "
                    "and logged %d)" % rscan["cycle_summary"])
        else:
            note = (" (no native_long_cycle_summary: the runtime did not close "
                    "its trace, so the true count is unknown)")
        print("       native_long_cycle lines, runtime log: %d%s"
              % (len(rscan["frames"]), note))
        pairs, median = clock_check(frames)
        if median is None:
            print("       clock check: no LONG FRAME line shares a runtime "
                  "sequence with a native_long_cycle line, so the two clocks "
                  "could not be checked against each other")
        elif abs(median) > 300000.0:
            print("       clock check: WARNING %d LONG FRAME line(s) share a "
                  "runtime sequence with a native_long_cycle line, %.0f s "
                  "apart at the median: the UTC offset is wrong; nothing "
                  "across the two logs can be trusted" % (pairs, median / 1000.0))
        else:
            print("       clock check: %d LONG FRAME line(s) share a runtime "
                  "sequence with a native_long_cycle line, %+.0f ms apart at "
                  "the median" % (pairs, median))
    if not frames:
        print("[edvr] no long frames in either log: nothing to lay against the "
              "periodic work.")
        return 0
    if args.infer_runs:
        notes = {}
        for sc in scans:
            more, found = infer_runs(sc, window_s)
            sc["events"] = sc["events"] + more
            notes.update(found)
        for op in sorted(notes):
            print("       inferred runs: %s, %d run(s) in %d window(s), every "
                  "%.2f s (each window's schedule has two or more logged times "
                  "on it)" % (op, notes[op]["runs"], notes[op]["windows"],
                              notes[op]["period"]))
        print("       an inferred run (est) is where the cadence says it ran, "
              "not a logged time" if notes else
              "       inferred runs: no operation had a schedule that two "
              "logged times could confirm")
    spans = [sc["last"] - sc["first"] for sc in scans]
    res = analyse_periodic([e for sc in scans for e in sc["events"]], frames,
                           window_s, max(spans))
    if not res["events"]:
        print("[edvr] no periodic events to lay the long frames against.")
        return 0
    have = [s for s in ("gfx", "rt") if res["counts"][s]]
    print("       long frames within +/-%g ms of any periodic event: %s"
          % (args.window_ms, "; ".join(
              "%s %d of %d" % (SRC_LABEL[s], res["touched"][s], res["counts"][s])
              for s in have)))
    print("       a frame ends at its line's time and lasts its ms; an event "
          "coincides with it when the event ended inside the frame or within "
          "the window of it")
    print("       chance = the matches independence alone would give (events x "
          "frame length over the flight)")
    print("       known/runs = the operation's runs that have a logged time (a "
          "SLOW line, at most one per 10 s, or a window's slowest run); a run "
          "nobody logged cannot coincide with anything here%s"
          % ("" if args.infer_runs else " (--infer-runs adds the ones a "
             "fixed cadence places)"))
    ranked = sorted(res["events"], key=lambda o: (
        -sum(res["hits"].get((o, s), 0) for s in have),
        order.get(o, len(order)), o))
    total_runs = {}
    for op, _, st in rows:
        total_runs[op] = total_runs.get(op, 0) + st["runs"]

    def runs_text(op):
        evs = res["events"][op]
        est = sum(1 for e in evs if e["kind"] == "est")
        return "%d%s/%s" % (len(evs) - est, ("+%d" % est) if est else "",
                            total_runs.get(op) or "?")

    cw = 16 if args.infer_runs else 10
    head = "%-19s %*s" % ("operation", cw,
                          "known+est/runs" if args.infer_runs else "known/runs")
    for s in have:
        head += "  %*s %7s" % (len(SRC_LABEL[s]) + 5, SRC_LABEL[s] + " hits",
                               "chance")
    print(head)
    for op in ranked:
        line = "%-19s %*s" % (op, cw, runs_text(op))
        for s in have:
            line += "  %*s %7.2f" % (
                len(SRC_LABEL[s]) + 5,
                "%d/%d" % (res["hits"].get((op, s), 0), res["counts"][s]),
                res["chance"].get((op, s), 0.0))
        print(line)

    # 3. Every long frame with an event beside it: the counts above say how
    # many, this says which, and an event's own ms says whether it could matter
    # (a 0.01 ms run beside a 190 ms frame is a coincidence, not a cause).
    matches = res["matches"]
    shown = matches[:MATCHED_ROWS]
    print("[edvr] 3. long frames with a periodic event beside them, largest "
          "event first (%d of %d long frame(s))" % (len(matches), len(frames)))
    if shown:
        print("       events: operation, SLOW run or window max, its ms, then "
              "its end minus the frame's end in ms")
        print("       %-17s %-15s %9s  %s"
              % ("source", "frame ends", "ms", "events beside it"))
    for m in shown:
        f = m["frame"]
        beside = "; ".join("%s %s %.3f (%+.0f)" % (
            e["op"], _kind_label(e), e["ms"], (e["at"] - f["t"]) * 1000.0)
            for e in sorted(m["events"], key=lambda e: -e["ms"]))
        print("       %-17s %-15s %9.1f  %s"
              % (SRC_LABEL[f["src"]], clk(f["t"]), f["ms"], beside))
    if len(matches) > len(shown):
        print("       ... and %d more, each with a smaller event beside it"
              % (len(matches) - len(shown)))

    # 4. The ten longest of each, with what was nearest.
    for s in have:
        tops = res["top"][s]
        print("[edvr] 4. the %d longest %s line(s), with the nearest periodic "
              "event" % (len(tops), SRC_LABEL[s]))
        print("       offset = the event's end minus the frame's end (negative: "
              "the event finished first); within = inside the window")
        print("       %-3s %-15s %9s  %-64s %10s  %s"
              % ("#", "frame ends", "ms", "nearest event", "offset ms",
                 "within"))
        for i, row in enumerate(tops, 1):
            f, near = row["frame"], row["near"]
            what = "-"
            offset = "-"
            if near:
                ev = near["event"]
                what = "%s %s %.3f ms, ended %s%s" % (
                    ev["op"], _kind_label(ev, True), ev["ms"], clk(ev["at"]),
                    (" " + ev["ctx"]) if ev["ctx"] else "")
                offset = "%+.1f" % near["offset_ms"]
            verdict = "no"
            if row["hit"]:
                verdict = "yes, inside the frame" if near["gap_s"] == 0.0 \
                    else "yes"
            print("       %-3d %-15s %9.1f  %-64s %10s  %s"
                  % (i, clk(f["t"]), f["ms"], what, offset, verdict))
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
    ap.add_argument("--tally", choices=["vh", "periodic"], default=None,
                    help="aggregate instead of dumping: vh counts eye-texture "
                         "DC lines per vh= hash, split by r= render-target "
                         "token, with the DC frame summaries as totals; "
                         "periodic lays the `periodic work:` timing against "
                         "the flight's long frames (graphics log plus its "
                         "runtime log)")
    ap.add_argument("--frame", type=int, default=None,
                    help="with --tally vh, restrict to this census frame "
                         "ordinal; frames past the census line cap have no "
                         "per-draw lines and are reported as summaries")
    ap.add_argument("--window-ms", type=float, default=100.0,
                    help="with --tally periodic, a long frame coincides with a "
                         "periodic event when the event's end time is inside "
                         "the frame or within this many ms of it (default 100)")
    ap.add_argument("--runtime-file", default=None,
                    help="with --tally periodic, read exactly this runtime "
                         "(edvr_openxr_*.log) log instead of pairing the one "
                         "that opened nearest the graphics log")
    ap.add_argument("--infer-runs", action="store_true",
                    help="with --tally periodic, also place the runs nobody "
                         "logged where a fixed cadence says they ran (only for "
                         "an operation whose schedule two logged times "
                         "confirm); they are marked est, and the default counts "
                         "logged times only")
    ap.add_argument("--root", default=None,
                    help="repository to resolve --expect-build against")
    ap.add_argument("--self-test", action="store_true",
                    help="check this script against itself and exit")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    if args.tally == "periodic":
        if not args.file and args.tag.lower() != "gfx":
            print("[edvr] --tally periodic reads the graphics log (--tag gfx) "
                  "and pairs its runtime log itself; drop --tag %s." % args.tag)
            return 1
        if args.window_ms < 0:
            print("[edvr] --window-ms cannot be negative.")
            return 1
        if args.frame is not None:
            print("[edvr] --frame belongs to --tally vh; ignored here.")

    native_dirs = None
    if args.file:
        path = os.path.abspath(args.file)
        if not os.path.isfile(path):
            print("[edvr] no such log: %s" % path)
            return 1
        logs = [("", "", path)]
        native_dirs = [os.path.dirname(path)]
    else:
        if args.dir:
            directories = [args.dir]
            native_dirs = [args.dir]
        else:
            game_dir = resolve_target(args.target)
            directories = log_dirs_for(game_dir, args.tag.lower())
            native_dirs = log_dirs_for(game_dir, "openxr")
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

    want = None
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

    if args.tally == "periodic":
        return print_periodic_report(path, text, ver, want, args, native_dirs)
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

    if not self_test_periodic():
        ok = False

    print("self-test: %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


def self_test_periodic():
    """--tally periodic on fixtures in a temp directory: the line formats
    (the exact strings the C++ rigs pin), the clocks, the matching, and every
    way the report has to say something is missing. Returns ok."""
    import contextlib
    import io
    import shutil

    ok = True

    def check(cond, what):
        nonlocal ok
        if not cond:
            print("periodic: %s" % what)
            ok = False

    base_days = (datetime.date(2026, 9, 29) - EPOCH.date()).days

    def T(h, m, s):
        """A local time on 2026-09-29, on the common clock."""
        return base_days * DAY + h * 3600 + m * 60 + s

    def long_frame(stamp, ms, seq):
        # The native-path line perf_monitor.cpp writes, tail included.
        return ("[%s] monitor: LONG FRAME -- %.1f ms between Presents (runtime "
                "predicted period 11.1 ms), no WaitGetPoses, CPU busy, "
                "compositor, reprojection, or door samples; game creations: 0 "
                "textures, 0 buffers, 0 shaders (0.0 MB); EDVR events: none. "
                "This is frame 812; the flip timeline is not armed, so there "
                "are no table changes to order against it. runtime sequence "
                "%d, game work 5.20 ms.\r\n" % (stamp, ms, seq))

    def put(path, text):
        with open(path, "wb") as f:
            f.write(text.encode("utf-8"))

    def run(argv):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = main(argv)
        return rc, buf.getvalue()

    version = "0.17.0-5-gabcdef1"
    version_gfx = ("[07:15:30.002] version %s (build 68C0A1F2) -- this DLL was "
                   "linked 2026-09-28 20:34:39 UTC\r\n" % version)

    # --- the exact strings src/common/periodic_work.h's rig pins ------------
    rig = ("[10:00:00.101] periodic work: journal_reglob SLOW ms=3.412 at "
           "10:00:00.100 files=1893\r\n"
           "[10:00:30.000] periodic work: journal_reglob n=5 total=11.012 "
           "max=3.412 at 10:00:00.100 slow=4 files=1893\r\n"
           "[12:34:56.800] periodic work: op_quiet n=31 total=15.500 max=0.500 "
           "at 12:34:56.789 slow=0\r\n")
    sc = scan_flight_log(rig, "gfx", base_days)
    st = sc["ops"].get("journal_reglob", {})
    check(st.get("windows") == 1 and st.get("runs") == 5 and
          st.get("slow_runs") == 4 and st.get("slow_lines") == 1 and
          abs(st.get("total_ms", 0) - 11.012) < 1e-9 and
          st.get("max_ms") == 3.412 and st.get("max_ctx") == "files=1893" and
          abs(st.get("max_at", 0) - T(10, 0, 0.1)) < 1e-6,
          "the SLOW and summary forms with a context did not parse: %r" % st)
    st = sc["ops"].get("op_quiet", {})
    check(st.get("runs") == 31 and st.get("max_ctx") == "" and
          st.get("slow_runs") == 0 and
          abs(st.get("max_at", 0) - T(12, 34, 56.789)) < 1e-6,
          "the summary form without a context did not parse: %r" % st)
    check(len(sc["events"]) == 3 and sc["unparsed"]["count"] == 0,
          "expected 3 events and no unparsed lines from the rig lines")

    tmp = tempfile.mkdtemp(prefix="edvr_log_periodic_")
    try:
        # --- one flight: graphics log (local prefix) + runtime log (UTC) ---
        # Local is UTC-7 here. (a) sits beside a summary's `at` with no SLOW
        # line for it; (b) is 30 ms after a journal_reglob SLOW line and (c)
        # 500 ms after it; (d) and the runtime's cycle (e) are beside the
        # runtime's frame_cycle_report, whose `at` is local in a UTC-prefixed
        # log. The VTableHook line says "monitor: LONG FRAME" and is no frame.
        gfx_text = (
            "[07:15:30.001] EDVR log -- unofficial VR fixes for Elite Dangerous: "
            "Odyssey\r\n" + version_gfx +
            long_frame("07:15:45.300", 33.0, 4001) +
            "[07:15:52.001] periodic work: journal_reglob SLOW ms=3.412 at "
            "07:15:52.000 files=1893\r\n"
            "[07:16:00.100] periodic work: journal_status n=30 total=6.000 "
            "max=0.500 at 07:15:45.250 slow=0\r\n"
            "[07:16:00.101] periodic work: journal_reglob n=5 total=11.012 "
            "max=3.412 at 07:15:52.000 slow=4 files=1893\r\n"
            "[07:16:02.101] periodic work: journal_reglob SLOW ms=2.500 at "
            "07:16:02.100 files=1893\r\n" +
            long_frame("07:16:02.130", 45.3, 4100) +
            "[07:16:02.131] VTableHook device: monitor: LONG FRAME -- this line "
            "is about FRAME 811 (the frame in progress is 812), 3.0 s after "
            "the timeline armed.\r\n" +
            long_frame("07:16:02.600", 30.0, 4110) +
            long_frame("07:16:29.930", 60.0, 4300) +
            "[07:16:31.000] monitor: 4 dropped or long frames were logged this "
            "session (of 60 at most).\r\n")
        rt_text = (
            "2026-09-29 14:15:31.205 UTC pid=4242 tid=1 module_init,version=%s,"
            "durable_log=1\r\n"
            "2026-09-29 14:16:30.010 UTC pid=4242 tid=9 native_long_cycle,"
            "sequence=4300,cycle_ms=58.0000,period_ms=11.1000,"
            "game_before_first_submit=3.0000,units=wall_ms\r\n"
            "2026-09-29 14:16:30.020 UTC pid=4242 tid=9 periodic work: "
            "frame_cycle_report n=2698 total=41.000 max=6.250 at 07:16:29.900 "
            "slow=1 samples=2700\r\n"
            "2026-09-29 14:16:31.000 UTC pid=4242 tid=9 native_long_cycle_summary,"
            "count=88,logged=1,threshold=2x_period\r\n" % version)
        flight = os.path.join(tmp, "flight")
        os.makedirs(flight)
        gfx_path = os.path.join(flight, "edvr_gfx_20260929_071530.log")
        rt_path = os.path.join(flight, "edvr_openxr_20260929_071531_200_4242.log")
        put(gfx_path, gfx_text)
        put(rt_path, rt_text)
        # A runtime log from another day, which must not be the one paired.
        put(os.path.join(flight, "edvr_openxr_20260928_101010_000_1111.log"),
            "2026-09-28 17:10:10.000 UTC pid=1111 tid=1 module_init,version=%s,"
            "durable_log=1\r\n" % version)

        # The clocks. Local is UTC-7, read off the runtime log's own file name.
        gscan, rscan, clock = scan_flight(gfx_path, gfx_text, rt_path, rt_text)
        check(clock["to_local"] == datetime.timedelta(hours=-7) and
              "file name" in clock["how"],
              "the UTC offset was not read off the file name: %r" % (clock,))
        check(len(gscan["frames"]) == 4 and len(rscan["frames"]) == 1,
              "long frame counts: %d LONG FRAME (want 4, the VTableHook line "
              "is no frame), %d native_long_cycle (want 1, its summary line is "
              "no cycle)" % (len(gscan["frames"]), len(rscan["frames"])))
        check(abs(rscan["frames"][0]["t"] - T(7, 16, 30.010)) < 1e-6,
              "a UTC-prefixed native_long_cycle did not land on local time")
        check(abs(rscan["ops"]["frame_cycle_report"]["max_at"] -
                  T(7, 16, 29.900)) < 1e-6,
              "the `at` of a UTC-prefixed frame_cycle_report is not the local "
              "time it names")
        check(rscan["cycle_summary"] == (88, 1) and
              gscan["frame_cap"] == (4, 60),
              "the closing count lines did not parse: %r %r"
              % (rscan["cycle_summary"], gscan["frame_cap"]))
        check(gscan["unparsed"]["count"] == 0 and rscan["unparsed"]["count"] == 0,
              "lines of the real formats were left unparsed: %r %r"
              % (gscan["unparsed"], rscan["unparsed"]))
        check(gscan["ops"]["journal_reglob"]["slow_lines"] == 2 and
              gscan["ops"]["journal_reglob"]["windows"] == 1,
              "journal_reglob: %r" % gscan["ops"]["journal_reglob"])

        events = gscan["events"] + rscan["events"]
        frames = gscan["frames"] + rscan["frames"]
        span = max(s["last"] - s["first"] for s in (gscan, rscan))
        res = analyse_periodic(events, frames, 0.100, span)
        # The summary and the SLOW line that name one run are one event.
        check([len(res["events"][o]) for o in
               ("frame_cycle_report", "journal_reglob", "journal_status")]
              == [1, 2, 1],
              "distinct events per operation: %r"
              % {o: len(v) for o, v in res["events"].items()})
        # (a) is attributed through a window's `at` alone; (b), 30 ms after a
        # journal_reglob SLOW line, to it; (c), 500 ms away, to nothing; (d)
        # and (e) to the runtime's report, across the two clocks.
        check(res["hits"] == {("journal_status", "gfx"): 1,
                              ("journal_reglob", "gfx"): 1,
                              ("frame_cycle_report", "gfx"): 1,
                              ("frame_cycle_report", "rt"): 1},
              "hits at +/-100 ms: %r" % (res["hits"],))
        check(res["counts"] == {"gfx": 4, "rt": 1} and
              res["touched"] == {"gfx": 3, "rt": 1},
              "counts %r touched %r" % (res["counts"], res["touched"]))
        check(all(abs(m["frame"]["t"] - T(7, 16, 2.6)) > 1e-3
                  for m in res["matches"]) and len(res["matches"]) == 4,
              "the frame 500 ms from the SLOW line was attributed to it")
        check([round(m["frame"]["t"] - T(7, 16, 0)) for m in res["matches"]] ==
              [30, 30, 2, -15],
              "matches are not largest event first: %r"
              % [m["frame"]["t"] - T(7, 16, 0) for m in res["matches"]])
        top = res["top"]["gfx"]
        check([round(r["frame"]["ms"], 1) for r in top] == [60.0, 45.3, 33.0, 30.0],
              "the longest frames are not in order")
        if len(top) == 4:
            far = top[3]
            check(far["near"]["event"]["op"] == "journal_reglob" and
                  abs(far["near"]["offset_ms"] + 500.0) < 1e-3 and
                  not far["hit"],
                  "the nearest event to the frame 500 ms away: %r"
                  % (far["near"],))
            check(top[1]["hit"] and top[1]["near"]["gap_s"] == 0.0 and
                  abs(top[1]["near"]["offset_ms"] + 30.0) < 1e-3,
                  "the frame 30 ms after the SLOW line: %r" % (top[1]["near"],))
        expect = sum(2 * (f["ms"] / 1000.0 + 0.2) / span
                     for f in frames if f["src"] == "gfx")
        # (Times are ~8e8 s on the common clock, so a difference carries ~1e-7 s.)
        check(abs(res["chance"][("journal_reglob", "gfx")] - expect) < 1e-6,
              "chance is not events x padded frame / span: %r vs %r"
              % (res["chance"][("journal_reglob", "gfx")], expect))
        tight = analyse_periodic(events, frames, 0.010, span)
        check(("frame_cycle_report", "rt") not in tight["hits"] and
              tight["touched"] == {"gfx": 2, "rt": 0},
              "--window-ms 10 still matched the cycle 52 ms from the run: %r"
              % (tight["hits"],))
        check(nearest_event(frames[0], []) is None, "nearest_event of nothing")
        # The window applies after a frame's end too, and a long frame owns
        # everything that ended inside it however long it ran.
        after = {"op": "x", "kind": "slow", "at": T(8, 0, 0.050), "ms": 2.0,
                 "ctx": "", "src": "gfx"}
        short = {"src": "gfx", "t": T(8, 0, 0), "ms": 30.0, "seq": 0}
        check(analyse_periodic([after], [short], 0.100, 60.0)["hits"] ==
              {("x", "gfx"): 1} and
              analyse_periodic([after], [short], 0.040, 60.0)["hits"] == {},
              "an event ending 50 ms after the frame did / did not match")
        inside = dict(after, at=T(8, 0, 9.0))
        long_one = {"src": "gfx", "t": T(8, 0, 10.0), "ms": 1117.1, "seq": 0}
        check(analyse_periodic([inside], [long_one], 0.010, 60.0)["hits"] ==
              {("x", "gfx"): 1},
              "an event that ended inside a 1.1 s frame did not match")
        pairs, median = clock_check(frames)
        check(pairs == 1 and abs(median + 80.0) < 1e-3,
              "the two clocks were not checked against each other by "
              "sequence: %r %r" % (pairs, median))

        # The report itself, through main(), on the directory it discovers.
        rc, out = run(["--dir", flight, "--tally", "periodic"])
        check(rc == 0, "periodic report exited %d" % rc)
        for want in ("UTC-07:00", "from the log's file name", "journal_reglob",
                     "frame_cycle_report", "07:16:30.010",
                     "graphics log spans 07:15:30.001 .. 07:16:31.000",
                     "LONG FRAME 3 of 4; native_long_cycle 1 of 1",
                     "LONG FRAME lines, graphics log: 4",
                     "the runtime counted 88 over twice its predicted period",
                     "1 LONG FRAME line(s) share a runtime sequence",
                     "-80 ms apart at the median",
                     "long frames with a periodic event beside them",
                     "journal_reglob SLOW 2.500 (-30)",
                     "the 4 longest LONG FRAME line(s)"):
            check(want in out, "the report lacks %r" % want)
        check("edvr_openxr_20260929_071531_200_4242.log" in out and
              "edvr_openxr_20260928" not in out,
              "the runtime log paired is not the one that opened nearest")
        rc, out = run(["--dir", flight, "--tally", "periodic", "--window-ms",
                       "10"])
        check(rc == 0 and "LONG FRAME 2 of 4; native_long_cycle 0 of 1" in out,
              "--window-ms did not narrow the match (rc=%d)" % rc)
        # --version and --expect-build keep their meaning with the new mode.
        rc, out = run(["--dir", flight, "--tally", "periodic", "--version"])
        check(rc == 0 and "periodic work vs long frames" not in out,
              "--version did not stop before the report")
        rc, out = run(["--dir", flight, "--tally", "periodic",
                       "--expect-build", version])
        check(rc == 0 and "runtime build matches" in out and
              "periodic work vs long frames" in out,
              "a matching build did not run the report (rc=%d)" % rc)
        rc, out = run(["--dir", flight, "--tally", "periodic",
                       "--expect-build", "0.17.0-9-g1234567"])
        check(rc == 2 and "BUILD MISMATCH" in out and
              "periodic work vs long frames" not in out,
              "a graphics-log build mismatch did not exit 2 (rc=%d)" % rc)

        # A runtime log from another build: refused with --expect-build,
        # named as a warning without it.
        stale = os.path.join(tmp, "stale")
        os.makedirs(stale)
        put(os.path.join(stale, "edvr_gfx_20260929_071530.log"), gfx_text)
        put(os.path.join(stale, "edvr_openxr_20260929_071531_200_4242.log"),
            rt_text.replace(version, "0.17.0-9-g1234567"))
        rc, out = run(["--dir", stale, "--tally", "periodic",
                       "--expect-build", version])
        check(rc == 2 and "BUILD MISMATCH (runtime log)" in out and
              "periodic work vs long frames" not in out,
              "a stale runtime log was not refused (rc=%d)" % rc)
        rc, out = run(["--dir", stale, "--tally", "periodic"])
        check(rc == 0 and "WARNING: the runtime log is from build "
              "0.17.0-9-g1234567" in out,
              "two builds in one flight went unremarked (rc=%d)" % rc)

        # No runtime log: said plainly, and the graphics half still reports.
        solo = os.path.join(tmp, "solo")
        os.makedirs(solo)
        put(os.path.join(solo, "edvr_gfx_20260929_071530.log"), gfx_text)
        rc, out = run(["--dir", solo, "--tally", "periodic"])
        # (d) needed the runtime's frame_cycle_report, which is not here.
        check(rc == 0 and "runtime log: NONE FOUND" in out and
              "LONG FRAME 2 of 4" in out and "native_long_cycle lines" not in out,
              "a missing runtime log was not reported (rc=%d)" % rc)
        # --infer-runs. slowop runs every 4 s and logs two SLOW lines a window;
        # the frame ends 10 ms after a run nobody logged (08:00:52.000), 4 s
        # from the nearest logged one. sparse has the same cadence but one
        # logged time (its window max), and dense runs every 0.1 s with two on
        # its grid: the first has too little to confirm a schedule and the
        # second spaces its runs too closely for a window around one to test
        # anything, so neither is inferred.
        infer_dir = os.path.join(tmp, "infer")
        os.makedirs(infer_dir)
        infer_text = (
            "[08:00:00.001] version %s (build 68C0A1F2)\r\n"
            "[08:00:10.000] periodic work: dense n=100 total=10.000 max=0.500 "
            "at 08:00:05.000 slow=0\r\n"
            "[08:00:12.001] periodic work: dense SLOW ms=2.100 at 08:00:12.000\r\n"
            "[08:00:16.001] periodic work: dense SLOW ms=2.200 at 08:00:16.000\r\n"
            "[08:00:20.000] periodic work: dense n=100 total=10.000 max=2.200 "
            "at 08:00:16.000 slow=2\r\n"
            "[08:00:36.000] periodic work: slowop n=9 total=22.500 max=2.700 "
            "at 08:00:24.003 slow=9 files=5\r\n"
            "[08:00:36.000] periodic work: sparse n=9 total=4.500 max=0.900 "
            "at 08:00:30.000 slow=0\r\n"
            "[08:00:48.001] periodic work: slowop SLOW ms=2.500 at 08:00:48.000 "
            "files=5\r\n" % version +
            long_frame("08:00:52.010", 30.0, 7000) +
            "[08:00:56.001] periodic work: sparse SLOW ms=0.700 at 08:00:56.000\r\n"
            "[08:01:00.002] periodic work: slowop SLOW ms=2.700 at 08:01:00.001 "
            "files=5\r\n"
            "[08:01:12.000] periodic work: slowop n=9 total=22.500 max=2.700 "
            "at 08:01:00.001 slow=9 files=5\r\n"
            "[08:01:12.000] periodic work: sparse n=9 total=4.500 max=0.700 "
            "at 08:00:56.000 slow=0\r\n")
        put(os.path.join(infer_dir, "edvr_gfx_20260929_080000.log"), infer_text)
        isc = scan_flight_log(infer_text, "gfx", base_days, 8 * 3600)
        more, notes = infer_runs(isc, 0.1)
        check(sorted(notes) == ["slowop"] and notes["slowop"]["runs"] == 7 and
              notes["slowop"]["windows"] == 1 and
              abs(notes["slowop"]["period"] - 4.0) < 1e-6,
              "infer_runs notes: %r" % (notes,))
        check(sorted(round(e["at"] - T(8, 0, 0)) for e in more) ==
              [40, 44, 52, 56, 64, 68, 72] and
              all(e["kind"] == "est" and abs(e["ms"] - 2.5) < 1e-9 for e in more),
              "inferred runs: %r" % [(e["op"], e["at"] - T(8, 0, 0)) for e in more])
        rc, out = run(["--dir", infer_dir, "--tally", "periodic"])
        check(rc == 0 and "LONG FRAME 0 of 1" in out and
              "inferred runs" not in out and " 3/18 " in out,
              "the default counted a run nobody logged (rc=%d):\n%s" % (rc, out))
        rc, out = run(["--dir", infer_dir, "--tally", "periodic", "--infer-runs"])
        check(rc == 0 and "LONG FRAME 1 of 1" in out and
              "inferred runs: slowop, 7 run(s) in 1 window(s), every 4.00 s" in out
              and "slowop est 2.500 (-10)" in out and " 3+7/18 " in out and
              "inferred runs: sparse" not in out and
              "inferred runs: dense" not in out,
              "--infer-runs did not place the unlogged run (rc=%d):\n%s"
              % (rc, out))
        # A zero window still infers: the spacing floor is a quarter second.
        rc, out = run(["--dir", infer_dir, "--tally", "periodic", "--infer-runs",
                       "--window-ms", "0"])
        check(rc == 0 and "inferred runs: slowop" in out and
              "inferred runs: dense" not in out,
              "--window-ms 0 changed the inference (rc=%d)" % rc)

        # The C++ side rewords a line: counted and shown, not a quiet flight.
        drift = os.path.join(tmp, "drift")
        os.makedirs(drift)
        put(os.path.join(drift, "edvr_gfx_20260929_071530.log"), gfx_text +
            "[07:16:32.000] periodic work: journal_tail n=5 total=1.000 "
            "max=0.400 at 07:16:31.900 slows=0\r\n"
            "[07:16:33.000] monitor: LONG FRAME -- about 40 ms between Presents\r\n")
        rc, out = run(["--dir", drift, "--tally", "periodic"])
        check(rc == 0 and "WARNING: 2 line(s) in the graphics log open like" in out
              and "slows=0" in out and "LONG FRAME lines, graphics log: 4" in out,
              "reworded lines went unremarked (rc=%d)" % rc)
        # And one too far from the graphics log's start to be its own.
        far_rt = os.path.join(tmp, "farrt")
        os.makedirs(far_rt)
        put(os.path.join(far_rt, "edvr_gfx_20260929_071530.log"), gfx_text)
        put(os.path.join(far_rt, "edvr_openxr_20260929_080000_000_1.log"),
            rt_text)
        rc, out = run(["--dir", far_rt, "--tally", "periodic"])
        check(rc == 0 and "runtime log: NONE FOUND" in out and
              "more than 15 min" in out,
              "a runtime log 44 min away was paired (rc=%d)" % rc)

        # No `periodic work:` lines at all: not "all was well".
        bare = os.path.join(tmp, "bare")
        os.makedirs(bare)
        put(os.path.join(bare, "edvr_gfx_20260929_070000.log"),
            "[07:00:00.002] version %s (build 68C0A1F2)\r\n" % version +
            long_frame("07:01:30.000", 40.0, 9))
        rc, out = run(["--dir", bare, "--tally", "periodic"])
        check(rc == 0 and "NO `periodic work:` lines in the graphics log" in out
              and "The timing was never wired into this build" in out and
              "no periodic events to lay the long frames against" in out,
              "a log with no periodic lines was not reported as such (rc=%d):"
              "\n%s" % (rc, out))
        put(os.path.join(bare, "edvr_gfx_20260929_070000.log"),
            "[07:00:00.002] version %s (build 68C0A1F2)\r\n" % version +
            long_frame("07:00:10.000", 40.0, 9))
        rc, out = run(["--dir", bare, "--tally", "periodic"])
        check(rc == 0 and "spans only 10 s" in out and
              "NO `periodic work:`" not in out,
              "a 10 s log was told its timing was never wired in")
        # A graphics log with no long frames, and a runtime log named by hand
        # (a quiet one, and 15 minutes after the graphics log) whose span does
        # not overlap it.
        put(os.path.join(bare, "edvr_gfx_20260929_070000.log"),
            "[07:00:00.002] version %s (build 68C0A1F2)\r\n"
            "[07:01:30.000] periodic work: journal_tail n=61 total=0.5 "
            "max=0.013 at 07:01:16.832 slow=0 bytes=0\r\n" % version)
        quiet_rt = os.path.join(bare, "edvr_openxr_20260929_071531_205_4242.log")
        put(quiet_rt, "2026-09-29 14:15:31.205 UTC pid=4242 tid=1 "
            "module_init,version=%s,durable_log=1\r\n" % version)
        rc, out = run(["--file", os.path.join(bare, "edvr_gfx_20260929_070000.log"),
                       "--tally", "periodic", "--runtime-file", quiet_rt])
        check(rc == 0 and "no long frames in either log" in out and
              "the two logs' time spans do not overlap" in out and
              "not seen in the graphics log: journal_status" in out,
              "no-long-frames / disjoint spans / not-seen (rc=%d):\n%s"
              % (rc, out))
        rc, out = run(["--file", os.path.join(bare, "edvr_gfx_20260929_070000.log"),
                       "--tally", "periodic", "--runtime-file",
                       os.path.join(tmp, "nope.log")])
        check(rc == 1 and "no such runtime log" in out,
              "a missing --runtime-file did not exit 1")
        rc, out = run(["--dir", flight, "--tally", "periodic", "--tag",
                       "openxr"])
        check(rc == 1 and "reads the graphics log" in out,
              "--tally periodic accepted a runtime --tag (rc=%d)" % rc)

        # A flight across midnight: the graphics prefix has no date. The frame
        # (00:00:00.010, a day on) ends 70 ms after a run that finished at
        # 23:59:59.940, and the summary line written after midnight names an
        # `at` from before it. The budget variant of the LONG FRAME line.
        night = os.path.join(tmp, "night")
        os.makedirs(night)
        night_text = (
            "[23:59:00.001] version %s (build 68C0A1F2)\r\n"
            "[23:59:59.950] periodic work: journal_tail SLOW ms=2.500 at "
            "23:59:59.940 bytes=512\r\n"
            "[00:00:00.010] monitor: LONG FRAME -- 40.0 ms between Presents "
            "(budget 11.1), of which the thread waited 3.0 in Present (busy "
            "37.0); the game's creations in it: 0 textures, 0 buffers (0.0 MB "
            "together), 0 shaders; EDVR this frame: boundary 0.10 ms.\r\n"
            "[00:00:00.100] periodic work: journal_status n=10 total=1.000 "
            "max=0.400 at 23:59:59.800 slow=0\r\n"
            "[00:00:05.000] a line after midnight\r\n" % version)
        night_path = os.path.join(night, "edvr_gfx_20260929_235900.log")
        put(night_path, night_text)
        ns, _, _ = scan_flight(night_path, night_text)
        frame = ns["frames"][0]
        tail = [e for e in ns["events"] if e["op"] == "journal_tail"][0]
        status = [e for e in ns["events"] if e["op"] == "journal_status"][0]
        check(abs((frame["t"] - tail["at"]) - 0.070) < 1e-6 and
              abs((frame["t"] - status["at"]) - 0.210) < 1e-6 and
              frame["t"] > T(23, 59, 59.9),
              "midnight: frame %.3f after the run, %.3f after the summary's "
              "`at`" % (frame["t"] - tail["at"], frame["t"] - status["at"]))
        check(abs(ns["last"] - (T(24, 0, 5.0))) < 1e-6,
              "the last line of a log that crossed midnight is not on the "
              "next day")
        rc, out = run(["--dir", night, "--tally", "periodic"])
        check(rc == 0 and "00:00:00.010 +1d" in out and
              "journal_tail SLOW 2.500 (-70)" in out and
              "journal_status" in out,
              "the report across midnight (rc=%d):\n%s" % (rc, out))

        # Clock conversion: a zone east of UTC, and the ways the file name
        # cannot give the offset.
        first = "2026-09-29 07:15:31.205 UTC pid=1 tid=1 module_init\r\n"
        delta, how, _ = runtime_to_local(
            "edvr_openxr_20260929_124531_200_4242.log", first)
        check(delta == datetime.timedelta(hours=5, minutes=30) and
              "file name" in how, "UTC+05:30: %r %r" % (delta, how))
        check(_fmt_zone(delta) == "UTC+05:30" and
              _fmt_zone(datetime.timedelta(hours=-7)) == "UTC-07:00",
              "zone text")
        for name in ("edvr_openxr_20260929_071531.log",           # no fields
                     "edvr_openxr_20260929_124931_200_4242.log",  # off by 4 min
                     "renamed.log"):
            delta, how, _ = runtime_to_local(name, first)
            check("time zone" in how,
                  "%s did not fall back to the machine's zone: %r" % (name, how))
        delta, how, first_utc = runtime_to_local("x.log", "no stamps here\r\n")
        check(first_utc is None and delta == datetime.timedelta(0) and
              "treated as UTC" in how, "a runtime log with no timestamps")
        check(fmt_clock(T(7, 16, 2.6) + DAY, base_days) == "07:16:02.600 +1d" and
              fmt_clock(T(23, 59, 59.9996), base_days) == "00:00:00.000 +1d",
              "fmt_clock day suffix or rounding: %r %r"
              % (fmt_clock(T(7, 16, 2.6) + DAY, base_days),
                 fmt_clock(T(23, 59, 59.9996), base_days)))
    except Exception:
        # A parser that stopped finding its lines fails here, not in a flight.
        import traceback
        traceback.print_exc()
        print("periodic: the checks stopped at an exception (above)")
        ok = False
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return ok


if __name__ == "__main__":
    sys.exit(main())
